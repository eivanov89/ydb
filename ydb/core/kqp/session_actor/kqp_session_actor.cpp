#include "kqp_session_actor.h"
#include "kqp_worker_common.h"
#include "kqp_query_state.h"
#include "kqp_query_stats.h"

#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/kqp/common/buffer/buffer.h>
#include <ydb/core/kqp/common/buffer/events.h>
#include <ydb/core/kqp/common/kqp_data_integrity_trails.h>
#include <ydb/core/kqp/common/kqp_lwtrace_probes.h>
#include <ydb/core/kqp/common/kqp_ru_calc.h>
#include <ydb/core/kqp/common/kqp_timeouts.h>
#include <ydb/core/kqp/common/kqp_tx.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/common/events/workload_service.h>
#include <ydb/core/kqp/common/simple/query_ast.h>
#include <ydb/core/kqp/common/batch/params.h>
#include <ydb/core/kqp/compile_service/kqp_compile_service.h>
#include <ydb/core/kqp/compile_service/kqp_fast_query.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/kqp/executer_actor/kqp_locks_helper.h>
#include <ydb/core/kqp/executer_actor/kqp_partitioned_executer.h>
#include <ydb/core/kqp/host/kqp_host_impl.h>
#include <ydb/core/kqp/opt/kqp_query_plan.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider.h>
#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <ydb/core/kqp/rm_service/kqp_snapshot_manager.h>
#include <ydb/core/ydb_convert/ydb_convert.h>
#include <ydb/core/tx/data_events/events.h>
#include <ydb/core/tx/data_events/payload_helper.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/kqp/rm_service/kqp_rm_service.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/library/operation_id/operation_id.h>
#include <ydb/core/kqp/common/kqp_yql.h>

#include <ydb/core/util/ulid.h>

#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/cputime.h>
#include <ydb/core/base/path.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/sys_view/service/sysview_service.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/library/yql/utils/actor_log/log.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/event_pb.h>
#include <ydb/library/actors/core/executor_thread.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

#include <util/string/printf.h>

#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/actors/wilson/wilson_trace.h>

LWTRACE_USING(KQP_PROVIDER);

namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NSchemeCache;

namespace {

std::optional<TString> TryDecodeYdbSessionId(const TString& sessionId) {
    if (sessionId.empty()) {
        return std::nullopt;
    }

    try {
        NOperationId::TOperationId opId(sessionId);
        TString id;
        const auto& ids = opId.GetValue("id");
        if (ids.size() != 1) {
            return std::nullopt;
        }

        return TString{*ids[0]};
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

TString ToLower(const TString& s) {
    TString lowerS;
    lowerS.reserve(s.size());
    for (auto c: s) {
        lowerS.push_back(std::tolower(c));
    }
    return lowerS;
}

TString ToUpper(const TString& s) {
    TString upperS;
    upperS.reserve(s.size());
    for (auto c: s) {
        upperS.push_back(std::toupper(c));
    }
    return upperS;
}

bool IsEqual(const TString& left, const TString& right) {
    if (left == right) {
        return true;
    }

    auto upperLeft = ToUpper(left);
    if (upperLeft == right) {
        return true;
    }

    auto lowerLeft = ToLower(left);
    return lowerLeft == right;
}

// XXX
template <typename TMap>
TMap::iterator FindCaseI(TMap& m, const TString& name) {
    auto it = m.find(name);
    if (it != m.end()) {
        return it;
    }
    it = m.find(ToLower(name));
    if (it != m.end()) {
        return it;
    }
    return m.find(ToUpper(name));
}

#define LOG_C(msg) LOG_CRIT_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_E(msg) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_W(msg) LOG_WARN_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_N(msg) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_I(msg) LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_D(msg) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)
#define LOG_T(msg) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_SESSION, LogPrefix() << msg)

void FillColumnsMeta(const NKqpProto::TKqpPhyQuery& phyQuery, NKikimrKqp::TQueryResponse& resp) {
    for (size_t i = 0; i < phyQuery.ResultBindingsSize(); ++i) {
        const auto& binding = phyQuery.GetResultBindings(i);
        auto ydbRes = resp.AddYdbResults();
        ydbRes->mutable_columns()->CopyFrom(binding.GetResultSetMeta().columns());
    }
}

bool FillTableSinkSettings(NKikimrKqp::TKqpTableSinkSettings& settings, const NKqpProto::TKqpSink& sink) {
    if (sink.GetTypeCase() == NKqpProto::TKqpSink::kInternalSink
        && sink.GetInternalSink().GetSettings().Is<NKikimrKqp::TKqpTableSinkSettings>())
    {
        return sink.GetInternalSink().GetSettings().UnpackTo(&settings);
    }

    return false;
}

bool IsBatchQuery(const NKqpProto::TKqpPhyQuery& physicalQuery) {
    NKikimrKqp::TKqpTableSinkSettings sinkSettings;
    for (const auto& tx : physicalQuery.GetTransactions()) {
        for (const auto& stage : tx.GetStages()) {
            for (auto& sink : stage.GetSinks()) {
                auto isFilledSettings = FillTableSinkSettings(sinkSettings, sink);
                if (isFilledSettings && sinkSettings.HasIsBatch() && sinkSettings.GetIsBatch()) {
                    return true;
                }
            }
        }
    }
    return false;
}

class TRequestFail : public yexception {
public:
    Ydb::StatusIds::StatusCode Status;
    std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> Issues;

    TRequestFail(Ydb::StatusIds::StatusCode status,
            std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
        : Status(status)
        , Issues(std::move(issues))
    {}
};

struct TKqpCleanupCtx {
    std::deque<TIntrusivePtr<TKqpTransactionContext>> TransactionsToBeAborted;
    bool IsWaitingForWorkerToClose = false;
    bool IsWaitingForWorkloadServiceCleanup = false;
    bool Final = false;
    TInstant Start = TInstant::Now();

    bool CleanupFinished() {
        return TransactionsToBeAborted.empty() && !IsWaitingForWorkerToClose && !IsWaitingForWorkloadServiceCleanup;
    }
};

class TFastSelect1 : public TActor<TFastSelect1> {
    public:
        static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
            return NKikimrServices::TActivity::KQP_FAST_QUERY_ACTOR;
        }

    public:
        TFastSelect1(TIntrusivePtr<TKqpCounters>& counters,
                const TActorId& replyTo,
                const TString& sessionId,
                TFastQueryPtr fastQuery,
                const TKqpQueryState::TQueryTxId& userTxId,
                ui64 lockId)
            : TActor(&TThis::ExecuteState)
            , Counters(counters)
            , ReplyTo(replyTo)
            , SessionId(sessionId)
            , FastQuery(std::move(fastQuery))
            , UserTxId(userTxId)
            , LockId(lockId)
            , StartedTs(TlsActivationContext->AsActorContext().Monotonic().Now())
        {
        }

        STATEFN(ExecuteState) {
            try {
                switch (ev->GetTypeRewrite()) {
                    hFunc(TEvKqpExecuter::TEvTxRequest, Handle);
                    hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle);
                    hFunc(TEvents::TEvUndelivered, Handle);
                default:
                    UnexpectedEvent("ExecuteState", ev);
                }
            } catch (const TRequestFail& ex) {
                ReplyQueryError(ex.Status, ex.what(), ex.Issues);
            } catch (const yexception& ex) {
                InternalError(ex.what());
            }
        }

    private:
        void Handle(TEvKqpExecuter::TEvTxRequest::TPtr& ev) {
            OnTxId(ev->Get()->Record.GetRequest().GetTxId());
        }

        void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
            OnTxId(ev->Get()->TxId);
        }

        void OnTxId(ui64 txId) {
            LOG_T("allocated txId " << txId << " for query " << FastQuery->PostgresQuery.Query);
            TxAllocatedTs = TlsActivationContext->AsActorContext().Monotonic().Now();
            TxId = txId;
            if (LockId == 0) {
                LockId = TxId;
                LockHandle = NLongTxService::TLockHandle(TxId, TActivationContext::ActorSystem());
            }

            Execute();
        }

        void Execute() {
            auto response = std::make_unique<TEvKqp::TEvQueryResponse>();
            response->Record.SetYdbStatus(Ydb::StatusIds::SUCCESS);

            auto& queryResponse = *response->Record.MutableResponse();
            queryResponse.SetSessionId(SessionId);
            queryResponse.AddYdbResults();

            queryResponse.MutableTxMeta()->set_id(UserTxId.GetValue().GetHumanStr());

            LOG_T("replying: " << response->ToString());

            Send<ESendingType::Tail>(ReplyTo, response.release());
            PassAway();
        }

        void Handle(TEvents::TEvUndelivered::TPtr&) {
            Execute();
        }

        void UnexpectedEvent(const TString&, TAutoPtr<NActors::IEventHandle>& ev) {
            InternalError(TStringBuilder() << "TFastQueryExecutorActor received unexpected event "
                << ev->GetTypeName() << Sprintf("(0x%08" PRIx32 ")", ev->GetTypeRewrite())
                << " sender: " << ev->Sender);
        }

        void InternalError(const TString& message) {
            LOG_E("Internal error, message: " << message);
            ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, message);
        }

        void ReplyQueryError(Ydb::StatusIds::StatusCode ydbStatus,
            const TString& message,
            std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
        {
            LOG_W(" error:" << message);

            auto queryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
            queryResponse->Record.SetYdbStatus(ydbStatus);

            if (issues) {
                auto* response = queryResponse->Record.MutableResponse();
                for (auto& i : *issues) {
                    response->AddQueryIssues()->Swap(&i);
                }
            }

            Send<ESendingType::Tail>(ReplyTo, queryResponse.release());
            PassAway();
        }

        TString LogPrefix() const {
            TStringBuilder result = TStringBuilder()
                << "Fast executor SessionId: " << SessionId << ", "
                << "ActorId: " << SelfId() << ", ";
            return result;
        }

    private:
        TIntrusivePtr<TKqpCounters> Counters;
        TActorId ReplyTo;
        TString SessionId;
        TFastQueryPtr FastQuery;
        TKqpQueryState::TQueryTxId UserTxId;
        ui64 LockId;

        ui64 TxId = 0;
        NLongTxService::TLockHandle LockHandle;

        TVector<TCell> KeyCells;
        std::unique_ptr<NKikimr::TKeyDesc> ResolvedKeys;

        TMonotonic StartedTs;
        TMonotonic TxAllocatedTs;
        TMonotonic StartResolveSchemeTs;
        TMonotonic StopResolveSchemeTs;
    };

class TFastSelectExecutorActor : public TActor<TFastSelectExecutorActor> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_FAST_QUERY_ACTOR;
    }

public:
    TFastSelectExecutorActor(TIntrusivePtr<TKqpCounters>& counters,
            const TActorId& replyTo,
            const TString& sessionId,
            TFastQueryPtr fastQuery,
            const ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue>& params,
            const IKqpGateway::TKqpSnapshot& snapshot,
            const TKqpQueryState::TQueryTxId& userTxId,
            ui64 lockId)
        : TActor(&TThis::ExecuteState)
        , Counters(counters)
        , ReplyTo(replyTo)
        , SessionId(sessionId)
        , FastQuery(std::move(fastQuery))
        , Parameters(params)
        , Snapshot(snapshot)
        , UserTxId(userTxId)
        , LockId(lockId)
        , StartedTs(TlsActivationContext->AsActorContext().Monotonic().Now())
    {
    }

    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvKqpExecuter::TEvTxRequest, Handle);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle);
                hFunc(TEvents::TEvUndelivered, Handle);
                HFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
                HFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, Handle);
                HFunc(TEvDataShard::TEvReadResult, HandleRead);
            default:
                UnexpectedEvent("ExecuteState", ev);
            }
        } catch (const TRequestFail& ex) {
            ReplyQueryError(ex.Status, ex.what(), ex.Issues);
        } catch (const yexception& ex) {
            InternalError(ex.what());
        }
    }

private:
    void Handle(TEvKqpExecuter::TEvTxRequest::TPtr& ev) {
        OnTxId(ev->Get()->Record.GetRequest().GetTxId());
    }

    void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
        OnTxId(ev->Get()->TxId);
    }

    void OnTxId(ui64 txId) {
        LOG_T("allocated txId " << txId << " for query " << FastQuery->PostgresQuery.Query);
        TxAllocatedTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TxId = txId;
        if (LockId == 0) {
            LockId = TxId;
            LockHandle = NLongTxService::TLockHandle(TxId, TActivationContext::ActorSystem());
        }

        if (FastQuery->ResolvedColumns.empty()) {
            return ResolveSchema(TlsActivationContext->AsActorContext());
        }

        ResolveShards(TlsActivationContext->AsActorContext());
    }

    void ResolveSchema(const TActorContext &) {
        StartResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TString path;
        if (FastQuery->PostgresQuery.TablePathPrefix) {
            path = FastQuery->PostgresQuery.TablePathPrefix;
        } else {
            path = FastQuery->Database;
        }
        path += "/" + FastQuery->TableName;
        LOG_T("Resolving " << path);

        auto request = std::make_unique<NSchemeCache::TSchemeCacheNavigate>();
        NSchemeCache::TSchemeCacheNavigate::TEntry entry;
        entry.Path = ::NKikimr::SplitPath(path);
        if (entry.Path.empty()) {
            return ReplyQueryError(Ydb::StatusIds::NOT_FOUND, "Invalid table path specified");
        }
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
        request->ResultSet.emplace_back(entry);
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release()));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev, const TActorContext &ctx) {
        StopResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        const auto* response = ev->Get();
        Y_ABORT_UNLESS(response->Request != nullptr);

        Y_ABORT_UNLESS(response->Request->ResultSet.size() == 1);
        const auto& entry = response->Request->ResultSet.front();

        FastQuery->TableId = entry.TableId;

        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            return ReplyQueryError(Ydb::StatusIds::SCHEME_ERROR, ToString(entry.Status));
        }

        FastQuery->ResolvedColumns.reserve(entry.Columns.size());
        FastQuery->Columns.reserve(entry.Columns.size());
        FastQuery->KeyColumns.reserve(entry.Columns.size());
        FastQuery->KeyColumnTypes.reserve(entry.Columns.size());
        for (const auto& [_, info] : entry.Columns) {
            FastQuery->ResolvedColumns[info.Name] = info;
            FastQuery->Columns.emplace_back(info.Id, TKeyDesc::EColumnOperation::Read, info.PType, 0, 0);
            if (info.KeyOrder != -1) {
                FastQuery->KeyColumns.emplace_back(info);
                FastQuery->KeyColumnTypes.emplace_back(info.PType);
            }
        }

        LOG_T("Resolved to tableId " << FastQuery->TableId << " with key columns size " << FastQuery->KeyColumns.size()
            << " and total columns size " << FastQuery->ResolvedColumns.size());

        ResolveShards(ctx);
    }

    void ResolveShards(const TActorContext &) {
        StartResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        KeyCells.reserve(FastQuery->KeyColumns.size());
        for (const auto& keyColumn: FastQuery->KeyColumns) {
            auto whereIt = FindCaseI(FastQuery->WhereColumnsToPos, keyColumn.Name);
            if (whereIt == FastQuery->WhereColumnsToPos.end()) {
                continue; // key prefix
            }
            auto position = whereIt->second;
            TString varName = "$" + FastQuery->PostgresQuery.PositionalNames[position - 1];
            auto it = FindCaseI(Parameters, varName);
            if (it == Parameters.end()) {
                TStringStream ss;
                ss << "Parameter " << varName << " not found, params are: ";
                for (const auto& param: Parameters) {
                    ss << param.first << ",";
                }
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, ss.Str());
            }

            const auto& typedValue = it->second;
            KeyCells.emplace_back(TCell::Make<i32>(typedValue.Getvalue().Getint32_value()));
        }

        TTableRange range(KeyCells);

        // XXX: for simplicity request all columns
        THolder<NKikimr::TKeyDesc> keyDesc(
            new TKeyDesc(
                FastQuery->TableId,
                range,
                TKeyDesc::ERowOperation::Read,
                FastQuery->KeyColumnTypes,
                FastQuery->Columns));

        auto request = std::make_unique<NSchemeCache::TSchemeCacheRequest>();
        request->ResultSet.emplace_back(std::move(keyDesc));

        auto resolveRequest = std::make_unique<TEvTxProxySchemeCache::TEvResolveKeySet>(request.release());
        Send(MakeSchemeCacheID(), resolveRequest.release());
    }

    void Handle(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr &ev, const TActorContext &) {
        StopResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TEvTxProxySchemeCache::TEvResolveKeySetResult *msg = ev->Get();
        Y_ABORT_UNLESS(msg->Request->ResultSet.size() == 1);

        ResolvedKeys.reset(msg->Request->ResultSet[0].KeyDescription.Release());

        const auto& partitions = ResolvedKeys->GetPartitions();
        if (partitions.size() == 0) {
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Failed to resolve shards");
        }

        // just a sanity check
        if (partitions.size() != 1) {
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Key resolved to multiple shards");
        }

        ResolvedShardId = partitions[0].ShardId;

        LOG_T("Table [" << FastQuery->TableId << "] shard: " << ResolvedShardId);
        Execute();
    }

    void Execute() {
        StartReadingTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        auto request = std::make_unique<NKikimr::TEvDataShard::TEvRead>();
        auto& record = request->Record;
        record.SetReadId(1); // we read once, so doesn't matter
        record.SetLockTxId(LockId);
        record.SetResultFormat(NKikimrDataEvents::FORMAT_CELLVEC);
        record.MutableTableId()->SetOwnerId(FastQuery->TableId.PathId.OwnerId);
        record.MutableTableId()->SetTableId(FastQuery->TableId.PathId.LocalPathId);
        record.MutableTableId()->SetSchemaVersion(FastQuery->TableId.SchemaVersion);

        for (const auto& name: FastQuery->ColumnsToSelect) {
            auto it = FindCaseI(FastQuery->ResolvedColumns, name);
            if (it == FastQuery->ResolvedColumns.end()) {
                TStringStream ss;
                ss << "Column " << name << " not found";
                return ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, ss.Str());
            }
            record.AddColumns(it->second.Id);
        }

        request->Keys.emplace_back(KeyCells);

        TRowVersion readVersion(Snapshot.Step, Snapshot.TxId);
        readVersion.ToProto(*record.MutableSnapshot());

        LOG_T("sending read request to " << FastQuery->TableName << "(" << ResolvedShardId << "): "
            << request->ToString());

        Send<ESendingType::Tail>(
            NKikimr::MakePipePerNodeCacheID(false),
            new TEvPipeCache::TEvForward(request.release(), ResolvedShardId, true),
            IEventHandle::FlagTrackDelivery,
            0,
            0);
    }

    void HandleRead(TEvDataShard::TEvReadResult::TPtr ev, const TActorContext &) {
        const auto* msg = ev->Get();
        LOG_T("received read result from shard " << ResolvedShardId << ": " << msg->ToString());

        auto response = std::make_unique<TEvKqp::TEvQueryResponse>();
        response->Record.SetYdbStatus(Ydb::StatusIds::SUCCESS);

        for (const auto& lock: msg->Record.GetTxLocks()) {
            response->Locks.emplace_back(lock);
        }
        response->LockHandle = std::move(LockHandle);

        auto& queryResponse = *response->Record.MutableResponse();
        queryResponse.SetSessionId(SessionId);
        auto& resultSets = *queryResponse.AddYdbResults();


        for (size_t i = 0; i < FastQuery->ColumnsToSelect.size(); ++i) {
            const auto& name = FastQuery->ColumnsToSelect[i];
            auto it = FindCaseI(FastQuery->ResolvedColumns, name);
            auto& column = *resultSets.Addcolumns();
            column.set_name(ToUpper(name)); // XXX

            // XXX XXX XXX (only some of the types)
            switch (it->second.PType.GetTypeId()) {
            case NScheme::NTypeIds::Int32:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::INT32);
                break;
            case NScheme::NTypeIds::Uint32:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UINT32);
                break;
            case NScheme::NTypeIds::Int64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::INT64);
                break;
            case NScheme::NTypeIds::Uint64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UINT64);
                break;
            case NScheme::NTypeIds::Double:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::DOUBLE);
                break;
            case NScheme::NTypeIds::Float:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::FLOAT);
                break;
            case NScheme::NTypeIds::Utf8:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UTF8);
                break;
            case NScheme::NTypeIds::Timestamp:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::TIMESTAMP);
                break;
            case NScheme::NTypeIds::Timestamp64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::TIMESTAMP64);
                break;
            default:
                ;
            }
        }

        for (size_t rowNum = 0; rowNum < msg->GetRowsCount(); ++rowNum) {
            auto& row = *resultSets.Addrows();
            const auto& resultRow = msg->GetCells(rowNum);
            for (size_t i = 0; i < FastQuery->ColumnsToSelect.size(); ++i) {
                const auto& name = FastQuery->ColumnsToSelect[i];
                auto it = FindCaseI(FastQuery->ResolvedColumns, name);

                const auto& cell = resultRow[i];

                // XXX: normally should not happen IIRC
                if (cell.Size() == 0) {
                    *row.add_items() = it->second.DefaultFromLiteral.value();
                    continue;
                }

                // XXX XXX XXX (only some of the types)
                switch (it->second.PType.GetTypeId()) {
                case NScheme::NTypeIds::Int32:
                    row.add_items()->set_int32_value(cell.AsValue<i32>());
                    break;
                case NScheme::NTypeIds::Uint32:
                    row.add_items()->set_uint32_value(cell.AsValue<ui32>());
                    break;
                case NScheme::NTypeIds::Timestamp64:
                case NScheme::NTypeIds::Int64:
                    row.add_items()->set_int64_value(cell.AsValue<i64>());
                    break;
                case NScheme::NTypeIds::Timestamp:
                case NScheme::NTypeIds::Uint64:
                    row.add_items()->set_uint64_value(cell.AsValue<ui64>());
                    break;
                case NScheme::NTypeIds::Double:
                    row.add_items()->set_double_value(cell.AsValue<double>());
                    break;
                case NScheme::NTypeIds::Float:
                    row.add_items()->set_float_value(cell.AsValue<float>());
                    break;
                case NScheme::NTypeIds::Utf8:
                    row.add_items()->set_text_value(""); // XXX
                    break;
                default:
                    *row.add_items() = it->second.DefaultFromLiteral.value();
                }
            }
        }

        queryResponse.MutableTxMeta()->set_id(UserTxId.GetValue().GetHumanStr());

        LOG_T("received read result from shard " << ResolvedShardId << ", replying: " << response->ToString());

        Send<ESendingType::Tail>(ReplyTo, response.release());

        StopReadingTs = TlsActivationContext->AsActorContext().Monotonic().Now();

        ReportMetrics();
        PassAway();
    }

    void Handle(TEvents::TEvUndelivered::TPtr&) {
        // assume, that it's from DS, because other messages are to local services
        Execute();
    }

    void UnexpectedEvent(const TString&, TAutoPtr<NActors::IEventHandle>& ev) {
        InternalError(TStringBuilder() << "TFastQueryExecutorActor received unexpected event "
            << ev->GetTypeName() << Sprintf("(0x%08" PRIx32 ")", ev->GetTypeRewrite())
            << " sender: " << ev->Sender);
    }

    void InternalError(const TString& message) {
        LOG_E("Internal error, message: " << message);
        ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, message);
    }

    void ReplyQueryError(Ydb::StatusIds::StatusCode ydbStatus,
        const TString& message,
        std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
    {
        LOG_W(" error:" << message);

        auto queryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        queryResponse->Record.SetYdbStatus(ydbStatus);

        if (issues) {
            auto* response = queryResponse->Record.MutableResponse();
            for (auto& i : *issues) {
                response->AddQueryIssues()->Swap(&i);
            }
        }

        Send<ESendingType::Tail>(ReplyTo, queryResponse.release());
        PassAway();
    }

    TString LogPrefix() const {
        TStringBuilder result = TStringBuilder()
            << "Fast executor SessionId: " << SessionId << ", "
            << "ActorId: " << SelfId() << ", ";
        return result;
    }

    void ReportMetrics() {
        if (!Counters) {
            return;
        }
        auto totalTime = StopReadingTs - StartedTs;
        auto timeToAllocateTx = TxAllocatedTs - StartedTs;
        auto timeToResolveScheme = StopResolveSchemeTs - StartResolveSchemeTs;
        auto timeToResolveShards = StopResolveShardsTs - StartResolveShardsTs;
        auto timeToRead = StopReadingTs - StartReadingTs;

        Counters->ReportFastSelect(
            totalTime.MicroSeconds(),
            timeToAllocateTx.MicroSeconds(),
            timeToResolveScheme.MicroSeconds(),
            timeToResolveShards.MicroSeconds(),
            timeToRead.MicroSeconds()
        );
    }

private:
    TIntrusivePtr<TKqpCounters> Counters;
    TActorId ReplyTo;
    TString SessionId;
    TFastQueryPtr FastQuery;
    ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue> Parameters;
    IKqpGateway::TKqpSnapshot Snapshot;
    TKqpQueryState::TQueryTxId UserTxId;
    ui64 LockId;

    ui64 TxId = 0;
    NLongTxService::TLockHandle LockHandle;

    TVector<TCell> KeyCells;
    std::unique_ptr<NKikimr::TKeyDesc> ResolvedKeys;
    ui64 ResolvedShardId = 0;

    TMonotonic StartedTs;
    TMonotonic TxAllocatedTs;
    TMonotonic StartResolveSchemeTs;
    TMonotonic StopResolveSchemeTs;
    TMonotonic StartResolveShardsTs;
    TMonotonic StopResolveShardsTs;
    TMonotonic StartReadingTs;
    TMonotonic StopReadingTs;
};

