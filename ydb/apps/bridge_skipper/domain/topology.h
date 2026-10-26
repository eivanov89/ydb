#pragma once

#include "types.h"

namespace NBridgeSkipper {

struct TTopology {
    std::vector<TString> ConfiguredPiles;
    std::vector<TNode> Nodes;
};

// Only the supported inventory paths are interpreted. Unrelated YDB settings
// and metadata versions are deliberately outside this parser's schema.
TTopology ParseMainConfig(const TString& yaml, const TString& scheme);
TString CanonicalTopology(const TTopology& topology);

struct TConfigReply {
    ui32 NodeId = 0;
    TTopology Topology;
    TInstant ReceivedAt;
};

class TTopologyTracker {
public:
    void ProvisionTentative(TTopology topology);
    TString Corroborate(const std::vector<TFrozenSample>& samples, const std::vector<TConfigReply>& replies,
        const std::optional<TClusterView>& bridgeView, bool prebridgeNotFound);
    void RememberVerified(const TNode& node);
    void InvalidateEndpoint(const TString& endpoint);
    // Verified nodes dropped because their configured address changed.
    std::vector<TNode> TakeReaddressed();
    bool RefreshDue() const;
    bool Stale() const;
    TInstant ValidUntil() const;
    bool CanDispatchLifecycle() const;
    bool CanDispatchEmergency() const;
    bool Confirmed() const;
    ui64 Revision() const;
    const TTopology& Topology() const;
    std::vector<TString> DisplayPileOrder(const std::optional<TClusterView>& bridgeView) const;

private:
    TTopology Configured;
    TTopology Current;
    bool HasTentative = false;
    bool IsConfirmed = false;
    bool CompetingChange = false;
    ui64 TopologyRevision = 0;
    TInstant LastConfirmed;
    TString ConfigFingerprint;
    std::map<TString, TNode> KnownEndpoints;
    std::vector<TNode> Readdressed;
};

} // namespace NBridgeSkipper
