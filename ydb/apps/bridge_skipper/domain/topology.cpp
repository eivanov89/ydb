#include "topology.h"

#include <ydb/apps/bridge_skipper/config/config.h>

#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <yaml-cpp/yaml.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace NBridgeSkipper {
namespace {

TString Scalar(const YAML::Node& node, const char* path) {
    if (!node || !node.IsScalar()) {
        ythrow yexception() << "Malformed required inventory field " << path;
    }
    return TString(node.Scalar());
}

void Field(TString& result, const TString& value) {
    result += TStringBuilder() << value.size() << ':' << value;
}

std::set<TString> Names(const TTopology& topology) {
    return {topology.ConfiguredPiles.begin(), topology.ConfiguredPiles.end()};
}

} // namespace

TTopology ParseMainConfig(const TString& yaml, const TString& scheme) {
    if (scheme != "grpc" && scheme != "grpcs") {
        ythrow yexception() << "Unsupported inventory endpoint scheme";
    }
    const auto root = YAML::Load(std::string(yaml.data(), yaml.size()));
    const auto config = root["config"];
    if (!root.IsMap() || !config || !config.IsMap()) {
        ythrow yexception() << "Missing config inventory mapping";
    }
    const auto bridge = config["bridge_config"];
    const auto piles = bridge["piles"];
    const auto hosts = config["hosts"];
    if (!bridge || !bridge.IsMap() || !piles || !piles.IsSequence() || !hosts || !hosts.IsSequence()) {
        ythrow yexception() << "Malformed bridge_config/hosts inventory";
    }
    TTopology result;
    std::set<TString> pileNames;
    for (const auto& pile : piles) {
        if (!pile.IsMap()) {
            ythrow yexception() << "Malformed bridge_config.piles entry";
        }
        const auto name = Scalar(pile["name"], "bridge_config.piles[].name");
        if (!pileNames.insert(name).second) {
            ythrow yexception() << "Duplicate configured pile name";
        }
        result.ConfiguredPiles.push_back(name);
    }
    if (pileNames.empty()) {
        ythrow yexception() << "Empty bridge pile inventory";
    }
    std::set<TString> logicalIds;
    for (const auto& host : hosts) {
        if (!host.IsMap()) {
            ythrow yexception() << "Malformed hosts entry";
        }
        TNode node;
        node.Host = Scalar(host["host"], "hosts[].host");
        if (node.Host.empty()) {
            ythrow yexception() << "Empty inventory hostname";
        }
        // hosts[].port is an interconnect port, not an SDK endpoint port.
        node.LogicalId = node.Host;
        if (host["node_id"].IsDefined()) {
            const auto id = host["node_id"].as<ui64>();
            if (!id || id > std::numeric_limits<ui32>::max()) {
                ythrow yexception() << "Invalid inventory node_id";
            }
            node.InventoryNodeId = static_cast<ui32>(id);
            node.LogicalId = TStringBuilder() << "node-id:" << id;
        } else if (host["port"].IsDefined()) {
            const auto port = host["port"].as<ui64>();
            if (!port || port > 65535) {
                ythrow yexception() << "Invalid inventory interconnect port";
            }
            node.LogicalId = TStringBuilder() << "host:" << node.Host.size() << ':' << node.Host << ":ic:" << port;
        }
        if (!logicalIds.insert(node.LogicalId).second) {
            ythrow yexception() << "Duplicate inventory node identity";
        }
        node.Source = "main_config";
        const auto location = host["location"];
        if (location && !location.IsMap()) {
            ythrow yexception() << "Malformed hosts[].location";
        }
        const auto pile = location ? location["bridge_pile_name"] : YAML::Node(YAML::NodeType::Undefined);
        if (pile.IsDefined()) {
            node.Pile = Scalar(pile, "hosts[].location.bridge_pile_name");
            if (!pileNames.contains(*node.Pile)) {
                ythrow yexception() << "Host names an unconfigured pile";
            }
            node.Binding = EBinding::Explicit;
        }
        if (node.Host.Contains("://")) {
            const auto endpoint = ParseEndpoint(node.Host);
            if (endpoint.Scheme != scheme) {
                ythrow yexception() << "Mixed inventory endpoint schemes";
            }
            node.Endpoint = endpoint.Uri;
            node.Host = endpoint.Host;
            node.DirectEndpoint = true;
        }
        result.Nodes.push_back(std::move(node));
    }
    return result;
}