class TFastSelectInExecutorActor : public TActor<TFastSelectInExecutorActor> {
    struct TVecHash {
        size_t operator()(const TVector<int32_t> vec) const {
            if (vec.empty()) {
                return 0;
            }
            size_t sum = 0;
            for (auto item: vec) {
                sum += item;
            }
            return sum % vec.size();
        }
    };

    using TRequestedKeysSet = THashSet<TVector<int32_t>, TVecHash>;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_FAST_QUERY_ACTOR;
    }

public:
    TFastSelectInExecutorActor(TIntrusivePtr<TKqpCounters>& counters,
            const TActorId& replyTo,
            const TString& sessionId,
            TFastQueryPtr fastQuery,
            const ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue>& params,
            const IKqpGateway::TKqpSnapshot& snapshot,
            const TKqpQueryState::TQueryTxId& userTxId,
            ui64 lockId)
        : TActor(&TThis::ExecuteState)
        , Counters(counters)
        , ReplyTo(replyTo)
        , SessionId(sessionId)
        , FastQuery(std::move(fastQuery))
        , Parameters(params)
        , Snapshot(snapshot)
        , UserTxId(userTxId)
        , LockId(lockId)
        , StartedTs(TlsActivationContext->AsActorContext().Monotonic().Now())
    {
    }

    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvKqpExecuter::TEvTxRequest, Handle);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle);
                hFunc(TEvents::TEvUndelivered, Handle);
                HFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
                HFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, Handle);
                HFunc(TEvDataShard::TEvReadResult, HandleRead);
            default:
                UnexpectedEvent("ExecuteState", ev);
            }
        } catch (const TRequestFail& ex) {
            ReplyQueryError(ex.Status, ex.what(), ex.Issues);
        } catch (const yexception& ex) {
            InternalError(ex.what());
        }
    }

private:
    void Handle(TEvKqpExecuter::TEvTxRequest::TPtr& ev) {
        OnTxId(ev->Get()->Record.GetRequest().GetTxId());
    }

    void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
        OnTxId(ev->Get()->TxId);
    }

    void OnTxId(ui64 txId) {
        TxAllocatedTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        LOG_T("allocated txId " << txId << " for query " << FastQuery->PostgresQuery.Query);
        TxId = txId;
        if (LockId == 0) {
            LockId = TxId;
            LockHandle = NLongTxService::TLockHandle(TxId, TActivationContext::ActorSystem());
        }

        if (FastQuery->ResolvedColumns.empty()) {
            return ResolveSchema(TlsActivationContext->AsActorContext());
        }

        ResolveShards(TlsActivationContext->AsActorContext());
    }

    void ResolveSchema(const TActorContext &) {
        StartResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TString path;
        if (FastQuery->PostgresQuery.TablePathPrefix) {
            path = FastQuery->PostgresQuery.TablePathPrefix;
        } else {
            path = FastQuery->Database;
        }
        path += "/" + FastQuery->TableName;
        LOG_T("Resolving " << path);

        auto request = std::make_unique<NSchemeCache::TSchemeCacheNavigate>();
        NSchemeCache::TSchemeCacheNavigate::TEntry entry;
        entry.Path = ::NKikimr::SplitPath(path);
        if (entry.Path.empty()) {
            return ReplyQueryError(Ydb::StatusIds::NOT_FOUND, "Invalid table path specified");
        }
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
        request->ResultSet.emplace_back(entry);
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release()));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev, const TActorContext &ctx) {
        StopResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        const auto* response = ev->Get();
        Y_ABORT_UNLESS(response->Request != nullptr);

        Y_ABORT_UNLESS(response->Request->ResultSet.size() == 1);
        const auto& entry = response->Request->ResultSet.front();

        FastQuery->TableId = entry.TableId;

        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            return ReplyQueryError(Ydb::StatusIds::SCHEME_ERROR, ToString(entry.Status));
        }

        FastQuery->ResolvedColumns.reserve(entry.Columns.size());
        FastQuery->Columns.reserve(entry.Columns.size());
        FastQuery->KeyColumns.reserve(entry.Columns.size());
        FastQuery->KeyColumnTypes.reserve(entry.Columns.size());
        for (const auto& [_, info] : entry.Columns) {
            FastQuery->ResolvedColumns[info.Name] = info;
            FastQuery->Columns.emplace_back(info.Id, TKeyDesc::EColumnOperation::Read, info.PType, 0, 0);
            if (info.KeyOrder != -1) {
                FastQuery->KeyColumns.emplace_back(info);
                FastQuery->KeyColumnTypes.emplace_back(info.PType);
            }
        }

        for (const auto& keyColumn: FastQuery->KeyColumns) {
            for (size_t i = 0; i < FastQuery->WhereSelectInColumns.size(); ++i) {
                if (IsEqual(keyColumn.Name, FastQuery->WhereSelectInColumns[i])) {
                    FastQuery->OrderedWhereSelectInColumns.push_back(i);
                }
            }
        }

        LOG_T("Resolved to tableId " << FastQuery->TableId << " with key columns size " << FastQuery->KeyColumns.size()
            << " and total columns size " << FastQuery->ResolvedColumns.size()
            << ", OrderedWhereColumns size " << FastQuery->OrderedWhereSelectInColumns.size()
            << ", WhereSelectInColumns size " << FastQuery->WhereSelectInColumns.size());

        ResolveShards(ctx);
    }

    void ResolveShards(const TActorContext &) {
        StartResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        LOG_T("Resolving " << FastQuery->WhereSelectInPos.size() << " points");
        ResolvedPoints.resize(FastQuery->WhereSelectInPos.size());
        auto request = std::make_unique<NSchemeCache::TSchemeCacheRequest>();

        for (size_t i = 0; i < FastQuery->WhereSelectInPos.size(); ++i) {
            const auto& pointToResolve = FastQuery->WhereSelectInPos[i];
            auto& resolvedData = ResolvedPoints[i];

            resolvedData.KeyCells.reserve(FastQuery->WhereSelectInColumns.size());

            for (size_t keyColumnIndex: FastQuery->OrderedWhereSelectInColumns) {
                const auto& keyColumnName = FastQuery->WhereSelectInColumns[keyColumnIndex];
                auto position = pointToResolve[keyColumnIndex];

                TString varName = "$" + FastQuery->PostgresQuery.PositionalNames[position - 1];
                auto it = FindCaseI(Parameters, varName);
                if (it == Parameters.end()) {
                    TStringStream ss;
                    ss << "Parameter " << varName << " not found, params are: ";
                    for (const auto& param: Parameters) {
                        ss << param.first << ",";
                    }
                    return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, ss.Str());
                }

                const auto& typedValue = it->second;
                LOG_T("Key column '" << keyColumnName << "' with value " << typedValue.Getvalue().Getint32_value());
                resolvedData.KeyCells.emplace_back(TCell::Make<i32>(typedValue.Getvalue().Getint32_value()));
                resolvedData.KeyInts.push_back(typedValue.Getvalue().Getint32_value());
            }

            TTableRange range(resolvedData.KeyCells);

            // XXX: for simplicity request all columns
            THolder<NKikimr::TKeyDesc> keyDesc(
                new TKeyDesc(
                    FastQuery->TableId,
                    range,
                    TKeyDesc::ERowOperation::Read,
                    FastQuery->KeyColumnTypes,
                    FastQuery->Columns));

            request->ResultSet.emplace_back(std::move(keyDesc));
        }

        auto resolveRequest = std::make_unique<TEvTxProxySchemeCache::TEvResolveKeySet>(request.release());
        Send(MakeSchemeCacheID(), resolveRequest.release());
    }

    void Handle(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr &ev, const TActorContext &) {
        StopResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TEvTxProxySchemeCache::TEvResolveKeySetResult *msg = ev->Get();
        Y_ABORT_UNLESS(msg->Request->ResultSet.size() == FastQuery->WhereSelectInPos.size());

        for (size_t i = 0; i < msg->Request->ResultSet.size(); ++i) {
            auto& resolvedData = ResolvedPoints[i];
            auto& result = msg->Request->ResultSet[i];
            resolvedData.ResolvedKeys.reset(result.KeyDescription.Release());

            const auto& partitions = resolvedData.ResolvedKeys->GetPartitions();
            if (partitions.size() == 0) {
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Failed to resolve shards");
            }
            // just a sanity check
            if (partitions.size() != 1) {
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Key resolved to multiple shards");
            }

            resolvedData.ResolvedShardId = partitions[0].ShardId;

            LOG_T("Table [" << FastQuery->TableId << "] point" << i << " shard: " << resolvedData.ResolvedShardId);
        }

        Execute();
    }

    void Execute() {
        StartReadingTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        Response = std::make_unique<TEvKqp::TEvQueryResponse>();
        auto& queryResponse = *Response->Record.MutableResponse();
        queryResponse.SetSessionId(SessionId);
        auto& resultSets = *queryResponse.AddYdbResults();
        queryResponse.MutableTxMeta()->set_id(UserTxId.GetValue().GetHumanStr());
        for (size_t i = 0; i < FastQuery->ColumnsToSelect.size(); ++i) {
            const auto& name = FastQuery->ColumnsToSelect[i];
            auto it = FindCaseI(FastQuery->ResolvedColumns, name);
            auto& column = *resultSets.Addcolumns();
            column.set_name(ToUpper(name)); // XXX

            // XXX XXX XXX (only some of the types)
            switch (it->second.PType.GetTypeId()) {
            case NScheme::NTypeIds::Int32:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::INT32);
                break;
            case NScheme::NTypeIds::Uint32:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UINT32);
                break;
            case NScheme::NTypeIds::Int64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::INT64);
                break;
            case NScheme::NTypeIds::Uint64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UINT64);
                break;
            case NScheme::NTypeIds::Double:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::DOUBLE);
                break;
            case NScheme::NTypeIds::Float:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::FLOAT);
                break;
            case NScheme::NTypeIds::Utf8:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::UTF8);
                break;
            case NScheme::NTypeIds::Timestamp:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::TIMESTAMP);
                break;
            case NScheme::NTypeIds::Timestamp64:
                column.mutable_type()->mutable_optional_type()->mutable_item()->set_type_id(Ydb::Type::TIMESTAMP64);
                break;
            default:
                ;
            }
        }

        TRequestedKeysSet requested;
        for (size_t i = 0; i < FastQuery->WhereSelectInPos.size(); ++i) {
            auto& resolvedData = ResolvedPoints[i];

            // IN (1, 2, 1) might have duplicated which we must eliminate
            if (!requested.insert(resolvedData.KeyInts).second) {
                continue;
            }

            auto request = std::make_unique<NKikimr::TEvDataShard::TEvRead>();
            auto& record = request->Record;
            record.SetReadId(i + 1);
            record.SetLockTxId(LockId);
            record.SetResultFormat(NKikimrDataEvents::FORMAT_CELLVEC);
            record.MutableTableId()->SetOwnerId(FastQuery->TableId.PathId.OwnerId);
            record.MutableTableId()->SetTableId(FastQuery->TableId.PathId.LocalPathId);
            record.MutableTableId()->SetSchemaVersion(FastQuery->TableId.SchemaVersion);

            // TODO
            Y_UNUSED(UserTxId);

            for (const auto& name: FastQuery->ColumnsToSelect) {
                auto it = FindCaseI(FastQuery->ResolvedColumns, name);
                if (it == FastQuery->ResolvedColumns.end()) {
                    TStringStream ss;
                    ss << "Column " << name << " not found";
                    return ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, ss.Str());
                }
                record.AddColumns(it->second.Id);
            }

            request->Keys.emplace_back(resolvedData.KeyCells);

            TRowVersion readVersion(Snapshot.Step, Snapshot.TxId);
            readVersion.ToProto(*record.MutableSnapshot());

            LOG_T("sending read request to " << FastQuery->TableName << "(" << resolvedData.ResolvedShardId << "): "
                << request->ToString());

            Send<ESendingType::Tail>(
                NKikimr::MakePipePerNodeCacheID(false),
                new TEvPipeCache::TEvForward(request.release(), resolvedData.ResolvedShardId, true),
                IEventHandle::FlagTrackDelivery,
                0,
                0);

            ++WaitingRepliesCount;
        }
    }

    void HandleRead(TEvDataShard::TEvReadResult::TPtr ev, const TActorContext &) {
        --WaitingRepliesCount;

        const auto* msg = ev->Get();
        LOG_T("received read result from shard " << ev->Sender << " with " << msg->GetRowsCount() << " rows: "
            << msg->ToString()
            << ", left results " << WaitingRepliesCount);

        for (const auto& lock: msg->Record.GetTxLocks()) {
            Response->Locks.emplace_back(lock);
        }
        Response->LockHandle = std::move(LockHandle);

        auto& queryResponse = *Response->Record.MutableResponse();
        auto& resultSets = *queryResponse.MutableYdbResults(0);

        for (size_t rowNum = 0; rowNum < msg->GetRowsCount(); ++rowNum) {
            auto& row = *resultSets.Addrows();
            const auto& resultRow = msg->GetCells(rowNum);
            LOG_T("adding result row of size " << resultRow.size() << ", requested cols size "
                << FastQuery->ColumnsToSelect.size());
            for (size_t i = 0; i < FastQuery->ColumnsToSelect.size(); ++i) {
                const auto& name = FastQuery->ColumnsToSelect[i];
                auto it = FindCaseI(FastQuery->ResolvedColumns, name);

                const auto& cell = resultRow[i];

                // XXX: normally should not happen IIRC
                if (cell.Size() == 0) {
                    *row.add_items() = it->second.DefaultFromLiteral.value();
                    continue;
                }

                // XXX XXX XXX (only some of the types)
                switch (it->second.PType.GetTypeId()) {
                case NScheme::NTypeIds::Int32: {
                    LOG_T("Value of " << name << " is " << cell.AsValue<i32>());
                    row.add_items()->set_int32_value(cell.AsValue<i32>());
                    break;
                }
                case NScheme::NTypeIds::Uint32:
                    row.add_items()->set_uint32_value(cell.AsValue<ui32>());
                    break;
                case NScheme::NTypeIds::Timestamp64:
                case NScheme::NTypeIds::Int64:
                    row.add_items()->set_int64_value(cell.AsValue<i64>());
                    break;
                case NScheme::NTypeIds::Timestamp:
                case NScheme::NTypeIds::Uint64:
                    row.add_items()->set_uint64_value(cell.AsValue<ui64>());
                    break;
                case NScheme::NTypeIds::Double:
                    row.add_items()->set_double_value(cell.AsValue<double>());
                    break;
                case NScheme::NTypeIds::Float:
                    row.add_items()->set_float_value(cell.AsValue<float>());
                    break;
                case NScheme::NTypeIds::Utf8:
                    row.add_items()->set_text_value(""); // XXX
                    break;
                default:
                    *row.add_items() = it->second.DefaultFromLiteral.value();
                }
            }
        }

        if (WaitingRepliesCount == 0) {
            Response->Record.SetYdbStatus(Ydb::StatusIds::SUCCESS);
            LOG_T("Replying: " << Response->ToString());

            Send<ESendingType::Tail>(ReplyTo, Response.release());

            StopReadingTs = TlsActivationContext->AsActorContext().Monotonic().Now();
            ReportMetrics();
            PassAway();
        }
    }

    void Handle(TEvents::TEvUndelivered::TPtr&) {
        // assume, that it's from DS, because other messages are to local services
        Execute();
    }

    void UnexpectedEvent(const TString&, TAutoPtr<NActors::IEventHandle>& ev) {
        InternalError(TStringBuilder() << "TFastQueryExecutorActor received unexpected event "
            << ev->GetTypeName() << Sprintf("(0x%08" PRIx32 ")", ev->GetTypeRewrite())
            << " sender: " << ev->Sender);
    }

    void InternalError(const TString& message) {
        LOG_E("Internal error, message: " << message);
        ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, message);
    }

    void ReplyQueryError(Ydb::StatusIds::StatusCode ydbStatus,
        const TString& message,
        std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
    {
        LOG_W(" error:" << message);

        auto queryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        queryResponse->Record.SetYdbStatus(ydbStatus);

        if (issues) {
            auto* response = queryResponse->Record.MutableResponse();
            for (auto& i : *issues) {
                response->AddQueryIssues()->Swap(&i);
            }
        }

        Send<ESendingType::Tail>(ReplyTo, queryResponse.release());
        PassAway();
    }

    TString LogPrefix() const {
        TStringBuilder result = TStringBuilder()
            << "Fast executor SessionId: " << SessionId << ", "
            << "ActorId: " << SelfId() << ", ";
        return result;
    }

    void ReportMetrics() {
        if (!Counters) {
            return;
        }

        auto totalTime = StopReadingTs - StartedTs;
        auto timeToAllocateTx = TxAllocatedTs - StartedTs;
        auto timeToResolveScheme = StopResolveSchemeTs - StartResolveSchemeTs;
        auto timeToResolveShards = StopResolveShardsTs - StartResolveShardsTs;
        auto timeToRead = StopReadingTs - StartReadingTs;

        Counters->ReportFastSelectIn(
            totalTime.MicroSeconds(),
            timeToAllocateTx.MicroSeconds(),
            timeToResolveScheme.MicroSeconds(),
            timeToResolveShards.MicroSeconds(),
            timeToRead.MicroSeconds()
        );
    }

private:
    TIntrusivePtr<TKqpCounters> Counters;
    TActorId ReplyTo;
    TString SessionId;
    TFastQueryPtr FastQuery;
    ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue> Parameters;
    IKqpGateway::TKqpSnapshot Snapshot;
    TKqpQueryState::TQueryTxId UserTxId;
    ui64 LockId;

    ui64 TxId = 0;
    NLongTxService::TLockHandle LockHandle;

    struct TResolvedData {
        TVector<TCell> KeyCells;
        TVector<int32_t> KeyInts;
        std::unique_ptr<NKikimr::TKeyDesc> ResolvedKeys;
        ui64 ResolvedShardId = 0;
    };
    TVector<TResolvedData> ResolvedPoints;

    size_t WaitingRepliesCount = 0;
    std::unique_ptr<TEvKqp::TEvQueryResponse> Response;

    TMonotonic StartedTs;
    TMonotonic TxAllocatedTs;
    TMonotonic StartResolveSchemeTs;
    TMonotonic StopResolveSchemeTs;
    TMonotonic StartResolveShardsTs;
    TMonotonic StopResolveShardsTs;
    TMonotonic StartReadingTs;
    TMonotonic StopReadingTs;
};

class TFastUpsertActor : public TActor<TFastUpsertActor> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_FAST_QUERY_ACTOR;
    }

public:
    TFastUpsertActor(TIntrusivePtr<TKqpCounters>& counters,
            const TActorId& replyTo,
            const TString& sessionId,
            TFastQueryPtr fastQuery,
            const ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue>& params,
            const IKqpGateway::TKqpSnapshot& snapshot,
            const TKqpQueryState::TQueryTxId& userTxId,
            ui64 lockId)
        : TActor(&TThis::ExecuteState)
        , Counters(counters)
        , ReplyTo(replyTo)
        , SessionId(sessionId)
        , FastQuery(std::move(fastQuery))
        , Parameters(params)
        , Snapshot(snapshot)
        , UserTxId(userTxId)
        , LockId(lockId)
        , StartedTs(TlsActivationContext->AsActorContext().Monotonic().Now())
    {
    }

    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvKqpExecuter::TEvTxRequest, Handle);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle);
                hFunc(TEvents::TEvUndelivered, Handle);
                HFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
                HFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, Handle);
                HFunc(NKikimr::NEvents::TDataEvents::TEvWriteResult, HandleWrite);
            default:
                UnexpectedEvent("ExecuteState", ev);
            }
        } catch (const TRequestFail& ex) {
            ReplyQueryError(ex.Status, ex.what(), ex.Issues);
        } catch (const yexception& ex) {
            InternalError(ex.what());
        }
    }

