#include "types.h"

#include <util/string/builder.h>
#include <util/string/hex.h>

#include <openssl/sha.h>

#include <algorithm>

namespace NBridgeSkipper {
namespace {

void Append(TString& result, const TString& value) {
    result += TStringBuilder() << value.size() << ':' << value;
}

TString Named(const TNamedPileStates& piles) {
    TString result;
    for (const auto& [name, state] : piles) {
        Append(result, name);
        result += TStringBuilder() << static_cast<ui32>(state) << ';';
    }
    return result;
}

EPileState Stored(EPileState state) {
    return IsSynchronized(state) ? EPileState::Synchronized : state;
}

} // namespace

bool IsKnownState(EPileState state) {
    return state >= EPileState::Primary && state <= EPileState::Inactive;
}

bool IsSynchronized(EPileState state) {
    return state == EPileState::Primary || state == EPileState::Promoted || state == EPileState::Synchronized;
}

bool IsActive(EPileState state) {
    return state != EPileState::Disconnected && state != EPileState::Inactive;
}

TString CanonicalNamedState(const TClusterView& view) {
    return Named(view.Piles);
}

TString DomainHash(const TString& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    return HexEncode(digest, sizeof(digest));
}

TString CanonicalFingerprint(const TClusterView& view) {
    TString result = TStringBuilder() << "bridge-v1;" << view.Generation << ';' << view.Converted << ';' << view.ConversionInProgress << ';';
    Append(result, Named(view.Piles));
    std::vector<TString> pending;
    for (const auto& item : view.Pending) {
        TString value;
        Append(value, item.Kind);
        value += item.Pile ? '1' : '0';
        if (item.Pile) {
            Append(value, *item.Pile);
        }
        Append(value, item.Error);
        Append(value, item.Progress);
        pending.push_back(std::move(value));
    }
    std::sort(pending.begin(), pending.end());
    result += TStringBuilder() << pending.size() << ';';
    for (const auto& item : pending) {
        Append(result, item);
    }
    for (const auto& [key, value] : view.Metadata) {
        Append(result, key);
        Append(result, value);
    }
    return DomainHash(result);
}

TString CanonicalDecision(const TDecision& decision) {
    TString result;
    result += TStringBuilder() << static_cast<ui32>(decision.Kind) << ';';
    for (const auto* name : {&decision.Candidate, &decision.Primary}) {
        result += *name ? '1' : '0';
        if (*name) {
            Append(result, **name);
        }
    }
    Append(result, Named(decision.Update));
    Append(result, decision.Profile);
    result += TStringBuilder() << static_cast<ui32>(decision.Source) << ';' << decision.Epoch.TopologyRevision << ';' << decision.Epoch.ReadinessGeneration << ';';
    Append(result, decision.Epoch.AdminFingerprint);
    Append(result, decision.Epoch.PolicyRevision);
    return result;
}

TString ValidateClusterView(const TClusterView& view) {
    if (!view.Metadata.empty()) {
        return "UNSUPPORTED_ADMIN_METADATA";
    }
    if (view.Pending.size() > 1) {
        return "CONTRADICTORY_PENDING_METADATA";
    }
    for (const auto& pending : view.Pending) {
        if ((pending.Kind != "ADD" && pending.Kind != "REMOVE") || !pending.Pile || !view.Piles.contains(*pending.Pile)) {
            return "UNSUPPORTED_PENDING_METADATA";
        }
    }
    size_t primaries = 0;
    size_t promoted = 0;
    for (const auto& [name, state] : view.Piles) {
        (void)name;
        if (!IsKnownState(state)) {
            return "UNSUPPORTED_PUBLIC_STATE";
        }
        primaries += state == EPileState::Primary;
        promoted += state == EPileState::Promoted;
    }
    if (!view.Converted && view.Piles.empty()) {
        return {};
    }
    if (primaries != 1 || promoted > 1) {
        return "INVALID_PUBLIC_ROLES";
    }
    if (!view.PileOrder.empty()) {
        const std::set<TString> ordered(view.PileOrder.begin(), view.PileOrder.end());
        if (ordered.size() != view.PileOrder.size() || ordered.size() != view.Piles.size()) {
            return "INVALID_PUBLIC_REGISTRY";
        }
        for (const auto& name : ordered) {
            if (!view.Piles.contains(name)) {
                return "INVALID_PUBLIC_REGISTRY";
            }
        }
    }
    return {};
}

TString ValidateExactUpdate(const TClusterView& view, const TExactUpdate& update) {
    if (const auto error = ValidateClusterView(view); !error.empty()) {
        return error;
    }
    if (update.empty()) {
        return "EMPTY_UPDATE";
    }
    if (view.ConversionInProgress) {
        return "CONVERSION_IN_PROGRESS";
    }
    auto result = view;
    size_t storedChanges = 0;
    bool allDisconnected = true;
    bool changed = false;
    for (const auto& [name, state] : update) {
        const auto old = view.Piles.find(name);
        if (old == view.Piles.end()) {
            return "UNKNOWN_PILE";
        }
        if (!IsKnownState(state) || state == EPileState::Inactive) {
            return "UNSUPPORTED_UPDATE_STATE";
        }
        if (old->second == EPileState::Suspended || old->second == EPileState::Inactive) {
            return "OPERATOR_ACTION_REQUIRED";
        }
        for (const auto& pending : view.Pending) {
            if (pending.Pile && *pending.Pile == name) {
                return "PENDING_TARGET";
            }
        }
        if (IsSynchronized(state) && !IsSynchronized(old->second)) {
            return "UNSYNCHRONIZED_SUCCESSOR";
        }
        if (Stored(state) != Stored(old->second)) {
            ++storedChanges;
            allDisconnected = allDisconnected && state == EPileState::Disconnected;
        }
        changed = changed || state != old->second;
        result.Piles[name] = state;
    }
    if (storedChanges > 1 && !allDisconnected) {
        return "MULTIPLE_STORED_CHANGES";
    }
    if (!changed) {
        return "ALREADY_SATISFIED";
    }
    return ValidateClusterView(result);
}

} // namespace NBridgeSkipper
