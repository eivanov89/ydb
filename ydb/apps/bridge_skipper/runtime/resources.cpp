#include "resources.h"

#include <util/folder/path.h>
#include <util/generic/algorithm.h>
#include <util/generic/guid.h>
#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/string/cast.h>
#include <util/system/fs.h>
#include <util/charset/utf8.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <library/cpp/json/json_writer.h>

#include <fcntl.h>
#include <unistd.h>

#include <future>
#include <limits>

namespace NBridgeSkipper {
namespace {

constexpr size_t MaxStorageJobs = 256;
constexpr size_t MaxStopReceipts = 128;

} // namespace

void TCompletionGate::Attach(NActors::TActorSystem* system) {
    std::lock_guard guard(Mutex);
    Y_ABORT_UNLESS(!System);
    System = system;
}

bool TCompletionGate::Send(NActors::TActorId recipient, NActors::IEventBase* event) {
    THolder<NActors::IEventBase> owned(event);
    std::lock_guard guard(Mutex);
    if (!System || !recipient) {
        return false;
    }
    return System->Send(new NActors::IEventHandle(recipient, {}, owned.Release()));
}

void TCompletionGate::Close() {
    // Serializes with Send; after return no producer can access the system.
    std::lock_guard guard(Mutex);
    System = nullptr;
}

TLocalStopLatch::TLocalStopLatch(TString processId)
    : Process(std::move(processId))
{
    Receipt.ProcessId = Process;
    Receipt.Stopped = false;
}

TLocalStopReceipt TLocalStopLatch::Stop(const TString& requestId, TString payloadHash, TString reason, bool* inserted) {
    std::lock_guard guard(Mutex);
    if (inserted) {
        *inserted = false;
    }
    if (const auto it = Duplicates.find(requestId); it != Duplicates.end()) {
        auto result = it->second;
        if (result.PayloadHash != payloadHash) {
            result.Error = "REQUEST_ID_REUSED";
        }
        return result;
    }
    Y_ABORT_UNLESS(Receipt.Revision != std::numeric_limits<ui64>::max());
    const auto revision = Receipt.Revision + 1;
    Receipt = {};
    Receipt.RequestId = requestId;
    Receipt.ProcessId = Process;
    Receipt.Revision = revision;
    Receipt.Pending = true;
    Receipt.PayloadHash = std::move(payloadHash);
    Receipt.Reason = std::move(reason);
    Receipt.AuditTime = TSkipperClock::Now().ToString();
    Stopped.store(true, std::memory_order_release);
    Duplicates.emplace(requestId, Receipt);
    if (inserted) {
        *inserted = true;
    }
    DuplicateOrder.push_back(requestId);
    if (DuplicateOrder.size() > MaxStopReceipts) {
        Duplicates.erase(DuplicateOrder.front());
        DuplicateOrder.pop_front();
    }
    return Receipt;
}

TLocalStopReceipt TLocalStopLatch::Current() const {
    std::lock_guard guard(Mutex);
    return Receipt;
}

std::optional<TLocalStopReceipt> TLocalStopLatch::Lookup(const TString& requestId, TStringBuf payloadHash) const {
    std::lock_guard guard(Mutex);
    const auto found = Duplicates.find(requestId);
    if (found == Duplicates.end()) {
        return std::nullopt;
    }
    auto result = found->second;
    if (!payloadHash.empty() && result.PayloadHash != payloadHash) {
        result.Error = "REQUEST_ID_REUSED";
    }
    return result;
}

void TLocalStopLatch::Remember(TLocalStopReceipt receipt) {
    std::lock_guard guard(Mutex);
    if (!Duplicates.contains(receipt.RequestId)) {
        DuplicateOrder.push_back(receipt.RequestId);
    }
    Duplicates[receipt.RequestId] = std::move(receipt);
    while (DuplicateOrder.size() > MaxStopReceipts) {
        Duplicates.erase(DuplicateOrder.front());
        DuplicateOrder.pop_front();
    }
}

bool TLocalStopLatch::IsStopped() const {
    return Stopped.load(std::memory_order_acquire);
}

bool TLocalStopLatch::Release(ui64 expectedRevision) {
    std::lock_guard guard(Mutex);
    if (Receipt.Revision != expectedRevision || !Receipt.Stopped) {
        return false;
    }
    Receipt.Stopped = false;
    Receipt.Pending = false;
    Receipt.Durable = true;
    Receipt.Error.clear();
    Stopped.store(false, std::memory_order_release);
    return true;
}

void TLocalStopLatch::SetPersistence(ui64 revision, bool durable, TString error) {
    std::lock_guard guard(Mutex);
    for (auto& [id, receipt] : Duplicates) {
        Y_UNUSED(id);
        if (receipt.Revision == revision && !receipt.Releasing) {
            receipt.Durable = durable;
            receipt.Pending = false;
            receipt.Error = error;
        }
    }
    if (Receipt.Revision == revision) {
        Receipt.Durable = durable;
        Receipt.Pending = false;
        Receipt.Error = std::move(error);
    }
}

void TLocalStopLatch::Restore(ui64 revision) {
    std::lock_guard guard(Mutex);
    Receipt = {};
    Receipt.ProcessId = Process;
    Receipt.Revision = revision;
    Receipt.Durable = true;
    Stopped.store(true, std::memory_order_release);
}

TRuntimeResources::TRuntimeResources(const TConfig& config, const TString& stateDir, const TString& memberId)
    : CompletionGate(std::make_shared<TCompletionGate>())
    , Directory(stateDir)
    , Member(memberId)
    , Process(TGUID::Create().AsUuidString())
    , Cluster(config.ClusterId)
    , LogCapacity(config.Logging.DisplayRingLines)
    , LogLevel(config.Logging.Level)
{
    StopLatch = std::make_shared<TLocalStopLatch>(Process);
    Counters = MakeIntrusive<NMonitoring::TDynamicCounters>();
    for (const TStringBuf name : {"storage_errors_total", "storage_jobs_total", "durable_checkpoints_skipped_total", "skipper_log_suppressed_total", "skipper_log_sink_dropped_total"}) {
        Counters->GetCounter(TString(name), true)->Set(0);
    }
    try {
        LogWriter = std::make_unique<TAsyncLogWriter>([counter = Counters->GetCounter("skipper_log_sink_dropped_total", true)] {
            counter->Inc();
        });
        StorageThread = std::thread([this] { StorageLoop(); });
        auto ready = std::make_shared<std::promise<void>>();
        const TStoreBinding binding{config.ClusterId, Member, "local", TGUID::Create().AsUuidString(), Process};
        Enqueue([this, binding, ready] {
            try {
                Store = std::make_unique<TLocalStore>(Directory, binding);
                Recovered = Store->Load();
                const TFsPath marker = TFsPath(Directory) / "local-stop";
                if (marker.Exists()) {
                    TFileInput input(marker.GetPath());
                    char data[32];
                    const auto size = input.Load(data, sizeof(data));
                    ui64 revision = 0;
                    if (size == sizeof(data) || !TryFromString(TStringBuf(data, size), revision) || revision == 0 || revision == std::numeric_limits<ui64>::max()) {
                        ythrow yexception() << "invalid local stop marker";
                    }
                    StopLatch->Restore(revision);
                }
                ready->set_value();
            } catch (...) {
                ready->set_exception(std::current_exception());
            }
        });
        ready->get_future().get();
        Log("startup_recovered", {{"member_id", Member}, {"applied_index", ToString(Recovered.AppliedIndex)}});
    } catch (...) {
        StopStorage();
        throw;
    }
}

TRuntimeResources::~TRuntimeResources() {
    // Joining from an actor destructor would deadlock/starve actor workers.
    // The process coordinator must have called StopStorage explicitly.
    Y_ABORT_UNLESS(!StorageThread.joinable());
}

const TDurableState& TRuntimeResources::InitialState() const {
    return Recovered;
}

const TString& TRuntimeResources::MemberId() const {
    return Member;
}

const TString& TRuntimeResources::ProcessId() const {
    return Process;
}

bool TRuntimeResources::Enqueue(std::function<void()> job) {
    std::lock_guard guard(StorageMutex);
    if (Stopping || Jobs.size() + ActiveJobs >= MaxStorageJobs) {
        return false;
    }
    Jobs.push_back(std::move(job));
    StorageChanged.notify_one();
    return true;
}

void TRuntimeResources::StorageLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock guard(StorageMutex);
            StorageChanged.wait(guard, [this] { return Stopping || !Jobs.empty(); });
            if (Jobs.empty() && Stopping) {
                break;
            }
            job = std::move(Jobs.front());
            Jobs.pop_front();
            ++ActiveJobs;
        }
        job();
        {
            std::lock_guard guard(StorageMutex);
            --ActiveJobs;
        }
    }
    Store.reset(); // SQLite close remains on its executor.
}