private:
    void Handle(TEvKqpExecuter::TEvTxRequest::TPtr& ev) {
        OnTxId(ev->Get()->Record.GetRequest().GetTxId());
    }

    void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
        OnTxId(ev->Get()->TxId);
    }

    void OnTxId(ui64 txId) {
        TxAllocatedTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        LOG_T("allocated txId " << txId << " for query " << FastQuery->PostgresQuery.Query);
        TxId = txId;
        if (LockId == 0) {
            LockId = TxId;
            LockHandle = NLongTxService::TLockHandle(TxId, TActivationContext::ActorSystem());
        }

        if (FastQuery->ResolvedColumns.empty()) {
            return ResolveSchema(TlsActivationContext->AsActorContext());
        }

        ResolveShards(TlsActivationContext->AsActorContext());
    }

    void ResolveSchema(const TActorContext &) {
        StartResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TString path = FastQuery->Database + "/" + FastQuery->TableName;
        LOG_T("Resolving " << path);

        auto request = std::make_unique<NSchemeCache::TSchemeCacheNavigate>();
        NSchemeCache::TSchemeCacheNavigate::TEntry entry;
        entry.Path = ::NKikimr::SplitPath(path);
        if (entry.Path.empty()) {
            return ReplyQueryError(Ydb::StatusIds::NOT_FOUND, "Invalid table path specified");
        }
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
        request->ResultSet.emplace_back(entry);
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release()));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev, const TActorContext &ctx) {
        const auto* response = ev->Get();
        Y_ABORT_UNLESS(response->Request != nullptr);

        Y_ABORT_UNLESS(response->Request->ResultSet.size() == 1);
        const auto& entry = response->Request->ResultSet.front();

        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            return ReplyQueryError(Ydb::StatusIds::SCHEME_ERROR, ToString(entry.Status));
        }

        // quick and dirty as everything here
        if (FastQuery->NeedToResolveIndex) {

            FastQuery->ResolvedIndex = TFastQuery::TResolvedIndex();

            // FIXME: +1 hack to get impl table
            FastQuery->ResolvedIndex->TableId = entry.TableId;
            FastQuery->ResolvedIndex->ColumnTypes.reserve(entry.Columns.size());
            FastQuery->ResolvedIndex->Columns.reserve(entry.Columns.size());

            for (const auto& [_, info] : entry.Columns) {
                if (info.KeyOrder != -1) {
                    FastQuery->ResolvedIndex->Columns.emplace_back(info.Id, TKeyDesc::EColumnOperation::Read, info.PType, 0, 0);
                    FastQuery->ResolvedIndex->ColumnTypes.emplace_back(info.PType);
                    for (size_t i = 0; i < FastQuery->UpsertParams.size(); ++i) {
                        if (IsEqual(info.Name, FastQuery->UpsertParams[i].ColumnName)) {
                            FastQuery->ResolvedIndex->OrderedColumnParams.push_back(i);
                        }
                    }
                } else {
                    return ReplyQueryError(
                        Ydb::StatusIds::BAD_REQUEST,
                        TStringBuilder() << "Covering indexes are not supported, column: " << info.Name);
                }
            }

            if (FastQuery->ResolvedIndex->OrderedColumnParams.size() != FastQuery->ResolvedIndex->Columns.size()) {
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Upsert doesn't have all needed columns");
            }

            LOG_T("Found index for table " << FastQuery->TableName
                << ", tableId: " << FastQuery->ResolvedIndex->TableId
                << ", index columns size: " << FastQuery->ResolvedIndex->Columns.size());

            StopResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
            ResolveShards(ctx);
            return;
        }

        FastQuery->TableId = entry.TableId;
        FastQuery->ResolvedColumns.reserve(entry.Columns.size());
        FastQuery->Columns.reserve(entry.Columns.size());
        FastQuery->KeyColumns.reserve(entry.Columns.size());
        FastQuery->KeyColumnTypes.reserve(entry.Columns.size());
        FastQuery->OrderedKeyParams.reserve(entry.Columns.size());

        TVector<size_t> nonKeyColumnsOrder;
        nonKeyColumnsOrder.reserve(entry.Columns.size());
        for (const auto& [_, info] : entry.Columns) {
            FastQuery->ResolvedColumns[info.Name] = info;
            FastQuery->Columns.emplace_back(info.Id, TKeyDesc::EColumnOperation::Read, info.PType, 0, 0);
            if (info.KeyOrder != -1) {
                FastQuery->KeyColumns.emplace_back(info);
                FastQuery->KeyColumnTypes.emplace_back(info.PType);
                for (size_t i = 0; i < FastQuery->UpsertParams.size(); ++i) {
                    if (IsEqual(info.Name, FastQuery->UpsertParams[i].ColumnName)) {
                        FastQuery->OrderedKeyParams.push_back(i);
                    }
                }
            } else {
                for (size_t i = 0; i < FastQuery->UpsertParams.size(); ++i) {
                    if (IsEqual(info.Name, FastQuery->UpsertParams[i].ColumnName)) {
                        nonKeyColumnsOrder.push_back(i);
                    }
                }
            }
        }

        FastQuery->AllOrderedParams = FastQuery->OrderedKeyParams;
        FastQuery->AllOrderedParams.insert(
            FastQuery->AllOrderedParams.end(),
            nonKeyColumnsOrder.begin(),
            nonKeyColumnsOrder.end());

        // handle index data if any

        if (entry.Indexes.size() > 1) {
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Table has more than one index");
        }

        LOG_T("Resolved to tableId " << FastQuery->TableId << " with key columns size " << FastQuery->KeyColumns.size()
            << " and total columns size " << FastQuery->ResolvedColumns.size()
            << ", OrderedKeyParams size " << FastQuery->OrderedKeyParams.size()
            << ", AllOrderedParams size " << FastQuery->AllOrderedParams.size());

        if (entry.Indexes.size() == 1) {
            FastQuery->NeedToResolveIndex = true;

            const auto& index = entry.Indexes[0];

            TString path = FastQuery->Database + "/" + FastQuery->TableName + "/" + index.GetName() + "/indexImplTable";
            LOG_T("Resolving index " << path);

            auto request = std::make_unique<NSchemeCache::TSchemeCacheNavigate>();
            NSchemeCache::TSchemeCacheNavigate::TEntry indexEntry;

            // XXX: this +1 is OK for this quick proto
            //indexEntry.TableId =
            //    TTableId(index.GetPathOwnerId(), index.GetLocalPathId() + 1, index.GetSchemaVersion());
            //indexEntry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
            indexEntry.Path = ::NKikimr::SplitPath(path);
            if (indexEntry.Path.empty()) {
                return ReplyQueryError(Ydb::StatusIds::NOT_FOUND, "Invalid index table path specified");
            }
            indexEntry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
            indexEntry.ShowPrivatePath = true;
            request->ResultSet.emplace_back(indexEntry);
            Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release()));
        } else {
            StopResolveSchemeTs = TlsActivationContext->AsActorContext().Monotonic().Now();
            ResolveShards(ctx);
        }
    }

    void ResolveShards(const TActorContext &) {
        StartResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        auto it = FindCaseI(Parameters, "$batch");
        if (it == Parameters.end()) {
            TStringStream ss;
            ss << "Parameter $batch not found, params are: ";
            for (const auto& param: Parameters) {
                ss << param.first << ",";
            }
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, ss.Str());
        }

        const auto& batchParam = it->second;
        const auto& batchType = batchParam.Gettype().Getlist_type().Getitem().Getstruct_type();
        const auto& batchValue = batchParam.Getvalue();
        if (batchValue.items_size() == 0) {
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "empty batch");
        }

        LOG_T("l " << batchValue.items_size() << ": " << batchParam.AsJSON());

        ResolvedPoints.resize(FastQuery->ResolvedIndex ? batchValue.items_size() * 2 : batchValue.items_size());

        auto request = std::make_unique<NSchemeCache::TSchemeCacheRequest>();

        for (int i = 0; i < batchValue.items_size(); ++i) {
            auto& resolvedData = ResolvedPoints[i];
            resolvedData.KeyCells.reserve(FastQuery->OrderedKeyParams.size());
            const auto& listItems = batchValue.Getitems(i);
            for (size_t j = 0; j < FastQuery->OrderedKeyParams.size(); ++j) {
                size_t itemIndex = FastQuery->OrderedKeyParams[j];
                const auto& item = listItems.Getitems(itemIndex);
                const auto& listType = batchType.Getmembers(itemIndex).Gettype();

                ::Ydb::Type::PrimitiveTypeId itemType = Ydb::Type::INT32;
                if (listType.has_optional_type()) {
                    itemType = listType.Getoptional_type().Getitem().Gettype_id();
                } else {
                    itemType = listType.Gettype_id();
                }

                if (itemType == Ydb::Type::INT64) {
                    LOG_T("Request key column " << j << " mapped to int64 item " << itemIndex
                        << "  with value " << item.Getint64_value() << ": " << listType.AsJSON());
                    resolvedData.KeyCells.emplace_back(TCell::Make<i64>(item.Getint64_value()));
                } else if (itemType == Ydb::Type::UTF8) {
                    LOG_T("Request key column " << j << " mapped to UTF8 item " << itemIndex
                        << "  with value " << item.Gettext_value() << ": " << listType.AsJSON());
                    resolvedData.KeyCells.emplace_back(TCell(item.Gettext_value().data(), item.Gettext_value().size()));
                } else {
                    // XXX: we don't check all possible types
                    LOG_T("Request key column " << j << " mapped to item " << itemIndex
                        << "  with value " << item.Getint32_value()  << ": " << listType.AsJSON());
                    resolvedData.KeyCells.emplace_back(TCell::Make<i32>(item.Getint32_value()));
                }
            }

            TTableRange range(resolvedData.KeyCells);

            // XXX: for simplicity request all columns
            THolder<NKikimr::TKeyDesc> keyDesc(
                new TKeyDesc(
                    FastQuery->TableId,
                    range,
                    TKeyDesc::ERowOperation::Read,
                    FastQuery->KeyColumnTypes,
                    FastQuery->Columns));

            request->ResultSet.emplace_back(std::move(keyDesc));
        }

        if (FastQuery->ResolvedIndex) {
            // the same as above, but for index (copy-pasted and modified)
            for (int i = 0; i < batchValue.items_size(); ++i) {
                auto& resolvedData = ResolvedPoints[i + batchValue.items_size()];
                resolvedData.KeyCells.reserve(FastQuery->ResolvedIndex->OrderedColumnParams.size());
                const auto& listItems = batchValue.Getitems(i);
                for (size_t j = 0; j < FastQuery->ResolvedIndex->OrderedColumnParams.size(); ++j) {
                    size_t itemIndex = FastQuery->ResolvedIndex->OrderedColumnParams[j];
                    const auto& item = listItems.Getitems(itemIndex);
                    const auto& listType = batchType.Getmembers(itemIndex).Gettype();

                    ::Ydb::Type::PrimitiveTypeId itemType = Ydb::Type::INT32;
                    if (listType.has_optional_type()) {
                        itemType = listType.Getoptional_type().Getitem().Gettype_id();
                    } else {
                        itemType = listType.Gettype_id();
                    }

                    if (itemType == Ydb::Type::INT64) {
                        LOG_T("Request index key column " << j << " mapped to int64 item " << itemIndex
                            << "  with value " << item.Getint64_value() << ": " << listType.AsJSON());
                        resolvedData.KeyCells.emplace_back(TCell::Make<i64>(item.Getint64_value()));
                    } else if (itemType == Ydb::Type::UTF8) {
                        LOG_T("Request index key column " << j << " mapped to UTF8 item " << itemIndex
                            << "  with value " << item.Gettext_value() << ": " << listType.AsJSON());
                        resolvedData.KeyCells.emplace_back(TCell(item.Gettext_value().data(), item.Gettext_value().size()));
                    } else {
                        // XXX: we don't check all possible types
                        LOG_T("Request index key column " << j << " mapped to item " << itemIndex
                            << "  with value " << item.Getint32_value()  << ": " << listType.AsJSON());
                        resolvedData.KeyCells.emplace_back(TCell::Make<i32>(item.Getint32_value()));
                    }
                }

                TTableRange range(resolvedData.KeyCells);

                THolder<NKikimr::TKeyDesc> keyDesc(
                    new TKeyDesc(
                        FastQuery->ResolvedIndex->TableId,
                        range,
                        TKeyDesc::ERowOperation::Read,
                        FastQuery->ResolvedIndex->ColumnTypes,
                        FastQuery->ResolvedIndex->Columns));

                request->ResultSet.emplace_back(std::move(keyDesc));
            }
        }

        auto resolveRequest = std::make_unique<TEvTxProxySchemeCache::TEvResolveKeySet>(request.release());
        Send(MakeSchemeCacheID(), resolveRequest.release());
    }

    void Handle(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr &ev, const TActorContext &) {
        StopResolveShardsTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        TEvTxProxySchemeCache::TEvResolveKeySetResult *msg = ev->Get();

        for (size_t i = 0; i < msg->Request->ResultSet.size(); ++i) {
            auto& resolvedData = ResolvedPoints[i];
            auto& result = msg->Request->ResultSet[i];
            resolvedData.ResolvedKeys.reset(result.KeyDescription.Release());

            const auto& partitions = resolvedData.ResolvedKeys->GetPartitions();
            if (partitions.size() == 0) {
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Failed to resolve shards");
            }
            // just a sanity check
            if (partitions.size() != 1) {
                return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "Key resolved to multiple shards");
            }

            resolvedData.ResolvedShardId = partitions[0].ShardId;

            LOG_T("Table [" << FastQuery->TableId << "] point" << i << " shard: " << resolvedData.ResolvedShardId);
        }

        Execute();
    }

    void Execute() {
        StartWritingTs = TlsActivationContext->AsActorContext().Monotonic().Now();
        Response = std::make_unique<TEvKqp::TEvQueryResponse>();
        auto& queryResponse = *Response->Record.MutableResponse();
        queryResponse.SetSessionId(SessionId);

        auto it = FindCaseI(Parameters, "$batch");
        const auto& batchParam = it->second;
        const auto& batchValue = batchParam.Getvalue();
        if (batchValue.items_size() == 0) {
            return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, "empty batch");
        }

        {
            // we suppose that here the order of items is the same as
            // UpsertParams and corresponding ColumnsToUpsert
            std::vector<ui32> columnIds;
            columnIds.reserve(FastQuery->ColumnsToUpsert.size());
            for (auto paramIndex: FastQuery->AllOrderedParams) {
                const auto& columnName = FastQuery->ColumnsToUpsert[paramIndex];
                auto it = FastQuery->ResolvedColumns.find(columnName);
                if (it == FastQuery->ResolvedColumns.end()) {
                    TStringStream ss;
                    ss << "Failed to resolve column '" << columnName << "'";
                    return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, ss.Str());
                }
                const auto& info = it->second;
                LOG_T("Added column " << info.Id);
                columnIds.emplace_back(info.Id);
            }

            // naive impl: we don't gather rows into batches
            // also we suppose that optional fields are at the end, TODO
            for (int i = 0; i < batchValue.items_size(); ++i) {
                TVector<TCell> cells;
                const auto& listItems = batchValue.Getitems(i);
                for (auto paramIndex: FastQuery->AllOrderedParams) {
                    const auto& columnName = FastQuery->ColumnsToUpsert[paramIndex];
                    const auto& item = listItems.Getitems(paramIndex);
                    const auto& colInfo = FastQuery->ResolvedColumns[columnName];
                    switch (colInfo.PType.GetTypeId()) {
                    case NYql::NProto::Int32:
                        cells.emplace_back(TCell::Make<i32>(item.Getint32_value()));
                        break;
                    case NYql::NProto::Uint32:
                        cells.emplace_back(TCell::Make<ui32>(item.Getuint32_value()));
                        break;
                    case NYql::NProto::Timestamp:
                    case NYql::NProto::Int64:
                    case NYql::NProto::Timestamp64:
                        cells.emplace_back(TCell::Make<i64>(item.Getint64_value()));
                        break;
                    case NYql::NProto::Uint64:
                        cells.emplace_back(TCell::Make<ui64>(item.Getuint64_value()));
                        break;
                    case NYql::NProto::Double:
                        cells.emplace_back(TCell::Make<double>(item.Getdouble_value()));
                        break;
                    case NYql::NProto::Float:
                        cells.emplace_back(TCell::Make<double>(item.Getfloat_value()));
                        break;
                    case NYql::NProto::Utf8:
                        cells.emplace_back(TCell::Make(item.Gettext_value()));
                        break;
                    default: {
                        TStringStream ss;
                        ss << "unknown type " << colInfo.PType.GetTypeId() << " of column '"
                            << FastQuery->ColumnsToUpsert[paramIndex] << "'";
                        LOG_T(ss.Str());
                        return ReplyQueryError(Ydb::StatusIds::UNSUPPORTED, ss.Str());
                    }
                    }
                }

                TSerializedCellMatrix matrix(cells, 1, cells.size());
                auto evWrite = std::make_unique<NKikimr::NEvents::TDataEvents::TEvWrite>(
                    TxId, NKikimrDataEvents::TEvWrite::MODE_IMMEDIATE);
                evWrite->SetLockId(LockId, SelfId().NodeId());
                ui64 payloadIndex = NKikimr::NEvWrite::TPayloadWriter<NKikimr::NEvents::TDataEvents::TEvWrite>(*evWrite)
                    .AddDataToPayload(matrix.ReleaseBuffer());
                evWrite->AddOperation(
                    NKikimrDataEvents::TEvWrite::TOperation::OPERATION_UPSERT,
                    FastQuery->TableId,
                    columnIds,
                    payloadIndex,
                    NKikimrDataEvents::FORMAT_CELLVEC);

                const auto& resolvedData = ResolvedPoints[i];
                LOG_T("Sending write to " << resolvedData.ResolvedShardId);
                Send<ESendingType::Tail>(
                    NKikimr::MakePipePerNodeCacheID(false),
                    new TEvPipeCache::TEvForward(evWrite.release(), resolvedData.ResolvedShardId, true),
                    IEventHandle::FlagTrackDelivery,
                    0,
                    0);
                ++WaitingRepliesCount;
            }
        }

        // naive impl: we don't gather rows into batches
        // also we suppose that optional fields are at the end, TODO
        if (FastQuery->ResolvedIndex) {
            // we suppose that here the order of items is the same as
            // UpsertParams and corresponding ColumnsToUpsert
            std::vector<ui32> indexColumnIds;
            indexColumnIds.reserve(FastQuery->ResolvedIndex->Columns.size());
            for (size_t i = 0; i < FastQuery->ResolvedIndex->OrderedColumnParams.size(); ++i) {
                size_t paramIndex = FastQuery->ResolvedIndex->OrderedColumnParams[i];
                const auto& columnName = FastQuery->ColumnsToUpsert[paramIndex];
                auto it = FastQuery->ResolvedColumns.find(columnName);
                if (it == FastQuery->ResolvedColumns.end()) {
                    TStringStream ss;
                    ss << "Failed to resolve index column '" << columnName << "'";
                    return ReplyQueryError(Ydb::StatusIds::BAD_REQUEST, ss.Str());
                }
                LOG_T("Added column " << FastQuery->ResolvedIndex->Columns[i].Column);

                // note, that IDs differ between table being indexed and index table impl
                indexColumnIds.emplace_back(FastQuery->ResolvedIndex->Columns[i].Column);
            }

            for (int i = 0; i < batchValue.items_size(); ++i) {
                TVector<TCell> cells;
                const auto& listItems = batchValue.Getitems(i);
                for (size_t j = 0; j < FastQuery->ResolvedIndex->OrderedColumnParams.size(); ++j) {
                    size_t paramIndex = FastQuery->ResolvedIndex->OrderedColumnParams[j];
                    const auto& colInfo =  FastQuery->ResolvedIndex->ColumnTypes[j];
                    const auto& item = listItems.Getitems(paramIndex);
                    switch (colInfo.GetTypeId()) {
                    case NYql::NProto::Int32:
                        cells.emplace_back(TCell::Make<i32>(item.Getint32_value()));
                        break;
                    case NYql::NProto::Uint32:
                        cells.emplace_back(TCell::Make<ui32>(item.Getuint32_value()));
                        break;
                    case NYql::NProto::Timestamp:
                    case NYql::NProto::Int64:
                    case NYql::NProto::Timestamp64:
                        cells.emplace_back(TCell::Make<i64>(item.Getint64_value()));
                        break;
                    case NYql::NProto::Uint64:
                        cells.emplace_back(TCell::Make<ui64>(item.Getuint64_value()));
                        break;
                    case NYql::NProto::Double:
                        cells.emplace_back(TCell::Make<double>(item.Getdouble_value()));
                        break;
                    case NYql::NProto::Float:
                        cells.emplace_back(TCell::Make<double>(item.Getfloat_value()));
                        break;
                    case NYql::NProto::Utf8:
                        cells.emplace_back(TCell::Make(item.Gettext_value()));
                        break;
                    default: {
                        TStringStream ss;
                        ss << "unknown type " << colInfo.GetTypeId() << " of column '"
                            << FastQuery->ColumnsToUpsert[paramIndex] << "'";
                        LOG_T(ss.Str());
                        return ReplyQueryError(Ydb::StatusIds::UNSUPPORTED, ss.Str());
                    }
                    }
                }

                TSerializedCellMatrix matrix(cells, 1, cells.size());
                auto evWrite = std::make_unique<NKikimr::NEvents::TDataEvents::TEvWrite>(
                    TxId, NKikimrDataEvents::TEvWrite::MODE_IMMEDIATE);
                evWrite->SetLockId(LockId, SelfId().NodeId());
                ui64 payloadIndex = NKikimr::NEvWrite::TPayloadWriter<NKikimr::NEvents::TDataEvents::TEvWrite>(*evWrite)
                    .AddDataToPayload(matrix.ReleaseBuffer());
                evWrite->AddOperation(
                    NKikimrDataEvents::TEvWrite::TOperation::OPERATION_UPSERT,
                    FastQuery->ResolvedIndex->TableId,
                    indexColumnIds,
                    payloadIndex,
                    NKikimrDataEvents::FORMAT_CELLVEC);

                const auto& resolvedData = ResolvedPoints[i + batchValue.items_size()];
                LOG_T("Sending write to index shard " << resolvedData.ResolvedShardId);
                Send<ESendingType::Tail>(
                    NKikimr::MakePipePerNodeCacheID(false),
                    new TEvPipeCache::TEvForward(evWrite.release(), resolvedData.ResolvedShardId, true),
                    IEventHandle::FlagTrackDelivery,
                    0,
                    0);
                ++WaitingRepliesCount;
            }
        }
    }

    void HandleWrite(NKikimr::NEvents::TDataEvents::TEvWriteResult::TPtr ev, const TActorContext &ctx) {
        Y_UNUSED(ev);
        Y_UNUSED(ctx);
        --WaitingRepliesCount;

        const auto* msg = ev->Get();
        if (msg->Record.GetStatus() != NKikimrDataEvents::TEvWriteResult::STATUS_COMPLETED) {
            LOG_T("received failed write result from shard " << ev->Sender << ": "
                << msg->ToString()
                << ", left results " << WaitingRepliesCount);
            return ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, "TEvWrite failed");
        }

        LOG_T("received successfull write result from shard " << ev->Sender << ": "
            << msg->ToString()
            << ", left results " << WaitingRepliesCount);

        for (const auto& lock: msg->Record.GetTxLocks()) {
            Response->Locks.emplace_back(lock);
        }

        Response->LockHandle = std::move(LockHandle);

        if (WaitingRepliesCount == 0) {
            Response->Record.SetYdbStatus(Ydb::StatusIds::SUCCESS);
            auto& queryResponse = *Response->Record.MutableResponse();
            queryResponse.SetSessionId(SessionId);
            queryResponse.MutableTxMeta()->set_id(UserTxId.GetValue().GetHumanStr());

            LOG_T("Replying: " << Response->ToString());

            Send<ESendingType::Tail>(ReplyTo, Response.release());
            StopWritingTs = TlsActivationContext->AsActorContext().Monotonic().Now();
            ReportMetrics();
            PassAway();
        }
    }

    void Handle(TEvents::TEvUndelivered::TPtr&) {
        // assume, that it's from DS, because other messages are to local services
        Execute();
    }

    void UnexpectedEvent(const TString&, TAutoPtr<NActors::IEventHandle>& ev) {
        InternalError(TStringBuilder() << "TFastQueryExecutorActor received unexpected event "
            << ev->GetTypeName() << Sprintf("(0x%08" PRIx32 ")", ev->GetTypeRewrite())
            << " sender: " << ev->Sender);
    }

    void InternalError(const TString& message) {
        LOG_E("Internal error, message: " << message);
        ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, message);
    }

    void ReplyQueryError(Ydb::StatusIds::StatusCode ydbStatus,
        const TString& message,
        std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
    {
        LOG_W(" error:" << message);

        auto response = std::make_unique<TEvKqp::TEvQueryResponse>();
        response->Record.SetYdbStatus(ydbStatus);

        auto& queryResponse = *response->Record.MutableResponse();
        queryResponse.SetSessionId(SessionId);
        queryResponse.MutableTxMeta()->set_id(UserTxId.GetValue().GetHumanStr());

        if (issues) {
            for (auto& i : *issues) {
                queryResponse.AddQueryIssues()->Swap(&i);
            }
        }

        Send<ESendingType::Tail>(ReplyTo, response.release());
        PassAway();
    }

    TString LogPrefix() const {
        TString result = TStringBuilder()
            << "Fast executor SessionId: " << SessionId << ", "
            << "ActorId: " << SelfId() << ", ";
        return result;
    }

    void ReportMetrics() {
        if (!Counters) {
            return;
        }

        auto totalTime = StopWritingTs - StartedTs;
        auto timeToAllocateTx = TxAllocatedTs - StartedTs;
        auto timeToResolveScheme = StopResolveSchemeTs - StartResolveSchemeTs;
        auto timeToResolveShards = StopResolveShardsTs - StartResolveShardsTs;
        auto timeToRead = StopWritingTs - StartWritingTs;

        Counters->ReportFastUpsert(
            totalTime.MicroSeconds(),
            timeToAllocateTx.MicroSeconds(),
            timeToResolveScheme.MicroSeconds(),
            timeToResolveShards.MicroSeconds(),
            timeToRead.MicroSeconds()
        );
    }

private:
    TIntrusivePtr<TKqpCounters> Counters;
    TActorId ReplyTo;
    TString SessionId;
    TFastQueryPtr FastQuery;
    ::google::protobuf::Map<TProtoStringType, ::Ydb::TypedValue> Parameters;
    IKqpGateway::TKqpSnapshot Snapshot;
    TKqpQueryState::TQueryTxId UserTxId;
    ui64 LockId;

    ui64 TxId = 0;
    NLongTxService::TLockHandle LockHandle;

    // both requested column data and index data
    struct TResolvedData {
        TVector<TCell> KeyCells;
        TVector<int32_t> KeyInts;
        std::unique_ptr<NKikimr::TKeyDesc> ResolvedKeys;
        ui64 ResolvedShardId = 0;
    };
    TVector<TResolvedData> ResolvedPoints;

    size_t WaitingRepliesCount = 0;
    std::unique_ptr<TEvKqp::TEvQueryResponse> Response;

    TMonotonic StartedTs;
    TMonotonic TxAllocatedTs;
    TMonotonic StartResolveSchemeTs;
    TMonotonic StopResolveSchemeTs;
    TMonotonic StartResolveShardsTs;
    TMonotonic StopResolveShardsTs;
    TMonotonic StartWritingTs;
    TMonotonic StopWritingTs;
};

class TKqpSessionActor : public TActorBootstrapped<TKqpSessionActor> {

    class TTimerGuard {
    public:
        TTimerGuard(TKqpSessionActor* this_)
        : This(this_)
        {
            if (This->QueryState) {
                YQL_ENSURE(!This->QueryState->CurrentTimer);
                This->QueryState->CurrentTimer.emplace();
            }
        }

        ~TTimerGuard() {
            if (This->QueryState) {
                This->QueryState->ResetTimer();
            }
        }

    private:
        TKqpSessionActor* This;
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_SESSION_ACTOR;
    }

