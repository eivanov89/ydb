#include "scheduler.h"

#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <algorithm>

namespace NBridgeSkipper {

TString ValidateSchedulerConfig(const TSchedulerConfig& c) {
    if (!c.SampleNodesPerPile || c.SampleNodesPerPile > 101 || !c.PerPileHealthConcurrency || c.GlobalAdminConcurrency < 3) {
        return "INVALID_SAMPLING_CAPACITY";
    }
    const std::vector<TDuration> durations = {c.RoundInterval, c.RoundDeadline, c.BracketAdminTimeout, c.HealthOperationTimeout,
        c.HealthClientTimeout, c.ObservationTtl, c.AdmissionBudget, c.DispatchMargin, c.SchedulerTolerance, c.ConfirmationStartGap};
    if (std::any_of(durations.begin(), durations.end(), [](auto value) { return !value; })) {
        return "NONPOSITIVE_DURATION";
    }
    if (c.BracketAdminTimeout + c.HealthClientTimeout + c.BracketAdminTimeout >= c.RoundDeadline
        || c.RoundDeadline > c.RoundInterval || c.HealthOperationTimeout >= c.HealthClientTimeout
        || c.HealthClientTimeout >= c.ObservationTtl
        || c.ObservationTtl < c.RoundDeadline + c.AdmissionBudget + c.DispatchMargin
        || c.ConfirmationStartGap < c.RoundInterval + c.SchedulerTolerance)
    {
        return "INVALID_TIMING_BUDGET";
    }
    return {};
}

std::vector<TNode> VerifiedInventory(const std::vector<TNode>& nodes) {
    std::map<ui32, std::vector<const TNode*>> byId;
    std::map<TString, std::set<ui32>> byEndpoint;
    for (const auto& node : nodes) {
        if (!node.NodeId || !node.Pile || node.Endpoint.empty() || !node.DirectEndpoint
            || (node.Binding != EBinding::Explicit && node.Binding != EBinding::ImplicitVerified))
        {
            continue;
        }
        byId[node.NodeId].push_back(&node);
        byEndpoint[node.Endpoint].insert(node.NodeId);
    }
    std::vector<TNode> result;
    for (const auto& [id, aliases] : byId) {
        (void)id;
        const auto* chosen = aliases.front();
        bool conflict = false;
        for (const auto* alias : aliases) {
            conflict = conflict || alias->Pile != chosen->Pile || byEndpoint[alias->Endpoint].size() > 1;
            if (alias->Endpoint < chosen->Endpoint) {
                chosen = alias;
            }
        }
        if (!conflict) {
            result.push_back(*chosen);
        }
    }
    return result;
}

TString VerifyBinding(TNode& node, ui32 responderId, const std::optional<TString>& responderPile,
    const std::set<TString>& confirmedPiles, bool implicitMappingQualified)
{
    if (!responderId || !responderPile || !confirmedPiles.contains(*responderPile)) {
        return "INVALID_RESPONDER_IDENTITY";
    }
    if (node.InventoryNodeId && node.InventoryNodeId != responderId) {
        node.Binding = EBinding::Conflict;
        node.AdminFingerprint.clear();
        return "INVENTORY_NODE_ID_MISMATCH";
    }
    if (node.NodeId && node.NodeId != responderId) {
        node.Binding = EBinding::ImplicitUnresolved;
        node.AdminFingerprint.clear();
        ++node.Revision;
        return "NODE_ID_CHANGED_REDISCOVER";
    }
    if (node.Pile && *node.Pile != *responderPile) {
        node.Binding = EBinding::Conflict;
        node.AdminFingerprint.clear();
        return "PILE_BINDING_CONFLICT";
    }
    if (!node.Pile) {
        if (!implicitMappingQualified) {
            return "IMPLICIT_PILE_UNRESOLVED";
        }
        node.Pile = responderPile;
        node.Binding = EBinding::ImplicitVerified;
    }
    node.NodeId = responderId;
    return {};
}

TBoundedScheduler::TBoundedScheduler(TSchedulerConfig config)
    : Config(std::move(config)) {
}

std::optional<TRoundReservation> TBoundedScheduler::Start(const std::vector<TNode>& inventory) {
    const auto now = TSkipperClock::Now();
    if ((Current && now < Current->Deadline) || (Scheduled && now < NextStart)) {
        return std::nullopt;
    }
    Current.reset();
    TRoundReservation round;
    round.Id = NextRoundId++;
    round.StartedAt = now;
    round.Deadline = now + Config.RoundDeadline;
    NextStart = now + Config.RoundInterval;
    Scheduled = true;
    round.Blocker = ValidateSchedulerConfig(Config);
    std::map<TString, std::vector<TNode>> piles;
    for (const auto& node : VerifiedInventory(inventory)) {
        piles[*node.Pile].push_back(node);
    }
    std::map<TString, size_t> busy;
    std::set<TString> endpoints;
    for (const auto& [id, chain] : Chains) {
        (void)id;
        ++busy[chain.Pile];
        endpoints.insert(chain.Endpoint);
    }
    for (auto& [pile, nodes] : piles) {
        const size_t count = std::min(Config.SampleNodesPerPile, nodes.size());
        TFrozenSample sample;
        sample.Pile = pile;
        sample.Threshold = count / 2 + 1;
        const size_t offset = Rotation[pile] % nodes.size();
        for (size_t index = 0; index < count; ++index) {
            const auto& node = nodes[(offset + index) % nodes.size()];
            sample.Nodes.push_back(node.NodeId);
            if (Chains.contains(node.NodeId) || endpoints.contains(node.Endpoint) || Admin.contains(node.Endpoint)) {
                round.Blocker = "UNSETTLED_CHAIN_CAPACITY";
            } else if (busy[pile] < Config.PerPileHealthConcurrency) {
                round.Launch.push_back(node);
                ++busy[pile];
            } else {
                round.Blocker = "UNSETTLED_CHAIN_CAPACITY";
            }
        }
        Rotation[pile] = (offset + count) % nodes.size();
        if (count > Config.PerPileHealthConcurrency) {
            round.Blocker = "CONFIG_BUDGET";
        }
        round.Samples.push_back(std::move(sample));
    }
    if (round.Blocker.empty() || round.Blocker == "UNSETTLED_CHAIN_CAPACITY") {
        for (const auto& node : round.Launch) {
            Chains.emplace(node.NodeId, TChain{round.Id, *node.Pile, node.Endpoint});
        }
    } else {
        round.Launch.clear();
    }
    Current = round;
    return round;
}

bool TBoundedScheduler::CloseDue() {
    if (Current && TSkipperClock::Now() >= Current->Deadline) {
        Current.reset();
        return true;
    }
    return false;
}

bool TBoundedScheduler::Settle(ui64 roundId, ui32 nodeId) {
    const auto found = Chains.find(nodeId);
    if (found == Chains.end() || found->second.RoundId != roundId) {
        return false;
    }
    Chains.erase(found);
    return true;
}

bool TBoundedScheduler::AcquireAdmin(const TString& endpoint, bool operation) {
    if (Config.GlobalAdminConcurrency < 3) {
        return false;
    }
    const size_t limit = Config.GlobalAdminConcurrency - (operation ? 0 : 2);
    if (endpoint.empty() || Admin.contains(endpoint) || Admin.size() >= limit) {
        return false;
    }
    for (const auto& [id, chain] : Chains) {
        (void)id;
        if (chain.Endpoint == endpoint) {
            return false;
        }
    }
    Admin.emplace(endpoint, operation);
    return true;
}

bool TBoundedScheduler::ReleaseAdmin(const TString& endpoint) {
    return Admin.erase(endpoint) != 0;
}

size_t TBoundedScheduler::InFlightChains() const {
    return Chains.size();
}

size_t TBoundedScheduler::InFlightAdmin() const {
    return Admin.size();
}

const std::optional<TRoundReservation>& TBoundedScheduler::CurrentRound() const {
    return Current;
}

} // namespace NBridgeSkipper