bool TRuntimeResources::SubmitStore(TStorageRequest request, NActors::TActorId replyTo) {
    const bool checkpoint = request.Kind == EStorageKind::Checkpoint;
    if (checkpoint && CheckpointPending.exchange(true)) {
        return false;
    }
    const bool queued = Enqueue([this, request = std::move(request), replyTo] {
        TStorageResult result;
        result.Id = request.Id;
        result.Kind = request.Kind;
        const auto started = TSkipperClock::Now();
        try {
            switch (request.Kind) {
                case EStorageKind::Commit:
                    if (!request.State) {
                        ythrow yexception() << "commit requires immutable installed state";
                    }
                    result.Proposal = Store->Propose(*request.State, request.Command);
                    result.Success = result.Proposal.Status == EProposalStatus::Committed;
                    break;
                case EStorageKind::ResolveUnknown:
                    result.Proposal = Store->ResolveUnknown(request.LookupId);
                    result.Success = result.Proposal.Status == EProposalStatus::Committed;
                    break;
                case EStorageKind::LookupCommand:
                    result.Command = Store->LookupCommand(request.LookupId);
                    result.Success = true;
                    break;
                case EStorageKind::LookupRequest:
                    result.Receipt = Store->LookupRequest(request.LookupId);
                    result.Success = true;
                    break;
                case EStorageKind::Checkpoint:
                    if (!request.State) {
                        ythrow yexception() << "checkpoint requires immutable installed state";
                    }
                    if (!Store->Checkpoint(*request.State)) {
                        Counters->GetCounter("durable_checkpoints_skipped_total", true)->Inc();
                        Log("checkpoint_skipped", {{"request_id", request.Id}, {"reason", "DURABLE_RECORD_TOO_LARGE"}});
                    }
                    result.Success = true;
                    break;
                case EStorageKind::Audit:
                    result.Audit = Store->ReadAudit(request.After, Min<size_t>(request.Limit, 1000) + 1);
                    result.HasMore = result.Audit.size() > Min<size_t>(request.Limit, 1000);
                    if (result.HasMore) {
                        result.Audit.pop_back();
                    }
                    result.Success = true;
                    break;
                case EStorageKind::LocalStopAudit: {
                    const TFsPath path = TFsPath(Directory) / "local-stop.audit";
                    if (path.Exists()) {
                        TFile audit(path.GetPath(), OpenExisting | RdOnly);
                        constexpr size_t maxBytes = 128 * 1024;
                        const ui64 length = audit.GetLength();
                        const ui64 offset = length > maxBytes ? length - maxBytes : 0;
                        audit.Seek(offset, sSet);
                        TString tail = TString::Uninitialized(Min<ui64>(length, maxBytes));
                        const auto size = audit.Read(tail.Detach(), tail.size());
                        tail.resize(size);
                        result.LocalStopAuditTruncated = offset != 0;
                        size_t start = 0;
                        if (offset) {
                            const auto newline = tail.find('\n');
                            start = newline == TString::npos ? tail.size() : newline + 1;
                        }
                        while (start < tail.size()) {
                            const auto end = tail.find('\n', start);
                            if (end == TString::npos) {
                                result.LocalStopAuditTruncated = true;
                                break;
                            }
                            if (end - start <= 8192) {
                                result.LocalStopAudit.emplace_back(tail.substr(start, end - start));
                            } else {
                                result.LocalStopAuditTruncated = true;
                            }
                            if (result.LocalStopAudit.size() > 100) {
                                result.LocalStopAudit.erase(result.LocalStopAudit.begin());
                                result.LocalStopAuditTruncated = true;
                            }
                            start = end + 1;
                        }
                    }
                    result.Success = true;
                    break;
                }
            }
            result.Stats = Store->Stats();
        } catch (...) {
            result.Error = "PERSISTENCE_ERROR";
            result.Proposal.Error = result.Error;
            result.Proposal.Status = EProposalStatus::Unknown;
            Counters->GetCounter("storage_errors_total", true)->Inc();
        }
        const auto finished = TSkipperClock::Now();
        Counters->GetCounter("storage_jobs_total", true)->Inc();
        Counters->GetCounter("storage_microseconds_total", true)->Add(finished >= started ? (finished - started).MicroSeconds() : 0);
        Counters->GetCounter("durable_log_rows")->Set(result.Stats.LogRows);
        Counters->GetCounter("durable_log_bytes")->Set(result.Stats.LogBytes);
        Counters->GetCounter("durable_snapshot_bytes")->Set(result.Stats.SnapshotBytes);
        Log("storage_completion", {{"request_id", request.Id}, {"command_id", request.Command.Id},
            {"status", result.Success ? TString("success") : result.Proposal.Status == EProposalStatus::Unknown ? TString("unknown") : TString("rejected")}});
        if (request.Kind == EStorageKind::Checkpoint) {
            CheckpointPending.store(false);
        }
        CompletionGate->Send(replyTo, new TEvRuntime::TEvStorageResult(std::move(result)));
    });
    if (!queued && checkpoint) {
        CheckpointPending.store(false);
    }
    return queued;
}

