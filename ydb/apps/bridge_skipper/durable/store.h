#pragma once

#include "durable.h"

#include <memory>

namespace NBridgeSkipper {

struct TStoreBinding {
    TString ClusterId;
    TString MemberId;
    TString Backend = "local";
    // Inject newly generated UUIDs at startup. Existing membership is retained.
    TString InitialMembershipIncarnation;
    TString ProcessIncarnation;
};

enum class EProposalStatus { Committed, Rejected, Unknown };
enum class EStoreFault { None, BeforeCommit, AfterCommit, BeforeCheckpointCommit, OversizedCheckpoint };

void SyncDirectory(const TString& directory);
// Resolves symlinks and ".." physically; an entry lives in its real parent.
TString PhysicalParentDirectory(const TString& directory);

struct TProposalResult {
    EProposalStatus Status = EProposalStatus::Rejected;
    TCommandResult Result;
    std::optional<TDurableState> State;
    TString Error;
};

struct TStoreStats {
    ui64 LogRows = 0;
    ui64 LogBytes = 0;
    ui64 SnapshotBytes = 0;
    ui64 ReplayCommands = 0;
};

struct TDurableAuditEntry {
    ui64 Index = 0;
    TDurableCommand Command;
    TCommandResult Result;
};

// Synchronous blocking primitives. Use exclusively on the storage executor;
// never call from control/HTTP actor workers. No callbacks or external effects.
class TLocalStore {
public:
    TLocalStore(const TString& directory, const TStoreBinding& binding);
    ~TLocalStore();
    TLocalStore(const TLocalStore&) = delete;
    TLocalStore& operator=(const TLocalStore&) = delete;

    TDurableState Load();
    TProposalResult Propose(const TDurableState& installed, const TDurableCommand& command);
    std::optional<TCommandResult> LookupCommand(TStringBuf id) const;
    std::optional<TStoredReceipt> LookupRequest(TStringBuf id) const;
    std::vector<TDurableAuditEntry> ReadAudit(ui64 after, size_t limit) const;
    // Resolves unknown completion by durable lookup and replay before writes.
    TProposalResult ResolveUnknown(TStringBuf commandId);
    // Returns false and keeps the previous snapshot when the state exceeds
    // MaxDurableRecordBytes; the complete log still recovers it.
    bool Checkpoint(const TDurableState& installed);
    TStoreStats Stats() const;
    const TString& ProcessIncarnation() const;
    bool Failed() const;
    void InjectFault(EStoreFault fault);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};

} // namespace NBridgeSkipper
