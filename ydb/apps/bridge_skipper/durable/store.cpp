#include "store.h"

#include <contrib/libs/sqlite3/sqlite3.h>

#include <util/generic/yexception.h>
#include <util/string/cast.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace NBridgeSkipper {
namespace {

void Check(sqlite3* db, int result) {
    Y_ENSURE(result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW, "SQLite: " << sqlite3_errmsg(db));
}

class TStatement {
public:
    TStatement(sqlite3* db, const char* sql)
        : Db(db)
    {
        Check(Db, sqlite3_prepare_v2(Db, sql, -1, &Statement, nullptr));
    }

    ~TStatement() {
        sqlite3_finalize(Statement);
    }

    void Text(int position, TStringBuf value) {
        Y_ENSURE(value.size() <= static_cast<size_t>(std::numeric_limits<int>::max()), "SQLite text too large");
        Check(Db, sqlite3_bind_text(Statement, position, value.data(), value.size(), SQLITE_TRANSIENT));
    }

    void Null(int position) {
        Check(Db, sqlite3_bind_null(Statement, position));
    }

    void Integer(int position, ui64 value) {
        Y_ENSURE(value <= static_cast<ui64>(std::numeric_limits<i64>::max()), "SQLite index exhausted");
        Check(Db, sqlite3_bind_int64(Statement, position, value));
    }

    bool Step() {
        const int result = sqlite3_step(Statement);
        Check(Db, result);
        return result == SQLITE_ROW;
    }

    TString Text(int column) const {
        const auto* value = sqlite3_column_text(Statement, column);
        return value ? TString(reinterpret_cast<const char*>(value), sqlite3_column_bytes(Statement, column)) : TString();
    }