void TRuntimeResources::PublishSnapshot(std::shared_ptr<const TControllerSnapshot> snapshot) {
    std::atomic_store_explicit(&Snapshot, std::move(snapshot), std::memory_order_release);
}

std::shared_ptr<const TControllerSnapshot> TRuntimeResources::ReadSnapshot() const {
    return std::atomic_load_explicit(&Snapshot, std::memory_order_acquire);
}

void TRuntimeResources::NotifyStop(NActors::TActorId notify) {
    auto event = MakeHolder<TEvRuntime::TEvLocalStopChanged>();
    event->Revision = StopLatch->Current().Revision;
    CompletionGate->Send(notify, event.Release());
}

void TRuntimeResources::WriteStopMarker(ui64 revision) {
    const TString marker = (TFsPath(Directory) / "local-stop").GetPath();
    const TString temporary = marker + ".tmp";
    TFile file(temporary, CreateAlways | WrOnly | AWUser | ARUser);
    const TString data = ToString(revision);
    file.Write(data.data(), data.size());
    file.Flush();
    file.Close();
    if (!NFs::Rename(temporary, marker)) {
        ythrow yexception() << "cannot publish stop marker";
    }
    SyncDirectory(Directory);
}

void TRuntimeResources::RemoveStopMarker() {
    const TFsPath marker = TFsPath(Directory) / "local-stop";
    if (marker.Exists() && !NFs::Remove(marker.GetPath())) {
        ythrow yexception() << "cannot remove stop marker";
    }
    SyncDirectory(Directory);
}