   TKqpSessionActor(const TActorId& owner,
            TKqpQueryCachePtr queryCache,
            std::shared_ptr<NKikimr::NKqp::NRm::IKqpResourceManager> resourceManager,
            std::shared_ptr<NKikimr::NKqp::NComputeActor::IKqpNodeComputeActorFactory> caFactory,
            const TString& sessionId, const TKqpSettings::TConstPtr& kqpSettings,
            const TKqpWorkerSettings& workerSettings,
            std::optional<TKqpFederatedQuerySetup> federatedQuerySetup,
            NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
            TIntrusivePtr<TModuleResolverState> moduleResolverState, TIntrusivePtr<TKqpCounters> counters,
            const NKikimrConfig::TQueryServiceConfig& queryServiceConfig,
            const TActorId& kqpTempTablesAgentActor)
        : Owner(owner)
        , QueryCache(std::move(queryCache))
        , SessionId(sessionId)
        , ResourceManager_(std::move(resourceManager))
        , CaFactory_(std::move(caFactory))
        , Counters(counters)
        , Settings(workerSettings)
        , AsyncIoFactory(std::move(asyncIoFactory))
        , ModuleResolverState(std::move(moduleResolverState))
        , FederatedQuerySetup(federatedQuerySetup)
        , KqpSettings(kqpSettings)
        , Config(CreateConfig(kqpSettings, workerSettings))
        , Transactions(*Config->_KqpMaxActiveTxPerSession.Get(), TDuration::Seconds(*Config->_KqpTxIdleTimeoutSec.Get()))
        , QueryServiceConfig(queryServiceConfig)
        , KqpTempTablesAgentActor(kqpTempTablesAgentActor)
        , GUCSettings(std::make_shared<TGUCSettings>())
    {
        RequestCounters = MakeIntrusive<TKqpRequestCounters>();
        RequestCounters->Counters = Counters;
        RequestCounters->DbCounters = Settings.DbCounters;
        RequestCounters->TxProxyMon = MakeIntrusive<NTxProxy::TTxProxyMon>(AppData()->Counters);
        CompilationCookie = std::make_shared<std::atomic<bool>>(true);

        FillSettings.AllResultsBytesLimit = Nothing();
        FillSettings.RowsLimitPerWrite = Config->_ResultRowsLimit.Get();
        FillSettings.Format = IDataProvider::EResultFormat::Custom;
        FillSettings.FormatDetails = TString(KikimrMkqlProtoFormat);
        FillGUCSettings();

        auto optSessionId = TryDecodeYdbSessionId(SessionId);
        YQL_ENSURE(optSessionId, "Can't decode ydb session Id");

        TempTablesState.SessionId = *optSessionId;
        TempTablesState.Database = Settings.Database;
        LOG_D("Create session actor with id " << TempTablesState.SessionId);
    }

    void Bootstrap() {
        LOG_D("session actor bootstrapped");
        Counters->ReportSessionActorCreated(Settings.DbCounters);
        CreationTime = TInstant::Now();

        Config->FeatureFlags = AppData()->FeatureFlags;

        RequestControls.Reqister(TlsActivationContext->AsActorContext());
        Become(&TKqpSessionActor::ReadyState);
    }

    TString LogPrefix() const {
        TStringBuilder result = TStringBuilder()
            << "SessionId: " << SessionId << ", "
            << "ActorId: " << SelfId() << ", "
            << "ActorState: " << CurrentStateFuncName() << ", ";
        if (Y_LIKELY(QueryState)) {
            result << "TraceId: " << QueryState->UserRequestContext->TraceId << ", ";
        }
        return result;
    }

    void MakeNewQueryState(TEvKqp::TEvQueryRequest::TPtr& ev) {
        ++QueryId;
        YQL_ENSURE(!QueryState);
        auto selfId = SelfId();
        auto as = TActivationContext::ActorSystem();
        ev->Get()->SetClientLostAction(selfId, as);
        QueryState = std::make_shared<TKqpQueryState>(
            ev, QueryId, Settings.Database, Settings.ApplicationName, Settings.Cluster, Settings.DbCounters, Settings.LongSession,
            Settings.TableService, Settings.QueryService, SessionId, AppData()->MonotonicTimeProvider->Now());
        if (QueryState->UserRequestContext->TraceId.empty()) {
            QueryState->UserRequestContext->TraceId = UlidGen.Next().ToString();
        }
    }

    void PassRequestToResourcePool() {
        if (QueryState->UserRequestContext->PoolConfig) {
            LOG_D("request placed into pool from cache: " << QueryState->UserRequestContext->PoolId);
            CompileQuery();
            return;
        }

        Send(MakeKqpWorkloadServiceId(SelfId().NodeId()), new NWorkload::TEvPlaceRequestIntoPool(
            QueryState->UserRequestContext->DatabaseId,
            SessionId,
            QueryState->UserRequestContext->PoolId,
            QueryState->UserToken
        ), IEventHandle::FlagTrackDelivery);

        QueryState->PoolHandlerActor = MakeKqpWorkloadServiceId(SelfId().NodeId());
        Become(&TKqpSessionActor::ExecuteState);
    }

    void ForwardRequest(TEvKqp::TEvQueryRequest::TPtr& ev) {
        if (!WorkerId) {
            std::unique_ptr<IActor> workerActor(CreateKqpWorkerActor(SelfId(), SessionId, KqpSettings, Settings,
                FederatedQuerySetup, ModuleResolverState, Counters, QueryServiceConfig, GUCSettings));
            WorkerId = RegisterWithSameMailbox(workerActor.release());
        }
        TlsActivationContext->Send(new IEventHandle(*WorkerId, SelfId(), QueryState->RequestEv.release(), ev->Flags, ev->Cookie,
                    nullptr, std::move(ev->TraceId)));
        Become(&TKqpSessionActor::ExecuteState);
    }

    void ForwardResponse(TEvKqp::TEvQueryResponse::TPtr& ev) {
        // fast query locks
        NKikimrMiniKQL::TResult lockResult;
        if (!ev->Get()->Locks.empty()) {
            MergeLocksWithFastQuery(ev->Get()->Locks);
            LOG_T("Fast query acquired " << ev->Get()->Locks.size() << " locks");

            if (ev->Get()->LockHandle) {
                QueryState->TxCtx->Locks.LockHandle = std::move(ev->Get()->LockHandle);
                LOG_T("Fast query created lock handle");
            }
        }

        QueryResponse = std::unique_ptr<TEvKqp::TEvQueryResponse>(ev->Release().Release());

        if (FastQueryExecutor) {
            auto *record = &QueryResponse->Record;
            auto *response = record->MutableResponse();

            FillStats(record);

            if (QueryState->TxCtx) {
                QueryState->TxCtx->OnEndQuery();
            }

            if (QueryState->Commit) {
                ResetTxState();
                Transactions.ReleaseTransaction(QueryState->TxId.GetValue());
                QueryState->TxId.Reset();
            }

            FillTxInfo(response);
            FastQueryExecutor = {};
        }

        Cleanup();
    }

    void ReplyTransactionNotFound(const TString& txId) {
        std::vector<TIssue> issues{YqlIssue(TPosition(), TIssuesIds::KIKIMR_TRANSACTION_NOT_FOUND,
            TStringBuilder() << "Transaction not found: " << txId)};
        ReplyQueryError(Ydb::StatusIds::NOT_FOUND, "", MessageFromIssues(issues));
    }

    void RollbackTx() {
        YQL_ENSURE(QueryState->HasTxControl(), "Can't perform ROLLBACK_TX: TxControl isn't set in TQueryRequest");
        const auto& txControl = QueryState->GetTxControl();
        QueryState->Commit = txControl.commit_tx();
        auto txId = TTxId::FromString(txControl.tx_id());
        if (auto ctx = Transactions.ReleaseTransaction(txId)) {
            ctx->Invalidate();
            if (!ctx->BufferActorId) {
                Transactions.AddToBeAborted(std::move(ctx));
            } else {
                TerminateBufferActor(ctx);
            }
            ReplySuccess();
        } else {
            ReplyTransactionNotFound(txControl.tx_id());
        }
    }

    void CommitTx() {
        YQL_ENSURE(QueryState->HasTxControl());
        const auto& txControl = QueryState->GetTxControl();
        YQL_ENSURE(txControl.tx_selector_case() == Ydb::Table::TransactionControl::kTxId, "Can't commit transaction - "
            << " there is no TxId in Query's TxControl");

        QueryState->Commit = true;

        auto txId = TTxId::FromString(txControl.tx_id());
        auto txCtx = Transactions.Find(txId);
        LOG_D("queryRequest TxControl: " << txControl.DebugString() << " txCtx: " << (void*)txCtx.Get());
        if (!txCtx) {
            ReplyTransactionNotFound(txControl.tx_id());
            return;
        }
        QueryState->TxCtx = std::move(txCtx);
        QueryState->QueryData = std::make_shared<TQueryData>(QueryState->TxCtx->TxAlloc);
        QueryState->TxId.SetValue(txId);
        if (!CheckTransactionLocks(/*tx*/ nullptr)) {
            return;
        }

        bool replied = ExecutePhyTx(/*tx*/ nullptr, /*commit*/ true);
        if (!replied) {
            Become(&TKqpSessionActor::ExecuteState);
        }
    }

    static bool IsQueryTypeSupported(NKikimrKqp::EQueryType type) {
        switch (type) {
            case NKikimrKqp::QUERY_TYPE_SQL_DML:
            case NKikimrKqp::QUERY_TYPE_PREPARED_DML:
            case NKikimrKqp::QUERY_TYPE_SQL_SCAN:
            case NKikimrKqp::QUERY_TYPE_AST_SCAN:
            case NKikimrKqp::QUERY_TYPE_AST_DML:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_CONCURRENT_QUERY:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_SCRIPT:
                return true;

            // should not be compiled. TODO: forward to request executer
            // not supported yet
            case NKikimrKqp::QUERY_TYPE_SQL_DDL:
            case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT:
            case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT_STREAMING:
            case NKikimrKqp::QUERY_TYPE_UNDEFINED:
                return false;
        }
    }

    void HandleClientLost(NGRpcService::TEvClientLost::TPtr&) {
        LOG_D("Got ClientLost event, send AbortExecution to executor: "
            << ExecuterId);

        if (ExecuterId) {
            auto abortEv = TEvKqp::TEvAbortExecution::Aborted("Client lost"); // any status code can be here

            Send(ExecuterId, abortEv.Release());
        } else {
            Cleanup();
        }
    }

    void Handle(TEvKqp::TEvQueryRequest::TPtr& ev) {
        if (CurrentStateFunc() != &TThis::ReadyState) {
            ReplyBusy(ev);
            return;
        }

        ui64 proxyRequestId = ev->Cookie;
        YQL_ENSURE(ev->Get()->GetSessionId() == SessionId,
                "Invalid session, expected: " << SessionId << ", got: " << ev->Get()->GetSessionId());

        NDataIntegrity::LogIntegrityTrails(ev, TlsActivationContext->AsActorContext());

        if (ev->Get()->HasYdbStatus() && ev->Get()->GetYdbStatus() != Ydb::StatusIds::SUCCESS) {
            NYql::TIssues issues;
            NYql::IssuesFromMessage(ev->Get()->GetQueryIssues(), issues);
            TString errMsg = issues.ToString();
            auto status = ev->Get()->GetYdbStatus();

            LOG_N(TKqpRequestInfo("", SessionId)
                << "Got invalid query request, reply with status: "
                << status
                << " msg: "
                << errMsg <<".");
            ReplyProcessError(ev, status, errMsg);
            return;
        }

        if (ShutdownState && ShutdownState->SoftTimeoutReached()) {
            // we reached the soft timeout, so at this point we don't allow to accept new queries for session.
            LOG_N("system shutdown requested: soft timeout reached, no queries can be accepted");
            ReplyProcessError(ev, Ydb::StatusIds::BAD_SESSION, "Session is under shutdown");
            CleanupAndPassAway();
            return;
        }

        MakeNewQueryState(ev);
        TTimerGuard timer(this);
        YQL_ENSURE(QueryState->GetDatabase() == Settings.Database,
                "Wrong database, expected:" << Settings.Database << ", got: " << QueryState->GetDatabase());

        auto action = QueryState->GetAction();

        LWTRACK(KqpSessionQueryRequest,
            QueryState->Orbit,
            QueryState->GetDatabase(),
            QueryState->GetType(),
            action,
            QueryState->GetQuery());

        LOG_D("received request,"
            << " proxyRequestId: " << proxyRequestId
            << " prepared: " << QueryState->HasPreparedQuery()
            << " tx_control: " << QueryState->HasTxControl()
            << " action: " << action
            << " type: " << QueryState->GetType()
            << " text: " << QueryState->GetQuery()
            << " rpcActor: " << QueryState->RequestActorId
            << " database: " << QueryState->GetDatabase()
            << " databaseId: " << QueryState->UserRequestContext->DatabaseId
            << " pool id: " << QueryState->UserRequestContext->PoolId
        );

        switch (action) {
            case NKikimrKqp::QUERY_ACTION_EXPLAIN:
            case NKikimrKqp::QUERY_ACTION_EXECUTE:
            case NKikimrKqp::QUERY_ACTION_PREPARE:
            case NKikimrKqp::QUERY_ACTION_EXECUTE_PREPARED: {
                auto type = QueryState->GetType();
                YQL_ENSURE(type != NKikimrKqp::QUERY_TYPE_UNDEFINED, "query type is undefined");

                if (action == NKikimrKqp::QUERY_ACTION_PREPARE) {
                   if (QueryState->KeepSession && !Settings.LongSession) {
                        ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST)
                            << "Expected KeepSession=false for non-execute requests";
                   }
                }

                if (!IsQueryTypeSupported(type)) {
                    return ForwardRequest(ev);
                }
                break;
            }
            case NKikimrKqp::QUERY_ACTION_BEGIN_TX: {
                YQL_ENSURE(QueryState->HasTxControl(),
                    "Can't perform BEGIN_TX: TxControl isn't set in TQueryRequest");
                const auto& txControl = QueryState->GetTxControl();
                QueryState->Commit = txControl.commit_tx();
                BeginTx(txControl.begin_tx());
                QueryState->CommandTagName = "BEGIN";
                ReplySuccess();
                return;
            }
            case NKikimrKqp::QUERY_ACTION_ROLLBACK_TX: {
                QueryState->CommandTagName = "ROLLBACK";
                return RollbackTx();
            }
            case NKikimrKqp::QUERY_ACTION_COMMIT_TX:
                QueryState->CommandTagName = "COMMIT";
                return CommitTx();

            // not supported yet
            case NKikimrKqp::QUERY_ACTION_VALIDATE:
            case NKikimrKqp::QUERY_ACTION_PARSE:
                return ForwardRequest(ev);

            case NKikimrKqp::QUERY_ACTION_TOPIC:
                return AddOffsetsToTransaction();
        }

        QueryState->UpdateTempTablesState(TempTablesState);