    ui64 Integer(int column) const {
        const auto value = sqlite3_column_int64(Statement, column);
        Y_ENSURE(value >= 0, "negative durable index");
        return value;
    }

private:
    sqlite3* Db = nullptr;
    sqlite3_stmt* Statement = nullptr;
};

void Execute(sqlite3* db, const char* sql) {
    Check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class TTransaction {
public:
    explicit TTransaction(sqlite3* db)
        : Db(db)
    {
        Execute(Db, "BEGIN IMMEDIATE");
    }

    ~TTransaction() {
        if (!Committed) {
            sqlite3_exec(Db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void Commit() {
        Execute(Db, "COMMIT");
        Committed = true;
    }

private:
    sqlite3* Db;
    bool Committed = false;
};

std::optional<TString> GetMeta(sqlite3* db, TStringBuf key) {
    TStatement query(db, "SELECT value FROM meta WHERE key=?1");
    query.Text(1, key);
    if (query.Step()) {
        return query.Text(0);
    }
    return std::nullopt;
}

void SetMeta(sqlite3* db, TStringBuf key, TStringBuf value) {
    TStatement statement(db, "INSERT INTO meta(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    statement.Text(1, key);
    statement.Text(2, value);
    statement.Step();
}

} // namespace

void SyncDirectory(const TString& directory) {
    const int descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Y_ENSURE(descriptor >= 0, "cannot open directory for sync: " << strerror(errno));
    const int result = fsync(descriptor);
    const int error = errno;
    close(descriptor);
    Y_ENSURE(result == 0, "cannot sync directory: " << strerror(error));
}

TString PhysicalParentDirectory(const TString& directory) {
    return TString(std::filesystem::canonical(directory.c_str()).parent_path().c_str());
}

struct TLocalStore::TImpl {
    sqlite3* Db = nullptr;
    int Lock = -1;
    TStoreBinding Binding;
    TDurableState Initial;
    bool Failed = false;
    bool Unknown = false;
    EStoreFault Fault = EStoreFault::None;
    ui64 ReplayCount = 0;
    TString InstalledDigest;

    ~TImpl() {
        if (Db) {
            sqlite3_close_v2(Db);
        }
        if (Lock >= 0) {
            flock(Lock, LOCK_UN);
            close(Lock);
        }
    }

    ui64 LastIndex() const {
        TStatement query(Db, "SELECT coalesce(max(idx),0) FROM log");
        Y_ENSURE(query.Step(), "missing log aggregate");
        return query.Integer(0);
    }
};

TLocalStore::TLocalStore(const TString& directory, const TStoreBinding& binding)
    : Impl(std::make_unique<TImpl>())
{
    Y_ENSURE(!binding.ClusterId.empty() && !binding.MemberId.empty() && binding.Backend == "local" && !binding.InitialMembershipIncarnation.empty() && !binding.ProcessIncarnation.empty() && binding.ProcessIncarnation != binding.InitialMembershipIncarnation, "invalid store binding/incarnation");
    Impl->Binding = binding;
    const std::filesystem::path path(directory.c_str());
    if (!std::filesystem::exists(path)) {
        Y_ENSURE(mkdir(directory.c_str(), 0700) == 0, "cannot create state directory: " << strerror(errno));
    }
    struct stat info;
    Y_ENSURE(lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 0077) == 0, "state directory must be owner-only and not a symlink");
    const auto lockPath = path / "controller.lock";
    Impl->Lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    Y_ENSURE(Impl->Lock >= 0 && flock(Impl->Lock, LOCK_EX | LOCK_NB) == 0, "state directory is locked or lock unavailable");
    const auto databasePath = path / "skipper.sqlite3";
    if (lstat(databasePath.c_str(), &info) == 0) {
        Y_ENSURE(S_ISREG(info.st_mode) && info.st_uid == geteuid(), "database must be an owned regular file");
    }
    Check(Impl->Db, sqlite3_open_v2(databasePath.c_str(), &Impl->Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr));
    Y_ENSURE(chmod(databasePath.c_str(), 0600) == 0, "cannot secure database");
    sqlite3_busy_timeout(Impl->Db, 0);
    {
        TStatement wal(Impl->Db, "PRAGMA journal_mode=WAL");
        Y_ENSURE(wal.Step() && wal.Text(0) == "wal", "WAL unavailable");
    }
    Execute(Impl->Db, "PRAGMA synchronous=FULL");
    Execute(Impl->Db, "PRAGMA foreign_keys=ON");
    {
        TStatement check(Impl->Db, "PRAGMA quick_check");
        Y_ENSURE(check.Step() && check.Text(0) == "ok" && !check.Step(), "corrupt durable database");
    }
    TTransaction transaction(Impl->Db);
    Execute(Impl->Db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL)");
    const auto schema = GetMeta(Impl->Db, "schema");
    if (schema) {
        Y_ENSURE(*schema == "1", "unsupported durable schema");
        Y_ENSURE(GetMeta(Impl->Db, "cluster_id") == binding.ClusterId && GetMeta(Impl->Db, "member_id") == binding.MemberId && GetMeta(Impl->Db, "backend") == binding.Backend, "state directory binding mismatch");
        Y_ENSURE(GetMeta(Impl->Db, "membership_incarnation") && !GetMeta(Impl->Db, "membership_incarnation")->empty(), "missing durable membership incarnation");
        TStatement tables(Impl->Db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('meta','log','snapshot')");
        Y_ENSURE(tables.Step() && tables.Integer(0) == 3, "missing durable table");
    } else {
        TStatement tables(Impl->Db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name<>'meta' AND name NOT LIKE 'sqlite_%'");
        Y_ENSURE(tables.Step() && tables.Integer(0) == 0, "unbound existing database");
        SetMeta(Impl->Db, "schema", "1");
        SetMeta(Impl->Db, "cluster_id", binding.ClusterId);
        SetMeta(Impl->Db, "member_id", binding.MemberId);
        SetMeta(Impl->Db, "backend", binding.Backend);
        SetMeta(Impl->Db, "membership_incarnation", binding.InitialMembershipIncarnation);
    }
    Y_ENSURE(!GetMeta(Impl->Db, "process_incarnation:" + binding.ProcessIncarnation) && GetMeta(Impl->Db, "membership_incarnation") != binding.ProcessIncarnation, "process incarnation reused on restart");
    Execute(Impl->Db, "CREATE TABLE IF NOT EXISTS log(idx INTEGER PRIMARY KEY CHECK(idx>0),command_id TEXT UNIQUE NOT NULL,request_id TEXT UNIQUE,caller_id TEXT NOT NULL CHECK(caller_id='local'),schema INTEGER NOT NULL,kind INTEGER NOT NULL,payload TEXT NOT NULL,hash TEXT NOT NULL,result TEXT NOT NULL,request_hash TEXT NOT NULL,audit_time TEXT NOT NULL,operation_id TEXT NOT NULL)");
    Execute(Impl->Db, "CREATE TABLE IF NOT EXISTS snapshot(singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema INTEGER NOT NULL,prefix INTEGER NOT NULL CHECK(prefix>=0),state TEXT NOT NULL,digest TEXT NOT NULL)");
    SetMeta(Impl->Db, "last_process_incarnation", binding.ProcessIncarnation);
    SetMeta(Impl->Db, "process_incarnation:" + binding.ProcessIncarnation, "started");
    transaction.Commit();
    // Make the directory, lock, database and WAL entries durable before any
    // command can commit; a previous run may have crashed before this point.
    SyncDirectory(directory);
    SyncDirectory(PhysicalParentDirectory(directory));
    Impl->Initial.ClusterId = binding.ClusterId;
    Impl->Initial.MemberId = binding.MemberId;
    Impl->Initial.Backend = binding.Backend;
    Impl->Initial.MembershipIncarnation = *GetMeta(Impl->Db, "membership_incarnation");
    // Read and validate every authoritative record before exposing the store.
    Load();
}

TLocalStore::~TLocalStore() = default;

TDurableState TLocalStore::Load() {
    try {
        TDurableState state = Impl->Initial;
        ui64 snapshotPrefix = 0;
        {
            TStatement snapshot(Impl->Db, "SELECT schema,prefix,state,digest FROM snapshot WHERE singleton=1");
            if (snapshot.Step()) {
                const auto bytes = snapshot.Text(2);
                Y_ENSURE(snapshot.Integer(0) == 1 && DurableHash(bytes) == snapshot.Text(3), "snapshot schema/integrity mismatch");
                state = DeserializeState(bytes);
                snapshotPrefix = snapshot.Integer(1);
                Y_ENSURE(state.AppliedIndex == snapshotPrefix && snapshotPrefix <= Impl->LastIndex(), "snapshot prefix mismatch");
                Y_ENSURE(state.ClusterId == Impl->Initial.ClusterId && state.MemberId == Impl->Initial.MemberId && state.Backend == Impl->Initial.Backend && state.MembershipIncarnation == Impl->Initial.MembershipIncarnation, "snapshot binding mismatch");
            }
        }
        ui64 expected = 1;
        Impl->ReplayCount = 0;
        TStatement log(Impl->Db, "SELECT idx,command_id,request_id,caller_id,schema,kind,payload,hash,result,request_hash,audit_time,operation_id FROM log ORDER BY idx");
        while (log.Step()) {
            const ui64 index = log.Integer(0);
            const auto bytes = log.Text(6);
            Y_ENSURE(index == expected++, "durable log gap/order failure");
            Y_ENSURE(DurableHash(bytes) == log.Text(7), "durable command integrity failure");
            const auto command = DeserializeCommand(bytes);
            const auto result = DeserializeResult(log.Text(8));
            Y_ENSURE(command.Id == log.Text(1) && command.RequestId.value_or(TString()) == log.Text(2) && log.Text(3) == "local" && command.Schema == log.Integer(4) && static_cast<ui64>(command.Kind) == log.Integer(5) && command.RequestHash == log.Text(9) && command.AuditTime == log.Text(10) && command.OperationId == log.Text(11) && result.Index == index, "durable log envelope mismatch");
            if (index > snapshotPrefix) {
                auto applied = Apply(state, command);
                Y_ENSURE(!applied.Duplicate && applied.State.AppliedIndex == index && applied.Result == result, "deterministic durable replay mismatch");
                state = std::move(applied.State);
                ++Impl->ReplayCount;
            } else {
                const auto receipt = state.Commands.find(command.Id);
                Y_ENSURE(receipt != state.Commands.end() && receipt->second.PayloadHash == log.Text(7) && receipt->second.Result == result, "snapshot/log prefix mismatch");
                if (command.RequestId) {
                    const auto request = state.Requests.find(*command.RequestId);
                    Y_ENSURE(request != state.Requests.end() && request->second.PayloadHash == command.RequestHash && request->second.Result == result, "snapshot request receipt mismatch");
                }
            }
        }
        Y_ENSURE(state.AppliedIndex + 1 == expected, "snapshot invents durable state");
        Y_ENSURE(state.Commands.size() == state.AppliedIndex, "snapshot contains invented command receipts");
        Impl->InstalledDigest = DurableHash(SerializeState(state));
        Impl->Failed = false;
        return state;
    } catch (...) {
        Impl->Failed = true;
        throw;
    }
}

TProposalResult TLocalStore::Propose(const TDurableState& installed, const TDurableCommand& command) {
    if (Impl->Failed || Impl->Unknown) {
        return {EProposalStatus::Rejected, {}, {}, "PERSISTENCE_ERROR_OR_UNKNOWN_ADMISSION"};
    }
    try {
        Y_ENSURE(installed.AppliedIndex == Impl->LastIndex() && DurableHash(SerializeState(installed)) == Impl->InstalledDigest, "installed state prefix/content mismatch");
        auto applied = Apply(installed, command);
        if (applied.Duplicate) {
            if (applied.Result.Code == "COMMAND_ID_REUSED" || applied.Result.Code == "REQUEST_ID_REUSED") {
                return {EProposalStatus::Rejected, applied.Result, {}, applied.Result.Code};
            }
            return {EProposalStatus::Committed, applied.Result, installed, {}};
        }
        if (applied.State.AppliedIndex != installed.AppliedIndex + 1) {
            return {EProposalStatus::Rejected, applied.Result, {}, applied.Result.Code};
        }
        const auto payload = SerializeCommand(command);
        if (payload.size() > MaxDurableRecordBytes) {
            return {EProposalStatus::Rejected, {}, {}, "DURABLE_RECORD_TOO_LARGE"};
        }
        TTransaction transaction(Impl->Db);
        TStatement insert(Impl->Db, "INSERT INTO log(idx,command_id,request_id,caller_id,schema,kind,payload,hash,result,request_hash,audit_time,operation_id) VALUES(?1,?2,?3,'local',?4,?5,?6,?7,?8,?9,?10,?11)");
        insert.Integer(1, applied.State.AppliedIndex);
        insert.Text(2, command.Id);
        if (command.RequestId) {
            insert.Text(3, *command.RequestId);
        } else {
            insert.Null(3);
        }
        insert.Integer(4, command.Schema);
        insert.Integer(5, static_cast<ui64>(command.Kind));
        insert.Text(6, payload);
        insert.Text(7, DurableHash(payload));
        insert.Text(8, SerializeResult(applied.Result));
        insert.Text(9, command.RequestHash);
        insert.Text(10, command.AuditTime);
        insert.Text(11, command.OperationId);
        insert.Step();
        const auto fault = Impl->Fault;
        Impl->Fault = EStoreFault::None;
        if (fault == EStoreFault::BeforeCommit) {
            ythrow yexception() << "injected disk/write failure before commit";
        }
        transaction.Commit();
        if (fault == EStoreFault::AfterCommit) {
            ythrow yexception() << "injected unknown completion after commit";
        }
        Impl->InstalledDigest = DurableHash(SerializeState(applied.State));
        return {EProposalStatus::Committed, applied.Result, std::move(applied.State), {}};
    } catch (const std::exception& error) {
        // Even a definite local rollback is exposed conservatively until lookup.
        Impl->Unknown = true;
        Impl->Failed = true;
        return {EProposalStatus::Unknown, {}, {}, TString(error.what())};
    }
}

std::optional<TCommandResult> TLocalStore::LookupCommand(TStringBuf id) const {
    TStatement query(Impl->Db, "SELECT result,payload,hash FROM log WHERE command_id=?1");
    query.Text(1, id);
    if (!query.Step()) {
        return std::nullopt;
    }
    Y_ENSURE(DurableHash(query.Text(1)) == query.Text(2), "lookup command integrity failure");
    return DeserializeResult(query.Text(0));
}

std::optional<TStoredReceipt> TLocalStore::LookupRequest(TStringBuf id) const {
    TStatement query(Impl->Db, "SELECT request_hash,result,payload,hash FROM log WHERE request_id=?1");
    query.Text(1, id);
    if (!query.Step()) {
        return std::nullopt;
    }
    Y_ENSURE(DurableHash(query.Text(2)) == query.Text(3), "lookup request integrity failure");
    return TStoredReceipt{query.Text(0), DeserializeResult(query.Text(1))};
}

std::vector<TDurableAuditEntry> TLocalStore::ReadAudit(ui64 after, size_t limit) const {
    Y_ENSURE(limit > 0 && limit <= 1001, "audit limit out of range");
    std::vector<TDurableAuditEntry> entries;
    if (after > static_cast<ui64>(std::numeric_limits<i64>::max())) {
        return entries;
    }
    TStatement query(Impl->Db, "SELECT idx,payload,hash,result FROM log WHERE idx>?1 ORDER BY idx LIMIT ?2");
    query.Integer(1, after);
    query.Integer(2, limit);
    while (query.Step()) {
        Y_ENSURE(DurableHash(query.Text(1)) == query.Text(2), "audit command integrity failure");
        TDurableAuditEntry entry;
        entry.Index = query.Integer(0);
        entry.Command = DeserializeCommand(query.Text(1));
        entry.Result = DeserializeResult(query.Text(3));
        Y_ENSURE(entry.Result.Index == entry.Index, "audit result index mismatch");
        entries.push_back(std::move(entry));
    }
    return entries;
}

TProposalResult TLocalStore::ResolveUnknown(TStringBuf commandId) {
    try {
        const auto result = LookupCommand(commandId);
        auto state = Load();
        Impl->Unknown = false;
        Impl->Failed = false;
        return {result ? EProposalStatus::Committed : EProposalStatus::Rejected, result.value_or(TCommandResult{}), std::move(state), result ? TString() : TString("COMMAND_NOT_COMMITTED")};
    } catch (const std::exception& error) {
        Impl->Failed = true;
        return {EProposalStatus::Unknown, {}, {}, TString(error.what())};
    }
}

bool TLocalStore::Checkpoint(const TDurableState& installed) {
    try {
        const auto bytes = SerializeState(installed);
        Y_ENSURE(!Impl->Failed && !Impl->Unknown && installed.AppliedIndex == Impl->LastIndex() && DurableHash(bytes) == Impl->InstalledDigest, "checkpoint requires exact installed durable prefix/state");
        const bool oversized = Impl->Fault == EStoreFault::OversizedCheckpoint;
        if (oversized) {
            Impl->Fault = EStoreFault::None;
        }
        if (oversized || bytes.size() > MaxDurableRecordBytes) {
            return false;
        }
        TTransaction transaction(Impl->Db);
        TStatement insert(Impl->Db, "INSERT INTO snapshot(singleton,schema,prefix,state,digest) VALUES(1,1,?1,?2,?3) ON CONFLICT(singleton) DO UPDATE SET schema=excluded.schema,prefix=excluded.prefix,state=excluded.state,digest=excluded.digest");
        insert.Integer(1, installed.AppliedIndex);
        insert.Text(2, bytes);
        insert.Text(3, DurableHash(bytes));
        insert.Step();
        if (Impl->Fault == EStoreFault::BeforeCheckpointCommit) {
            Impl->Fault = EStoreFault::None;
            ythrow yexception() << "injected checkpoint failure";
        }
        transaction.Commit();
        return true;
    } catch (...) {
        Impl->Failed = true;
        throw;
    }
}

TStoreStats TLocalStore::Stats() const {
    TStoreStats stats;
    TStatement log(Impl->Db, "SELECT count(*),coalesce(sum(length(payload)+length(result)),0) FROM log");
    Y_ENSURE(log.Step(), "missing log stats");
    stats.LogRows = log.Integer(0);
    stats.LogBytes = log.Integer(1);
    TStatement snapshot(Impl->Db, "SELECT coalesce(sum(length(state)),0) FROM snapshot");
    Y_ENSURE(snapshot.Step(), "missing snapshot stats");
    stats.SnapshotBytes = snapshot.Integer(0);
    stats.ReplayCommands = Impl->ReplayCount;
    return stats;
}

const TString& TLocalStore::ProcessIncarnation() const {
    return Impl->Binding.ProcessIncarnation;
}

bool TLocalStore::Failed() const {
    return Impl->Failed || Impl->Unknown;
}

void TLocalStore::InjectFault(EStoreFault fault) {
    Impl->Fault = fault;
}

} // namespace NBridgeSkipper