void TRuntimeResources::AuditLocalStop(const TLocalStopReceipt& receipt, TStringBuf action) {
    NJson::TJsonValue record(NJson::JSON_MAP);
    record["action"] = TString(action);
    record["caller_id"] = "local";
    record["process_id"] = receipt.ProcessId;
    record["revision"] = ToString(receipt.Revision);
    record["request_id"] = receipt.RequestId;
    record["payload_hash"] = receipt.PayloadHash;
    record["reason"] = receipt.Reason;
    record["audit_time"] = receipt.AuditTime;
    const auto line = NJson::WriteJson(record, false) + "\n";
    const TFsPath path = TFsPath(Directory) / "local-stop.audit";
    const bool existed = path.Exists();
    TFile audit(path.GetPath(), OpenAlways | WrOnly | ForAppend | AWUser | ARUser);
    audit.Write(line.data(), line.size());
    audit.Flush();
    if (!existed) {
        SyncDirectory(Directory);
    }
}

TLocalStopReceipt TRuntimeResources::StopLocal(const TString& requestId, NActors::TActorId notify, TString payloadHash, TString reason) {
    bool inserted = false;
    auto result = StopLatch->Stop(requestId, std::move(payloadHash), std::move(reason), &inserted);
    if (!result.Error.empty()) {
        return result;
    }
    NotifyStop(notify);
    if (inserted) {
        Log("local_stop", {{"request_id", requestId}, {"stop_revision", ToString(result.Revision)}});
    }
    if (!inserted || !result.Pending) {
        return result;
    }
    if (!Enqueue([this, receipt = result, notify] {
            try {
                WriteStopMarker(receipt.Revision);
                AuditLocalStop(receipt, "stop");
                StopLatch->SetPersistence(receipt.Revision, true);
            } catch (...) {
                StopLatch->SetPersistence(receipt.Revision, false, "STOP_MARKER_VOLATILE");
            }
            NotifyStop(notify);
        })) {
        StopLatch->SetPersistence(result.Revision, false, "STOP_MARKER_VOLATILE");
    }
    return StopLatch->Lookup(requestId).value_or(result);
}