        if (QueryState->UserRequestContext->PoolId) {
            PassRequestToResourcePool();
        } else {
            CompileQuery();
        }
    }

    void Handle(TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Get()->SourceType == TKqpWorkloadServiceEvents::EvPlaceRequestIntoPool) {
            LOG_I("Failed to deliver request to workload service");
            CompileQuery();
        }
    }

    void Handle(NWorkload::TEvContinueRequest::TPtr& ev) {
        YQL_ENSURE(QueryState);
        QueryState->ContinueTime = TInstant::Now();

        if (ev->Get()->Status == Ydb::StatusIds::UNSUPPORTED) {
            LOG_T("Failed to place request in resource pool, feature flag is disabled");
            QueryState->UserRequestContext->PoolId.clear();
            CompileQuery();
            return;
        }

        const TString& poolId = ev->Get()->PoolId;
        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage> issues;
            NYql::IssuesToMessage(std::move(ev->Get()->Issues), &issues);
            ReplyQueryError(ev->Get()->Status, TStringBuilder() << "Query failed during adding/waiting in workload pool " << poolId, issues);
            return;
        }

        LOG_D("continue request, pool id: " << poolId);
        QueryState->PoolHandlerActor = ev->Sender;
        QueryState->UserRequestContext->PoolId = poolId;
        QueryState->UserRequestContext->PoolConfig = ev->Get()->PoolConfig;
        CompileQuery();
    }

    void AddOffsetsToTransaction() {
        YQL_ENSURE(QueryState);
        if (!PrepareQueryTransaction()) {
            return;
        }

        QueryState->AddOffsetsToTransaction();

        auto navigate = QueryState->BuildSchemeCacheNavigate();

        Become(&TKqpSessionActor::ExecuteState);
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(navigate.release()));
    }

    void CompileQuery() {
        YQL_ENSURE(QueryState);
        QueryState->CompilationRunning = true;

        Become(&TKqpSessionActor::ExecuteState);

        // fast query path
        if (QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_EXECUTE) {
            TFastQueryPtr fastQuery = FastQueryHelper.HandleYQL(QueryState->GetQuery());
            if (fastQuery && fastQuery->ExecutionType != TFastQuery::EExecutionType::UNSUPPORTED) {
                if (!fastQuery->DatabaseId) {
                    fastQuery->Database = QueryState->Database;
                    fastQuery->DatabaseId = QueryState->UserRequestContext->DatabaseId;
                }
                Counters->ReportFastQuery(Settings.DbCounters);
                LOG_T("Query is a fast query: " << QueryState->GetQuery() << ", " << fastQuery->ToString());

                QueryState->FastQuery = std::move(fastQuery);
                OnSuccessCompileRequest();
                return;
            } else {
                Counters->ReportRegularQuery(Settings.DbCounters);
                LOG_T("Query is not fast query: " << QueryState->GetQuery()
                    << (fastQuery ? fastQuery->ToString() : TString()) );
            }
        }

        // quick path
        if (QueryState->TryGetFromCache(*QueryCache, GUCSettings, Counters, SelfId()) && !QueryState->CompileResult->NeedToSplit) {
            LWTRACK(KqpSessionQueryCompiled, QueryState->Orbit, TStringBuilder() << QueryState->CompileResult->Status);

            // even if we have successfully compilation result, it doesn't mean anything
            // in terms of current schema version of the table if response of compilation is from the cache.
            // because of that, we are forcing to run schema version check
            if (QueryState->NeedCheckTableVersions()) {
                auto ev = QueryState->BuildNavigateKeySet();
                Send(MakeSchemeCacheID(), ev.release());
                return;
            }

            OnSuccessCompileRequest();
            return;
        }

        // TODO: in some cases we could reply right here (e.g. there is uid and query is missing), but
        // for extra sanity we make extra hop to the compile service, which might handle the issue better

        auto ev = QueryState->BuildCompileRequest(CompilationCookie, GUCSettings);
        LOG_D("Sending CompileQuery request");

        Send(MakeKqpCompileServiceID(SelfId().NodeId()), ev.release(), 0, QueryState->QueryId,
            QueryState->KqpSessionSpan.GetTraceId());
    }

    void CompileSplittedQuery() {
        YQL_ENSURE(QueryState);
        auto ev = QueryState->BuildCompileSplittedRequest(CompilationCookie, GUCSettings);
        LOG_D("Sending CompileSplittedQuery request");

        Send(MakeKqpCompileServiceID(SelfId().NodeId()), ev.release(), 0, QueryState->QueryId,
            QueryState->KqpSessionSpan.GetTraceId());
        Become(&TKqpSessionActor::ExecuteState);
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        const auto* response = ev->Get();
        YQL_ENSURE(response->Request);
        YQL_ENSURE(QueryState);
        // outdated response from scheme cache.
        // ignoring that.
        if (response->Request->Cookie < QueryId)
            return;

        if (QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_TOPIC) {
            ProcessTopicOps(ev);
            return;
        }

        // table versions are not the same. need the query recompilation.
        if (!QueryState->EnsureTableVersions(*response)) {
            auto ev = QueryState->BuildReCompileRequest(CompilationCookie, GUCSettings);
            Send(MakeKqpCompileServiceID(SelfId().NodeId()), ev.release(), 0, QueryState->QueryId,
                QueryState->KqpSessionSpan.GetTraceId());
            return;
        }

        OnSuccessCompileRequest();
    }

    void Handle(TEvKqp::TEvCompileResponse::TPtr& ev) {
        // outdated event from previous query.
        // ignoring that.
        if (ev->Cookie < QueryId) {
            return;
        }

        YQL_ENSURE(QueryState);
        TTimerGuard timer(this);

        // saving compile response and checking that compilation status
        // is success.
        if (!QueryState->SaveAndCheckCompileResult(ev->Get())) {
            LWTRACK(KqpSessionQueryCompiled, QueryState->Orbit, TStringBuilder() << QueryState->CompileResult->Status);

            if (QueryState->CompileResult->NeedToSplit) {
                if (!QueryState->HasTxControl()) {
                    YQL_ENSURE(QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_EXECUTE);
                    auto ev = QueryState->BuildSplitRequest(CompilationCookie, GUCSettings);
                    Send(MakeKqpCompileServiceID(SelfId().NodeId()), ev.release(), 0, QueryState->QueryId,
                        QueryState->KqpSessionSpan.GetTraceId());
                } else {
                    NYql::TIssues issues;
                    ReplyQueryError(
                        ::Ydb::StatusIds::StatusCode::StatusIds_StatusCode_BAD_REQUEST,
                        "CTAS statement can be executed only in NoTx mode.",
                        MessageFromIssues(issues));
                }
            } else {
                ReplyQueryCompileError();
            }
            return;
        }

        LWTRACK(KqpSessionQueryCompiled, QueryState->Orbit, TStringBuilder() << QueryState->CompileResult->Status);
        // even if we have successfully compilation result, it doesn't mean anything
        // in terms of current schema version of the table if response of compilation is from the cache.
        // because of that, we are forcing to run schema version check
        if (QueryState->NeedCheckTableVersions()) {
            auto ev = QueryState->BuildNavigateKeySet();
            Send(MakeSchemeCacheID(), ev.release());
            return;
        }

        OnSuccessCompileRequest();
    }

    void Handle(TEvKqp::TEvParseResponse::TPtr& ev) {
        QueryState->SaveAndCheckParseResult(std::move(*ev->Get()));
        CompileStatement();
    }

    void CompileStatement() {
        // quick path
        if (QueryState->TryGetFromCache(*QueryCache, GUCSettings, Counters, SelfId()) && !QueryState->CompileResult->NeedToSplit) {
            LWTRACK(KqpSessionQueryCompiled, QueryState->Orbit, TStringBuilder() << QueryState->CompileResult->Status);

            // even if we have successfully compilation result, it doesn't mean anything
            // in terms of current schema version of the table if response of compilation is from the cache.
            // because of that, we are forcing to run schema version check
            if (QueryState->NeedCheckTableVersions()) {
                auto ev = QueryState->BuildNavigateKeySet();
                Send(MakeSchemeCacheID(), ev.release());
                return;
            }

            OnSuccessCompileRequest();
            return;
        }

        // TODO: in some cases we could reply right here (e.g. there is uid and query is missing), but
        // for extra sanity we make extra hop to the compile service, which might handle the issue better

        auto request = QueryState->BuildCompileRequest(CompilationCookie, GUCSettings);
        LOG_D("Sending CompileQuery request (statement)");

        Send(MakeKqpCompileServiceID(SelfId().NodeId()), request.release(), 0, QueryState->QueryId,
            QueryState->KqpSessionSpan.GetTraceId());
    }

    void Handle(TEvKqp::TEvSplitResponse::TPtr& ev) {
        // outdated event from previous query.
        // ignoring that.
        if (ev->Cookie < QueryId) {
            return;
        }

        YQL_ENSURE(QueryState);
        TTimerGuard timer(this);
        if (!QueryState->SaveAndCheckSplitResult(ev->Get())) {
            ReplySplitError(ev->Get());
            return;
        }
        OnSuccessSplitRequest();
    }

    void OnSuccessSplitRequest() {
        YQL_ENSURE(ExecuteNextStatementPart());
    }

    bool ExecuteNextStatementPart() {
        if (QueryState->PrepareNextStatementPart()) {
            CompileSplittedQuery();
            return true;
        }
        return false;
    }

    void OnSuccessCompileRequest() {
        if (QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_PREPARE ||
            QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_EXPLAIN)
        {
            return ReplyPrepareResult();
        }

        if (QueryState->FastQuery) {
            if (!PrepareQueryTransaction()) {
                return;
            }
        } else {
            if (!PrepareQueryContext()) {
                return;
            }
        }

        Become(&TKqpSessionActor::ExecuteState);
        QueryState->TxCtx->OnBeginQuery();

        if (QueryState->FastQuery) {
            if (QueryState->FastQuery->ExecutionType == TFastQuery::EExecutionType::SELECT1) {
                ExecuteFastQuery();
                return;
            } else {
                AcquireMvccSnapshot();
                return;
            }
        }

        if (QueryState->NeedPersistentSnapshot()) {
            AcquirePersistentSnapshot();
            return;
        } else if (QueryState->NeedSnapshot(*Config)) {
            AcquireMvccSnapshot();
            return;
        }

        // Can reply inside (in case of deferred-only transactions) and become ReadyState
        ExecuteOrDefer();
    }

    void AcquirePersistentSnapshot() {
        LOG_D("acquire persistent snapshot");
        AcquireSnapshotSpan = NWilson::TSpan(TWilsonKqp::SessionAcquireSnapshot, QueryState->KqpSessionSpan.GetTraceId(),
            "SessionActor.AcquirePersistentSnapshot");
        auto timeout = QueryState->QueryDeadlines.TimeoutAt - TAppData::TimeProvider->Now();

        auto* snapMgr = CreateKqpSnapshotManager(Settings.Database, timeout);
        auto snapMgrActorId = RegisterWithSameMailbox(snapMgr);

        auto ev = std::make_unique<TEvKqpSnapshot::TEvCreateSnapshotRequest>(QueryState->PreparedQuery->GetQueryTables(), QueryId, std::move(QueryState->Orbit));
        Send(snapMgrActorId, ev.release());

        QueryState->TxCtx->SnapshotHandle.ManagingActor = snapMgrActorId;
    }

    void DiscardPersistentSnapshot(const IKqpGateway::TKqpSnapshotHandle& handle) {
        if (handle.ManagingActor) { // persistent snapshot was acquired
            Send(handle.ManagingActor, new TEvKqpSnapshot::TEvDiscardSnapshot());
        }
    }

    void AcquireMvccSnapshot() {
        AcquireSnapshotSpan = NWilson::TSpan(TWilsonKqp::SessionAcquireSnapshot, QueryState->KqpSessionSpan.GetTraceId(),
            "SessionActor.AcquireMvccSnapshot");
        LOG_D("acquire mvcc snapshot");
        auto timeout = QueryState->QueryDeadlines.TimeoutAt - TAppData::TimeProvider->Now();

        auto* snapMgr = CreateKqpSnapshotManager(Settings.Database, timeout);
        auto snapMgrActorId = RegisterWithSameMailbox(snapMgr);

        auto ev = std::make_unique<TEvKqpSnapshot::TEvCreateSnapshotRequest>(QueryId, std::move(QueryState->Orbit));
        Send(snapMgrActorId, ev.release());
    }

    Ydb::StatusIds::StatusCode StatusForSnapshotError(NKikimrIssues::TStatusIds::EStatusCode status) {
        switch (status) {
            case NKikimrIssues::TStatusIds::SCHEME_ERROR:
                return Ydb::StatusIds::SCHEME_ERROR;
            case NKikimrIssues::TStatusIds::TIMEOUT:
                return Ydb::StatusIds::TIMEOUT;
            case NKikimrIssues::TStatusIds::OVERLOADED:
                return Ydb::StatusIds::OVERLOADED;
            default:
                // snapshot is acquired before transactions execution, so we can return UNAVAILABLE here
                return Ydb::StatusIds::UNAVAILABLE;
        }
    }

    void Handle(TEvKqpSnapshot::TEvCreateSnapshotResponse::TPtr& ev) {
        if (ev->Cookie < QueryId || CurrentStateFunc() != &TThis::ExecuteState) {
            return;
        }

        TTimerGuard timer(this);

        auto *response = ev->Get();

        if (QueryState) {
            QueryState->Orbit = std::move(response->Orbit);
        }

        LOG_T("read snapshot result: " << StatusForSnapshotError(response->Status) << ", step: " << response->Snapshot.Step
            << ", tx id: " << response->Snapshot.TxId);
        if (response->Status != NKikimrIssues::TStatusIds::SUCCESS) {
            auto& issues = response->Issues;
            AcquireSnapshotSpan.EndError(issues.ToString());
            ReplyQueryError(StatusForSnapshotError(response->Status), "", MessageFromIssues(issues));
            return;
        }
        AcquireSnapshotSpan.EndOk();
        QueryState->TxCtx->SnapshotHandle.Snapshot = response->Snapshot;

        if (QueryState->FastQuery) {
            return ExecuteFastQuery();
        }

        // Can reply inside (in case of deferred-only transactions) and become ReadyState
        ExecuteOrDefer();
    }

    void ExecuteFastQuery() {
        ui64 lockId = QueryState->TxCtx->Locks.GetLockTxId();

        IActor* actor = nullptr;
        switch (QueryState->FastQuery->ExecutionType) {
        case TFastQuery::EExecutionType::SELECT_QUERY:
            actor = new TFastSelectExecutorActor(
                Counters,
                SelfId(),
                SessionId,
                QueryState->FastQuery,
                QueryState->GetYdbParameters(),
                QueryState->TxCtx->SnapshotHandle.Snapshot,
                QueryState->TxId,
                lockId);
            break;
        case TFastQuery::EExecutionType::SELECT_IN_QUERY:
            actor = new TFastSelectInExecutorActor(
                Counters,
                SelfId(),
                SessionId,
                QueryState->FastQuery,
                QueryState->GetYdbParameters(),
                QueryState->TxCtx->SnapshotHandle.Snapshot,
                QueryState->TxId,
                lockId);
            break;
        case TFastQuery::EExecutionType::UPSERT:
            QueryState->TxCtx->SetHasFastWrites();
            actor = new TFastUpsertActor(
                Counters,
                SelfId(),
                SessionId,
                QueryState->FastQuery,
                QueryState->GetYdbParameters(),
                QueryState->TxCtx->SnapshotHandle.Snapshot,
                QueryState->TxId,
                lockId);
            break;
        case TFastQuery::EExecutionType::SELECT1:
            actor = new TFastSelect1(
                Counters,
                SelfId(),
                SessionId,
                QueryState->FastQuery,
                QueryState->TxId,
                lockId);
            break;
        default:
            Y_VERIFY(actor);
        }

        // same mailbox
        FastQueryExecutor = TlsActivationContext->ExecutorThread.RegisterActor(
            actor,
            &TlsActivationContext->Mailbox,
            this->SelfId());

        auto ev = std::make_unique<TEvTxUserProxy::TEvProposeKqpTransaction>(FastQueryExecutor);
        Send<ESendingType::Tail>(MakeTxProxyID(), ev.release());
    }

    void BeginTx(const Ydb::Table::TransactionSettings& settings) {
        QueryState->TxId.SetValue(UlidGen.Next());
        QueryState->TxCtx = MakeIntrusive<TKqpTransactionContext>(false, AppData()->FunctionRegistry,
            AppData()->TimeProvider, AppData()->RandomProvider);

        auto& alloc = QueryState->TxCtx->TxAlloc;
        ui64 mkqlInitialLimit = Settings.MkqlInitialMemoryLimit;

        const auto& queryLimitsProto = Settings.TableService.GetQueryLimits();
        const auto& phaseLimitsProto = queryLimitsProto.GetPhaseLimits();
        ui64 mkqlMaxLimit = phaseLimitsProto.GetComputeNodeMemoryLimitBytes();
        mkqlMaxLimit = mkqlMaxLimit ? mkqlMaxLimit : ui64(Settings.MkqlMaxMemoryLimit);

        alloc->Alloc->SetLimit(mkqlInitialLimit);
        alloc->Alloc->Ref().SetIncreaseMemoryLimitCallback([this, &alloc, mkqlMaxLimit](ui64 currentLimit, ui64 required) {
            if (required < mkqlMaxLimit) {
                LOG_D("Increase memory limit from " << currentLimit << " to " << required);
                alloc->Alloc->SetLimit(required);
            }
        });

        QueryState->QueryData = std::make_shared<TQueryData>(QueryState->TxCtx->TxAlloc);
        QueryState->TxCtx->SetIsolationLevel(settings);
        QueryState->TxCtx->OnBeginQuery();

        if (QueryState->TxCtx->EffectiveIsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SNAPSHOT_RW
                && !Settings.TableService.GetEnableSnapshotIsolationRW()) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST)
                << "Writes aren't supported for Snapshot Isolation";
        }

        if (!Transactions.CreateNew(QueryState->TxId.GetValue(), QueryState->TxCtx)) {
            std::vector<TIssue> issues{
                YqlIssue({}, TIssuesIds::KIKIMR_TOO_MANY_TRANSACTIONS)};
            ythrow TRequestFail(Ydb::StatusIds::BAD_SESSION,
                                MessageFromIssues(issues))
                << "Too many transactions, current active: " << Transactions.Size()
                << " MaxTxPerSession: " << Transactions.MaxSize();
        }

        Counters->ReportTxCreated(Settings.DbCounters);
        Counters->ReportBeginTransaction(Settings.DbCounters, Transactions.EvictedTx, Transactions.Size(), Transactions.ToBeAbortedSize());
    }

    Ydb::Table::TransactionControl GetTxControlWithImplicitTx() {
        if (!QueryState->ImplicitTxId) {
            Ydb::Table::TransactionSettings settings;
            settings.mutable_serializable_read_write();
            BeginTx(settings);
            QueryState->ImplicitTxId = QueryState->TxId;
        }
        Ydb::Table::TransactionControl control;
        control.set_commit_tx(QueryState->ProcessingLastStatement() && QueryState->ProcessingLastStatementPart());
        control.set_tx_id(QueryState->ImplicitTxId->GetValue().GetHumanStr());
        return control;
    }

    bool PrepareQueryTransaction() {
        const bool hasTxControl = QueryState->HasTxControl();
        if (hasTxControl || QueryState->HasImplicitTx()) {
            const auto& txControl = hasTxControl ? QueryState->GetTxControl() : GetTxControlWithImplicitTx();

            QueryState->Commit = txControl.commit_tx();
            switch (txControl.tx_selector_case()) {
                case Ydb::Table::TransactionControl::kTxId: {
                    auto txId = TTxId::FromString(txControl.tx_id());
                    auto txCtx = Transactions.Find(txId);
                    if (!txCtx) {
                        ReplyTransactionNotFound(txControl.tx_id());
                        return false;
                    }
                    QueryState->TxCtx = txCtx;
                    QueryState->QueryData = std::make_shared<TQueryData>(QueryState->TxCtx->TxAlloc);
                    if (hasTxControl) {
                        QueryState->TxId.SetValue(txId);
                    }
                    break;
                }
                case Ydb::Table::TransactionControl::kBeginTx: {
                    BeginTx(txControl.begin_tx());
                    break;
                }
                case Ydb::Table::TransactionControl::TX_SELECTOR_NOT_SET:
                    ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST)
                        << "wrong TxControl: tx_selector must be set";
                    break;
            }
        } else {
            QueryState->TxCtx = MakeIntrusive<TKqpTransactionContext>(false, AppData()->FunctionRegistry,
                AppData()->TimeProvider, AppData()->RandomProvider);
            QueryState->QueryData = std::make_shared<TQueryData>(QueryState->TxCtx->TxAlloc);
            QueryState->TxCtx->EffectiveIsolationLevel = NKikimrKqp::ISOLATION_LEVEL_UNDEFINED;
        }

        return true;
    }

    bool PrepareQueryContext() {
        YQL_ENSURE(QueryState);
        if (!PrepareQueryTransaction()) {
            return false;
        }

        const NKqpProto::TKqpPhyQuery& phyQuery = QueryState->PreparedQuery->GetPhysicalQuery();
        const bool hasOlapWrite = ::NKikimr::NKqp::HasOlapTableWriteInTx(phyQuery);
        const bool hasOltpWrite = ::NKikimr::NKqp::HasOltpTableWriteInTx(phyQuery);
        const bool hasOlapRead = ::NKikimr::NKqp::HasOlapTableReadInTx(phyQuery);
        const bool hasOltpRead = ::NKikimr::NKqp::HasOltpTableReadInTx(phyQuery);
        QueryState->TxCtx->HasOlapTable |= hasOlapWrite || hasOlapRead;
        QueryState->TxCtx->HasOltpTable |= hasOltpWrite || hasOltpRead;
        QueryState->TxCtx->HasTableWrite |= hasOlapWrite || hasOltpWrite;
        QueryState->TxCtx->HasTableRead |= hasOlapRead || hasOltpRead;
        if (QueryState->TxCtx->HasOlapTable && QueryState->TxCtx->HasOltpTable && QueryState->TxCtx->HasTableWrite
                && !Settings.TableService.GetEnableHtapTx() && !QueryState->IsSplitted()) {
            ReplyQueryError(Ydb::StatusIds::PRECONDITION_FAILED,
                            "Write transactions between column and row tables are disabled at current time.");
            return false;
        }
        if (QueryState->TxCtx->EffectiveIsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SNAPSHOT_RW
            && QueryState->TxCtx->HasOltpTable) {
            ReplyQueryError(Ydb::StatusIds::PRECONDITION_FAILED,
                            "SnapshotRW can only be used with olap tables.");
            return false;
        }

        QueryState->TxCtx->SetTempTables(QueryState->TempTablesState);
        QueryState->TxCtx->ApplyPhysicalQuery(phyQuery, QueryState->Commit);
        auto [success, issues] = QueryState->TxCtx->ApplyTableOperations(phyQuery.GetTableOps(), phyQuery.GetTableInfos(),
            EKikimrQueryType::Dml);
        if (!success) {
            YQL_ENSURE(!issues.Empty());
            ReplyQueryError(GetYdbStatus(issues), "", MessageFromIssues(issues));
            return false;
        }

        auto action = QueryState->GetAction();
        auto type = QueryState->GetType();

        if (action == NKikimrKqp::QUERY_ACTION_EXECUTE && type == NKikimrKqp::QUERY_TYPE_SQL_DML
            || action == NKikimrKqp::QUERY_ACTION_EXECUTE && type == NKikimrKqp::QUERY_TYPE_AST_DML)
        {
            type = NKikimrKqp::QUERY_TYPE_PREPARED_DML;
            action = NKikimrKqp::QUERY_ACTION_EXECUTE_PREPARED;
        }

        switch (action) {
            case NKikimrKqp::QUERY_ACTION_EXECUTE:
                YQL_ENSURE(
                    type == NKikimrKqp::QUERY_TYPE_SQL_SCAN ||
                    type == NKikimrKqp::QUERY_TYPE_AST_SCAN ||
                    type == NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY ||
                    type == NKikimrKqp::QUERY_TYPE_SQL_GENERIC_CONCURRENT_QUERY ||
                    type == NKikimrKqp::QUERY_TYPE_SQL_GENERIC_SCRIPT
                );
                break;

            case NKikimrKqp::QUERY_ACTION_EXECUTE_PREPARED:
                YQL_ENSURE(type == NKikimrKqp::QUERY_TYPE_PREPARED_DML);
                break;

            default:
                YQL_ENSURE(false, "Unexpected query action: " << action);
        }

        try {
            const auto& parameters = QueryState->GetYdbParameters();
            QueryState->QueryData->ParseParameters(parameters);
            if (QueryState->CompileResult && QueryState->CompileResult->GetAst() && QueryState->CompileResult->GetAst()->PgAutoParamValues) {
                const auto& params = dynamic_cast<TKqpAutoParamBuilder*>(QueryState->CompileResult->GetAst()->PgAutoParamValues.Get())->Values;
                for(const auto& [name, param] : params) {
                    if (!parameters.contains(name)) {
                        QueryState->QueryData->AddTypedValueParam(name, param);
                    }
                }
            }
        } catch(const yexception& ex) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST) << ex.what();
        } catch(const TMemoryLimitExceededException&) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST) << BuildMemoryLimitExceptionMessage();
        }
        return true;
    }

    void ReplyPrepareResult() {
        YQL_ENSURE(QueryState);
        QueryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        FillCompileStatus(QueryState->CompileResult, QueryResponse->Record);

        auto ru = NRuCalc::CpuTimeToUnit(TDuration::MicroSeconds(QueryState->CompileStats.CpuTimeUs));
        auto& record = QueryResponse->Record;
        record.SetConsumedRu(ru);

        Cleanup(IsFatalError(record.GetYdbStatus()));
    }

    IKqpGateway::TExecPhysicalRequest PrepareBaseRequest(TKqpQueryState *queryState, TTxAllocatorState::TPtr alloc) {
        IKqpGateway::TExecPhysicalRequest request(alloc);

        if (queryState) {
            auto now = TAppData::TimeProvider->Now();
            request.Timeout = queryState->QueryDeadlines.TimeoutAt - now;
            if (auto cancelAt = queryState->QueryDeadlines.CancelAt) {
                request.CancelAfter = cancelAt - now;
            }

            request.StatsMode = queryState->GetStatsMode();
            request.ProgressStatsPeriod = queryState->GetProgressStatsPeriod();
            request.QueryType = queryState->GetType();
            request.OutputChunkMaxSize = queryState->GetOutputChunkMaxSize();
            if (Y_LIKELY(queryState->PreparedQuery)) {
                ui64 resultSetsCount = queryState->PreparedQuery->GetPhysicalQuery().ResultBindingsSize();
                request.AllowTrailingResults = (resultSetsCount == 1 && queryState->Statements.size() <= 1);
                request.AllowTrailingResults &= (QueryState->RequestEv->GetSupportsStreamTrailingResult());
            }
        }

        const auto& limits = GetQueryLimits(Settings);
        request.MaxAffectedShards = limits.PhaseLimits.AffectedShardsLimit;
        request.TotalReadSizeLimitBytes = limits.PhaseLimits.TotalReadSizeLimitBytes;
        request.MkqlMemoryLimit = limits.PhaseLimits.ComputeNodeMemoryLimitBytes;
        return request;
    }

    IKqpGateway::TExecPhysicalRequest PrepareLiteralRequest(TKqpQueryState *queryState) {
        auto request = PrepareBaseRequest(queryState, queryState->TxCtx->TxAlloc);
        request.NeedTxId = false;
        return request;
    }

    IKqpGateway::TExecPhysicalRequest PreparePhysicalRequest(TKqpQueryState *queryState,
        TTxAllocatorState::TPtr alloc)
    {
        auto request = PrepareBaseRequest(queryState, alloc);

        if (queryState) {
            request.Snapshot = queryState->TxCtx->GetSnapshot();
            request.IsolationLevel = *queryState->TxCtx->EffectiveIsolationLevel;
            request.UserTraceId = queryState->UserRequestContext->TraceId;
        } else {
            request.IsolationLevel = NKikimrKqp::ISOLATION_LEVEL_SERIALIZABLE;
        }

        return request;
    }

    IKqpGateway::TExecPhysicalRequest PrepareScanRequest(TKqpQueryState *queryState) {
        auto request = PrepareBaseRequest(queryState, queryState->TxCtx->TxAlloc);

        request.MaxComputeActors = Config->_KqpMaxComputeActors.Get().GetRef();
        YQL_ENSURE(queryState);
        request.Snapshot = queryState->TxCtx->GetSnapshot();

        return request;
    }

    IKqpGateway::TExecPhysicalRequest PrepareGenericRequest(TKqpQueryState *queryState) {
        auto request = PrepareBaseRequest(queryState, queryState->TxCtx->TxAlloc);

        if (queryState) {
            request.Snapshot = queryState->TxCtx->GetSnapshot();
            request.IsolationLevel = *queryState->TxCtx->EffectiveIsolationLevel;
        } else {
            request.IsolationLevel = NKikimrKqp::ISOLATION_LEVEL_SERIALIZABLE;
        }

        return request;
    }

    IKqpGateway::TExecPhysicalRequest PrepareRequest(const TKqpPhyTxHolder::TConstPtr& tx, bool literal,
        TKqpQueryState *queryState)
    {
        if (literal) {
            YQL_ENSURE(tx);
            return PrepareLiteralRequest(QueryState.get());
        }

        if (!tx) {
            return PreparePhysicalRequest(QueryState.get(), queryState->TxCtx->TxAlloc);
        }

        switch (tx->GetType()) {
            case NKqpProto::TKqpPhyTx::TYPE_COMPUTE:
                return PreparePhysicalRequest(QueryState.get(), queryState->TxCtx->TxAlloc);
            case NKqpProto::TKqpPhyTx::TYPE_DATA:
                return PreparePhysicalRequest(QueryState.get(), queryState->TxCtx->TxAlloc);
            case NKqpProto::TKqpPhyTx::TYPE_SCAN:
                return PrepareScanRequest(QueryState.get());
            case NKqpProto::TKqpPhyTx::TYPE_GENERIC:
                return PrepareGenericRequest(QueryState.get());
            default:
                YQL_ENSURE(false, "Unexpected physical tx type: " << (int)tx->GetType());
        }
    }

    bool CheckTransactionLocks(const TKqpPhyTxHolder::TConstPtr& tx) {
        auto& txCtx = *QueryState->TxCtx;
        const bool broken = txCtx.TxManager
            ? !!txCtx.TxManager->GetLockIssue()
            : txCtx.Locks.Broken();

        if (!txCtx.DeferredEffects.Empty() && broken) {
            ReplyQueryError(Ydb::StatusIds::ABORTED, "tx has deferred effects, but locks are broken",
                MessageFromIssues(std::vector<TIssue>{txCtx.TxManager ? *txCtx.TxManager->GetLockIssue() : txCtx.Locks.GetIssue()}));
            return false;
        }

        if (tx && tx->GetHasEffects() && broken) {
            ReplyQueryError(Ydb::StatusIds::ABORTED, "tx has effects, but locks are broken",
                MessageFromIssues(std::vector<TIssue>{txCtx.TxManager ? *txCtx.TxManager->GetLockIssue() : txCtx.Locks.GetIssue()}));
            return false;
        }

        return true;
    }

    bool CheckTopicOperations() {
        auto& txCtx = *QueryState->TxCtx;

        if (txCtx.TopicOperations.IsValid()) {
            return true;
        }

        std::vector<TIssue> issues {
            YqlIssue({}, TIssuesIds::KIKIMR_BAD_REQUEST, "Incorrect offset ranges in the transaction.")
        };
        ReplyQueryError(Ydb::StatusIds::ABORTED, "incorrect offset ranges in the tx",
                        MessageFromIssues(issues));

        return false;
    }

    void ExecuteOrDefer() {
        bool haveWork = QueryState->PreparedQuery &&
                QueryState->CurrentTx < QueryState->PreparedQuery->GetPhysicalQuery().TransactionsSize()
                    || QueryState->Commit && !QueryState->Commited;

        if (!haveWork) {
            ReplySuccess();
            return;
        }

        bool isBatchQuery = IsBatchQuery(QueryState->PreparedQuery->GetPhysicalQuery());

        TKqpPhyTxHolder::TConstPtr tx;
        try {
            tx = QueryState->GetCurrentPhyTx(isBatchQuery);
        } catch (const yexception& ex) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST) << ex.what();
        }

        if (!CheckTransactionLocks(tx) || !CheckTopicOperations()) {
            return;
        }

        if (Settings.TableService.GetEnableOltpSink() && isBatchQuery) {
            if (!Settings.TableService.GetEnableBatchUpdates()) {
                ReplyQueryError(Ydb::StatusIds::PRECONDITION_FAILED,
                        "Batch updates and deletes are disabled at current time.");
            }

            ExecutePartitioned(tx);
        } else if (QueryState->TxCtx->ShouldExecuteDeferredEffects(tx)) {
            ExecuteDeferredEffectsImmediately(tx);
        } else if (auto commit = QueryState->ShouldCommitWithCurrentTx(tx); commit || tx) {
            ExecutePhyTx(tx, commit);
        } else {
            ReplySuccess();
        }
    }

    void ExecutePartitioned(const TKqpPhyTxHolder::TConstPtr& tx) {
        if (QueryState->HasTxControl()) {
            NYql::TIssues issues;
            return ReplyQueryError(
                ::Ydb::StatusIds::StatusCode::StatusIds_StatusCode_BAD_REQUEST,
                "BATCH operation can be executed only in NoTx mode.",
                MessageFromIssues(issues));
        }

        auto& txCtx = *QueryState->TxCtx;

        auto literalRequest = PrepareLiteralRequest(QueryState.get());
        auto physicalRequest = PreparePhysicalRequest(QueryState.get(), txCtx.TxAlloc);

        if (QueryState->PreparedQuery->GetTransactions().size() == 2) {
            literalRequest.Transactions.emplace_back(tx, QueryState->QueryData);
        } else {
            physicalRequest.Transactions.emplace_back(tx, QueryState->QueryData);
        }

        QueryState->TxCtx->OnNewExecutor(false);
        QueryState->Commited = true;

        SendToPartitionedExecuter(QueryState->TxCtx.Get(), std::move(literalRequest), std::move(physicalRequest));
        QueryState->CurrentTx += QueryState->PreparedQuery->GetTransactions().size();
    }

    void ExecuteDeferredEffectsImmediately(const TKqpPhyTxHolder::TConstPtr& tx) {
        YQL_ENSURE(QueryState->TxCtx->ShouldExecuteDeferredEffects(tx));

        auto& txCtx = *QueryState->TxCtx;
        auto request = PrepareRequest(/* tx */ nullptr, /* literal */ false, QueryState.get());

        for (const auto& effect : txCtx.DeferredEffects) {
            request.Transactions.emplace_back(effect.PhysicalTx, effect.Params);

            LOG_D("TExecPhysicalRequest, add DeferredEffect to Transaction,"
                << " current Transactions.size(): " << request.Transactions.size());
        }

        request.AcquireLocksTxId = txCtx.Locks.GetLockTxId();
        request.UseImmediateEffects = true;
        request.PerShardKeysSizeLimitBytes = Config->_CommitPerShardKeysSizeLimitBytes.Get().GetRef();

        txCtx.HasImmediateEffects = true;
        txCtx.ClearDeferredEffects();

        SendToExecuter(QueryState->TxCtx.Get(), std::move(request));
        if (!tx) {
            ++QueryState->CurrentTx;
        }
    }

    bool ExecutePhyTx(const TKqpPhyTxHolder::TConstPtr& tx, bool commit) {
        if (tx) {
            switch (tx->GetType()) {
                case NKqpProto::TKqpPhyTx::TYPE_SCHEME:
                    YQL_ENSURE(tx->StagesSize() == 0);
                    if (QueryState->HasTxControl() && !QueryState->HasImplicitTx() && QueryState->TxCtx->EffectiveIsolationLevel != NKikimrKqp::ISOLATION_LEVEL_UNDEFINED) {
                        ReplyQueryError(Ydb::StatusIds::PRECONDITION_FAILED,
                            "Scheme operations cannot be executed inside transaction");
                        return true;
                    }

                    SendToSchemeExecuter(tx);
                    ++QueryState->CurrentTx;
                    return false;

                case NKqpProto::TKqpPhyTx::TYPE_DATA:
                case NKqpProto::TKqpPhyTx::TYPE_GENERIC:
                    if (QueryState->TxCtx->EffectiveIsolationLevel == NKikimrKqp::ISOLATION_LEVEL_UNDEFINED) {
                        ReplyQueryError(Ydb::StatusIds::PRECONDITION_FAILED,
                            "Data operations cannot be executed outside of transaction");
                        return true;
                    }

                    break;

                default:
                    break;
            }
        }

        auto& txCtx = *QueryState->TxCtx;
        bool literal = tx && tx->IsLiteralTx();
        const bool hasLocks = txCtx.TxManager ? txCtx.TxManager->HasLocks() : txCtx.Locks.HasLocks();

        if (commit) {
            if (txCtx.TxHasEffects() || hasLocks || txCtx.TopicOperations.HasOperations()) {
                // Cannot perform commit in literal execution
                literal = false;
            } else if (!tx) {
                // Commit is no-op
                ReplySuccess();
                return true;
            }
        }

        auto request = PrepareRequest(tx, literal, QueryState.get());

        LOG_D("ExecutePhyTx, tx: " << (void*)tx.get() << " literal: " << literal << " commit: " << commit
                << " txCtx.DeferredEffects.size(): " << txCtx.DeferredEffects.Size() << ", hasLocks: " << hasLocks);

        if (!CheckTopicOperations()) {
            return true;
        }

        YQL_ENSURE(tx || commit);
        if (tx) {
            switch (tx->GetType()) {
                case NKqpProto::TKqpPhyTx::TYPE_COMPUTE:
                case NKqpProto::TKqpPhyTx::TYPE_DATA:
                case NKqpProto::TKqpPhyTx::TYPE_SCAN:
                case NKqpProto::TKqpPhyTx::TYPE_GENERIC:
                    break;
                default:
                    YQL_ENSURE(false, "Unexpected physical tx type in data query: " << (ui32)tx->GetType());
            }
        }

        try {
            QueryState->QueryData->PrepareParameters(tx, QueryState->PreparedQuery, txCtx.TxAlloc->TypeEnv);
        } catch (const yexception& ex) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST) << ex.what();
        }

        if (tx) {
            request.Transactions.emplace_back(tx, QueryState->QueryData);
        }

        QueryState->TxCtx->OnNewExecutor(literal);

        if (literal) {
            Y_ENSURE(QueryState);
            request.Orbit = std::move(QueryState->Orbit);
            request.TraceId = QueryState->KqpSessionSpan.GetTraceId();
            auto response = ExecuteLiteral(std::move(request), RequestCounters, SelfId(), QueryState->UserRequestContext);
            ++QueryState->CurrentTx;
            ProcessExecuterResult(response.get());
            return true;
        } else if (commit) {
            QueryState->Commited = true;

            for (const auto& effect : txCtx.DeferredEffects) {
                request.Transactions.emplace_back(effect.PhysicalTx, effect.Params);

                LOG_D("TExecPhysicalRequest, add DeferredEffect to Transaction,"
                       << " current Transactions.size(): " << request.Transactions.size());
            }

            if (!txCtx.DeferredEffects.Empty()) {
                request.PerShardKeysSizeLimitBytes = Config->_CommitPerShardKeysSizeLimitBytes.Get().GetRef();
            }

            request.HasFastWrites = txCtx.TxHasFastWrites();

            if (Settings.TableService.GetEnableOltpSink()) {
                if (txCtx.TxHasEffects() || hasLocks || txCtx.TopicOperations.HasOperations()) {
                    request.AcquireLocksTxId = txCtx.Locks.GetLockTxId();
                }

                if (hasLocks || txCtx.TopicOperations.HasOperations()) {
                    if (!txCtx.GetSnapshot().IsValid() || txCtx.TxHasEffects() || txCtx.TopicOperations.HasOperations()) {
                        LOG_D("TExecPhysicalRequest, tx has commit locks");
                        request.LocksOp = ELocksOp::Commit;
                    } else {
                        LOG_D("TExecPhysicalRequest, tx has rollback locks");
                        request.LocksOp = ELocksOp::Rollback;
                    }
                } else if (txCtx.TxHasEffects()) {
                    LOG_D("TExecPhysicalRequest, need commit locks");
                    request.LocksOp = ELocksOp::Commit;
                }

                for (auto& [lockId, lock] : txCtx.Locks.LocksMap) {
                    auto dsLock = ExtractLock(lock.GetValueRef(txCtx.Locks.LockType));
                    request.DataShardLocks[dsLock.GetDataShard()].emplace_back(dsLock);
                }
            } else {
                AFL_ENSURE(!tx || !txCtx.HasOlapTable);
                AFL_ENSURE(txCtx.DeferredEffects.Empty() || !txCtx.HasOlapTable);
                if (hasLocks || txCtx.TopicOperations.HasOperations()) {
                    bool hasUncommittedEffects = false;
                    for (auto& [lockId, lock] : txCtx.Locks.LocksMap) {
                        auto dsLock = ExtractLock(lock.GetValueRef(txCtx.Locks.LockType));
                        request.DataShardLocks[dsLock.GetDataShard()].emplace_back(dsLock);
                        hasUncommittedEffects |=  dsLock.GetHasWrites();
                    }
                    if (!txCtx.GetSnapshot().IsValid() || (tx && txCtx.TxHasEffects()) || !txCtx.DeferredEffects.Empty() || hasUncommittedEffects || txCtx.TopicOperations.HasOperations()) {
                        LOG_D("TExecPhysicalRequest, tx has commit locks");
                        request.LocksOp = ELocksOp::Commit;
                    } else {
                        LOG_D("TExecPhysicalRequest, tx has rollback locks");
                        request.LocksOp = ELocksOp::Rollback;
                    }
                }
            }
            request.TopicOperations = std::move(txCtx.TopicOperations);
        } else if (QueryState->ShouldAcquireLocks(tx) && (!txCtx.HasOlapTable || Settings.TableService.GetEnableOlapSink())) {
            request.AcquireLocksTxId = txCtx.Locks.GetLockTxId();

            if (!txCtx.CanDeferEffects()) {
                request.UseImmediateEffects = true;
            }
        }

        LWTRACK(KqpSessionPhyQueryProposeTx,
            QueryState->Orbit,
            QueryState->CurrentTx,
            request.Transactions.size(),
            (txCtx.TxManager ? txCtx.TxManager->GetShardsCount() : txCtx.Locks.Size()),
            request.AcquireLocksTxId.Defined());

        SendToExecuter(QueryState->TxCtx.Get(), std::move(request));
        ++QueryState->CurrentTx;
        return false;
    }

    void FillGUCSettings() {
        if (Settings.Database) {
            GUCSettings->Set("ydb_database", Settings.Database.substr(1, Settings.Database.size() - 1));
        }
        if (Settings.UserName) {
            GUCSettings->Set("ydb_user", *Settings.UserName);
        }
    }

    void SendToSchemeExecuter(const TKqpPhyTxHolder::TConstPtr& tx) {
        YQL_ENSURE(QueryState);

        auto userToken = QueryState->UserToken;
        const TString clientAddress = QueryState->ClientAddress;
        const TString requestType = QueryState->GetRequestType();
        const bool temporary = GetTemporaryTableInfo(tx).has_value();

        auto executerActor = CreateKqpSchemeExecuter(tx, QueryState->GetType(), SelfId(), requestType, Settings.Database, userToken, clientAddress,
            temporary, TempTablesState.SessionId, QueryState->UserRequestContext, KqpTempTablesAgentActor);

        ExecuterId = RegisterWithSameMailbox(executerActor);
    }

    static ui32 GetResultsCount(const IKqpGateway::TExecPhysicalRequest& req) {
        ui32 results = 0;
        for (const auto& transaction : req.Transactions) {
            results += transaction.Body->ResultsSize();
        }
        return results;
    }

    void SendToExecuter(TKqpTransactionContext* txCtx, IKqpGateway::TExecPhysicalRequest&& request, bool isRollback = false) {
        if (QueryState) {
            request.Orbit = std::move(QueryState->Orbit);
            QueryState->StatementResultSize = GetResultsCount(request);
        }
        request.PerRequestDataSizeLimit = RequestControls.PerRequestDataSizeLimit;
        request.MaxShardCount = RequestControls.MaxShardCount;
        request.TraceId = QueryState ? QueryState->KqpSessionSpan.GetTraceId() : NWilson::TTraceId();
        request.CaFactory_ = CaFactory_;
        request.ResourceManager_ = ResourceManager_;
        LOG_D("Sending to Executer TraceId: " << request.TraceId.GetTraceId() << " " << request.TraceId.GetSpanIdSize());

        if (Settings.TableService.GetEnableOltpSink() && !txCtx->TxManager) {
            txCtx->TxManager = CreateKqpTransactionManager();
            txCtx->TxManager->SetAllowVolatile(AppData()->FeatureFlags.GetEnableDataShardVolatileTransactions());
        }

        if (Settings.TableService.GetEnableOltpSink()
            && !txCtx->BufferActorId
            && (txCtx->HasTableWrite || request.TopicOperations.GetSize() != 0)) {
            txCtx->TxManager->SetTopicOperations(std::move(request.TopicOperations));
            txCtx->TxManager->AddTopicsToShards();

            auto alloc = std::make_shared<NKikimr::NMiniKQL::TScopedAlloc>(
                __LOCATION__, NKikimr::TAlignedPagePoolCounters(), true, false);

            const auto& queryLimitsProto = Settings.TableService.GetQueryLimits();
            const auto& bufferLimitsProto = queryLimitsProto.GetBufferLimits();
            const ui64 writeBufferMemoryLimit = bufferLimitsProto.HasWriteBufferMemoryLimitBytes()
                ? bufferLimitsProto.GetWriteBufferMemoryLimitBytes()
                : ui64(Settings.MkqlMaxMemoryLimit);
            const ui64 writeBufferInitialMemoryLimit = writeBufferMemoryLimit < ui64(Settings.MkqlInitialMemoryLimit)
                ? writeBufferMemoryLimit
                : ui64(Settings.MkqlInitialMemoryLimit);
            alloc->SetLimit(writeBufferInitialMemoryLimit);
            alloc->Ref().SetIncreaseMemoryLimitCallback([this, alloc=alloc.get(), writeBufferMemoryLimit](ui64 currentLimit, ui64 required) {
                if (required < writeBufferMemoryLimit) {
                    LOG_D("Increase memory limit from " << currentLimit << " to " << required);
                    alloc->SetLimit(required);
                }
            });

            TKqpBufferWriterSettings settings {
                .SessionActorId = SelfId(),
                .TxManager = txCtx->TxManager,
                .TraceId = request.TraceId.GetTraceId(),
                .Counters = Counters,
                .TxProxyMon = RequestCounters->TxProxyMon,
                .Alloc = std::move(alloc),
            };
            auto* actor = CreateKqpBufferWriterActor(std::move(settings));
            txCtx->BufferActorId = RegisterWithSameMailbox(actor);
        } else if (Settings.TableService.GetEnableOltpSink() && txCtx->BufferActorId) {
            txCtx->TxManager->SetTopicOperations(std::move(request.TopicOperations));
            txCtx->TxManager->AddTopicsToShards();
        }

        auto executerActor = CreateKqpExecuter(std::move(request), Settings.Database,
            QueryState ? QueryState->UserToken : TIntrusiveConstPtr<NACLib::TUserToken>(),
            RequestCounters, Settings.TableService,
            AsyncIoFactory, QueryState ? QueryState->PreparedQuery : nullptr, SelfId(),
            QueryState ? QueryState->UserRequestContext : MakeIntrusive<TUserRequestContext>("", Settings.Database, SessionId),
            QueryState ? QueryState->StatementResultIndex : 0, FederatedQuerySetup,
            (QueryState && QueryState->RequestEv->GetSyntax() == Ydb::Query::Syntax::SYNTAX_PG)
                ? GUCSettings : nullptr,
            txCtx->ShardIdToTableInfo, txCtx->TxManager, txCtx->BufferActorId);

        auto exId = RegisterWithSameMailbox(executerActor);
        LOG_D("Created new KQP executer: " << exId << " isRollback: " << isRollback);
        auto ev = std::make_unique<TEvTxUserProxy::TEvProposeKqpTransaction>(exId);
        Send(MakeTxProxyID(), ev.release());
        if (!isRollback) {
            YQL_ENSURE(!ExecuterId);
        }
        ExecuterId = exId;
    }

    void SendToPartitionedExecuter(TKqpTransactionContext* txCtx, IKqpGateway::TExecPhysicalRequest&& literalRequest,
        IKqpGateway::TExecPhysicalRequest&& physicalRequest)
    {
        physicalRequest.Orbit = std::move(QueryState->Orbit);
        QueryState->StatementResultSize = GetResultsCount(physicalRequest);

        literalRequest.TraceId = QueryState->KqpSessionSpan.GetTraceId();

        physicalRequest.LocksOp = ELocksOp::Commit;
        physicalRequest.PerRequestDataSizeLimit = RequestControls.PerRequestDataSizeLimit;
        physicalRequest.MaxShardCount = RequestControls.MaxShardCount;
        physicalRequest.TraceId = QueryState
            ? QueryState->KqpSessionSpan.GetTraceId()
            : NWilson::TTraceId();
        physicalRequest.CaFactory_ = CaFactory_;
        physicalRequest.ResourceManager_ = ResourceManager_;

        const auto& queryLimitsProto = Settings.TableService.GetQueryLimits();
        const auto& bufferLimitsProto = queryLimitsProto.GetBufferLimits();
        const ui64 writeBufferMemoryLimit = bufferLimitsProto.HasWriteBufferMemoryLimitBytes()
            ? bufferLimitsProto.GetWriteBufferMemoryLimitBytes()
            : ui64(Settings.MkqlMaxMemoryLimit);
        const ui64 writeBufferInitialMemoryLimit = writeBufferMemoryLimit < ui64(Settings.MkqlInitialMemoryLimit)
            ? writeBufferMemoryLimit
            : ui64(Settings.MkqlInitialMemoryLimit);

        TKqpPartitionedExecuterSettings settings{
            .LiteralRequest = std::move(literalRequest),
            .PhysicalRequest = std::move(physicalRequest),
            .SessionActorId = SelfId(),
            .FuncRegistry = AppData()->FunctionRegistry,
            .TimeProvider = AppData()->TimeProvider,
            .RandomProvider = AppData()->RandomProvider,
            .Database = Settings.Database,
            .UserToken = QueryState
                ? QueryState->UserToken
                : TIntrusiveConstPtr<NACLib::TUserToken>(),
            .RequestCounters = RequestCounters,
            .TableServiceConfig = Settings.TableService,
            .AsyncIoFactory = AsyncIoFactory,
            .PreparedQuery = QueryState
                ? QueryState->PreparedQuery
                : nullptr,
            .UserRequestContext = QueryState
                ? QueryState->UserRequestContext
                : MakeIntrusive<TUserRequestContext>("", Settings.Database, SessionId),
            .StatementResultIndex = QueryState
                ? QueryState->StatementResultIndex
                : 0,
            .FederatedQuerySetup = FederatedQuerySetup,
            .GUCSettings = GUCSettings,
            .ShardIdToTableInfo = txCtx->ShardIdToTableInfo,
            .WriteBufferInitialMemoryLimit = writeBufferInitialMemoryLimit,
            .WriteBufferMemoryLimit = writeBufferMemoryLimit,
        };

        auto executerActor = CreateKqpPartitionedExecuter(std::move(settings));

        ExecuterId = RegisterWithSameMailbox(executerActor);
        LOG_D("Created new KQP partitioned executer: " << ExecuterId);
    }


    template<typename T>
    void HandleNoop(T&) {
    }

    bool MergeLocksWithTxResult(const NKikimrKqp::TExecuterTxResult& result) {
        if (result.HasLocks()) {
            auto& txCtx = QueryState->TxCtx;
            const auto& locks = result.GetLocks();
            auto [success, issues] = MergeLocks(locks.GetType(), locks.GetValue(), *txCtx);
            if (!success) {
                if (!txCtx->GetSnapshot().IsValid() || txCtx->TxHasEffects()) {
                    ReplyQueryError(Ydb::StatusIds::ABORTED,  "Error while locks merge",
                        MessageFromIssues(issues));
                    return false;
                }

                if (txCtx->GetSnapshot().IsValid()) {
                    txCtx->Locks.MarkBroken(issues.back());
                }
            }
        }

        return true;
    }

    bool MergeLocksWithFastQuery(const TVector<NKikimrDataEvents::TLock>& locks) {
        if (locks.empty()) {
            return true;
        }

        NKikimrMiniKQL::TResult result;
        BuildLocks(result, locks);

        auto& txCtx = QueryState->TxCtx;
        auto [success, issues] = MergeLocks(result.GetType(), result.GetValue(), *txCtx);
        if (!success) {
            if (!txCtx->GetSnapshot().IsValid() || txCtx->TxHasEffects()) {
                ReplyQueryError(Ydb::StatusIds::ABORTED,  "Error while locks merge",
                    MessageFromIssues(issues));
                return false;
            }

            if (txCtx->GetSnapshot().IsValid()) {
                txCtx->Locks.MarkBroken(issues.back());
            }
        }

        return true;
    }

    void InvalidateQuery() {
        if (QueryState->CompileResult) {
            auto invalidateEv = MakeHolder<TEvKqp::TEvCompileInvalidateRequest>(
                QueryState->CompileResult->Uid, Settings.DbCounters);

            Send(MakeKqpCompileServiceID(SelfId().NodeId()), invalidateEv.Release());
        }
    }

    void HandleExecute(TEvKqpExecuter::TEvTxResponse::TPtr& ev) {
        // outdated response from dead executer.
        // it this case we should just ignore the event.
        if (ExecuterId != ev->Sender)
            return;

        TTimerGuard timer(this);
        ProcessExecuterResult(ev->Get());
    }

    void HandleExecute(TEvKqpExecuter::TEvExecuterProgress::TPtr& ev) {
        if (QueryState && QueryState->RequestActorId) {
            if (ExecuterId != ev->Sender) {
                return;
            }

            if (QueryState->ReportStats()) {
                NKqpProto::TKqpStatsQuery& stats = *ev->Get()->Record.MutableQueryStats();
                NKqpProto::TKqpStatsQuery executionStats;
                executionStats.Swap(&stats);
                stats = QueryState->QueryStats.ToProto();
                stats.MutableExecutions()->MergeFrom(executionStats.GetExecutions());
                ev->Get()->Record.SetQueryPlan(SerializeAnalyzePlan(stats, QueryState->UserRequestContext->PoolId));
                stats.SetDurationUs((TInstant::Now() - QueryState->StartTime).MicroSeconds());
            }

            LOG_D("Forwarded TEvExecuterProgress to " << QueryState->RequestActorId);
            Send(QueryState->RequestActorId, ev->Release().Release(), 0, QueryState->ProxyRequestId);
        }
    }

    std::optional<std::pair<bool, std::pair<TString, TKqpTempTablesState::TTempTableInfo>>>
    GetTemporaryTableInfo(TKqpPhyTxHolder::TConstPtr tx) {
        if (!tx) {
            return std::nullopt;
        }

        auto optPath = tx->GetSchemeOpTempTablePath();
        if (!optPath) {
            return std::nullopt;
        }
        const auto& [isCreate, path] = *optPath;
        if (isCreate) {
            auto userToken = QueryState ? QueryState->UserToken : TIntrusiveConstPtr<NACLib::TUserToken>();
            return {{true, {JoinPath({path.first, path.second}), {path.second, path.first, userToken}}}};
        }

        auto it = TempTablesState.FindInfo(JoinPath({path.first, path.second}), true);
        if (it == TempTablesState.TempTables.end()) {
            return std::nullopt;
        }

        return {{false, {it->first, {}}}};
    }

    void UpdateTempTablesState() {
        if (!QueryState->PreparedQuery) {
            return;
        }
        auto tx = QueryState->PreparedQuery->GetPhyTxOrEmpty(QueryState->CurrentTx - 1);
        if (!tx) {
            return;
        }
        if (QueryState->IsCreateTableAs()) {
            return;
        }

        auto optInfo = GetTemporaryTableInfo(tx);
        if (optInfo) {
            auto [isCreate, info] = *optInfo;
            if (isCreate) {
                TempTablesState.TempTables[info.first] = info.second;
            } else {
                TempTablesState.TempTables.erase(info.first);
            }
            QueryState->UpdateTempTablesState(TempTablesState);
        }
    }

    void ProcessExecuterResult(TEvKqpExecuter::TEvTxResponse* ev) {
        QueryState->Orbit = std::move(ev->Orbit);

        auto* response = ev->Record.MutableResponse();

        LOG_D("TEvTxResponse, CurrentTx: " << QueryState->CurrentTx
            << "/" << (QueryState->PreparedQuery ? QueryState->PreparedQuery->GetPhysicalQuery().TransactionsSize() : 0)
            << " response.status: " << response->GetStatus());

        ExecuterId = TActorId{};

        auto& executerResults = *response->MutableResult();
        if (executerResults.HasStats()) {
            QueryState->QueryStats.Executions.emplace_back();
            QueryState->QueryStats.Executions.back().Swap(executerResults.MutableStats());
        }

        if (QueryState->TxCtx->TxManager) {
            QueryState->ParticipantNodes = QueryState->TxCtx->TxManager->GetParticipantNodes();
        } else {
            for (auto nodeId : ev->ParticipantNodes) {
                QueryState->ParticipantNodes.emplace(nodeId);
            }
        }

        if (response->GetStatus() != Ydb::StatusIds::SUCCESS) {
            const auto executionType = ev->ExecutionType;

            LOG_D("TEvTxResponse has non-success status, CurrentTx: " << QueryState->CurrentTx
                << " ExecutionType: " << executionType);

            auto status = response->GetStatus();
            TIssues issues;
            IssuesFromMessage(response->GetIssues(), issues);

            // Invalidate query cache on scheme/internal errors
            switch (status) {
                case Ydb::StatusIds::ABORTED: {
                    if (QueryState->TxCtx->TxManager && QueryState->TxCtx->TxManager->BrokenLocks()) {
                        YQL_ENSURE(!issues.Empty());
                    } else if (ev->BrokenLockPathId) {
                        YQL_ENSURE(!QueryState->TxCtx->TxManager);
                        issues.AddIssue(GetLocksInvalidatedIssue(*QueryState->TxCtx, *ev->BrokenLockPathId));
                    } else if (ev->BrokenLockShardId) {
                        YQL_ENSURE(!QueryState->TxCtx->TxManager);
                        issues.AddIssue(GetLocksInvalidatedIssue(*QueryState->TxCtx->ShardIdToTableInfo, *ev->BrokenLockShardId));
                    }
                    break;
                }
                case Ydb::StatusIds::SCHEME_ERROR:
                case Ydb::StatusIds::INTERNAL_ERROR:
                    InvalidateQuery();
                    issues.AddIssue(YqlIssue(TPosition(), TIssuesIds::KIKIMR_QUERY_INVALIDATED,
                        TStringBuilder() << "Query invalidated on scheme/internal error during "
                            << executionType << " execution"));

                    // SCHEME_ERROR during execution is a soft (retriable) error, we abort query execution,
                    // invalidate query cache, and return ABORTED as retriable status.
                    if (status == Ydb::StatusIds::SCHEME_ERROR &&
                        executionType != TEvKqpExecuter::TEvTxResponse::EExecutionType::Scheme)
                    {
                        status = Ydb::StatusIds::ABORTED;
                    }

                    break;

                default:
                    break;
            }

            ReplyQueryError(status, "", MessageFromIssues(issues));
            return;
        }

        YQL_ENSURE(QueryState);

        UpdateTempTablesState();

        LWTRACK(KqpSessionPhyQueryTxResponse, QueryState->Orbit, QueryState->CurrentTx, ev->ResultRowsCount);
        QueryState->QueryData->ClearPrunedParams();

        if (!ev->GetTxResults().empty()) {
            QueryState->QueryData->AddTxResults(QueryState->CurrentTx - 1, std::move(ev->GetTxResults()));
        }
        QueryState->QueryData->AddTxHolders(std::move(ev->GetTxHolders()));

        QueryState->TxCtx->AcceptIncomingSnapshot(ev->Snapshot);

        if (ev->LockHandle) {
            QueryState->TxCtx->Locks.LockHandle = std::move(ev->LockHandle);
        }

        if (!QueryState->TxCtx->TxManager && !MergeLocksWithTxResult(executerResults)) {
            return;
        }

        if (!response->GetIssues().empty()){
            NYql::IssuesFromMessage(response->GetIssues(), QueryState->Issues);
        }

        ExecuteOrDefer();
    }

    void HandleExecute(TEvKqpExecuter::TEvStreamData::TPtr& ev) {
        YQL_ENSURE(QueryState && QueryState->RequestActorId);
        LOG_D("Forwarded TEvStreamData to " << QueryState->RequestActorId);
        TlsActivationContext->Send(ev->Forward(QueryState->RequestActorId));
    }

    void HandleExecute(TEvKqpExecuter::TEvStreamDataAck::TPtr& ev) {
        TlsActivationContext->Send(ev->Forward(ExecuterId));
    }

    void HandleExecute(TEvKqp::TEvAbortExecution::TPtr& ev) {
        auto& msg = ev->Get()->Record;

        TString logMsg = TStringBuilder() << "got TEvAbortExecution in " << CurrentStateFuncName();
        LOG_I(logMsg << ", status: " << NYql::NDqProto::StatusIds_StatusCode_Name(msg.GetStatusCode()) << " send to: " << ExecuterId);

        auto issues = ev->Get()->GetIssues();
        TStringBuilder reason = TStringBuilder() << "Cancelling after " << (AppData()->MonotonicTimeProvider->Now() - QueryState->StartedAt).MilliSeconds() << "ms";
        if (QueryState->CompilationRunning) {
            reason << " during compilation";
        } else if (ExecuterId) {
            reason << " during execution";
        } else {
            reason << " in " << CurrentStateFuncName();
        }
        issues.AddIssue(reason);

        if (ExecuterId) {
            auto abortEv = MakeHolder<TEvKqp::TEvAbortExecution>(msg.GetStatusCode(), issues);
            Send(ExecuterId, abortEv.Release(), IEventHandle::FlagTrackDelivery);
        } else {
            ReplyQueryError(NYql::NDq::DqStatusToYdbStatus(msg.GetStatusCode()), "", MessageFromIssues(issues));
        }
    }

    void Handle(TEvKqpBuffer::TEvError::TPtr& ev) {
        auto& msg = *ev->Get();

        TString logMsg = TStringBuilder() << "got TEvKqpBuffer::TEvError in " << CurrentStateFuncName()
            << ", status: " << NYql::NDqProto::StatusIds_StatusCode_Name(msg.StatusCode) << " send to: " << ExecuterId << " from: " << ev->Sender;

        if (!QueryState || !QueryState->TxCtx || QueryState->TxCtx->BufferActorId != ev->Sender) {
            LOG_E(logMsg <<  ": Ignored error.");
            return;
        } else {
            LOG_W(logMsg);
        }

        if (ExecuterId) {
            Send(ExecuterId, new TEvKqpBuffer::TEvError{msg.StatusCode, std::move(msg.Issues)}, IEventHandle::FlagTrackDelivery);
        } else {
            ReplyQueryError(NYql::NDq::DqStatusToYdbStatus(msg.StatusCode), logMsg, MessageFromIssues(msg.Issues));
        }
    }

    void CollectSystemViewQueryStats(const TKqpQueryStats* stats, TDuration queryDuration,
        const TString& database, ui64 requestUnits)
    {
        auto type = QueryState->GetType();
        switch (type) {
            case NKikimrKqp::QUERY_TYPE_SQL_DML:
            case NKikimrKqp::QUERY_TYPE_PREPARED_DML:
            case NKikimrKqp::QUERY_TYPE_SQL_SCAN:
            case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT:
            case NKikimrKqp::QUERY_TYPE_SQL_SCRIPT_STREAMING:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_CONCURRENT_QUERY:
            case NKikimrKqp::QUERY_TYPE_SQL_GENERIC_SCRIPT: {
                TString text = QueryState->ExtractQueryText();
                if (IsQueryAllowedToLog(text)) {
                    auto userSID = QueryState->UserToken->GetUserSID();
                    CollectQueryStats(TlsActivationContext->AsActorContext(), stats, queryDuration, text,
                        userSID, QueryState->ParametersSize, database, type, requestUnits);
                }
                break;
            }
            default:
                break;
        }
    }

    void FillSystemViewQueryStats(NKikimrKqp::TEvQueryResponse* record) {
        YQL_ENSURE(QueryState);
        auto* stats = &QueryState->QueryStats;

        stats->DurationUs = ((TInstant::Now() - QueryState->StartTime).MicroSeconds());
        stats->WorkerCpuTimeUs = (QueryState->GetCpuTime().MicroSeconds());
        if (const auto continueTime = QueryState->ContinueTime) {
            stats->QueuedTimeUs = (continueTime - QueryState->StartTime).MicroSeconds();
        }
        if (QueryState->CompileResult) {
            stats->Compilation.emplace();
            stats->Compilation->FromCache = (QueryState->CompileStats.FromCache);
            stats->Compilation->DurationUs = (QueryState->CompileStats.DurationUs);
            stats->Compilation->CpuTimeUs = (QueryState->CompileStats.CpuTimeUs);
        }

        if (IsExecuteAction(QueryState->GetAction())) {
            auto ru = CalcRequestUnit(*stats);

            if (record != nullptr) {
                record->SetConsumedRu(ru);
            }

            auto now = TInstant::Now();
            auto queryDuration = now - QueryState->StartTime;
            CollectSystemViewQueryStats(stats, queryDuration, QueryState->GetDatabase(), ru);
        }
    }

    void FillStats(NKikimrKqp::TEvQueryResponse* record) {
        YQL_ENSURE(QueryState);
        // workaround to ensure that request was not transfered to worker.
        if (WorkerId || !QueryState->RequestEv) {
            return;
        }

        FillSystemViewQueryStats(record);

        auto *response = record->MutableResponse();
        auto requestInfo = TKqpRequestInfo(QueryState->UserRequestContext->TraceId, SessionId);

        if (IsExecuteAction(QueryState->GetAction())) {
            auto queryDuration = TDuration::MicroSeconds(QueryState->QueryStats.DurationUs);
            SlowLogQuery(TlsActivationContext->AsActorContext(), Config.Get(), requestInfo, queryDuration,
                record->GetYdbStatus(), QueryState->UserToken, QueryState->ParametersSize, record,
                [this]() { return this->QueryState->ExtractQueryText(); });
        }

        if (QueryState->ReportStats()) {
            auto stats = QueryState->QueryStats.ToProto();
            if (QueryState->GetStatsMode() >= Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL) {
                response->SetQueryPlan(SerializeAnalyzePlan(stats, QueryState->UserRequestContext->PoolId));
                if (QueryState->CompileResult) {
                    auto preparedQuery = QueryState->CompileResult->PreparedQuery;
                    if (preparedQuery) {
                        response->SetQueryAst(preparedQuery->GetPhysicalQuery().GetQueryAst());
                    }
                }
            }
            response->MutableQueryStats()->Swap(&stats);
        }
    }

    template<class TEvRecord>
    void AddTrailingInfo(TEvRecord& record) {
        if (ShutdownState) {
            LOG_D("session is closing, set trailing metadata to request session shutdown");
            record.SetWorkerIsClosing(true);
        }
    }

    void FillTxInfo(NKikimrKqp::TQueryResponse* response) {
        YQL_ENSURE(QueryState);
        response->MutableTxMeta()->set_id(QueryState->TxId.GetValue().GetHumanStr());

        if (QueryState->TxCtx) {
            auto txInfo = QueryState->TxCtx->GetInfo();
            LOG_I("txInfo "
                << " Status: " << txInfo.Status
                << " Kind: " << txInfo.Kind
                << " TotalDuration: " << txInfo.TotalDuration.SecondsFloat()*1e3
                << " ServerDuration: " << txInfo.ServerDuration.SecondsFloat()*1e3
                << " QueriesCount: " << txInfo.QueriesCount);
            Counters->ReportTransaction(Settings.DbCounters, txInfo);
        }
    }

    void UpdateQueryExecutionCountes() {
        auto now = TInstant::Now();
        auto queryDuration = now - QueryState->StartTime;

        Counters->ReportQueryLatency(Settings.DbCounters, QueryState->GetAction(), queryDuration);

        if (QueryState->MaxReadType == ETableReadType::FullScan) {
            Counters->ReportQueryWithFullScan(Settings.DbCounters);
        } else if (QueryState->MaxReadType == ETableReadType::Scan) {
            Counters->ReportQueryWithRangeScan(Settings.DbCounters);
        }

        auto& stats = QueryState->QueryStats;

        ui32 affectedShardsCount = 0;
        ui64 readBytesCount = 0;
        ui64 readRowsCount = 0;
        for (const auto& exec : stats.GetExecutions()) {
            for (const auto& table : exec.GetTables()) {
                affectedShardsCount = std::max(affectedShardsCount, table.GetAffectedPartitions());
                readBytesCount += table.GetReadBytes();
                readRowsCount += table.GetReadRows();
            }
        }

        Counters->ReportQueryAffectedShards(Settings.DbCounters, affectedShardsCount);
        Counters->ReportQueryReadRows(Settings.DbCounters, readRowsCount);
        Counters->ReportQueryReadBytes(Settings.DbCounters, readBytesCount);
        Counters->ReportQueryReadSets(Settings.DbCounters, stats.ReadSetsCount);
        Counters->ReportQueryMaxShardReplySize(Settings.DbCounters, stats.MaxShardReplySize);
        Counters->ReportQueryMaxShardProgramSize(Settings.DbCounters, stats.MaxShardProgramSize);
    }

    void ReplySuccess(bool addEmptyResultSet = false) {
        YQL_ENSURE(QueryState);
        auto resEv = std::make_unique<TEvKqp::TEvQueryResponse>();
        auto *record = &resEv->Record;
        auto *response = record->MutableResponse();

        if (QueryState->CompileResult) {
            AddQueryIssues(*response, QueryState->CompileResult->Issues);
        }

        AddQueryIssues(*response, QueryState->Issues);

        if (addEmptyResultSet) {
            response->AddYdbResults();
        }

        FillStats(record);

        if (QueryState->CommandTagName) {
            auto *extraInfo = response->MutableExtraInfo();
            auto* pgExtraInfo = extraInfo->MutablePgInfo();
            pgExtraInfo->SetCommandTag(*QueryState->CommandTagName);
        }

        if (QueryState->TxCtx) {
            QueryState->TxCtx->OnEndQuery();
        }

        if (QueryState->Commit) {
            ResetTxState();
            Transactions.ReleaseTransaction(QueryState->TxId.GetValue());
            QueryState->TxId.Reset();
        }

        FillTxInfo(response);

        UpdateQueryExecutionCountes();

        bool replyQueryId = false;
        bool replyQueryParameters = false;
        bool replyTopicOperations = false;

        switch (QueryState->GetAction()) {
            case NKikimrKqp::QUERY_ACTION_PREPARE:
                replyQueryId = true;
                replyQueryParameters = true;
                break;

            case NKikimrKqp::QUERY_ACTION_EXECUTE:
                replyQueryParameters = replyQueryId = QueryState->GetQueryKeepInCache();
                break;

            case NKikimrKqp::QUERY_ACTION_PARSE:
            case NKikimrKqp::QUERY_ACTION_VALIDATE:
                replyQueryParameters = true;
                break;

            case NKikimrKqp::QUERY_ACTION_TOPIC:
                replyTopicOperations = true;
                break;

            default:
                break;
        }

        if (replyQueryParameters) {
            YQL_ENSURE(QueryState->PreparedQuery);
            for (auto& param : QueryState->GetResultParams()) {
                *response->AddQueryParameters() = param;
            }
        }

        if (replyQueryId) {
            TString queryId;
            if (QueryState->CompileResult) {
                queryId = QueryState->CompileResult->Uid;
            }
            response->SetPreparedQuery(queryId);
        }

        if (replyTopicOperations) {
            if (HasTopicWriteId()) {
                auto* w = response->MutableTopicOperations();
                auto* writeId = w->MutableWriteId();
                writeId->SetNodeId(SelfId().NodeId());
                writeId->SetKeyId(GetTopicWriteId());
            }
        }

        response->SetQueryDiagnostics(QueryState->ReplayMessage);

        // Result for scan query is sent directly to target actor.
        Y_ABORT_UNLESS(response->GetArena());
        if (QueryState->PreparedQuery) {
            auto& phyQuery = QueryState->PreparedQuery->GetPhysicalQuery();
            size_t trailingResultsCount = 0;
            for (size_t i = 0; i < phyQuery.ResultBindingsSize(); ++i) {
                if (QueryState->IsStreamResult()) {
                    if (QueryState->QueryData->HasTrailingTxResult(phyQuery.GetResultBindings(i))) {
                        auto ydbResult = QueryState->QueryData->GetYdbTxResult(
                            phyQuery.GetResultBindings(i), response->GetArena(), {});

                        YQL_ENSURE(ydbResult);
                        ++trailingResultsCount;
                        YQL_ENSURE(trailingResultsCount <= 1);
                        response->AddYdbResults()->Swap(ydbResult);
                    }

                    continue;
                }

                TMaybe<ui64> effectiveRowsLimit = FillSettings.RowsLimitPerWrite;
                if (QueryState->PreparedQuery->GetResults(i).GetRowsLimit()) {
                    effectiveRowsLimit = QueryState->PreparedQuery->GetResults(i).GetRowsLimit();
                }
                auto* ydbResult = QueryState->QueryData->GetYdbTxResult(phyQuery.GetResultBindings(i), response->GetArena(), effectiveRowsLimit);
                response->AddYdbResults()->Swap(ydbResult);
            }
        }

        resEv->Record.SetYdbStatus(Ydb::StatusIds::SUCCESS);
        LOG_D("Create QueryResponse for action: " << QueryState->GetAction() << " with SUCCESS status");

        QueryResponse = std::move(resEv);


        ProcessNextStatement();
    }

    void ProcessNextStatement() {
        if (ExecuteNextStatementPart()) {
            return;
        }

        if (QueryState->ProcessingLastStatement()) {
            Cleanup();
            return;
        }
        QueryState->PrepareNextStatement();
        CompileStatement();
    }

    void ReplyQueryCompileError() {
        YQL_ENSURE(QueryState);
        QueryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        FillCompileStatus(QueryState->CompileResult, QueryResponse->Record);

        auto txId = TTxId();
        if (QueryState->HasTxControl()) {
            const auto& txControl = QueryState->GetTxControl();
            if (txControl.tx_selector_case() == Ydb::Table::TransactionControl::kTxId) {
                txId = TTxId::FromString(txControl.tx_id());
            }
        }

        LOG_W("ReplyQueryCompileError, status " << QueryState->CompileResult->Status << " remove tx with tx_id: " << txId.GetHumanStr());
        if (auto ctx = Transactions.ReleaseTransaction(txId)) {
            ctx->Invalidate();
            if (!ctx->BufferActorId) {
                Transactions.AddToBeAborted(std::move(ctx));
            } else {
                TerminateBufferActor(ctx);
            }
        }

        auto* record = &QueryResponse->Record;
        FillTxInfo(record->MutableResponse());
        record->SetConsumedRu(1);

        Cleanup(IsFatalError(record->GetYdbStatus()));
    }

    void ReplySplitError(TEvKqp::TEvSplitResponse* ev) {
        QueryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        auto& record = QueryResponse->Record;

        record.SetYdbStatus(ev->Status);
        auto& response = *record.MutableResponse();
        AddQueryIssues(response, ev->Issues);

        auto txId = TTxId();
        if (auto ctx = Transactions.ReleaseTransaction(txId)) {
            ctx->Invalidate();
            if (!ctx->BufferActorId) {
                Transactions.AddToBeAborted(std::move(ctx));
            } else {
                TerminateBufferActor(ctx);
            }
        }

        FillTxInfo(record.MutableResponse());

        Cleanup(false);
    }

    void ReplyProcessError(const TEvKqp::TEvQueryRequest::TPtr& request, Ydb::StatusIds::StatusCode ydbStatus,
            const TString& message)
    {
        ui64 proxyRequestId = request->Cookie;
        LOG_W("Reply query error, msg: " << message << " proxyRequestId: " << proxyRequestId);
        auto response = std::make_unique<TEvKqp::TEvQueryResponse>();
        response->Record.SetYdbStatus(ydbStatus);
        auto issue = MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, message);
        NYql::TIssues issues;
        issues.AddIssue(issue);
        NYql::IssuesToMessage(issues, response->Record.MutableResponse()->MutableQueryIssues());
        AddTrailingInfo(response->Record);

        NDataIntegrity::LogIntegrityTrails(
            request->Get()->GetTraceId(),
            request->Get()->GetAction(),
            request->Get()->GetType(),
            response,
            TlsActivationContext->AsActorContext()
        );

        Send(request->Sender, response.release(), 0, proxyRequestId);
    }

    void ReplyBusy(TEvKqp::TEvQueryRequest::TPtr& ev) {
        ReplyProcessError(ev, Ydb::StatusIds::SESSION_BUSY, "Pending previous query completion");
    }

    static bool IsFatalError(const Ydb::StatusIds::StatusCode status) {
        switch (status) {
            case Ydb::StatusIds::INTERNAL_ERROR:
            case Ydb::StatusIds::BAD_SESSION:
                return true;
            default:
                return false;
        }
    }

    void Reply() {
        Y_ABORT_UNLESS(QueryState);
        YQL_ENSURE(Counters);

        auto& record = QueryResponse->Record;
        auto& response = *record.MutableResponse();
        const auto& status = record.GetYdbStatus();

        AddTrailingInfo(record);

        if (QueryState->KeepSession) {
            response.SetSessionId(SessionId);
        }

        if (status == Ydb::StatusIds::SUCCESS) {
            if (QueryState) {
                if (QueryState->KqpSessionSpan) {
                    QueryState->KqpSessionSpan.EndOk();
                }
                LWTRACK(KqpSessionReplySuccess, QueryState->Orbit, record.GetArena() ? record.GetArena()->SpaceUsed() : 0);
            }
        } else {
            if (QueryState) {
                if (QueryState->KqpSessionSpan) {
                    QueryState->KqpSessionSpan.EndError(response.DebugString());
                }
                LWTRACK(KqpSessionReplyError, QueryState->Orbit, TStringBuilder() << status);
            }
        }

        Counters->ReportResponseStatus(Settings.DbCounters, record.ByteSize(), record.GetYdbStatus());
        for (auto& issue : record.GetResponse().GetQueryIssues()) {
            Counters->ReportIssues(Settings.DbCounters, CachedIssueCounters, issue);
        }

        NDataIntegrity::LogIntegrityTrails(
            QueryState->UserRequestContext->TraceId,
            QueryState->GetAction(),
            QueryState->GetType(),
            QueryResponse,
            TlsActivationContext->AsActorContext()
        );

        Send<ESendingType::Tail>(QueryState->Sender, QueryResponse.release(), 0, QueryState->ProxyRequestId);
        LOG_D("Sent query response back to proxy, proxyRequestId: " << QueryState->ProxyRequestId
            << ", proxyId: " << QueryState->Sender.ToString());

        if (IsFatalError(status)) {
            LOG_N("SessionActor destroyed due to " << status);
            Counters->ReportSessionActorClosedError(Settings.DbCounters);
        }
    }

    void FillCompileStatus(const TKqpCompileResult::TConstPtr& compileResult,
        NKikimrKqp::TEvQueryResponse& ev)
    {
        ev.SetYdbStatus(compileResult->Status);

        auto& response = *ev.MutableResponse();
        AddQueryIssues(response, compileResult->Issues);

        if (compileResult->Status == Ydb::StatusIds::SUCCESS) {
            response.SetPreparedQuery(compileResult->Uid);

            auto& preparedQuery = compileResult->PreparedQuery;
            for (auto& param : QueryState->GetResultParams()) {
                *response.AddQueryParameters() = param;
            }

            response.SetQueryPlan(preparedQuery->GetPhysicalQuery().GetQueryPlan());
            response.SetQueryAst(preparedQuery->GetPhysicalQuery().GetQueryAst());
            response.SetQueryDiagnostics(QueryState->ReplayMessage);

            const auto& phyQuery = QueryState->PreparedQuery->GetPhysicalQuery();
            FillColumnsMeta(phyQuery, response);
        } else {
            if (compileResult->Status == Ydb::StatusIds::TIMEOUT && QueryState->QueryDeadlines.CancelAt) {
                // The compile timeout cause cancelation execution of request.
                // So in case of cancel after we can reply with canceled status
                ev.SetYdbStatus(Ydb::StatusIds::CANCELLED);
            }

            auto& preparedQuery = compileResult->PreparedQuery;
            if (preparedQuery && QueryState->ReportStats() && QueryState->GetStatsMode() >= Ydb::Table::QueryStatsCollection::STATS_COLLECTION_FULL) {
                response.SetQueryAst(preparedQuery->GetPhysicalQuery().GetQueryAst());
            }
        }
    }

    void HandleReady(TEvKqp::TEvCloseSessionRequest::TPtr&) {
        LOG_I("Session closed due to explicit close event");
        Counters->ReportSessionActorClosedRequest(Settings.DbCounters);
        CleanupAndPassAway();
    }

    void HandleExecute(TEvKqp::TEvCloseSessionRequest::TPtr&) {
        YQL_ENSURE(QueryState);
        QueryState->KeepSession = false;
    }

    void HandleCleanup(TEvKqp::TEvCloseSessionRequest::TPtr&) {
        YQL_ENSURE(CleanupCtx);
        if (!CleanupCtx->Final) {
            YQL_ENSURE(QueryState);
            QueryState->KeepSession = false;
        }
    }

    void Handle(TEvKqp::TEvInitiateSessionShutdown::TPtr& ev) {
        if (!ShutdownState) {
            LOG_N("Started session shutdown");
            ShutdownState = TSessionShutdownState(ev->Get()->SoftTimeoutMs, ev->Get()->HardTimeoutMs);
            ScheduleNextShutdownTick();
        }
    }

    void ScheduleNextShutdownTick() {
        Schedule(TDuration::MilliSeconds(ShutdownState->GetNextTickMs()), new TEvKqp::TEvContinueShutdown());
    }

    void Handle(TEvKqp::TEvContinueShutdown::TPtr&) {
        YQL_ENSURE(ShutdownState);
        ShutdownState->MoveToNextState();
        if (ShutdownState->HardTimeoutReached()) {
            LOG_N("Reached hard shutdown timeout");
            Send(SelfId(), new TEvKqp::TEvCloseSessionRequest());
        } else {
            ScheduleNextShutdownTick();
            LOG_I("Schedule next shutdown tick");
        }
    }

    void SendRollbackRequest(TKqpTransactionContext* txCtx) {
        if (QueryState) {
            LWTRACK(KqpSessionSendRollback, QueryState->Orbit, QueryState->CurrentTx);
        }

        auto allocPtr = std::make_shared<TTxAllocatorState>(AppData()->FunctionRegistry,
            AppData()->TimeProvider, AppData()->RandomProvider);
        auto request = PreparePhysicalRequest(nullptr, allocPtr);

        request.LocksOp = ELocksOp::Rollback;

        if (!txCtx->TxManager) {
            // Should tx with empty LocksMap be aborted?
            for (auto& [lockId, lock] : txCtx->Locks.LocksMap) {
                auto dsLock = ExtractLock(lock.GetValueRef(txCtx->Locks.LockType));
                request.DataShardLocks[dsLock.GetDataShard()].emplace_back(dsLock);
            }
        }

        SendToExecuter(txCtx, std::move(request), true);
    }

    void ResetTxState() {
        if (QueryState->TxCtx) {
            TerminateBufferActor(QueryState->TxCtx);
            QueryState->TxCtx->ClearDeferredEffects();
            QueryState->TxCtx->Locks.Clear();
            QueryState->TxCtx->TxManager.reset();
            QueryState->TxCtx->Finish();
        }
    }

    void CleanupAndPassAway() {
        Cleanup(true);
    }

    void Cleanup(bool isFinal = false) {
        isFinal = isFinal || QueryState && !QueryState->KeepSession;

        if (QueryState && QueryState->TxCtx) {
            auto& txCtx = QueryState->TxCtx;
            if (txCtx->IsInvalidated()) {
                if (FastQueryExecutor) {
                    FastQueryExecutor = {};
                } else {
                    if (!txCtx->BufferActorId) {
                        Transactions.AddToBeAborted(txCtx);
                    } else {
                        TerminateBufferActor(txCtx);
                    }
                }
                Transactions.ReleaseTransaction(QueryState->TxId.GetValue());
            }
            DiscardPersistentSnapshot(txCtx->SnapshotHandle);
        }

        if (isFinal && QueryState)
            TerminateBufferActor(QueryState->TxCtx);

        if (isFinal)
            Counters->ReportSessionActorClosedRequest(Settings.DbCounters);

        if (isFinal) {
            // no longer intrested in any compilation responses
            CompilationCookie->store(false);
        }

        if (isFinal) {
            LOG_T("Cleanup3");
            Transactions.FinalCleanup();
            Counters->ReportTxAborted(Settings.DbCounters, Transactions.ToBeAbortedSize());
        }

        auto workerId = WorkerId;
        if (WorkerId) {
            auto ev = std::make_unique<TEvKqp::TEvCloseSessionRequest>();
            ev->Record.MutableRequest()->SetSessionId(SessionId);
            Send(*WorkerId, ev.release());
            WorkerId.reset();

            YQL_ENSURE(!CleanupCtx);
            CleanupCtx.reset(new TKqpCleanupCtx);
            CleanupCtx->IsWaitingForWorkerToClose = true;
        }

        if (Transactions.ToBeAbortedSize()) {
            if (!CleanupCtx)
                CleanupCtx.reset(new TKqpCleanupCtx);
            CleanupCtx->Final = isFinal;
            CleanupCtx->TransactionsToBeAborted = Transactions.ReleaseToBeAborted();
            SendRollbackRequest(CleanupCtx->TransactionsToBeAborted.front().Get());
        }

        if (QueryState && QueryState->PoolHandlerActor) {
            if (!CleanupCtx) {
                CleanupCtx.reset(new TKqpCleanupCtx);
            }
            CleanupCtx->Final = isFinal;
            CleanupCtx->IsWaitingForWorkloadServiceCleanup = true;

            const auto& stats = QueryState->QueryStats;
            auto event = std::make_unique<NWorkload::TEvCleanupRequest>(
                QueryState->UserRequestContext->DatabaseId, SessionId, QueryState->UserRequestContext->PoolId,
                TDuration::MicroSeconds(stats.DurationUs), TDuration::MicroSeconds(stats.WorkerCpuTimeUs)
            );

            auto forwardId = MakeKqpWorkloadServiceId(SelfId().NodeId());
            Send(new IEventHandle(*QueryState->PoolHandlerActor, SelfId(), event.release(), IEventHandle::FlagForwardOnNondelivery, 0, &forwardId));
            QueryState->PoolHandlerActor = Nothing();
        }

        if (QueryState && QueryState->IsSingleNodeExecution()) {
            Counters->TotalSingleNodeReqCount->Inc();
            if (!QueryState->IsLocalExecution(SelfId().NodeId())) {
                Counters->NonLocalSingleNodeReqCount->Inc();
            }
        }

        LOG_I("Cleanup start, isFinal: " << isFinal << " CleanupCtx: " << bool{CleanupCtx}
            << " TransactionsToBeAborted.size(): " << (CleanupCtx ? CleanupCtx->TransactionsToBeAborted.size() : 0)
            << " WorkerId: " << (workerId ? *workerId : TActorId())
            << " WorkloadServiceCleanup: " << (CleanupCtx ? CleanupCtx->IsWaitingForWorkloadServiceCleanup : false));
        if (CleanupCtx) {
            Become(&TKqpSessionActor::CleanupState);
        } else {
            EndCleanup(isFinal);
        }
    }

    void HandleCleanup(TEvKqp::TEvCloseSessionResponse::TPtr&) {
        CleanupCtx->IsWaitingForWorkerToClose = false;
        if (CleanupCtx->CleanupFinished()) {
            EndCleanup(CleanupCtx->Final);
        }
    }

    void HandleNoop(TEvents::TEvUndelivered::TPtr& ev) {
        // outdated TEvUndelivered from another executer.
        // it this case we should just ignore the event.
        Y_ENSURE(ExecuterId != ev->Sender);
    }

    void HandleCleanup(TEvKqpExecuter::TEvTxResponse::TPtr& ev) {
        if (ev->Sender != ExecuterId) {
            return;
        }
        if (QueryState) {
            QueryState->Orbit = std::move(ev->Get()->Orbit);
        }

        auto& response = ev->Get()->Record.GetResponse();
        if (response.GetStatus() != Ydb::StatusIds::SUCCESS) {
            TIssues issues;
            IssuesFromMessage(response.GetIssues(), issues);
            LOG_E("Failed to cleanup: " << issues.ToString());
            EndCleanup(CleanupCtx->Final);
            return;
        }

        YQL_ENSURE(CleanupCtx);
        CleanupCtx->TransactionsToBeAborted.pop_front();
        if (CleanupCtx->TransactionsToBeAborted.size()) {
            SendRollbackRequest(CleanupCtx->TransactionsToBeAborted.front().Get());
        } else if (CleanupCtx->CleanupFinished()) {
            EndCleanup(CleanupCtx->Final);
        }
    }

    void HandleCleanup(NWorkload::TEvCleanupResponse::TPtr& ev) {
        YQL_ENSURE(CleanupCtx);
        CleanupCtx->IsWaitingForWorkloadServiceCleanup = false;

        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS && ev->Get()->Status != Ydb::StatusIds::NOT_FOUND) {
            LOG_E("Failed to cleanup workload service " << ev->Get()->Status << ": " << ev->Get()->Issues.ToOneLineString());
        }

        if (CleanupCtx->CleanupFinished()) {
            EndCleanup(CleanupCtx->Final);
        }
    }

    void EndCleanup(bool isFinal) {
        LOG_D("EndCleanup, isFinal: " << isFinal);

        if (QueryResponse)
            Reply();

        if (CleanupCtx)
            Counters->ReportSessionActorCleanupLatency(Settings.DbCounters, TInstant::Now() - CleanupCtx->Start);

        if (isFinal) {
            auto userToken = QueryState ? QueryState->UserToken : TIntrusiveConstPtr<NACLib::TUserToken>();
            Become(&TKqpSessionActor::FinalCleanupState);

            LOG_D("Cleanup temp tables: " << TempTablesState.TempTables.size());
            auto tempTablesManager = CreateKqpTempTablesManager(
                std::move(TempTablesState), std::move(userToken), SelfId(), Settings.Database);

            RegisterWithSameMailbox(tempTablesManager);
            return;
        } else {
            CleanupCtx.reset();
            bool doNotKeepSession = QueryState && !QueryState->KeepSession;
            QueryState.reset();
            if (doNotKeepSession) {
                // TEvCloseSessionRequest was received in final=false CleanupState, so actor should rerun Cleanup with final=true
                CleanupAndPassAway();
            } else {
                Become(&TKqpSessionActor::ReadyState);
            }
        }
        ExecuterId = TActorId{};
    }

    template<class T>
    static google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage> MessageFromIssues(const T& issues) {
        google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage> issueMessage;
        for (const auto& i : issues) {
            IssueToMessage(i, issueMessage.Add());
        }
        return issueMessage;
    }

    void ReplyQueryError(Ydb::StatusIds::StatusCode ydbStatus,
        const TString& message, std::optional<google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>> issues = {})
    {
        LOG_W("Create QueryResponse for error on request, msg: " << message);

        QueryResponse = std::make_unique<TEvKqp::TEvQueryResponse>();
        QueryResponse->Record.SetYdbStatus(ydbStatus);

        auto* response = QueryResponse->Record.MutableResponse();

        Y_ENSURE(QueryState);
        if (QueryState->CompileResult) {
            AddQueryIssues(*response, QueryState->CompileResult->Issues);
        }

        FillStats(&QueryResponse->Record);

        if (issues) {
            for (auto& i : *issues) {
                response->AddQueryIssues()->Swap(&i);
            }
        }

        if (message) {
            IssueToMessage(TIssue{message}, response->AddQueryIssues());
        }

        if (QueryState->TxCtx) {
            QueryState->TxCtx->OnEndQuery();
            QueryState->TxCtx->Invalidate();
        }

        FillTxInfo(response);

        ExecuterId = TActorId{};
        Cleanup(IsFatalError(ydbStatus));
    }

    void Handle(TEvKqp::TEvCancelQueryRequest::TPtr& ev) {
        {
            auto abort = MakeHolder<NYql::NDq::TEvDq::TEvAbortExecution>(NYql::NDqProto::StatusIds::CANCELLED, "Request was canceled");
            Send(SelfId(), abort.Release());
        }

        {
            auto resp = MakeHolder<TEvKqp::TEvCancelQueryResponse>();
            resp->Record.SetStatus(Ydb::StatusIds::SUCCESS);
            Send(ev->Sender, resp.Release(), 0, ev->Cookie);
        }
    }

    void HandleFinalCleanup(TEvents::TEvGone::TPtr&) {
        auto lifeSpan = TInstant::Now() - CreationTime;
        Counters->ReportSessionActorFinished(Settings.DbCounters, lifeSpan);
        Counters->ReportQueriesPerSessionActor(Settings.DbCounters, QueryId);

        auto closeEv = std::make_unique<TEvKqp::TEvCloseSessionResponse>();
        closeEv->Record.SetStatus(Ydb::StatusIds::SUCCESS);
        closeEv->Record.MutableResponse()->SetSessionId(SessionId);
        closeEv->Record.MutableResponse()->SetClosed(true);
        Send(Owner, closeEv.release());

        LOG_D("Session actor destroyed");
        PassAway();
    }

    void HandleFinalCleanup(TEvKqp::TEvQueryRequest::TPtr& ev) {
        ReplyProcessError(ev, Ydb::StatusIds::BAD_SESSION, "Session is under shutdown");
    }

    STFUNC(ReadyState) {
        try {
            switch (ev->GetTypeRewrite()) {
                // common event handles for all states.
                hFunc(TEvKqp::TEvInitiateSessionShutdown, Handle);
                hFunc(TEvKqp::TEvContinueShutdown, Handle);
                hFunc(TEvKqpSnapshot::TEvCreateSnapshotResponse, Handle);
                hFunc(TEvKqp::TEvQueryRequest, Handle);

                hFunc(TEvKqp::TEvCloseSessionRequest, HandleReady);
                hFunc(TEvKqp::TEvCancelQueryRequest, Handle);

                // forgotten messages from previous aborted request
                hFunc(TEvKqp::TEvCompileResponse, HandleNoop);
                hFunc(TEvKqp::TEvSplitResponse, HandleNoop);
                hFunc(TEvKqpExecuter::TEvTxResponse, HandleNoop);
                hFunc(TEvKqpExecuter::TEvExecuterProgress, HandleNoop)
                hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, HandleNoop);
                hFunc(TEvents::TEvUndelivered, HandleNoop);
                hFunc(NWorkload::TEvContinueRequest, HandleNoop);
                // message from KQP proxy in case of our reply just after kqp proxy timer tick
                hFunc(NYql::NDq::TEvDq::TEvAbortExecution, HandleNoop);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, HandleNoop);

            default:
                UnexpectedEvent("ReadyState", ev);
            }
        } catch (const TRequestFail& ex) {
            ReplyQueryError(ex.Status, ex.what(), ex.Issues);
        } catch (const yexception& ex) {
            InternalError(ex.what());
        } catch (const TMemoryLimitExceededException&) {
            ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR,
                BuildMemoryLimitExceptionMessage());
        }
    }

    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                // common event handles for all states.
                hFunc(TEvKqp::TEvInitiateSessionShutdown, Handle);
                hFunc(TEvKqp::TEvContinueShutdown, Handle);
                hFunc(TEvKqpSnapshot::TEvCreateSnapshotResponse, Handle);
                hFunc(TEvKqp::TEvQueryRequest, Handle);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, Handle);

                hFunc(TEvents::TEvUndelivered, Handle);
                hFunc(NWorkload::TEvContinueRequest, Handle);
                hFunc(TEvKqpExecuter::TEvTxResponse, HandleExecute);
                hFunc(TEvKqpExecuter::TEvExecuterProgress, HandleExecute)

                hFunc(TEvKqpExecuter::TEvStreamData, HandleExecute);
                hFunc(TEvKqpExecuter::TEvStreamDataAck, HandleExecute);

                hFunc(NYql::NDq::TEvDq::TEvAbortExecution, HandleExecute);
                hFunc(TEvKqpBuffer::TEvError, Handle);

                hFunc(TEvKqp::TEvCloseSessionRequest, HandleExecute);
                hFunc(NGRpcService::TEvClientLost, HandleClientLost);
                hFunc(TEvKqp::TEvCancelQueryRequest, Handle);

                // forgotten messages from previous aborted request
                hFunc(TEvKqp::TEvCompileResponse, Handle);
                hFunc(TEvKqp::TEvParseResponse, Handle);
                hFunc(TEvKqp::TEvSplitResponse, Handle);
                hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);

                // always come from WorkerActor
                hFunc(TEvKqp::TEvQueryResponse, ForwardResponse);
            default:
                UnexpectedEvent("ExecuteState", ev);
            }
        } catch (const TRequestFail& ex) {
            ReplyQueryError(ex.Status, ex.what(), ex.Issues);
        } catch (const yexception& ex) {
            InternalError(ex.what());
        } catch (const TMemoryLimitExceededException&) {
            ReplyQueryError(Ydb::StatusIds::UNDETERMINED,
                BuildMemoryLimitExceptionMessage());
        }
    }

    // optional -- only if there were any TransactionsToBeAborted
    STATEFN(CleanupState) {
        try {
            switch (ev->GetTypeRewrite()) {
                // common event handles for all states.
                hFunc(TEvKqp::TEvInitiateSessionShutdown, Handle);
                hFunc(TEvKqp::TEvContinueShutdown, Handle);
                hFunc(TEvKqpSnapshot::TEvCreateSnapshotResponse, Handle);
                hFunc(TEvKqp::TEvQueryRequest, Handle);

                hFunc(TEvKqpExecuter::TEvTxResponse, HandleCleanup);
                hFunc(NWorkload::TEvCleanupResponse, HandleCleanup);

                hFunc(TEvKqp::TEvCloseSessionRequest, HandleCleanup);
                hFunc(NGRpcService::TEvClientLost, HandleNoop);
                hFunc(TEvKqp::TEvCancelQueryRequest, HandleNoop);

                // forgotten messages from previous aborted request
                hFunc(TEvKqp::TEvCompileResponse, HandleNoop);
                hFunc(TEvKqp::TEvSplitResponse, HandleNoop);
                hFunc(NYql::NDq::TEvDq::TEvAbortExecution, HandleNoop);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, HandleNoop);
                hFunc(TEvents::TEvUndelivered, HandleNoop);
                hFunc(TEvTxUserProxy::TEvAllocateTxIdResult, HandleNoop);
                hFunc(TEvKqpExecuter::TEvStreamData, HandleNoop);
                hFunc(NWorkload::TEvContinueRequest, HandleNoop);

                // always come from WorkerActor
                hFunc(TEvKqp::TEvCloseSessionResponse, HandleCleanup);
                hFunc(TEvKqp::TEvQueryResponse, HandleNoop);
                hFunc(TEvKqpExecuter::TEvExecuterProgress, HandleNoop)
            default:
                UnexpectedEvent("CleanupState", ev);
            }
        } catch (const yexception& ex) {
            InternalError(ex.what());
        } catch (const TMemoryLimitExceededException&) {
            ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR,
                BuildMemoryLimitExceptionMessage());
        }
    }

    STATEFN(FinalCleanupState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvents::TEvGone, HandleFinalCleanup);
                hFunc(TEvents::TEvUndelivered, HandleNoop);
                hFunc(TEvKqpSnapshot::TEvCreateSnapshotResponse, Handle);
                hFunc(NWorkload::TEvContinueRequest, HandleNoop);
                hFunc(TEvKqp::TEvQueryRequest, HandleFinalCleanup);
            }
        } catch (const yexception& ex) {
            InternalError(ex.what());
        } catch (const TMemoryLimitExceededException&) {
            ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR,
                BuildMemoryLimitExceptionMessage());
        }
    }