TString CanonicalTopology(const TTopology& topology) {
    TString result;
    for (const auto& pile : Names(topology)) {
        Field(result, pile);
    }
    result += '|';
    std::vector<TString> hosts;
    for (const auto& node : topology.Nodes) {
        TString value;
        Field(value, node.LogicalId);
        Field(value, node.Host);
        Field(value, node.Endpoint);
        value += node.Pile ? '1' : '0';
        if (node.Pile) {
            Field(value, *node.Pile);
        }
        hosts.push_back(std::move(value));
    }
    std::sort(hosts.begin(), hosts.end());
    for (const auto& host : hosts) {
        Field(result, host);
    }
    return DomainHash(result);
}

void TTopologyTracker::ProvisionTentative(TTopology topology) {
    if (!HasTentative && !IsConfirmed) {
        Configured = topology;
        Current = std::move(topology);
        HasTentative = true;
    }
}

TString TTopologyTracker::Corroborate(const std::vector<TFrozenSample>& samples, const std::vector<TConfigReply>& replies,
    const std::optional<TClusterView>& bridgeView, bool prebridgeNotFound)
{
    const auto now = TSkipperClock::Now();
    if (!bridgeView && !prebridgeNotFound) {
        return "BRIDGE_MEMBERSHIP_UNCONFIRMED";
    }
    std::set<TString> bridgeNames;
    if (bridgeView) {
        if (!ValidateClusterView(*bridgeView).empty()) {
            return "INVALID_ADMIN_VIEW";
        }
        for (const auto& [name, state] : bridgeView->Piles) {
            (void)state;
            bridgeNames.insert(name);
        }
    }
    std::map<ui32, const TConfigReply*> byNode;
    std::set<TString> bodies;
    for (const auto& reply : replies) {
        if (!reply.NodeId || reply.ReceivedAt > now || now - reply.ReceivedAt >= TDuration::Seconds(60)) {
            continue;
        }
        bool sampled = false;
        for (const auto& sample : samples) {
            sampled = sampled || std::find(sample.Nodes.begin(), sample.Nodes.end(), reply.NodeId) != sample.Nodes.end();
        }
        if (!sampled) {
            continue;
        }
        if (!byNode.emplace(reply.NodeId, &reply).second) {
            return "DUPLICATE_RESPONDER_ID";
        }
        bodies.insert(CanonicalTopology(reply.Topology));
    }
    if (bodies.size() > 1) {
        CompetingChange = true;
        return "CONFIG_CONFLICT";
    }
    for (const auto& sample : samples) {
        if (sample.Nodes.empty() || sample.Threshold != sample.Nodes.size() / 2 + 1) {
            continue;
        }
        std::map<TString, std::vector<const TConfigReply*>> groups;
        std::set<ui32> seen;
        for (const auto id : sample.Nodes) {
            const auto found = byNode.find(id);
            if (found != byNode.end() && seen.insert(id).second) {
                groups[CanonicalTopology(found->second->Topology)].push_back(found->second);
            }
        }
        for (const auto& [fingerprint, group] : groups) {
            if (group.size() < sample.Threshold) {
                continue;
            }
            if (bridgeView && Names(group.front()->Topology) != bridgeNames) {
                CompetingChange = true;
                return "MEMBERSHIP_RECONCILIATION_REQUIRED";
            }
            if (!IsConfirmed || ConfigFingerprint != fingerprint) {
                ++TopologyRevision;
            }
            ConfigFingerprint = fingerprint;
            const auto previous = std::exchange(Configured, group.front()->Topology);
            Current = Configured;
            for (auto& node : Current.Nodes) {
                const auto old = std::find_if(previous.Nodes.begin(), previous.Nodes.end(), [&](const auto& candidate) {
                    return candidate.LogicalId == node.LogicalId;
                });
                const bool readdressed = old == previous.Nodes.end() || old->Host != node.Host || old->Endpoint != node.Endpoint;
                for (auto it = KnownEndpoints.begin(); it != KnownEndpoints.end(); ++it) {
                    const auto& known = it->second;
                    if (known.LogicalId != node.LogicalId || (node.Pile && node.Pile != known.Pile)) {
                        continue;
                    }
                    if (readdressed) {
                        Readdressed.push_back(known);
                        KnownEndpoints.erase(it);
                        break;
                    }
                    const auto configuredBinding = node.Binding;
                    node = known;
                    if (configuredBinding == EBinding::Explicit) {
                        node.Binding = EBinding::Explicit;
                    }
                    break;
                }
            }
            IsConfirmed = true;
            HasTentative = true;
            CompetingChange = false;
            std::vector<TInstant> received;
            for (const auto* reply : group) {
                received.push_back(reply->ReceivedAt);
            }
            std::sort(received.begin(), received.end(), std::greater<TInstant>());
            LastConfirmed = received[sample.Threshold - 1];
            return {};
        }
    }
    return "NO_CORROBORATED_CONFIG";
}