TLocalStopReceipt TRuntimeResources::ReleaseLocal(const TString& requestId, const TString& processId, ui64 revision, NActors::TActorId notify, TString payloadHash, TString reason) {
    if (const auto duplicate = StopLatch->Lookup(requestId, payloadHash)) {
        return *duplicate;
    }
    // A receipt records only this request; latch persistence errors stay in the latch.
    const auto current = StopLatch->Current();
    TLocalStopReceipt result;
    result.RequestId = requestId;
    result.ProcessId = current.ProcessId;
    result.Revision = current.Revision;
    result.Stopped = current.Stopped;
    result.PayloadHash = std::move(payloadHash);
    result.Reason = std::move(reason);
    result.AuditTime = TSkipperClock::Now().ToString();
    result.Releasing = true;
    if (processId != Process || revision != result.Revision || !result.Stopped) {
        result.Error = "STALE_LOCAL_STOP";
        return result;
    }
    result.Pending = true;
    StopLatch->Remember(result);
    if (!Enqueue([this, receipt = result, revision, notify]() mutable {
            if (StopLatch->Current().Revision != revision) {
                receipt.Pending = false;
                receipt.Error = "STALE_LOCAL_STOP";
                StopLatch->Remember(receipt);
                return;
            }
            try {
                AuditLocalStop(receipt, "release");
                RemoveStopMarker();
                if (StopLatch->Release(revision)) {
                    receipt.Stopped = false;
                    receipt.Durable = true;
                } else {
                    receipt.Error = "STALE_LOCAL_STOP";
                }
            } catch (...) {
                // A remove/fsync failure must never reopen the live gate.
                StopLatch->SetPersistence(revision, false, "STOP_RELEASE_FAILED");
                receipt.Error = "STOP_RELEASE_FAILED";
            }
            receipt.Pending = false;
            StopLatch->Remember(receipt);
            NotifyStop(notify);
        })) {
        result.Pending = false;
        result.Error = "BUSY";
        StopLatch->Remember(result);
    }
    return result;
}

std::optional<TLocalStopReceipt> TRuntimeResources::LookupLocalReceipt(const TString& requestId) const {
    return StopLatch->Lookup(requestId);
}

bool TRuntimeResources::AcquireSubmission() {
    if (!AdmissionOpen()) {
        return false;
    }
    auto count = Submissions.load(std::memory_order_relaxed);
    while (count < 256) {
        if (Submissions.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
            if (!AdmissionOpen()) {
                ReleaseSubmission();
                return false;
            }
            return true;
        }
    }
    return false;
}

void TRuntimeResources::ReleaseSubmission() {
    const auto previous = Submissions.fetch_sub(1, std::memory_order_acq_rel);
    Y_ABORT_UNLESS(previous > 0);
}

void TRuntimeResources::CloseAdmission() {
    Open.store(false, std::memory_order_release);
}

bool TRuntimeResources::AdmissionOpen() const {
    return Open.load(std::memory_order_acquire);
}

void TRuntimeResources::StopStorage() {
    CloseAdmission();
    {
        std::lock_guard guard(StorageMutex);
        Stopping = true;
    }
    StorageChanged.notify_all();
    if (StorageThread.joinable()) {
        StorageThread.join();
    }
    if (LogWriter) {
        LogWriter->Stop();
    }
}

size_t TRuntimeResources::PendingStorage() const {
    std::lock_guard guard(StorageMutex);
    return Jobs.size() + ActiveJobs;
}

