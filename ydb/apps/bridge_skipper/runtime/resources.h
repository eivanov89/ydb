#pragma once

#include "events.h"
#include "log_writer.h"

#include <ydb/apps/bridge_skipper/config/config.h>
#include <ydb/library/actors/core/actorsystem.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace NBridgeSkipper {

struct TControllerSnapshot;

struct TRuntimeLogPage {
    std::vector<TString> Lines;
    ui64 Next = 0;
    bool HasMore = false;
    bool Lost = false;
    bool Reset = false;
    ui64 Oldest = 0;
};

// All foreign producers retain this object, never an actor or activation.
class TCompletionGate {
public:
    void Attach(NActors::TActorSystem* system);
    bool Send(NActors::TActorId recipient, NActors::IEventBase* event);
    void Close();

private:
    std::mutex Mutex;
    NActors::TActorSystem* System = nullptr;
};

struct TLocalStopReceipt {
    TString RequestId;
    TString ProcessId;
    ui64 Revision = 0;
    bool Stopped = true;
    bool Durable = false;
    bool Pending = false;
    TString Error;
    TString PayloadHash;
    TString Reason;
    TString AuditTime;
    bool Releasing = false;
};

// Linearization of stop and release is independent of controller progress.
class TLocalStopLatch {
public:
    explicit TLocalStopLatch(TString processId);
    TLocalStopReceipt Stop(const TString& requestId, TString payloadHash = {}, TString reason = {}, bool* inserted = nullptr);
    TLocalStopReceipt Current() const;
    std::optional<TLocalStopReceipt> Lookup(const TString& requestId, TStringBuf payloadHash = {}) const;
    void Remember(TLocalStopReceipt receipt);
    bool IsStopped() const;
    bool Release(ui64 expectedRevision);
    void SetPersistence(ui64 revision, bool durable, TString error = {});
    void Restore(ui64 revision);

private:
    TString Process;
    std::atomic<bool> Stopped{false};
    mutable std::mutex Mutex;
    TLocalStopReceipt Receipt;
    std::map<TString, TLocalStopReceipt> Duplicates;
    std::deque<TString> DuplicateOrder;
};

class TRuntimeResources : public std::enable_shared_from_this<TRuntimeResources> {
public:
    // Startup may block on store recovery; construct on the main coordinator.
    TRuntimeResources(const TConfig& config, const TString& stateDir, const TString& memberId);
    ~TRuntimeResources();
    TRuntimeResources(const TRuntimeResources&) = delete;
    TRuntimeResources& operator=(const TRuntimeResources&) = delete;

    const TDurableState& InitialState() const;
    const TString& MemberId() const;
    const TString& ProcessId() const;
    bool SubmitStore(TStorageRequest request, NActors::TActorId replyTo);
    void PublishSnapshot(std::shared_ptr<const TControllerSnapshot> snapshot);
    std::shared_ptr<const TControllerSnapshot> ReadSnapshot() const;
    TLocalStopReceipt StopLocal(const TString& requestId, NActors::TActorId notify = {}, TString payloadHash = {}, TString reason = {});
    TLocalStopReceipt ReleaseLocal(const TString& requestId, const TString& processId, ui64 revision, NActors::TActorId notify = {}, TString payloadHash = {}, TString reason = {});
    std::optional<TLocalStopReceipt> LookupLocalReceipt(const TString& requestId) const;
    bool AcquireSubmission();
    void ReleaseSubmission();
    void CloseAdmission();
    bool AdmissionOpen() const;
    // Coordinator only: waits for blocking jobs/producers. Never an actor call.
    void StopStorage();
    size_t PendingStorage() const;
    void Log(TStringBuf event, std::initializer_list<std::pair<TStringBuf, TString>> fields = {}, TStringBuf level = "info");
    TRuntimeLogPage ReadLogs(ui64 after, size_t limit, bool reset = false) const;

    std::shared_ptr<TCompletionGate> CompletionGate;
    std::shared_ptr<TLocalStopLatch> StopLatch;
    TIntrusivePtr<NMonitoring::TDynamicCounters> Counters;
    std::atomic<bool> ControllerDraining{false};

private:
    bool Enqueue(std::function<void()> job);
    void StorageLoop();
    void NotifyStop(NActors::TActorId notify);
    void WriteStopMarker(ui64 revision);
    void RemoveStopMarker();
    void AuditLocalStop(const TLocalStopReceipt& receipt, TStringBuf action);

    TString Directory;
    TString Member;
    TString Process;
    TString Cluster;
    TDurableState Recovered;
    std::unique_ptr<TLocalStore> Store;
    mutable std::mutex StorageMutex;
    std::condition_variable StorageChanged;
    std::deque<std::function<void()>> Jobs;
    std::thread StorageThread;
    bool Stopping = false;
    size_t ActiveJobs = 0;
    std::atomic<bool> Open{true};
    std::atomic<size_t> Submissions{0};
    std::atomic<bool> CheckpointPending{false};
    std::shared_ptr<const TControllerSnapshot> Snapshot;
    mutable std::mutex LogMutex;
    std::deque<std::pair<ui64, TString>> LogLines;
    ui64 NextLogSequence = 1;
    size_t LogCapacity = 1000;
    TString LogLevel;
    std::unique_ptr<TAsyncLogWriter> LogWriter;
    std::map<TString, TInstant> RepeatedDiagnostics;
};

} // namespace NBridgeSkipper