void TTopologyTracker::RememberVerified(const TNode& node) {
    if (!node.NodeId || !node.Pile || node.Endpoint.empty() || !node.DirectEndpoint
        || (node.Binding != EBinding::Explicit && node.Binding != EBinding::ImplicitVerified))
    {
        return;
    }
    for (auto it = KnownEndpoints.begin(); it != KnownEndpoints.end();) {
        if (it->second.LogicalId == node.LogicalId) {
            it = KnownEndpoints.erase(it);
        } else {
            ++it;
        }
    }
    KnownEndpoints[node.Endpoint] = node;
    for (auto& current : Current.Nodes) {
        if (current.LogicalId == node.LogicalId && (!current.Pile || current.Pile == node.Pile)) {
            current = node;
        } else if (current.Endpoint == node.Endpoint && current.NodeId) {
            // An endpoint binds at most one node; the displaced node reverts
            // to its configured, unverified form.
            const auto revision = current.Revision;
            const auto configured = std::find_if(Configured.Nodes.begin(), Configured.Nodes.end(), [&](const auto& candidate) {
                return candidate.LogicalId == current.LogicalId;
            });
            if (configured != Configured.Nodes.end()) {
                current = *configured;
            } else {
                current.NodeId = 0;
                current.AdminFingerprint.clear();
            }
            current.Revision = revision + 1;
        }
    }
}

void TTopologyTracker::InvalidateEndpoint(const TString& endpoint) {
    KnownEndpoints.erase(endpoint);
    for (auto& node : Current.Nodes) {
        if (node.Endpoint == endpoint) {
            node.NodeId = 0;
            node.Binding = EBinding::Conflict;
            node.AdminFingerprint.clear();
            ++node.Revision;
        }
    }
}

std::vector<TNode> TTopologyTracker::TakeReaddressed() {
    return std::exchange(Readdressed, {});
}

bool TTopologyTracker::RefreshDue() const {
    return !IsConfirmed || TSkipperClock::Now() - LastConfirmed >= TDuration::Seconds(30);
}

bool TTopologyTracker::Stale() const {
    return !IsConfirmed || TSkipperClock::Now() < LastConfirmed || TSkipperClock::Now() - LastConfirmed >= TDuration::Seconds(60);
}

TInstant TTopologyTracker::ValidUntil() const {
    return IsConfirmed ? LastConfirmed + TDuration::Seconds(60) : TInstant::Zero();
}

bool TTopologyTracker::CanDispatchLifecycle() const {
    return IsConfirmed && !CompetingChange && !Stale();
}

bool TTopologyTracker::CanDispatchEmergency() const {
    return IsConfirmed && !CompetingChange;
}

bool TTopologyTracker::Confirmed() const {
    return IsConfirmed;
}

ui64 TTopologyTracker::Revision() const {
    return TopologyRevision;
}

const TTopology& TTopologyTracker::Topology() const {
    return Current;
}

std::vector<TString> TTopologyTracker::DisplayPileOrder(const std::optional<TClusterView>& bridgeView) const {
    return bridgeView && bridgeView->Converted ? bridgeView->PileOrder : Current.ConfiguredPiles;
}

} // namespace NBridgeSkipper