private:

    TString CurrentStateFuncName() const {
        const auto& func = CurrentStateFunc();
        if (func == &TThis::ReadyState) {
            return "ReadyState";
        } else if (func == &TThis::ExecuteState) {
            return "ExecuteState";
        } else if (func == &TThis::CleanupState) {
            return "CleanupState";
        } else {
            return "unknown state";
        }
    }

    void UnexpectedEvent(const TString& state, TAutoPtr<NActors::IEventHandle>& ev) {
        InternalError(TStringBuilder() << "TKqpSessionActor in state " << state << " received unexpected event " <<
                ev->GetTypeName() << Sprintf("(0x%08" PRIx32 ")", ev->GetTypeRewrite()) << " sender: " << ev->Sender);
    }

    void InternalError(const TString& message) {
        LOG_E("Internal error, message: " << message);
        if (QueryState) {
            ReplyQueryError(Ydb::StatusIds::INTERNAL_ERROR, message);
        } else {
            CleanupAndPassAway();
        }
    }

    TString BuildMemoryLimitExceptionMessage() const {
        if (QueryState && QueryState->TxCtx) {
            return TStringBuilder() << "Memory limit exception at " << CurrentStateFuncName()
                << ", current limit is " << QueryState->TxCtx->TxAlloc->Alloc->GetLimit() << " bytes.";
        } else {
            return TStringBuilder() << "Memory limit exception at " << CurrentStateFuncName();
        }
    }

    void ProcessTopicOps(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        YQL_ENSURE(ev->Get()->Request);
        if (ev->Get()->Request->Cookie < QueryId) {
            return;
        }

        NSchemeCache::TSchemeCacheNavigate* response = ev->Get()->Request.Get();

        Ydb::StatusIds_StatusCode status;
        TString message;

        if (QueryState->IsAccessDenied(*response, message)) {
            ythrow TRequestFail(Ydb::StatusIds::UNAUTHORIZED) << message;
        }
        if (QueryState->HasErrors(*response, message)) {
            ythrow TRequestFail(Ydb::StatusIds::SCHEME_ERROR) << message;
        }

        QueryState->TopicOperations.ProcessSchemeCacheNavigate(response->ResultSet, status, message);
        if (status != Ydb::StatusIds::SUCCESS) {
            ythrow TRequestFail(status) << message;
        }

        if (!QueryState->TryMergeTopicOffsets(QueryState->TopicOperations, message)) {
            ythrow TRequestFail(Ydb::StatusIds::BAD_REQUEST) << message;
        }

        if (HasTopicWriteOperations() && !HasTopicWriteId()) {
            Send(MakeTxProxyID(), new TEvTxUserProxy::TEvAllocateTxId, 0, QueryState->QueryId);
        } else {
            ReplySuccess();
        }
    }

    void Handle(TEvTxUserProxy::TEvAllocateTxIdResult::TPtr& ev) {
        if (CurrentStateFunc() != &TThis::ExecuteState || ev->Cookie < QueryId) {
            return;
        }

        YQL_ENSURE(QueryState);
        YQL_ENSURE(QueryState->GetAction() == NKikimrKqp::QUERY_ACTION_TOPIC);
        SetTopicWriteId(NLongTxService::TLockHandle(ev->Get()->TxId, TActivationContext::ActorSystem()));
        ReplySuccess();
    }

    bool HasTopicWriteOperations() const {
        return QueryState->TxCtx->TopicOperations.HasWriteOperations();
    }

    bool HasTopicWriteId() const {
        return QueryState->TxCtx->TopicOperations.HasWriteId();
    }

    ui64 GetTopicWriteId() const {
        return QueryState->TxCtx->TopicOperations.GetWriteId();
    }

    void SetTopicWriteId(NLongTxService::TLockHandle handle) {
        QueryState->TxCtx->TopicOperations.SetWriteId(std::move(handle));
    }

    void TerminateBufferActor(TIntrusivePtr<TKqpTransactionContext> ctx) {
        if (ctx && ctx->BufferActorId) {
            Send(ctx->BufferActorId, new TEvKqpBuffer::TEvTerminate{});
            ctx->BufferActorId = {};
        }
    }