void TRuntimeResources::Log(TStringBuf event, std::initializer_list<std::pair<TStringBuf, TString>> fields, TStringBuf level) {
    const auto rank = [](TStringBuf value) {
        if (value == "trace") {
            return 0;
        }
        if (value == "debug") {
            return 1;
        }
        if (value == "info") {
            return 2;
        }
        if (value == "warn") {
            return 3;
        }
        if (value == "error") {
            return 4;
        }
        return 5;
    };
    if (rank(level) < rank(LogLevel)) {
        return;
    }
    NJson::TJsonValue record(NJson::JSON_MAP);
    record["event"] = TString(event.substr(0, 128));
    record["timestamp"] = TSkipperClock::Now().ToString();
    record["process_id"] = Process;
    record["cluster_id"] = TString(TStringBuf(Cluster).substr(0, 512));
    record["level"] = TString(level);
    const auto safe = [](TStringBuf input) {
        TString result(input.substr(0, 512));
        while (!result.empty() && !IsUtf(result)) {
            result.pop_back();
        }
        TString lower(result);
        char* data = lower.Detach();
        for (size_t index = 0; index < lower.size(); ++index) {
            if (data[index] >= 'A' && data[index] <= 'Z') {
                data[index] += 'a' - 'A';
            }
        }
        for (const TStringBuf marker : {"bearer ", "authorization", "password", "secret", "token", "private key", "-----begin", "/users/", "/home/", ".pem", ".key"}) {
            if (lower.Contains(marker)) {
                return TString("[redacted]");
            }
        }
        return result;
    };
    record["cluster_id"] = safe(Cluster);
    size_t count = 0;
    for (const auto& [key, value] : fields) {
        if (++count > 16) {
            break;
        }
        bool allowed = false;
        for (const TStringBuf known : {"request_id", "command_id", "operation_id", "attempt_id", "member_id", "method", "status",
                "endpoint", "applied_index", "stop_revision", "kind", "controller_revision", "duration_microseconds", "response_bytes",
                "profile", "access", "plaintext_token_opt_in", "build"}) {
            allowed = allowed || key == known;
        }
        if (!allowed) {
            continue;
        }
        record[TString(key.substr(0, 64))] = safe(value);
    }
    TString line;
    try {
        line = NJson::WriteJson(record, false, true, true);
    } catch (...) {
        line = "{\"event\":\"invalid_diagnostic_fields\"}";
    }
    if (line.size() > 8192) {
        line = "{\"event\":\"diagnostic_record_limit\"}";
    }
    const bool transition = event != "read_rpc_result" && level != "debug" && level != "trace";
    {
        std::lock_guard guard(LogMutex);
        if (!transition) {
            TString key(event);
            for (const TStringBuf field : {"method", "status", "endpoint"}) {
                key += '\n';
                if (record.Has(field)) {
                    key += record[field].GetString();
                }
            }
            const auto now = TSkipperClock::Now();
            const auto previous = RepeatedDiagnostics.find(key);
            if (previous != RepeatedDiagnostics.end() && now >= previous->second && now - previous->second < TDuration::Seconds(5)) {
                Counters->GetCounter("skipper_log_suppressed_total", true)->Inc();
                return;
            }
            if (RepeatedDiagnostics.size() == 256 && previous == RepeatedDiagnostics.end()) {
                RepeatedDiagnostics.erase(RepeatedDiagnostics.begin());
            }
            RepeatedDiagnostics[key] = now;
        }
        line = TString("{\"sequence\":\"") + ToString(NextLogSequence) + "\"," + line.substr(1);
        LogLines.emplace_back(NextLogSequence++, line);
        while (LogLines.size() > LogCapacity) {
            LogLines.pop_front();
        }
    }
    if (!LogWriter->Write(std::move(line), transition)) {
        Counters->GetCounter("skipper_log_sink_dropped_total", true)->Inc();
    }
}

TRuntimeLogPage TRuntimeResources::ReadLogs(ui64 after, size_t limit, bool reset) const {
    TRuntimeLogPage page;
    page.Next = after;
    limit = Min<size_t>(limit, 200);
    std::lock_guard guard(LogMutex);
    page.Oldest = LogLines.empty() ? NextLogSequence : LogLines.front().first;
    page.Reset = reset || after >= NextLogSequence;
    if (page.Reset) {
        after = page.Oldest - 1;
        page.Next = after;
    }
    page.Lost = !page.Reset && after < page.Oldest - 1;
    for (const auto& [sequence, json] : LogLines) {
        if (sequence > after) {
            if (page.Lines.size() == limit) {
                page.HasMore = true;
                break;
            }
            // Keep record strings owned; HTTP parses bounded UTF-8 JSON outside
            // this short lock and adds its API envelope/cursor projection.
            page.Lines.push_back(json);
            page.Next = sequence;
        }
    }
    return page;
}

} // namespace NBridgeSkipper