private:
    TActorId Owner;
    TKqpQueryCachePtr QueryCache;
    TString SessionId;

    std::shared_ptr<NKikimr::NKqp::NRm::IKqpResourceManager> ResourceManager_;
    std::shared_ptr<NKikimr::NKqp::NComputeActor::IKqpNodeComputeActorFactory> CaFactory_;
    // cached lookups to issue counters
    THashMap<ui32, ::NMonitoring::TDynamicCounters::TCounterPtr> CachedIssueCounters;
    TInstant CreationTime;
    TIntrusivePtr<TKqpCounters> Counters;
    TIntrusivePtr<TKqpRequestCounters> RequestCounters;
    TKqpWorkerSettings Settings;
    NYql::NDq::IDqAsyncIoFactory::TPtr AsyncIoFactory;
    TIntrusivePtr<TModuleResolverState> ModuleResolverState;
    std::optional<TKqpFederatedQuerySetup> FederatedQuerySetup;
    TKqpSettings::TConstPtr KqpSettings;
    std::optional<TActorId> WorkerId;
    TActorId ExecuterId;
    NWilson::TSpan AcquireSnapshotSpan;

    std::shared_ptr<TKqpQueryState> QueryState;
    std::unique_ptr<TKqpCleanupCtx> CleanupCtx;
    ui32 QueryId = 0;
    TKikimrConfiguration::TPtr Config;
    IDataProvider::TFillSettings FillSettings;
    TTransactionsCache Transactions;
    std::unique_ptr<TEvKqp::TEvQueryResponse> QueryResponse;
    std::optional<TSessionShutdownState> ShutdownState;
    TULIDGenerator UlidGen;
    NTxProxy::TRequestControls RequestControls;

    TKqpTempTablesState TempTablesState;

    NKikimrConfig::TQueryServiceConfig QueryServiceConfig;
    TActorId KqpTempTablesAgentActor;
    std::shared_ptr<std::atomic<bool>> CompilationCookie;

    TGUCSettings::TPtr GUCSettings;

    TFastQueryHelper FastQueryHelper;
    TActorId FastQueryExecutor;
};

} // namespace

IActor* CreateKqpSessionActor(const TActorId& owner,
    TKqpQueryCachePtr queryCache,
    std::shared_ptr<NKikimr::NKqp::NRm::IKqpResourceManager> resourceManager,
    std::shared_ptr<NKikimr::NKqp::NComputeActor::IKqpNodeComputeActorFactory> caFactory, const TString& sessionId,
    const TKqpSettings::TConstPtr& kqpSettings, const TKqpWorkerSettings& workerSettings,
    std::optional<TKqpFederatedQuerySetup> federatedQuerySetup,
    NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
    TIntrusivePtr<TModuleResolverState> moduleResolverState, TIntrusivePtr<TKqpCounters> counters,
    const NKikimrConfig::TQueryServiceConfig& queryServiceConfig,
    const TActorId& kqpTempTablesAgentActor)
{
    return new TKqpSessionActor(
        owner, std::move(queryCache),
        std::move(resourceManager), std::move(caFactory), sessionId, kqpSettings, workerSettings, federatedQuerySetup,
                                std::move(asyncIoFactory),  std::move(moduleResolverState), counters,
                                queryServiceConfig, kqpTempTablesAgentActor);
}

}
}
