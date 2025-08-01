#include "kqp_executer.h"
#include "kqp_executer_impl.h"
#include "kqp_locks_helper.h"
#include "kqp_planner.h"
#include "kqp_table_resolver.h"
#include "kqp_tasks_validate.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/client/minikql_compile/db_key_resolver.h>
#include <ydb/core/kqp/common/buffer/events.h>
#include <ydb/core/kqp/common/kqp_data_integrity_trails.h>
#include <ydb/core/kqp/common/kqp_tx_manager.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/common/simple/reattach.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/core/kqp/compute_actor/kqp_compute_actor.h>
#include <ydb/core/kqp/common/kqp_tx.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/runtime/kqp_transport.h>
#include <ydb/core/kqp/opt/kqp_query_plan.h>
#include <ydb/core/tx/columnshard/columnshard.h>
#include <ydb/core/tx/data_events/common/error_codes.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/long_tx_service/public/events.h>
#include <ydb/core/tx/long_tx_service/public/lock_handle.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/persqueue/events/global.h>

#include <ydb/library/yql/dq/runtime/dq_columns_resolve.h>
#include <ydb/library/yql/dq/tasks/dq_connection_builder.h>
#include <yql/essentials/public/issue/yql_issue_message.h>


namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NYql::NDq;
using namespace NLongTxService;

namespace {

static constexpr ui32 ReplySizeLimit = 48 * 1024 * 1024; // 48 MB

class TKqpDataExecuter : public TKqpExecuterBase<TKqpDataExecuter, EExecType::Data> {
    using TBase = TKqpExecuterBase<TKqpDataExecuter, EExecType::Data>;
    using TKqpSnapshot = IKqpGateway::TKqpSnapshot;

    struct TReattachState {
        TReattachInfo ReattachInfo;
        ui64 Cookie = 0;

        bool ShouldReattach(TInstant now) {
            ++Cookie; // invalidate any previous cookie

            return ::NKikimr::NKqp::ShouldReattach(now, ReattachInfo);
        }

        void Reattached() {
            ReattachInfo.Reattaching = false;
        }
    };

    struct TShardState {
        enum class EState {
            Initial,
            Preparing,      // planned tx only
            Prepared,       // planned tx only
            Executing,
            Finished
        };

        EState State = EState::Initial;
        TSet<ui64> TaskIds;

        struct TDatashardState {
            ui64 ShardMinStep = 0;
            ui64 ShardMaxStep = 0;
            ui64 ReadSize = 0;
            bool ShardReadLocks = false;
            bool Follower = false;
        };
        TMaybe<TDatashardState> DatashardState;

        TReattachState ReattachState;
        ui32 RestartCount = 0;
        bool Restarting = false;
    };

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KQP_DATA_EXECUTER_ACTOR;
    }

    TKqpDataExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database,
        const TIntrusiveConstPtr<NACLib::TUserToken>& userToken,
        TKqpRequestCounters::TPtr counters, bool streamResult,
        const TExecuterConfig& executerConfig,
        NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
        const TActorId& creator, const TIntrusivePtr<TUserRequestContext>& userRequestContext,
        ui32 statementResultIndex, const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup,
        const TGUCSettings::TPtr& GUCSettings,
        TPartitionPruner::TConfig partitionPrunerConfig,
        const TShardIdToTableInfoPtr& shardIdToTableInfo,
        const IKqpTransactionManagerPtr& txManager,
        const TActorId bufferActorId,
        TMaybe<NBatchOperations::TSettings> batchOperationSettings = Nothing())
        : TBase(std::move(request), std::move(asyncIoFactory), federatedQuerySetup, GUCSettings, std::move(partitionPrunerConfig),
            database, userToken, counters, executerConfig, userRequestContext, statementResultIndex, TWilsonKqp::DataExecuter,
            "DataExecuter", streamResult, bufferActorId, txManager, std::move(batchOperationSettings))
        , ShardIdToTableInfo(shardIdToTableInfo)
        , AllowOlapDataQuery(executerConfig.TableServiceConfig.GetAllowOlapDataQuery())
        , WaitCAStatsTimeout(TDuration::MilliSeconds(executerConfig.TableServiceConfig.GetQueryLimits().GetWaitCAStatsTimeoutMs()))
    {
        Target = creator;

        YQL_ENSURE(Request.IsolationLevel != NKikimrKqp::ISOLATION_LEVEL_UNDEFINED);

        if (Request.AcquireLocksTxId || Request.LocksOp == ELocksOp::Commit || Request.LocksOp == ELocksOp::Rollback) {
            YQL_ENSURE(Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SERIALIZABLE
                || Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_SNAPSHOT_RW);
        }

        ReadOnlyTx = IsReadOnlyTx();
    }

    bool CheckExecutionComplete() {
        ui32 notFinished = 0;
        for (const auto& x : ShardStates) {
            YQL_ENSURE(!TxManager);
            if (x.second.State != TShardState::EState::Finished) {
                notFinished++;
                LOG_D("ActorState: " << CurrentStateFuncName()
                    << ", datashard " << x.first << " not finished yet: " << ToString(x.second.State));
            }
        }
        if (notFinished == 0 && TBase::CheckExecutionComplete()) {
            return true;
        }

        if (IsDebugLogEnabled()) {
            auto sb = TStringBuilder() << "ActorState: " << CurrentStateFuncName()
                << ", waiting for " << (Planner ? Planner->GetPendingComputeActors().size() : 0) << " compute actor(s) and "
                << notFinished << " datashard(s): ";
            if (Planner) {
                for (const auto& shardId : Planner->GetPendingComputeActors()) {
                    sb << "CA " << shardId.first << ", ";
                }
            }
            for (const auto& [shardId, shardState] : ShardStates) {
                if (shardState.State != TShardState::EState::Finished) {
                    sb << "DS " << shardId << " (" << ToString(shardState.State) << "), ";
                }
            }
            LOG_D(sb);
        }
        return false;
    }

    bool ForceAcquireSnapshot() const {
        const bool forceSnapshot = (
            !GetSnapshot().IsValid() &&
            ReadOnlyTx &&
            !ImmediateTx &&
            !HasPersistentChannels &&
            !HasOlapTable &&
            (!Database.empty() || AppData()->EnableMvccSnapshotWithLegacyDomainRoot)
        );

        return forceSnapshot;
    }

    bool GetUseFollowers() const {
        return (
            // first, we must specify read stale flag.
            Request.IsolationLevel == NKikimrKqp::ISOLATION_LEVEL_READ_STALE &&
            // next, if snapshot is already defined, so in this case followers are not allowed.
            !GetSnapshot().IsValid() &&
            // ensure that followers are allowed only for read only transactions.
            ReadOnlyTx &&
            // if we are forced to acquire snapshot by some reason, so we cannot use followers.
            !ForceAcquireSnapshot()
        );
    }

    void Finalize() {
        Y_ABORT_UNLESS(!AlreadyReplied);
        if (LocksBroken) {
            YQL_ENSURE(ResponseEv->BrokenLockShardId);
            return ReplyErrorAndDie(Ydb::StatusIds::ABORTED, {});
        }

        auto addLocks = [this](const ui64 taskId, const auto& data) {
            if (data.GetData().template Is<NKikimrTxDataShard::TEvKqpInputActorResultInfo>()) {
                NKikimrTxDataShard::TEvKqpInputActorResultInfo info;
                YQL_ENSURE(data.GetData().UnpackTo(&info), "Failed to unpack settings");
                NDataIntegrity::LogIntegrityTrails("InputActorResult", Request.UserTraceId, TxId, info, TlsActivationContext->AsActorContext());
                for (auto& lock : info.GetLocks()) {
                    if (!TxManager) {
                        Locks.push_back(lock);
                    }

                    const auto& task = TasksGraph.GetTask(taskId);
                    const auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);
                    ShardIdToTableInfo->Add(lock.GetDataShard(), stageInfo.Meta.TableKind == ETableKind::Olap, stageInfo.Meta.TablePath);

                    if (TxManager) {
                        TxManager->AddShard(lock.GetDataShard(), stageInfo.Meta.TableKind == ETableKind::Olap, stageInfo.Meta.TablePath);
                        TxManager->AddAction(lock.GetDataShard(), IKqpTransactionManager::EAction::READ);
                        TxManager->AddLock(lock.GetDataShard(), lock);
                    }
                }

                if (!BatchOperationSettings.Empty() && info.HasBatchOperationMaxKey()) {
                    if (ResponseEv->BatchOperationMaxKeys.empty()) {
                        for (auto keyId : info.GetBatchOperationKeyIds()) {
                            ResponseEv->BatchOperationKeyIds.push_back(keyId);
                        }
                    }

                    ResponseEv->BatchOperationMaxKeys.emplace_back(info.GetBatchOperationMaxKey());
                }
            } else if (data.GetData().template Is<NKikimrKqp::TEvKqpOutputActorResultInfo>()) {
                NKikimrKqp::TEvKqpOutputActorResultInfo info;
                YQL_ENSURE(data.GetData().UnpackTo(&info), "Failed to unpack settings");
                NDataIntegrity::LogIntegrityTrails("OutputActorResult", Request.UserTraceId, TxId, info, TlsActivationContext->AsActorContext());
                for (auto& lock : info.GetLocks()) {
                    if (!TxManager) {
                        Locks.push_back(lock);
                    }

                    const auto& task = TasksGraph.GetTask(taskId);
                    const auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);
                    ShardIdToTableInfo->Add(lock.GetDataShard(), stageInfo.Meta.TableKind == ETableKind::Olap, stageInfo.Meta.TablePath);
                    if (TxManager) {
                        YQL_ENSURE(stageInfo.Meta.TableKind == ETableKind::Olap);
                        IKqpTransactionManager::TActionFlags flags = IKqpTransactionManager::EAction::WRITE;
                        if (info.GetHasRead()) {
                            flags |= IKqpTransactionManager::EAction::READ;
                        }

                        TxManager->AddShard(lock.GetDataShard(), stageInfo.Meta.TableKind == ETableKind::Olap, stageInfo.Meta.TablePath);
                        TxManager->AddAction(lock.GetDataShard(), flags);
                        TxManager->AddLock(lock.GetDataShard(), lock);
                    }
                }
            }
        };

        for (auto& [_, extraData] : ExtraData) {
            for (const auto& source : extraData.Data.GetSourcesExtraData()) {
                addLocks(extraData.TaskId, source);
            }
            for (const auto& transform : extraData.Data.GetInputTransformsData()) {
                addLocks(extraData.TaskId, transform);
            }
            for (const auto& sink : extraData.Data.GetSinksExtraData()) {
                addLocks(extraData.TaskId, sink);
            }
            if (extraData.Data.HasComputeExtraData()) {
                addLocks(extraData.TaskId, extraData.Data.GetComputeExtraData());
            }
        }

        if (TxManager) {
            TxManager->SetHasSnapshot(GetSnapshot().IsValid());
        }

        if (!BufferActorId || (ReadOnlyTx && Request.LocksOp != ELocksOp::Rollback)) {
            Become(&TKqpDataExecuter::FinalizeState);
            MakeResponseAndPassAway();
            return;
        }  else if (Request.LocksOp == ELocksOp::Commit && !ReadOnlyTx) {
            Become(&TKqpDataExecuter::FinalizeState);
            LOG_D("Send Commit to BufferActor=" << BufferActorId);

            auto event = std::make_unique<NKikimr::NKqp::TEvKqpBuffer::TEvCommit>();
            event->ExecuterActorId = SelfId();
            event->TxId = TxId;
            Send<ESendingType::Tail>(
                BufferActorId,
                event.release(),
                IEventHandle::FlagTrackDelivery,
                0,
                ExecuterSpan.GetTraceId());
            return;
        } else if (Request.LocksOp == ELocksOp::Rollback) {
            Become(&TKqpDataExecuter::FinalizeState);
            LOG_D("Send Rollback to BufferActor=" << BufferActorId);

            auto event = std::make_unique<NKikimr::NKqp::TEvKqpBuffer::TEvRollback>();
            event->ExecuterActorId = SelfId();
            Send<ESendingType::Tail>(
                BufferActorId,
                event.release(),
                IEventHandle::FlagTrackDelivery,
                0,
                ExecuterSpan.GetTraceId());
            MakeResponseAndPassAway();
            return;
        } else if (Request.UseImmediateEffects) {
            Become(&TKqpDataExecuter::FinalizeState);
            LOG_D("Send Flush to BufferActor=" << BufferActorId);

            auto event = std::make_unique<NKikimr::NKqp::TEvKqpBuffer::TEvFlush>();
            event->ExecuterActorId = SelfId();
            Send<ESendingType::Tail>(
                BufferActorId,
                event.release(),
                IEventHandle::FlagTrackDelivery,
                0,
                ExecuterSpan.GetTraceId());
            return;
        } else {
            Become(&TKqpDataExecuter::FinalizeState);
            MakeResponseAndPassAway();
            return;
        }
    }

    STATEFN(FinalizeState) {
        try {
            switch(ev->GetTypeRewrite()) {
                hFunc(TEvKqp::TEvAbortExecution, HandleFinalize);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                hFunc(TEvKqpBuffer::TEvResult, HandleFinalize);
                hFunc(TEvents::TEvUndelivered, HandleFinalize);

                IgnoreFunc(TEvColumnShard::TEvProposeTransactionResult);
                IgnoreFunc(TEvDataShard::TEvProposeTransactionResult);
                IgnoreFunc(TEvDataShard::TEvProposeTransactionRestart);
                IgnoreFunc(TEvDataShard::TEvProposeTransactionAttachResult);
                IgnoreFunc(TEvPersQueue::TEvProposeTransactionResult);
                IgnoreFunc(NKikimr::NEvents::TDataEvents::TEvWriteResult);
                IgnoreFunc(TEvPrivate::TEvReattachToShard);
                IgnoreFunc(TEvDqCompute::TEvState);
                IgnoreFunc(TEvDqCompute::TEvChannelData);
                IgnoreFunc(TEvKqpExecuter::TEvStreamDataAck);
                IgnoreFunc(TEvPipeCache::TEvDeliveryProblem);
                IgnoreFunc(TEvInterconnect::TEvNodeDisconnected);
                IgnoreFunc(TEvKqpNode::TEvStartKqpTasksResponse);
                IgnoreFunc(TEvInterconnect::TEvNodeConnected);
                default:
                    UnexpectedEvent("FinalizeState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void HandleFinalize(TEvKqp::TEvAbortExecution::TPtr& ev) {
        if (IsCancelAfterAllowed(ev)) {
            TBase::HandleAbortExecution(ev);
        } else {
            LOG_D("Got TEvAbortExecution from : " << ev->Sender << " but cancelation is not alowed");
        }
    }

    void HandleFinalize(TEvKqpBuffer::TEvResult::TPtr& ev) {
        if (ev->Get()->Stats) {
            if (Stats) {
                Stats->AddBufferStats(std::move(*ev->Get()->Stats));
            }
        }
        MakeResponseAndPassAway();
    }

    void HandleFinalize(TEvents::TEvUndelivered::TPtr&) {
        auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE, "Buffer actor isn't available.");
        ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
    }

    void MakeResponseAndPassAway() {
        ResponseEv->Record.MutableResponse()->SetStatus(Ydb::StatusIds::SUCCESS);
        Counters->TxProxyMon->ReportStatusOK->Inc();

        ResponseEv->Snapshot = GetSnapshot();

        if (!Locks.empty() || (TxManager && TxManager->HasLocks())) {
            if (LockHandle) {
                ResponseEv->LockHandle = std::move(LockHandle);
            }
            if (!TxManager) {
                BuildLocks(*ResponseEv->Record.MutableResponse()->MutableResult()->MutableLocks(), Locks);
            }
        }

        if (TxManager) {
            for (const ui64& shardId : TxManager->GetShards()) {
                Stats->AffectedShards.insert(shardId);
            }
        }

        auto resultSize = ResponseEv->GetByteSize();
        if (resultSize > (int)ReplySizeLimit) {
            TString message;
            if (ResponseEv->TxResults.size() == 1 && !ResponseEv->TxResults[0].QueryResultIndex.Defined()) {
                message = TStringBuilder() << "Intermediate data materialization exceeded size limit"
                    << " (" << resultSize << " > " << ReplySizeLimit << ")."
                    << " This usually happens when trying to write large amounts of data or to perform lookup"
                    << " by big collection of keys in single query. Consider using smaller batches of data.";
            } else {
                message = TStringBuilder() << "Query result size limit exceeded. ("
                    << resultSize << " > " << ReplySizeLimit << ")";
            }

            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_RESULT_UNAVAILABLE, message);
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED, issue);
            Counters->Counters->TxReplySizeExceededError->Inc();
            return;
        }

        LWTRACK(KqpDataExecuterFinalize, ResponseEv->Orbit, TxId, LastShard, ResponseEv->ResultsSize(), ResponseEv->GetByteSize());

        ExecuterSpan.EndOk();

        AlreadyReplied = true;
        PassAway();
    }

    STATEFN(WaitResolveState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvKqpExecuter::TEvTableResolveStatus, HandleResolve);
                hFunc(NShardResolver::TEvShardsResolveStatus, HandleResolve);
                hFunc(TEvPrivate::TEvResourcesSnapshot, HandleResolve);
                hFunc(TEvSaveScriptExternalEffectResponse, HandleResolve);
                hFunc(TEvDescribeSecretsResponse, HandleResolve);
                hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                default:
                    UnexpectedEvent("WaitResolveState", ev->GetTypeRewrite());
            }

        } catch (const yexception& e) {
            InternalError(e.what());
        } catch (const TMemoryLimitExceededException&) {
            RuntimeError(Ydb::StatusIds::PRECONDITION_FAILED, NYql::TIssues({NYql::TIssue(BuildMemoryLimitExceptionMessage())}));
        }
        ReportEventElapsedTime();
    }

private:
    bool IsCancelAfterAllowed(const TEvKqp::TEvAbortExecution::TPtr& ev) const {
        return ReadOnlyTx || ev->Get()->Record.GetStatusCode() != NYql::NDqProto::StatusIds::CANCELLED;
    }

    TString CurrentStateFuncName() const override {
        const auto& func = CurrentStateFunc();
        if (func == &TThis::PrepareState) {
            return "PrepareState";
        } else if (func == &TThis::ExecuteState) {
            return "ExecuteState";
        } else if (func == &TThis::WaitSnapshotState) {
            return "WaitSnapshotState";
        } else if (func == &TThis::WaitResolveState) {
            return "WaitResolveState";
        } else if (func == &TThis::WaitShutdownState) {
            return "WaitShutdownState";
        } else if (func == &TThis::FinalizeState) {
            return "FinalizeState";
        } else {
            return TBase::CurrentStateFuncName();
        }
    }

    STATEFN(PrepareState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvColumnShard::TEvProposeTransactionResult, HandlePrepare);
                hFunc(TEvDataShard::TEvProposeTransactionResult, HandlePrepare);
                hFunc(TEvDataShard::TEvProposeTransactionRestart, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionAttachResult, HandlePrepare);
                hFunc(TEvPersQueue::TEvProposeTransactionResult, HandlePrepare);
                hFunc(NKikimr::NEvents::TDataEvents::TEvWriteResult, HandlePrepare);
                hFunc(TEvPrivate::TEvReattachToShard, HandleExecute);
                hFunc(TEvDqCompute::TEvState, HandlePrepare); // from CA
                hFunc(TEvDqCompute::TEvChannelData, HandleChannelData); // from CA
                hFunc(TEvKqpExecuter::TEvStreamDataAck, HandleStreamAck);
                hFunc(TEvPipeCache::TEvDeliveryProblem, HandlePrepare);
                hFunc(TEvKqp::TEvAbortExecution, HandlePrepare);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                hFunc(TEvents::TEvUndelivered, HandleUndelivered);
                hFunc(TEvInterconnect::TEvNodeDisconnected, HandleDisconnected);
                hFunc(TEvKqpNode::TEvStartKqpTasksResponse, HandleStartKqpTasksResponse);
                IgnoreFunc(TEvInterconnect::TEvNodeConnected);
                default: {
                    CancelProposal(0);
                    UnexpectedEvent("PrepareState", ev->GetTypeRewrite());
                }
            }
        } catch (const yexception& e) {
            CancelProposal(0);
            InternalError(e.what());
        } catch (const TMemoryLimitExceededException& e) {
            CancelProposal(0);
            RuntimeError(Ydb::StatusIds::PRECONDITION_FAILED, NYql::TIssues({NYql::TIssue(BuildMemoryLimitExceptionMessage())}));
        }

        ReportEventElapsedTime();
    }

    void HandlePrepare(TEvPersQueue::TEvProposeTransactionResult::TPtr& ev) {
        auto& event = ev->Get()->Record;
        const ui64 tabletId = event.GetOrigin();

        LOG_D("Got propose result" <<
              ", PQ tablet: " << tabletId <<
              ", status: " << NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(event.GetStatus()));

        TShardState* state = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(state, "Unexpected propose result from unknown PQ tablet " << tabletId);

        switch (event.GetStatus()) {
        case NKikimrPQ::TEvProposeTransactionResult::PREPARED:
            if (!ShardPrepared(*state, event)) {
                return CancelProposal(tabletId);
            }
            return CheckPrepareCompleted();
        case NKikimrPQ::TEvProposeTransactionResult::COMPLETE:
            YQL_ENSURE(false);
        default:
            CancelProposal(tabletId);
            return PQTabletError(event);
        }
    }

    void HandlePrepare(TEvDataShard::TEvProposeTransactionResult::TPtr& ev) {
        TEvDataShard::TEvProposeTransactionResult* res = ev->Get();
        ResponseEv->Orbit.Join(res->Orbit);
        const ui64 shardId = res->GetOrigin();
        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "Unexpected propose result from unknown tabletId " << shardId);

        NDataIntegrity::LogIntegrityTrails("Prepare", Request.UserTraceId, ev, TlsActivationContext->AsActorContext());
        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(res->GetStatus())
            << ", error: " << res->GetError());

        if (Stats) {
            Stats->AddDatashardPrepareStats(std::move(*res->Record.MutableTxStats()));
        }

        switch (res->GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED: {
                if (!ShardPrepared(*shardState, res->Record)) {
                    return CancelProposal(shardId);
                }
                return CheckPrepareCompleted();
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE: {
                YQL_ENSURE(false);
            }
            default: {
                CancelProposal(shardId);
                return ShardError(res->Record);
            }
        }
    }

    void HandlePrepare(TEvColumnShard::TEvProposeTransactionResult::TPtr& ev) {
        TEvColumnShard::TEvProposeTransactionResult* res = ev->Get();
        const ui64 shardId = res->Record.GetOrigin();
        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "Unexpected propose result from unknown tabletId " << shardId);

        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxColumnShard::EResultStatus_Name(res->Record.GetStatus())
            << ", error: " << res->Record.GetStatusMessage());

//        if (Stats) {
//            Stats->AddDatashardPrepareStats(std::move(*res->Record.MutableTxStats()));
//        }

        switch (res->Record.GetStatus()) {
            case NKikimrTxColumnShard::EResultStatus::PREPARED:
            {
                if (!ShardPrepared(*shardState, res->Record)) {
                    return CancelProposal(shardId);
                }
                return CheckPrepareCompleted();
            }
            case NKikimrTxColumnShard::EResultStatus::SUCCESS:
            {
                YQL_ENSURE(false);
            }
            default:
            {
                CancelProposal(shardId);
                return ShardError(res->Record);
            }
        }
    }

    void HandlePrepare(NKikimr::NEvents::TDataEvents::TEvWriteResult::TPtr& ev) {
        auto* res = ev->Get();

        const ui64 shardId = res->Record.GetOrigin();
        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "Unexpected propose result from unknown tabletId " << shardId);

        NYql::TIssues issues;
        NYql::IssuesFromMessage(res->Record.GetIssues(), issues);

        NDataIntegrity::LogIntegrityTrails("Prepare", Request.UserTraceId, ev, TlsActivationContext->AsActorContext());
        LOG_D("Recv EvWriteResult (prepare) from ShardID=" << shardId
            << ", Status=" << NKikimrDataEvents::TEvWriteResult::EStatus_Name(ev->Get()->GetStatus())
            << ", TxId=" << ev->Get()->Record.GetTxId()
            << ", Locks= " << [&]() {
                TStringBuilder builder;
                for (const auto& lock : ev->Get()->Record.GetTxLocks()) {
                    builder << lock.ShortDebugString();
                }
                return builder;
            }()
            << ", Cookie=" << ev->Cookie
            << ", error=" << issues.ToString());

        if (Stats) {
            Stats->AddDatashardPrepareStats(std::move(*res->Record.MutableTxStats()));
        }

        switch (ev->Get()->GetStatus()) {
            case NKikimrDataEvents::TEvWriteResult::STATUS_UNSPECIFIED: {
                YQL_ENSURE(false);
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_PREPARED: {
                if (!ShardPrepared(*shardState, res->Record)) {
                    return;
                }
                return CheckPrepareCompleted();
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_COMPLETED: {
                YQL_ENSURE(false);
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN: {
                LOG_D("Broken locks: " << res->Record.DebugString());
                YQL_ENSURE(shardState->State == TShardState::EState::Preparing);
                Counters->TxProxyMon->TxResultAborted->Inc();
                LocksBroken = true;
                ResponseEv->BrokenLockShardId = shardId;

                if (!res->Record.GetTxLocks().empty()) {
                    ResponseEv->BrokenLockPathId = NYql::TKikimrPathId(
                        res->Record.GetTxLocks(0).GetSchemeShard(),
                        res->Record.GetTxLocks(0).GetPathId());
                }
                ReplyErrorAndDie(Ydb::StatusIds::ABORTED, {});
                return;
            }
            default:
            {
                return ShardError(res->Record);
            }
        }
    }

    void HandlePrepare(TEvDataShard::TEvProposeTransactionAttachResult::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 tabletId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState, "Unknown tablet " << tabletId);

        if (ev->Cookie != shardState->ReattachState.Cookie) {
            return;
        }

        switch (shardState->State) {
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
                break;
            case TShardState::EState::Initial:
            case TShardState::EState::Executing:
            case TShardState::EState::Finished:
                YQL_ENSURE(false, "Unexpected shard " << tabletId << " state " << ToString(shardState->State));
        }

        if (record.GetStatus() == NKikimrProto::OK) {
            // Transaction still exists at this shard
            LOG_D("Reattached to shard " << tabletId << ", state was: " << ToString(shardState->State));
            shardState->State = TShardState::EState::Prepared;
            shardState->ReattachState.Reattached();
            return CheckPrepareCompleted();
        }

        LOG_E("Shard " << tabletId << " transaction lost during reconnect: " << record.GetStatus());

        CancelProposal(tabletId);
        ReplyUnavailable(TStringBuilder() << "Disconnected from shard " << tabletId);
    }

    void HandlePrepare(TEvDqCompute::TEvState::TPtr& ev) {
        if (ev->Get()->Record.GetState() == NDqProto::COMPUTE_STATE_FAILURE) {
            CancelProposal(0);
        }
        HandleComputeState(ev);
    }

    void HandlePrepare(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        TEvPipeCache::TEvDeliveryProblem* msg = ev->Get();
        auto* shardState = ShardStates.FindPtr(msg->TabletId);
        YQL_ENSURE(shardState, "EvDeliveryProblem from unknown tablet " << msg->TabletId);

        bool wasRestarting = std::exchange(shardState->Restarting, false);

        // We can only be sure tx was not prepared if initial propose was not delivered
        bool notPrepared = msg->NotDelivered && (shardState->RestartCount == 0);

        switch (shardState->State) {
            case TShardState::EState::Preparing: {
                // Disconnected while waiting for initial propose response

                LOG_I("Shard " << msg->TabletId << " propose error, notDelivered: " << msg->NotDelivered
                    << ", notPrepared: " << notPrepared << ", wasRestart: " << wasRestarting);

                if (notPrepared) {
                    CancelProposal(msg->TabletId);
                    return ReplyUnavailable(TStringBuilder() << "Could not deliver program to shard " << msg->TabletId);
                }

                CancelProposal(0);

                if (wasRestarting) {
                    // We are waiting for propose and have a restarting flag, which means shard was
                    // persisting our tx. We did not receive a reply, so we cannot be sure if it
                    // succeeded or not, but we know that it could not apply any side effects, since
                    // we don't start transaction planning until prepare phase is complete.
                    return ReplyUnavailable(TStringBuilder() << "Could not prepare program on shard " << msg->TabletId);
                }

                return ReplyUnavailable(TStringBuilder() << "Disconnected from shard " << msg->TabletId);
            }

            case TShardState::EState::Prepared: {
                // Disconnected while waiting for other shards to prepare

                if ((wasRestarting || shardState->ReattachState.ReattachInfo.Reattaching) &&
                    shardState->ReattachState.ShouldReattach(TlsActivationContext->Now()))
                {
                    LOG_N("Shard " << msg->TabletId << " delivery problem (already prepared, reattaching in "
                        << shardState->ReattachState.ReattachInfo.Delay << ")");

                    Schedule(shardState->ReattachState.ReattachInfo.Delay, new TEvPrivate::TEvReattachToShard(msg->TabletId));
                    ++shardState->RestartCount;
                    return;
                }

                LOG_N("Shard " << msg->TabletId << " delivery problem (already prepared)"
                    << (msg->NotDelivered ? ", last message not delivered" : ""));

                CancelProposal(0);
                return ReplyUnavailable(TStringBuilder() << "Disconnected from shard " << msg->TabletId);
            }

            case TShardState::EState::Initial:
            case TShardState::EState::Executing:
            case TShardState::EState::Finished:
                YQL_ENSURE(false, "Unexpected shard " << msg->TabletId << " state " << ToString(shardState->State));
        }
    }

    void HandlePrepare(TEvKqp::TEvAbortExecution::TPtr& ev) {
        if (IsCancelAfterAllowed(ev)) {
            CancelProposal(0);
            TBase::HandleAbortExecution(ev);
        } else {
            LOG_D("Got TEvAbortExecution from : " << ev->Sender << " but cancelation is not alowed");
        }
    }

    void CancelProposal(ui64 exceptShardId) {
        for (auto& [shardId, state] : ShardStates) {
            if (shardId != exceptShardId &&
                (state.State == TShardState::EState::Preparing
                 || state.State == TShardState::EState::Prepared
                 || (state.State == TShardState::EState::Executing && ImmediateTx)))
            {
                ui64 id = shardId;
                LOG_D("Send CancelTransactionProposal to shard: " << id);

                state.State = TShardState::EState::Finished;

                YQL_ENSURE(state.DatashardState.Defined());
                //nothing to cancel on follower
                if (!state.DatashardState->Follower) {
                    Send(MakePipePerNodeCacheID(/* allowFollowers */ false), new TEvPipeCache::TEvForward(
                        new TEvDataShard::TEvCancelTransactionProposal(TxId), shardId, /* subscribe */ false));
                }
            }
        }
    }

    template<class E>
    bool ShardPreparedImpl(TShardState& state, const E& result) {
        YQL_ENSURE(state.State == TShardState::EState::Preparing);
        state.State = TShardState::EState::Prepared;

        state.DatashardState->ShardMinStep = result.GetMinStep();
        state.DatashardState->ShardMaxStep = result.GetMaxStep();

        ui64 coordinator = 0;
        if (result.DomainCoordinatorsSize()) {
            auto domainCoordinators = TCoordinators(TVector<ui64>(result.GetDomainCoordinators().begin(),
                                                                  result.GetDomainCoordinators().end()));
            coordinator = domainCoordinators.Select(TxId);
        }

        if (coordinator && !TxCoordinator) {
            TxCoordinator = coordinator;
        }

        if (!TxCoordinator || TxCoordinator != coordinator) {
            LOG_E("Handle TEvProposeTransactionResult: unable to select coordinator. Tx canceled, actorId: " << SelfId()
                << ", previously selected coordinator: " << TxCoordinator
                << ", coordinator selected at propose result: " << coordinator);

            Counters->TxProxyMon->TxResultAborted->Inc();
            ReplyErrorAndDie(Ydb::StatusIds::CANCELLED, MakeIssue(
                NKikimrIssues::TIssuesIds::TX_DECLINED_IMPLICIT_COORDINATOR, "Unable to choose coordinator."));
            return false;
        }

        LastPrepareReply = TInstant::Now();
        if (!FirstPrepareReply) {
            FirstPrepareReply = LastPrepareReply;
        }

        return true;
    }

    bool ShardPrepared(TShardState& state, const NKikimrTxDataShard::TEvProposeTransactionResult& result) {
        bool success = ShardPreparedImpl(state, result);
        if (success) {
            state.DatashardState->ReadSize += result.GetReadSize();
        }
        return success;
    }

    bool ShardPrepared(TShardState& state, const NKikimrTxColumnShard::TEvProposeTransactionResult& result) {
        return ShardPreparedImpl(state, result);
    }

    bool ShardPrepared(TShardState& state, const NKikimrPQ::TEvProposeTransactionResult& result) {
        return ShardPreparedImpl(state, result);
    }

    bool ShardPrepared(TShardState& state, const NKikimrDataEvents::TEvWriteResult& result) {
        return ShardPreparedImpl(state, result);
    }

    void ShardError(const NKikimrTxDataShard::TEvProposeTransactionResult& result) {
        if (result.ErrorSize() != 0) {
            TStringBuilder message;
            message << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(result.GetStatus()) << ": ";
            for (const auto &err : result.GetError()) {
                if (err.GetKind() == NKikimrTxDataShard::TError::REPLY_SIZE_EXCEEDED) {
                    Counters->Counters->DataShardTxReplySizeExceededError->Inc();
                }
                message << "[" << err.GetKind() << "] " << err.GetReason() << "; ";
            }
            LOG_E(message);
        }

        switch (result.GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED: {
                Counters->TxProxyMon->TxResultShardOverloaded->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OVERLOADED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::ABORTED: {
                Counters->TxProxyMon->TxResultAborted->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_ABORTED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::ABORTED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::TRY_LATER: {
                Counters->TxProxyMon->TxResultShardTryLater->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::RESULT_UNAVAILABLE: {
                Counters->TxProxyMon->TxResultResultUnavailable->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_RESULT_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNDETERMINED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::CANCELLED: {
                Counters->TxProxyMon->TxResultCancelled->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_CANCELLED);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::CANCELLED, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::BAD_REQUEST: {
                Counters->TxProxyMon->TxResultCancelled->Inc();
                if (HasMissingSnapshotError(result)) {
                    auto issue = YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED);
                    AddDataShardErrors(result, issue);
                    return ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED, issue);
                }
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_BAD_REQUEST);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::BAD_REQUEST, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::EXEC_ERROR: {
                Counters->TxProxyMon->TxResultExecError->Inc();
                for (auto& er : result.GetError()) {
                    if (er.GetKind() == NKikimrTxDataShard::TError::PROGRAM_ERROR) {
                        auto issue = YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED);
                        issue.AddSubIssue(new TIssue(TStringBuilder() << "Data shard error: [PROGRAM_ERROR] " << er.GetReason()));
                        return ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED, issue);
                    }
                }
                auto issue = YqlIssue({}, TIssuesIds::DEFAULT_ERROR, "Error executing transaction (ExecError): Execution failed");
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::GENERIC_ERROR, issue);
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::ERROR: {
                Counters->TxProxyMon->TxResultError->Inc();
                for (auto& er : result.GetError()) {
                    switch (er.GetKind()) {
                        case NKikimrTxDataShard::TError::SCHEME_CHANGED:
                        case NKikimrTxDataShard::TError::SCHEME_ERROR:
                            return ReplyErrorAndDie(Ydb::StatusIds::SCHEME_ERROR, YqlIssue({},
                                TIssuesIds::KIKIMR_SCHEME_MISMATCH, er.GetReason()));
                        //TODO Split OUT_OF_SPACE and DISK_SPACE_EXHAUSTED cases. The first one is temporary, the second one is permanent.
                        case NKikimrTxDataShard::TError::OUT_OF_SPACE:
                        case NKikimrTxDataShard::TError::DISK_SPACE_EXHAUSTED: {
                            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                            AddDataShardErrors(result, issue);
                            return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
                        }
                        default:
                            break;
                    }
                }
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            default: {
                Counters->TxProxyMon->TxResultFatal->Inc();
                auto issue = YqlIssue({}, TIssuesIds::DEFAULT_ERROR, "Error executing transaction: transaction failed.");
                AddDataShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::GENERIC_ERROR, issue);
            }
        }
    }

    void ShardError(const NKikimrTxColumnShard::TEvProposeTransactionResult& result) {
        if (!!result.GetStatusMessage()) {
            TStringBuilder message;
            message << NKikimrTxColumnShard::EResultStatus_Name(result.GetStatus()) << ": ";
            message << "[" << result.GetStatusMessage() << "]" << "; ";
            LOG_E(message);
        }

        switch (result.GetStatus()) {
            case NKikimrTxColumnShard::EResultStatus::OVERLOADED:
            {
                Counters->TxProxyMon->TxResultShardOverloaded->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OVERLOADED);
                AddColumnShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED, issue);
            }
            case NKikimrTxColumnShard::EResultStatus::ABORTED:
            {
                Counters->TxProxyMon->TxResultAborted->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_ABORTED);
                AddColumnShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::ABORTED, issue);
            }
            case NKikimrTxColumnShard::EResultStatus::TIMEOUT:
            {
                Counters->TxProxyMon->TxResultShardTryLater->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddColumnShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            case NKikimrTxColumnShard::EResultStatus::ERROR:
            {
                Counters->TxProxyMon->TxResultError->Inc();
                auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
                AddColumnShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
            }
            default:
            {
                Counters->TxProxyMon->TxResultFatal->Inc();
                auto issue = YqlIssue({}, TIssuesIds::DEFAULT_ERROR, "Error executing transaction: transaction failed." + NKikimrTxColumnShard::EResultStatus_Name(result.GetStatus()));
                AddColumnShardErrors(result, issue);
                return ReplyErrorAndDie(Ydb::StatusIds::GENERIC_ERROR, issue);
            }
        }
    }

    void ShardError(const NKikimrDataEvents::TEvWriteResult& result) {
        NYql::TIssues issues;
        NYql::IssuesFromMessage(result.GetIssues(), issues);
        auto statusConclusion = NEvWrite::NErrorCodes::TOperator::GetStatusInfo(result.GetStatus());
        AFL_ENSURE(statusConclusion.IsSuccess())("error", statusConclusion.GetErrorMessage());
        if (result.GetStatus() == NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN) {
            issues.AddIssue(NYql::YqlIssue({}, statusConclusion->GetIssueCode(), statusConclusion->GetIssueGeneralText()));
        }
        return ReplyErrorAndDie(statusConclusion->GetYdbStatusCode(), issues);
    }

    void PQTabletError(const NKikimrPQ::TEvProposeTransactionResult& result) {
        NYql::TIssuesIds::EIssueCode issueCode;
        Ydb::StatusIds::StatusCode statusCode;

        switch (result.GetStatus()) {
        default: {
            issueCode = TIssuesIds::DEFAULT_ERROR;
            statusCode = Ydb::StatusIds::GENERIC_ERROR;
            break;
        }
        case NKikimrPQ::TEvProposeTransactionResult::ABORTED: {
            issueCode = TIssuesIds::KIKIMR_OPERATION_ABORTED;
            statusCode = Ydb::StatusIds::ABORTED;
            break;
        }
        case NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST: {
            issueCode = TIssuesIds::KIKIMR_BAD_REQUEST;
            statusCode = Ydb::StatusIds::BAD_REQUEST;
            break;
        }
        case NKikimrPQ::TEvProposeTransactionResult::CANCELLED: {
            issueCode = TIssuesIds::KIKIMR_OPERATION_CANCELLED;
            statusCode = Ydb::StatusIds::CANCELLED;
            break;
        }
        case NKikimrPQ::TEvProposeTransactionResult::OVERLOADED: {
            issueCode = TIssuesIds::KIKIMR_OVERLOADED;
            statusCode = Ydb::StatusIds::OVERLOADED;
            break;
        }
        }

        if (result.ErrorsSize()) {
            ReplyErrorAndDie(statusCode, YqlIssue({}, issueCode, result.GetErrors(0).GetReason()));
        } else {
            ReplyErrorAndDie(statusCode, YqlIssue({}, issueCode));
        }
    }

    void CheckPrepareCompleted() {
        YQL_ENSURE(!TxManager);
        for (const auto& [_, state] : ShardStates) {
            if (state.State != TShardState::EState::Prepared) {
                LOG_D("Not all shards are prepared, waiting...");
                return;
            }
        }

        Counters->TxProxyMon->TxPrepareSpreadHgram->Collect((LastPrepareReply - FirstPrepareReply).MilliSeconds());

        LOG_D("All shards prepared, become ExecuteState.");
        Become(&TKqpDataExecuter::ExecuteState);
        ExecutePlanned();
    }

    void ExecutePlanned() {
        YQL_ENSURE(!TxManager);
        YQL_ENSURE(!LocksBroken);
        YQL_ENSURE(TxCoordinator);
        auto ev = MakeHolder<TEvTxProxy::TEvProposeTransaction>();
        ev->Record.SetCoordinatorID(TxCoordinator);

        auto& transaction = *ev->Record.MutableTransaction();
        auto& affectedSet = *transaction.MutableAffectedSet();
        affectedSet.Reserve(static_cast<int>(ShardStates.size()));

        ui64 aggrMinStep = 0;
        ui64 aggrMaxStep = Max<ui64>();
        ui64 totalReadSize = 0;

        for (auto& [shardId, state] : ShardStates) {
            YQL_ENSURE(state.State == TShardState::EState::Prepared);
            state.State = TShardState::EState::Executing;

            YQL_ENSURE(state.DatashardState.Defined());
            YQL_ENSURE(!state.DatashardState->Follower);

            aggrMinStep = Max(aggrMinStep, state.DatashardState->ShardMinStep);
            aggrMaxStep = Min(aggrMaxStep, state.DatashardState->ShardMaxStep);
            totalReadSize += state.DatashardState->ReadSize;

            auto& item = *affectedSet.Add();
            item.SetTabletId(shardId);

            ui32 affectedFlags = 0;
            if (state.DatashardState->ShardReadLocks) {
                affectedFlags |= TEvTxProxy::TEvProposeTransaction::AffectedRead;
            }

            for (auto taskId : state.TaskIds) {
                auto& task = TasksGraph.GetTask(taskId);
                auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);

                if (stageInfo.Meta.HasReads()) {
                    affectedFlags |= TEvTxProxy::TEvProposeTransaction::AffectedRead;
                }
                if (stageInfo.Meta.HasWrites()) {
                    affectedFlags |= TEvTxProxy::TEvProposeTransaction::AffectedWrite;
                }
            }

            item.SetFlags(affectedFlags);
        }

        ui64 sizeLimit = Request.PerRequestDataSizeLimit;
        if (Request.TotalReadSizeLimitBytes > 0) {
            sizeLimit = sizeLimit
                ? std::min(sizeLimit, Request.TotalReadSizeLimitBytes)
                : Request.TotalReadSizeLimitBytes;
        }

        if (totalReadSize > sizeLimit) {
            auto msg = TStringBuilder() << "Transaction total read size " << totalReadSize << " exceeded limit " << sizeLimit;
            LOG_N(msg);
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, msg));
            return;
        }

        transaction.SetTxId(TxId);
        transaction.SetMinStep(aggrMinStep);
        transaction.SetMaxStep(aggrMaxStep);

        if (VolatileTx) {
            transaction.SetFlags(TEvTxProxy::TEvProposeTransaction::FlagVolatile);
        }

        NDataIntegrity::LogIntegrityTrails("PlannedTx", "", Request.UserTraceId, TxId, {}, TlsActivationContext->AsActorContext());

        LOG_D("Execute planned transaction, coordinator: " << TxCoordinator << " for " << affectedSet.size() << "shards");
        Send(MakePipePerNodeCacheID(false), new TEvPipeCache::TEvForward(ev.Release(), TxCoordinator, /* subscribe */ true));
    }

private:
    STATEFN(ExecuteState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvColumnShard::TEvProposeTransactionResult, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionResult, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionRestart, HandleExecute);
                hFunc(TEvDataShard::TEvProposeTransactionAttachResult, HandleExecute);
                hFunc(TEvPersQueue::TEvProposeTransactionResult, HandleExecute);
                hFunc(NKikimr::NEvents::TDataEvents::TEvWriteResult, HandleExecute);
                hFunc(TEvPrivate::TEvReattachToShard, HandleExecute);
                hFunc(TEvPipeCache::TEvDeliveryProblem, HandleExecute);
                hFunc(TEvents::TEvUndelivered, HandleUndelivered);
                hFunc(TEvPrivate::TEvRetry, HandleRetry);
                hFunc(TEvInterconnect::TEvNodeDisconnected, HandleDisconnected);
                hFunc(TEvKqpNode::TEvStartKqpTasksResponse, HandleStartKqpTasksResponse);
                hFunc(TEvTxProxy::TEvProposeTransactionStatus, HandleExecute);
                hFunc(TEvDqCompute::TEvState, HandleComputeState);
                hFunc(NYql::NDq::TEvDqCompute::TEvChannelData, HandleChannelData);
                hFunc(TEvKqpExecuter::TEvStreamDataAck, HandleStreamAck);
                hFunc(TEvKqp::TEvAbortExecution, HandleExecute);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                IgnoreFunc(TEvInterconnect::TEvNodeConnected);
                default:
                    UnexpectedEvent("ExecuteState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        } catch (const TMemoryLimitExceededException) {
            if (ReadOnlyTx) {
                RuntimeError(Ydb::StatusIds::PRECONDITION_FAILED, NYql::TIssues({NYql::TIssue(BuildMemoryLimitExceptionMessage())}));
            } else {
                RuntimeError(Ydb::StatusIds::UNDETERMINED, NYql::TIssues({NYql::TIssue(BuildMemoryLimitExceptionMessage())}));
            }
        }
        ReportEventElapsedTime();
    }

    void HandleExecute(TEvPersQueue::TEvProposeTransactionResult::TPtr& ev) {
        NKikimrPQ::TEvProposeTransactionResult& event = ev->Get()->Record;

        LOG_D("Got propose result" <<
              ", topic tablet: " << event.GetOrigin() <<
              ", status: " << NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(event.GetStatus()));

        TShardState *state = ShardStates.FindPtr(event.GetOrigin());
        YQL_ENSURE(state);

        switch (event.GetStatus()) {
            case NKikimrPQ::TEvProposeTransactionResult::COMPLETE: {
                YQL_ENSURE(state->State == TShardState::EState::Executing);

                state->State = TShardState::EState::Finished;
                CheckExecutionComplete();

                return;
            }
            case NKikimrPQ::TEvProposeTransactionResult::PREPARED: {
                YQL_ENSURE(false);
            }
            default: {
                PQTabletError(event);
                return;
            }
        }
    }

    void HandleExecute(TEvKqp::TEvAbortExecution::TPtr& ev) {
        if (IsCancelAfterAllowed(ev)) {
            if (ImmediateTx) {
                CancelProposal(0);
            }
            TBase::HandleAbortExecution(ev);
        } else {
            LOG_D("Got TEvAbortExecution from : " << ev->Sender << " but cancelation is not alowed");
        }
    }

    void Handle(TEvKqpBuffer::TEvError::TPtr& ev) {
        auto& msg = *ev->Get();
        TBase::HandleAbortExecution(msg.StatusCode, msg.Issues, false);
    }

    void HandleExecute(TEvColumnShard::TEvProposeTransactionResult::TPtr& ev) {
        TEvColumnShard::TEvProposeTransactionResult* res = ev->Get();
        const ui64 shardId = res->Record.GetOrigin();
        LastShard = shardId;

        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState);

        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxColumnShard::EResultStatus_Name(res->Record.GetStatus())
            << ", error: " << res->Record.GetStatusMessage());

//        if (Stats) {
//            Stats->AddDatashardStats(std::move(*res->Record.MutableComputeActorStats()),
//                std::move(*res->Record.MutableTxStats()));
//        }

        switch (res->Record.GetStatus()) {
            case NKikimrTxColumnShard::EResultStatus::SUCCESS:
            {
                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;

                Counters->TxProxyMon->ResultsReceivedCount->Inc();
//                Counters->TxProxyMon->ResultsReceivedSize->Add(res->GetTxResult().size());

//                for (auto& lock : res->Record.GetTxLocks()) {
//                    LOG_D("Shard " << shardId << " completed, store lock " << lock.ShortDebugString());
//                    Locks.emplace_back(std::move(lock));
//                }

                Counters->TxProxyMon->TxResultComplete->Inc();

                CheckExecutionComplete();
                return;
            }
            case NKikimrTxColumnShard::EResultStatus::PREPARED:
            {
                YQL_ENSURE(false);
            }
            default:
            {
                return ShardError(res->Record);
            }
        }
    }

    void HandleExecute(NKikimr::NEvents::TDataEvents::TEvWriteResult::TPtr& ev) {
        auto* res = ev->Get();
        const ui64 shardId = res->Record.GetOrigin();
        LastShard = shardId;

        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState);

        NYql::TIssues issues;
        NYql::IssuesFromMessage(res->Record.GetIssues(), issues);

        NDataIntegrity::LogIntegrityTrails("Execute", Request.UserTraceId, ev, TlsActivationContext->AsActorContext());
        LOG_D("Recv EvWriteResult (execute) from ShardID=" << shardId
            << ", Status=" << NKikimrDataEvents::TEvWriteResult::EStatus_Name(ev->Get()->GetStatus())
            << ", TxId=" << ev->Get()->Record.GetTxId()
            << ", Locks= " << [&]() {
                TStringBuilder builder;
                for (const auto& lock : ev->Get()->Record.GetTxLocks()) {
                    builder << lock.ShortDebugString();
                }
                return builder;
            }()
            << ", Cookie=" << ev->Cookie
            << ", error=" << issues.ToString());

        if (Stats) {
            Stats->AddDatashardStats(std::move(*res->Record.MutableTxStats()));
        }

        if (TxManager) {
            TxManager->AddParticipantNode(ev->Sender.NodeId());
        }

        switch (ev->Get()->GetStatus()) {
            case NKikimrDataEvents::TEvWriteResult::STATUS_UNSPECIFIED: {
                YQL_ENSURE(false);
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_PREPARED: {
                YQL_ENSURE(false);
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_COMPLETED: {
                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;

                Counters->TxProxyMon->ResultsReceivedCount->Inc();
                Counters->TxProxyMon->TxResultComplete->Inc();

                CheckExecutionComplete();
                return;
            }
            case NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN: {
                LOG_D("Broken locks: " << res->Record.DebugString());
                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;
                Counters->TxProxyMon->TxResultAborted->Inc();
                LocksBroken = true;
                ResponseEv->BrokenLockShardId = shardId;

                if (!res->Record.GetTxLocks().empty()) {
                    ResponseEv->BrokenLockPathId = NYql::TKikimrPathId(
                        res->Record.GetTxLocks(0).GetSchemeShard(),
                        res->Record.GetTxLocks(0).GetPathId());
                    ReplyErrorAndDie(Ydb::StatusIds::ABORTED, {});
                    return;
                }
                CheckExecutionComplete();
                return;
            }
            default:
            {
                return ShardError(res->Record);
            }
        }
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionResult::TPtr& ev) {
        YQL_ENSURE(!TxManager);
        TEvDataShard::TEvProposeTransactionResult* res = ev->Get();
        ResponseEv->Orbit.Join(res->Orbit);
        const ui64 shardId = res->GetOrigin();
        LastShard = shardId;

        ParticipantNodes.emplace(ev->Sender.NodeId());

        TShardState* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState);

        NDataIntegrity::LogIntegrityTrails("Execute", Request.UserTraceId, ev, TlsActivationContext->AsActorContext());
        LOG_D("Got propose result, shard: " << shardId << ", status: "
            << NKikimrTxDataShard::TEvProposeTransactionResult_EStatus_Name(res->GetStatus())
            << ", error: " << res->GetError());

        if (Stats) {
            Stats->AddDatashardStats(
                std::move(*res->Record.MutableComputeActorStats()),
                std::move(*res->Record.MutableTxStats()),
                TDuration::MilliSeconds(AggregationSettings.GetCollectLongTasksStatsTimeoutMs()));
        }

        switch (res->GetStatus()) {
            case NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE: {
                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;

                Counters->TxProxyMon->ResultsReceivedCount->Inc();
                Counters->TxProxyMon->ResultsReceivedSize->Add(res->GetTxResult().size());

                for (auto& lock : res->Record.GetTxLocks()) {
                    LOG_D("Shard " << shardId << " completed, store lock " << lock.ShortDebugString());
                    Locks.emplace_back(std::move(lock));
                }

                Counters->TxProxyMon->TxResultComplete->Inc();

                CheckExecutionComplete();
                return;
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::LOCKS_BROKEN: {
                LOG_D("Broken locks: " << res->Record.DebugString());

                YQL_ENSURE(shardState->State == TShardState::EState::Executing);
                shardState->State = TShardState::EState::Finished;

                Counters->TxProxyMon->TxResultAborted->Inc(); // TODO: dedicated counter?
                LocksBroken = true;
                ResponseEv->BrokenLockShardId = shardId; // todo: without responseEv

                if (!res->Record.GetTxLocks().empty()) {
                    ResponseEv->BrokenLockPathId = NYql::TKikimrPathId(
                        res->Record.GetTxLocks(0).GetSchemeShard(),
                        res->Record.GetTxLocks(0).GetPathId());
                    return ReplyErrorAndDie(Ydb::StatusIds::ABORTED, {});
                }

                CheckExecutionComplete();
                return;
            }
            case NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED: {
                YQL_ENSURE(false);
            }
            default: {
                return ShardError(res->Record);
            }
        }
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionRestart::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 shardId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(shardId);
        YQL_ENSURE(shardState, "restart tx event from unknown tabletId: " << shardId << ", tx: " << TxId);

        LOG_D("Got transaction restart event from tabletId: " << shardId << ", state: " << ToString(shardState->State)
            << ", txPlanned: " << TxPlanned);

        switch (shardState->State) {
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
            case TShardState::EState::Executing: {
                shardState->Restarting = true;
                return;
            }
            case TShardState::EState::Finished: {
                return;
            }
            case TShardState::EState::Initial: {
                YQL_ENSURE(false);
            }
        }
    }

    void HandleExecute(TEvDataShard::TEvProposeTransactionAttachResult::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const ui64 tabletId = record.GetTabletId();

        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState, "Unknown tablet " << tabletId);

        if (ev->Cookie != shardState->ReattachState.Cookie) {
            return;
        }

        switch (shardState->State) {
            case TShardState::EState::Executing:
                break;
            case TShardState::EState::Initial:
            case TShardState::EState::Preparing:
            case TShardState::EState::Prepared:
            case TShardState::EState::Finished:
                return;
        }

        if (record.GetStatus() == NKikimrProto::OK) {
            // Transaction still exists at this shard
            LOG_N("Reattached to shard " << tabletId << ", state was: " << ToString(shardState->State));
            shardState->ReattachState.Reattached();

            CheckExecutionComplete();
            return;
        }

        LOG_E("Shard " << tabletId << " transaction lost during reconnect: " << record.GetStatus());

        ReplyTxStateUnknown(tabletId);
    }

    void HandleExecute(TEvPrivate::TEvReattachToShard::TPtr& ev) {
        const ui64 tabletId = ev->Get()->TabletId;
        auto* shardState = ShardStates.FindPtr(tabletId);
        YQL_ENSURE(shardState);

        LOG_I("Reattach to shard " << tabletId);

        Send(MakePipePerNodeCacheID(false), new TEvPipeCache::TEvForward(
            new TEvDataShard::TEvProposeTransactionAttach(tabletId, TxId),
            tabletId, /* subscribe */ true), 0, ++shardState->ReattachState.Cookie);
    }

    void HandleExecute(TEvTxProxy::TEvProposeTransactionStatus::TPtr &ev) {
        TEvTxProxy::TEvProposeTransactionStatus* res = ev->Get();
        LOG_D("Got transaction status, status: " << res->GetStatus());

        switch (res->GetStatus()) {
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusAccepted:
                Counters->TxProxyMon->ClientTxStatusAccepted->Inc();
                break;
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusProcessed:
                Counters->TxProxyMon->ClientTxStatusProcessed->Inc();
                break;
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusConfirmed:
                Counters->TxProxyMon->ClientTxStatusConfirmed->Inc();
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusPlanned:
                Counters->TxProxyMon->ClientTxStatusPlanned->Inc();
                TxPlanned = true;
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusOutdated:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusDeclined:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusDeclinedNoSpace:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusRestarting:
                Counters->TxProxyMon->ClientTxStatusCoordinatorDeclined->Inc();
                CancelProposal(0);
                ReplyUnavailable(TStringBuilder() << "Failed to plan transaction, status: " << res->GetStatus());
                break;

            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusUnknown:
            case TEvTxProxy::TEvProposeTransactionStatus::EStatus::StatusAborted:
                Counters->TxProxyMon->ClientTxStatusCoordinatorDeclined->Inc();
                InternalError(TStringBuilder() << "Unexpected TEvProposeTransactionStatus status: " << res->GetStatus());
                break;
        }
    }

    void HandleExecute(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        TEvPipeCache::TEvDeliveryProblem* msg = ev->Get();

        LOG_D("DeliveryProblem to shard " << msg->TabletId << ", notDelivered: " << msg->NotDelivered
            << ", txPlanned: " << TxPlanned << ", coordinator: " << TxCoordinator);

        if (msg->TabletId == TxCoordinator) {
            if (msg->NotDelivered) {
                LOG_E("Not delivered to coordinator " << msg->TabletId << ", abort execution");
                CancelProposal(0);
                return ReplyUnavailable("Delivery problem: could not plan transaction.");
            }

            if (TxPlanned) {
                // We lost pipe to coordinator, but we already know tx is planned
                return;
            }

            LOG_E("Delivery problem to coordinator " << msg->TabletId << ", abort execution");
            return ReplyTxStateUnknown(msg->TabletId);
        }

        auto* shardState = ShardStates.FindPtr(msg->TabletId);
        YQL_ENSURE(shardState, "EvDeliveryProblem from unknown shard " << msg->TabletId);

        bool wasRestarting = std::exchange(shardState->Restarting, false);

        switch (shardState->State) {
            case TShardState::EState::Prepared: // is it correct?
                LOG_E("DeliveryProblem to shard " << msg->TabletId << ", notDelivered: " << msg->NotDelivered
                    << ", txPlanned: " << TxPlanned << ", coordinator: " << TxCoordinator);
                Y_DEBUG_ABORT_UNLESS(false);
                // Proceed with query processing
                [[fallthrough]];
            case TShardState::EState::Executing: {
                if ((wasRestarting || shardState->ReattachState.ReattachInfo.Reattaching) &&
                     shardState->ReattachState.ShouldReattach(TlsActivationContext->Now()))
                {
                    LOG_N("Shard " << msg->TabletId << " lost pipe while waiting for reply (reattaching in "
                        << shardState->ReattachState.ReattachInfo.Delay << ")");

                    Schedule(shardState->ReattachState.ReattachInfo.Delay, new TEvPrivate::TEvReattachToShard(msg->TabletId));
                    ++shardState->RestartCount;
                    return;
                }

                LOG_N("Shard " << msg->TabletId << " lost pipe while waiting for reply"
                    << (msg->NotDelivered ? " (last message not delivered)" : ""));

                return ReplyTxStateUnknown(msg->TabletId);
            }

            case TShardState::EState::Finished: {
                return;
            }

            case TShardState::EState::Initial:
            case TShardState::EState::Preparing:
                YQL_ENSURE(false, "Unexpected shard " << msg->TabletId << " state " << ToString(shardState->State));
        }
    }

private:
    bool IsReadOnlyTx() const {
        if (BufferActorId && TxManager->GetTopicOperations().HasOperations()) {
            YQL_ENSURE(!Request.UseImmediateEffects);
            return false;
        }

        if (!BufferActorId && Request.TopicOperations.HasOperations()) {
            YQL_ENSURE(!Request.UseImmediateEffects);
            return false;
        }

        if (Request.LocksOp == ELocksOp::Commit) {
            YQL_ENSURE(!Request.UseImmediateEffects);
            return false;
        }

        for (const auto& tx : Request.Transactions) {
            for (const auto& stage : tx.Body->GetStages()) {
                if (stage.GetIsEffectsStage()) {
                    return false;
                }
            }
        }

        return true;
    }

    void BuildDatashardTasks(TStageInfo& stageInfo) {
        THashMap<ui64, ui64> shardTasks; // shardId -> taskId
        auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);

        auto getShardTask = [&](ui64 shardId) -> TTask& {
            YQL_ENSURE(!TxManager);
            auto it  = shardTasks.find(shardId);
            if (it != shardTasks.end()) {
                return TasksGraph.GetTask(it->second);
            }
            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.Type = TTaskMeta::TTaskType::DataShard;
            task.Meta.ExecuterId = SelfId();
            task.Meta.ShardId = shardId;
            shardTasks.emplace(shardId, task.Id);

            FillSecureParamsFromStage(task.Meta.SecureParams, stage);
            BuildSinks(stage, stageInfo, task);

            return task;
        };

        const auto& tableInfo = stageInfo.Meta.TableConstInfo;
        const auto& keyTypes = tableInfo->KeyColumnTypes;

        for (auto& op : stage.GetTableOps()) {
            Y_DEBUG_ABORT_UNLESS(stageInfo.Meta.TablePath == op.GetTable().GetPath());
            switch (op.GetTypeCase()) {
                case NKqpProto::TKqpPhyTableOperation::kReadRanges:
                case NKqpProto::TKqpPhyTableOperation::kReadRange: {
                    auto columns = BuildKqpColumns(op, tableInfo);
                    bool isFullScan = false;
                    auto partitions = PartitionPruner.Prune(op, stageInfo, isFullScan);
                    auto readSettings = ExtractReadSettings(op, stageInfo, HolderFactory(), TypeEnv());

                    if (!readSettings.ItemsLimit && isFullScan) {
                        Counters->Counters->FullScansExecuted->Inc();
                    }

                    for (auto& [shardId, shardInfo] : partitions) {
                        YQL_ENSURE(!shardInfo.KeyWriteRanges);

                        auto& task = getShardTask(shardId);
                        MergeReadInfoToTaskMeta(task.Meta, shardId, shardInfo.KeyReadRanges, readSettings,
                            columns, op, /*isPersistentScan*/ false);
                    }

                    break;
                }

                case NKqpProto::TKqpPhyTableOperation::kUpsertRows:
                case NKqpProto::TKqpPhyTableOperation::kDeleteRows: {
                    YQL_ENSURE(stage.InputsSize() <= 1, "Effect stage with multiple inputs: " << stage.GetProgramAst());

                    auto result = PartitionPruner.PruneEffect(op, stageInfo);
                    for (auto& [shardId, shardInfo] : result) {
                        YQL_ENSURE(!shardInfo.KeyReadRanges);
                        YQL_ENSURE(shardInfo.KeyWriteRanges);

                        auto& task = getShardTask(shardId);

                        if (!task.Meta.Writes) {
                            task.Meta.Writes.ConstructInPlace();
                            task.Meta.Writes->Ranges = std::move(*shardInfo.KeyWriteRanges);
                        } else {
                            task.Meta.Writes->Ranges.MergeWritePoints(std::move(*shardInfo.KeyWriteRanges), keyTypes);
                        }

                        if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kDeleteRows) {
                            task.Meta.Writes->AddEraseOp();
                        } else {
                            task.Meta.Writes->AddUpdateOp();
                        }

                        for (const auto& [name, info] : shardInfo.ColumnWrites) {
                            auto& column = tableInfo->Columns.at(name);

                            auto& taskColumnWrite = task.Meta.Writes->ColumnWrites[column.Id];
                            taskColumnWrite.Column.Id = column.Id;
                            taskColumnWrite.Column.Type = column.Type;
                            taskColumnWrite.Column.Name = name;
                            taskColumnWrite.MaxValueSizeBytes = std::max(taskColumnWrite.MaxValueSizeBytes,
                                info.MaxValueSizeBytes);
                        }
                        ShardsWithEffects.insert(shardId);
                    }

                    break;
                }

                case NKqpProto::TKqpPhyTableOperation::kReadOlapRange: {
                    YQL_ENSURE(false, "The previous check did not work! Data query read does not support column shard tables." << Endl
                        << this->DebugString());
                }

                default: {
                    YQL_ENSURE(false, "Unexpected table operation: " << (ui32) op.GetTypeCase() << Endl
                        << this->DebugString());
                }
            }
        }

        LOG_D("Stage " << stageInfo.Id << " will be executed on " << shardTasks.size() << " shards.");

        for (auto& shardTask : shardTasks) {
            auto& task = TasksGraph.GetTask(shardTask.second);
            LOG_D("ActorState: " << CurrentStateFuncName()
                << ", stage: " << stageInfo.Id << " create datashard task: " << shardTask.second
                << ", shard: " << shardTask.first
                << ", meta: " << task.Meta.ToString(keyTypes, *AppData()->TypeRegistry));
        }
    }

    void ExecuteDatashardTransaction(ui64 shardId, NKikimrTxDataShard::TKqpTransaction& kqpTx, const bool isOlap)
    {
        YQL_ENSURE(ReadOnlyTx || !TxManager);
        TShardState shardState;
        shardState.State = ImmediateTx ? TShardState::EState::Executing : TShardState::EState::Preparing;
        shardState.DatashardState.ConstructInPlace();
        shardState.DatashardState->Follower = GetUseFollowers();

        if (Deadline) {
            TDuration timeout = *Deadline - TAppData::TimeProvider->Now();
            kqpTx.MutableRuntimeSettings()->SetTimeoutMs(timeout.MilliSeconds());
        }
        kqpTx.MutableRuntimeSettings()->SetExecType(NDqProto::TComputeRuntimeSettings::DATA);
        kqpTx.MutableRuntimeSettings()->SetStatsMode(GetDqStatsModeShard(Request.StatsMode));

        kqpTx.MutableRuntimeSettings()->SetUseSpilling(false);

        NKikimrTxDataShard::TDataTransaction dataTransaction;
        dataTransaction.MutableKqpTransaction()->Swap(&kqpTx);
        dataTransaction.SetImmediate(ImmediateTx);
        dataTransaction.SetReadOnly(ReadOnlyTx);
        if (CancelAt) {
            dataTransaction.SetCancelAfterMs((*CancelAt - AppData()->TimeProvider->Now()).MilliSeconds());
        }
        if (Request.PerShardKeysSizeLimitBytes) {
            YQL_ENSURE(!ReadOnlyTx);
            dataTransaction.SetPerShardKeysSizeLimitBytes(Request.PerShardKeysSizeLimitBytes);
        }

        const auto& lockTxId = TasksGraph.GetMeta().LockTxId;
        if (lockTxId) {
            dataTransaction.SetLockTxId(*lockTxId);
            dataTransaction.SetLockNodeId(SelfId().NodeId());
        }
        if (TasksGraph.GetMeta().LockMode && ImmediateTx) {
            dataTransaction.SetLockMode(*TasksGraph.GetMeta().LockMode);
        }

        for (auto& task : dataTransaction.GetKqpTransaction().GetTasks()) {
            shardState.TaskIds.insert(task.GetId());
        }

        auto locksCount = dataTransaction.GetKqpTransaction().GetLocks().LocksSize();
        shardState.DatashardState->ShardReadLocks = locksCount > 0;

        LOG_D("State: " << CurrentStateFuncName()
            << ", Executing KQP transaction on shard: " << shardId
            << ", tasks: [" << JoinStrings(shardState.TaskIds.begin(), shardState.TaskIds.end(), ",") << "]"
            << ", lockTxId: " << lockTxId
            << ", locks: " << dataTransaction.GetKqpTransaction().GetLocks().ShortDebugString()
            << ", immediate: " << ImmediateTx);

        std::unique_ptr<IEventBase> ev;
        if (isOlap) {
            const ui32 flags =
                (ImmediateTx ? NKikimrTxColumnShard::ETransactionFlag::TX_FLAG_IMMEDIATE: 0);
            ev.reset(new TEvColumnShard::TEvProposeTransaction(
                NKikimrTxColumnShard::TX_KIND_DATA,
                SelfId(),
                TxId,
                dataTransaction.SerializeAsString(),
                flags));
        } else {
            const ui32 flags =
                (ImmediateTx ? NTxDataShard::TTxFlags::Immediate : 0) |
                (VolatileTx ? NTxDataShard::TTxFlags::VolatilePrepare : 0);
            std::unique_ptr<TEvDataShard::TEvProposeTransaction> evData;
            if (GetSnapshot().IsValid()
                    && (ReadOnlyTx
                        || Request.UseImmediateEffects
                        || (Request.LocksOp == ELocksOp::Unspecified
                            && TasksGraph.GetMeta().LockMode == NKikimrDataEvents::OPTIMISTIC_SNAPSHOT_ISOLATION))) {
                evData.reset(new TEvDataShard::TEvProposeTransaction(
                    NKikimrTxDataShard::TX_KIND_DATA,
                    SelfId(),
                    TxId,
                    dataTransaction.SerializeAsString(),
                    GetSnapshot().Step,
                    GetSnapshot().TxId,
                    flags));
            } else {
                evData.reset(new TEvDataShard::TEvProposeTransaction(
                    NKikimrTxDataShard::TX_KIND_DATA,
                    SelfId(),
                    TxId,
                    dataTransaction.SerializeAsString(),
                    flags));
            }

            NDataIntegrity::LogIntegrityTrails("DatashardTx", dataTransaction.GetKqpTransaction().GetLocks().ShortDebugString(),
                Request.UserTraceId, TxId, shardId, TlsActivationContext->AsActorContext());

            ResponseEv->Orbit.Fork(evData->Orbit);
            ev = std::move(evData);
        }
        auto traceId = ExecuterSpan.GetTraceId();

        LOG_D("ExecuteDatashardTransaction traceId.verbosity: " << std::to_string(traceId.GetVerbosity()));

        Send(MakePipePerNodeCacheID(GetUseFollowers()), new TEvPipeCache::TEvForward(ev.release(), shardId, true), 0, 0, std::move(traceId));

        auto result = ShardStates.emplace(shardId, std::move(shardState));
        YQL_ENSURE(result.second);
    }

    void ExecuteEvWriteTransaction(ui64 shardId, NKikimrDataEvents::TEvWrite& evWrite) {
        YQL_ENSURE(!TxManager);
        TShardState shardState;
        shardState.State = ImmediateTx ? TShardState::EState::Executing : TShardState::EState::Preparing;
        shardState.DatashardState.ConstructInPlace();

        auto evWriteTransaction = std::make_unique<NKikimr::NEvents::TDataEvents::TEvWrite>();
        evWriteTransaction->Record = evWrite;
        evWriteTransaction->Record.SetTxMode(ImmediateTx ? NKikimrDataEvents::TEvWrite::MODE_IMMEDIATE : NKikimrDataEvents::TEvWrite::MODE_PREPARE);
        evWriteTransaction->Record.SetTxId(TxId);

        auto locksCount = evWriteTransaction->Record.GetLocks().LocksSize();
        shardState.DatashardState->ShardReadLocks = locksCount > 0;

        LOG_D("State: " << CurrentStateFuncName()
            << ", Executing EvWrite (PREPARE) on shard: " << shardId
            << ", TxId: " << TxId
            << ", locks: " << evWriteTransaction->Record.GetLocks().ShortDebugString());

        auto traceId = ExecuterSpan.GetTraceId();

        NDataIntegrity::LogIntegrityTrails("EvWriteTx", evWriteTransaction->Record.GetLocks().ShortDebugString(),
            Request.UserTraceId, TxId, shardId, TlsActivationContext->AsActorContext());

        auto shardsToString = [](const auto& shards) {
            TStringBuilder builder;
            for (const auto& shard : shards) {
                builder << shard << " ";
            }
            return builder;
        };

        LOG_D("Send EvWrite to ShardID=" << shardId
            << ", TxId=" << evWriteTransaction->Record.GetTxId()
            << ", TxMode=" << evWriteTransaction->Record.GetTxMode()
            << ", LockTxId=" << evWriteTransaction->Record.GetLockTxId() << ", LockNodeId=" << evWriteTransaction->Record.GetLockNodeId()
            << ", LocksOp=" << NKikimrDataEvents::TKqpLocks::ELocksOp_Name(evWriteTransaction->Record.GetLocks().GetOp())
            << ", SendingShards=" << shardsToString(evWriteTransaction->Record.GetLocks().GetSendingShards())
            << ", ReceivingShards=" << shardsToString(evWriteTransaction->Record.GetLocks().GetReceivingShards())
            << ", Locks= " << [&]() {
                TStringBuilder builder;
                for (const auto& lock : evWriteTransaction->Record.GetLocks().GetLocks()) {
                    builder << lock.ShortDebugString();
                }
                return builder;
            }());

        LOG_D("ExecuteEvWriteTransaction traceId.verbosity: " << std::to_string(traceId.GetVerbosity()));

        Send(MakePipePerNodeCacheID(false), new TEvPipeCache::TEvForward(evWriteTransaction.release(), shardId, true), 0, 0, std::move(traceId));

        auto result = ShardStates.emplace(shardId, std::move(shardState));
        YQL_ENSURE(result.second);
    }

    bool WaitRequired() const {
        return SecretSnapshotRequired || ResourceSnapshotRequired || SaveScriptExternalEffectRequired;
    }

    void HandleResolve(TEvDescribeSecretsResponse::TPtr& ev) {
        YQL_ENSURE(ev->Get()->Description.Status == Ydb::StatusIds::SUCCESS, "failed to get secrets snapshot with issues: " << ev->Get()->Description.Issues.ToOneLineString());

        for (size_t i = 0; i < SecretNames.size(); ++i) {
            SecureParams.emplace(SecretNames[i], ev->Get()->Description.SecretValues[i]);
        }

        SecretSnapshotRequired = false;
        if (!WaitRequired()) {
            Execute();
        }
    }

    void HandleResolve(TEvPrivate::TEvResourcesSnapshot::TPtr& ev) {
        if (ev->Get()->Snapshot.empty()) {
            LOG_E("Can not find default state storage group for database " << Database);
        }
        ResourcesSnapshot = std::move(ev->Get()->Snapshot);
        ResourceSnapshotRequired = false;
        if (!WaitRequired()) {
            Execute();
        }
    }

    void HandleResolve(TEvSaveScriptExternalEffectResponse::TPtr& ev) {
        YQL_ENSURE(ev->Get()->Status == Ydb::StatusIds::SUCCESS, "failed to save script external effect with issues: " << ev->Get()->Issues.ToOneLineString());

        SaveScriptExternalEffectRequired = false;
        if (!WaitRequired()) {
            Execute();
        }
    }

    void DoExecute() {
        const auto& requestContext = GetUserRequestContext();
        auto scriptExternalEffect = std::make_unique<TEvSaveScriptExternalEffectRequest>(
            requestContext->CurrentExecutionId, requestContext->Database,
            requestContext->CustomerSuppliedId, UserToken ? UserToken->GetUserSID() : ""
        );
        for (const auto& transaction : Request.Transactions) {
            for (const auto& secretName : transaction.Body->GetSecretNames()) {
                SecretSnapshotRequired = true;
                SecretNames.push_back(secretName);
            }
            for (const auto& stage : transaction.Body->GetStages()) {
                if (stage.SourcesSize() > 0 && stage.GetSources(0).GetTypeCase() == NKqpProto::TKqpSource::kExternalSource) {
                    ResourceSnapshotRequired = true;
                    HasExternalSources = true;
                }
                if (requestContext->CurrentExecutionId) {
                    for (const auto& sink : stage.GetSinks()) {
                        if (sink.GetTypeCase() == NKqpProto::TKqpSink::kExternalSink) {
                            SaveScriptExternalEffectRequired = true;
                            scriptExternalEffect->Description.Sinks.push_back(sink.GetExternalSink());
                        }
                    }
                }
            }
        }
        scriptExternalEffect->Description.SecretNames = SecretNames;

        if (!WaitRequired()) {
            return Execute();
        }
        if (SecretSnapshotRequired) {
            GetSecretsSnapshot();
        }
        if (ResourceSnapshotRequired) {
            GetResourcesSnapshot();
        }
        if (SaveScriptExternalEffectRequired) {
            SaveScriptExternalEffect(std::move(scriptExternalEffect));
        }
    }

    bool HasDmlOperationOnOlap(NKqpProto::TKqpPhyTx_EType queryType, const NKqpProto::TKqpPhyStage& stage) {
        if (queryType == NKqpProto::TKqpPhyTx::TYPE_DATA && !AllowOlapDataQuery) {
            return true;
        }

        for (const auto& input : stage.GetInputs()) {
            if (input.GetTypeCase() == NKqpProto::TKqpPhyConnection::kStreamLookup) {
                return true;
            }
        }

        for (const auto &tableOp : stage.GetTableOps()) {
            if (tableOp.GetTypeCase() != NKqpProto::TKqpPhyTableOperation::kReadOlapRange) {
                return true;
            }
        }

        return false;
    }

    void Execute() {
        LWTRACK(KqpDataExecuterStartExecute, ResponseEv->Orbit, TxId);

        size_t sourceScanPartitionsCount = 0;
        for (ui32 txIdx = 0; txIdx < Request.Transactions.size(); ++txIdx) {
            auto& tx = Request.Transactions[txIdx];
            auto scheduledTaskCount = ScheduleByCost(tx, ResourcesSnapshot);
            AFL_ENSURE(tx.Body->StagesSize() < (static_cast<ui64>(1) << PriorityTxShift));
            for (ui32 stageIdx = 0; stageIdx < tx.Body->StagesSize(); ++stageIdx) {
                auto& stage = tx.Body->GetStages(stageIdx);
                const auto stageId = TStageId(txIdx, stageIdx);
                auto& stageInfo = TasksGraph.GetStageInfo(stageId);
                AFL_ENSURE(stageInfo.Id == stageId);

                if (stageInfo.Meta.ShardKind == NSchemeCache::ETableKind::KindAsyncIndexTable) {
                    TMaybe<TString> error;

                    if (stageInfo.Meta.ShardKey->RowOperation != TKeyDesc::ERowOperation::Read) {
                        error = TStringBuilder() << "Non-read operations can't be performed on async index table"
                            << ": " << stageInfo.Meta.ShardKey->TableId;
                    } else if (Request.IsolationLevel != NKikimrKqp::ISOLATION_LEVEL_READ_STALE) {
                        error = TStringBuilder() << "Read operation can be performed on async index table"
                            << ": " << stageInfo.Meta.ShardKey->TableId << " only with StaleRO isolation level";
                    }

                    if (error) {
                        LOG_E(*error);
                        ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                            YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, *error));
                        return;
                    }
                }

                if ((stageInfo.Meta.IsOlap() && HasDmlOperationOnOlap(tx.Body->GetType(), stage))) {
                    auto error = TStringBuilder() << "Data manipulation queries do not support column shard tables.";
                    LOG_E(error);
                    ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                        YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, error));
                    return;
                }

                LOG_D("Stage " << stageInfo.Id << " AST: " << stage.GetProgramAst());

                if (stage.SourcesSize() > 0) {
                    switch (stage.GetSources(0).GetTypeCase()) {
                        case NKqpProto::TKqpSource::kReadRangesSource:
                            if (auto partitionsCount = BuildScanTasksFromSource(
                                    stageInfo,
                                    /* limitTasksPerNode */ StreamResult || EnableReadsMerge)) {
                                sourceScanPartitionsCount += *partitionsCount;
                            } else {
                                UnknownAffectedShardCount = true;
                            }
                            break;
                        case NKqpProto::TKqpSource::kExternalSource: {
                            auto it = scheduledTaskCount.find(stageIdx);
                            BuildReadTasksFromSource(stageInfo, ResourcesSnapshot, it != scheduledTaskCount.end() ? it->second.TaskCount : 0);
                        }
                            break;
                        default:
                            YQL_ENSURE(false, "unknown source type");
                    }
                } else if ((AllowOlapDataQuery || StreamResult) && stageInfo.Meta.IsOlap() && stage.SinksSize() == 0) {
                    BuildScanTasksFromShards(stageInfo, tx.Body->EnableShuffleElimination());
                } else if (stageInfo.Meta.IsSysView()) {
                    BuildSysViewScanTasks(stageInfo);
                } else if (stageInfo.Meta.ShardOperations.empty() || stage.SinksSize() > 0) {
                    BuildComputeTasks(stageInfo, std::max<ui32>(ShardsOnNode.size(), ResourcesSnapshot.size()));
                } else {
                    BuildDatashardTasks(stageInfo);
                }

                if (stage.GetIsSinglePartition()) {
                    YQL_ENSURE(stageInfo.Tasks.size() <= 1, "Unexpected multiple tasks in single-partition stage");
                }

                TasksGraph.GetMeta().AllowWithSpilling |= stage.GetAllowWithSpilling();
                BuildKqpStageChannels(TasksGraph, stageInfo, TxId, /* enableSpilling */ TasksGraph.GetMeta().AllowWithSpilling, tx.Body->EnableShuffleElimination());
            }

            ResponseEv->InitTxResult(tx.Body);
            BuildKqpTaskGraphResultChannels(TasksGraph, tx.Body, txIdx);
        }

        TIssue validateIssue;
        if (!ValidateTasks(TasksGraph, EExecType::Data, /* enableSpilling */ TasksGraph.GetMeta().AllowWithSpilling, validateIssue)) {
            ReplyErrorAndDie(Ydb::StatusIds::INTERNAL_ERROR, validateIssue);
            return;
        }

        if (Stats) {
            Stats->Prepare();
        }

        THashMap<ui64, TVector<NDqProto::TDqTask*>> datashardTasks;  // shardId -> [task]
        THashMap<ui64, TVector<ui64>> remoteComputeTasks;  // shardId -> [task]
        TVector<ui64> computeTasks;

        for (auto& task : TasksGraph.GetTasks()) {
            auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);
            if (task.Meta.ShardId && (task.Meta.Reads || task.Meta.Writes)) {
                NYql::NDqProto::TDqTask* protoTask = ArenaSerializeTaskToProto(TasksGraph, task, true);
                datashardTasks[task.Meta.ShardId].emplace_back(protoTask);

                ShardIdToTableInfo->Add(task.Meta.ShardId, stageInfo.Meta.TableKind == ETableKind::Olap, stageInfo.Meta.TablePath);
            } else if (stageInfo.Meta.IsSysView()) {
                computeTasks.emplace_back(task.Id);
            } else {
                if (task.Meta.ShardId) {
                    remoteComputeTasks[task.Meta.ShardId].emplace_back(task.Id);
                } else {
                    computeTasks.emplace_back(task.Id);
                }
            }
        }

        for(const auto& channel: TasksGraph.GetChannels()) {
            if (IsCrossShardChannel(TasksGraph, channel)) {
                HasPersistentChannels = true;
                break;
            }
        }

        if (computeTasks.size() > Request.MaxComputeActors) {
            LOG_N("Too many compute actors: " << computeTasks.size());
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED, TStringBuilder()
                    << "Requested too many execution units: " << computeTasks.size()));
            return;
        }

        ui32 shardsLimit = Request.MaxAffectedShards;
        if (i64 msc = (i64) Request.MaxShardCount; msc > 0) {
            shardsLimit = std::min(shardsLimit, (ui32) msc);
        }
        const size_t shards = datashardTasks.size() + sourceScanPartitionsCount;

        if (shardsLimit > 0 && shards > shardsLimit) {
            LOG_W("Too many affected shards: datashardTasks=" << shards << ", limit: " << shardsLimit);
            Counters->TxProxyMon->TxResultError->Inc();
            ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                YqlIssue({}, TIssuesIds::KIKIMR_PRECONDITION_FAILED, TStringBuilder()
                    << "Affected too many shards: " << datashardTasks.size()));
            return;
        }

        bool fitSize = AllOf(datashardTasks, [this](const auto& x){ return ValidateTaskSize(x.second); });
        if (!fitSize) {
            Counters->TxProxyMon->TxResultError->Inc();
            return;
        }

        TTopicTabletTxs topicTxs;
        TDatashardTxs datashardTxs;
        TEvWriteTxs evWriteTxs;

        if (!TxManager) {
            BuildDatashardTxs(datashardTasks, datashardTxs, evWriteTxs, topicTxs);
        }

        // Single-shard datashard transactions are always immediate
        auto topicSize = (BufferActorId) ? TxManager->GetTopicOperations().GetSize() : Request.TopicOperations.GetSize();
        ImmediateTx = (datashardTxs.size() + evWriteTxs.size() + topicSize + sourceScanPartitionsCount) <= 1
                    && !UnknownAffectedShardCount
                    && evWriteTxs.empty()
                    && !HasOlapTable;

        switch (Request.IsolationLevel) {
            // OnlineRO with AllowInconsistentReads = true
            case NKikimrKqp::ISOLATION_LEVEL_READ_UNCOMMITTED:
                YQL_ENSURE(ReadOnlyTx);
                YQL_ENSURE(!VolatileTx);
                TasksGraph.GetMeta().AllowInconsistentReads = true;
                ImmediateTx = true;
                break;

            default:
                break;
        }

        if (ImmediateTx) {
            // Transaction cannot be both immediate and volatile
            YQL_ENSURE(!VolatileTx);
        }

        if ((ReadOnlyTx || Request.UseImmediateEffects) && GetSnapshot().IsValid()) {
            // Snapshot reads are always immediate
            // Uncommitted writes are executed without coordinators, so they can be immediate
            YQL_ENSURE(!VolatileTx);
            ImmediateTx = true;
        }

        ComputeTasks = std::move(computeTasks);
        TopicTxs = std::move(topicTxs);
        DatashardTxs = std::move(datashardTxs);
        EvWriteTxs = std::move(evWriteTxs);
        RemoteComputeTasks = std::move(remoteComputeTasks);

        TasksGraph.GetMeta().UseFollowers = GetUseFollowers();

        if (RemoteComputeTasks) {
            TSet<ui64> shardIds;
            for (const auto& [shardId, _] : RemoteComputeTasks) {
                shardIds.insert(shardId);
            }

            ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::ExecuterShardsResolve, ExecuterSpan.GetTraceId(), "WaitForShardsResolve", NWilson::EFlags::AUTO_END);
            auto kqpShardsResolver = CreateKqpShardsResolver(
                SelfId(), TxId, TasksGraph.GetMeta().UseFollowers, std::move(shardIds));
            RegisterWithSameMailbox(kqpShardsResolver);
            Become(&TKqpDataExecuter::WaitResolveState);
        } else {
            OnShardsResolve();
        }
    }

    void HandleResolve(TEvKqpExecuter::TEvTableResolveStatus::TPtr& ev) {
        if (!TBase::HandleResolve(ev)) return;

        if (TxManager) {
            for (const auto& [stageId, stageInfo] : TasksGraph.GetStagesInfo()) {
                if (stageInfo.Meta.ShardKey) {
                    TxManager->SetPartitioning(stageInfo.Meta.TableId, stageInfo.Meta.ShardKey->Partitioning);
                }
                for (const auto& indexMeta : stageInfo.Meta.IndexMetas) {
                    if (indexMeta.ShardKey) {
                        TxManager->SetPartitioning(indexMeta.TableId, indexMeta.ShardKey->Partitioning);
                    }
                }
            }
        }

        if (StreamResult || EnableReadsMerge || AllowOlapDataQuery) {
            TSet<ui64> shardIds;
            for (auto& [stageId, stageInfo] : TasksGraph.GetStagesInfo()) {
                if (stageInfo.Meta.IsOlap()) {
                    HasOlapTable = true;
                }
                const auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);
                if (stage.SourcesSize() > 0 && stage.GetSources(0).GetTypeCase() == NKqpProto::TKqpSource::kReadRangesSource) {
                    YQL_ENSURE(stage.SourcesSize() == 1, "multiple sources in one task are not supported");
                    HasDatashardSourceScan = true;
                }
            }
            if (HasOlapTable) {
                for (auto& [stageId, stageInfo] : TasksGraph.GetStagesInfo()) {
                    if (stageInfo.Meta.ShardKey) {
                        for (auto& partition : stageInfo.Meta.ShardKey->GetPartitions()) {
                            shardIds.insert(partition.ShardId);
                        }
                    }
                }
            }
            if (HasDatashardSourceScan) {
                for (auto& [stageId, stageInfo] : TasksGraph.GetStagesInfo()) {
                    YQL_ENSURE(stageId == stageInfo.Id);
                    const auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);
                    if (stage.SourcesSize() > 0 && stage.GetSources(0).GetTypeCase() == NKqpProto::TKqpSource::kReadRangesSource) {
                        const auto& source = stage.GetSources(0).GetReadRangesSource();
                        bool isFullScan;
                        SourceScanStageIdToParititions[stageInfo.Id] = PartitionPruner.Prune(source, stageInfo, isFullScan);
                        if (isFullScan && !source.HasItemsLimit()) {
                            Counters->Counters->FullScansExecuted->Inc();
                        }
                        for (const auto& [shardId, _] : SourceScanStageIdToParititions.at(stageId)) {
                            shardIds.insert(shardId);
                        }
                    }
                }
            }

            if (shardIds.size() <= 1 && HasDatashardSourceScan) {
                // nothing to merge
                HasDatashardSourceScan = false;
            }

            if ((HasOlapTable || HasDatashardSourceScan) && shardIds) {
                LOG_D("Start resolving tablets nodes... (" << shardIds.size() << ")");
                ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::ExecuterShardsResolve, ExecuterSpan.GetTraceId(), "WaitForShardsResolve", NWilson::EFlags::AUTO_END);
                auto kqpShardsResolver = CreateKqpShardsResolver(
                    this->SelfId(), TxId, false, std::move(shardIds));
                KqpShardsResolverId = this->RegisterWithSameMailbox(kqpShardsResolver);
                return;
            } else if (HasOlapTable) {
                ResourceSnapshotRequired = true;
            }
        }
        DoExecute();
    }

    void HandleResolve(NShardResolver::TEvShardsResolveStatus::TPtr& ev) {
        if (!TBase::HandleResolve(ev)) {
            return;
        }
        if (HasOlapTable || HasDatashardSourceScan) {
            ResourceSnapshotRequired = ResourceSnapshotRequired || HasOlapTable;
            DoExecute();
            return;
        }

        OnShardsResolve();
    }

    void OnShardsResolve() {
        if (ForceAcquireSnapshot()) {
            YQL_ENSURE(!VolatileTx);
            auto longTxService = NLongTxService::MakeLongTxServiceID(SelfId().NodeId());
            Send(longTxService, new NLongTxService::TEvLongTxService::TEvAcquireReadSnapshot(Database));

            LOG_T("Create temporary mvcc snapshot, become WaitSnapshotState");
            Become(&TKqpDataExecuter::WaitSnapshotState);
            ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterAcquireSnapshot, ExecuterSpan.GetTraceId(), "WaitForSnapshot");

            return;
        }

        ContinueExecute();
    }

private:
    STATEFN(WaitSnapshotState) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(NLongTxService::TEvLongTxService::TEvAcquireReadSnapshotResult, Handle);
                hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
                hFunc(TEvKqpBuffer::TEvError, Handle);
                default:
                    UnexpectedEvent("WaitSnapshotState", ev->GetTypeRewrite());
            }
        } catch (const yexception& e) {
            InternalError(e.what());
        }
        ReportEventElapsedTime();
    }

    void Handle(NLongTxService::TEvLongTxService::TEvAcquireReadSnapshotResult::TPtr& ev) {
        auto& record = ev->Get()->Record;

        LOG_T("read snapshot result: " << record.GetStatus() << ", step: " << record.GetSnapshotStep()
            << ", tx id: " << record.GetSnapshotTxId());

        if (record.GetStatus() != Ydb::StatusIds::SUCCESS) {
            ExecuterStateSpan.EndError(TStringBuilder() << Ydb::StatusIds::StatusCode_Name(record.GetStatus()));
            ReplyErrorAndDie(record.GetStatus(), record.MutableIssues());
            return;
        }
        ExecuterStateSpan.EndOk();

        SetSnapshot(record.GetSnapshotStep(), record.GetSnapshotTxId());
        ImmediateTx = true;

        ContinueExecute();
    }

    using TDatashardTxs = THashMap<ui64, NKikimrTxDataShard::TKqpTransaction*>;
    using TEvWriteTxs = THashMap<ui64, NKikimrDataEvents::TEvWrite*>;
    using TTopicTabletTxs = NTopic::TTopicOperationTransactions;

    void ContinueExecute() {
        if (Stats) {
            //Stats->AffectedShards = datashardTxs.size();
            Stats->DatashardStats.reserve(DatashardTxs.size());
            //Stats->ComputeStats.reserve(computeTasks.size());
        }

        ExecuteTasks();

        if (CheckExecutionComplete()) {
            return;
        }

        ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::DataExecuterRunTasks, ExecuterSpan.GetTraceId(), "RunTasks", NWilson::EFlags::AUTO_END);
        if (ImmediateTx) {
            LOG_D("ActorState: " << CurrentStateFuncName()
                << ", immediate tx, become ExecuteState");
            Become(&TKqpDataExecuter::ExecuteState);
        } else {
            LOG_D("ActorState: " << CurrentStateFuncName()
                << ", not immediate tx, become PrepareState");
            Become(&TKqpDataExecuter::PrepareState);
        }
    }

    void BuildDatashardTxs(
            THashMap<ui64, TVector<NDqProto::TDqTask*>>& datashardTasks,
            TDatashardTxs& datashardTxs,
            TEvWriteTxs& evWriteTxs,
            TTopicTabletTxs& topicTxs) {
        for (auto& [shardId, tasks]: datashardTasks) {
            auto [it, success] = datashardTxs.emplace(
                shardId,
                TasksGraph.GetMeta().Allocate<NKikimrTxDataShard::TKqpTransaction>());

            YQL_ENSURE(success, "unexpected duplicates in datashard transactions");
            NKikimrTxDataShard::TKqpTransaction* dsTxs = it->second;
            dsTxs->MutableTasks()->Reserve(tasks.size());
            for (auto& task: tasks) {
                dsTxs->AddTasks()->Swap(task);
            }
        }

        // Note: when locks map is present it will be mutated to avoid copying data
        auto& locksMap = Request.DataShardLocks;
        if (!locksMap.empty()) {
            YQL_ENSURE(Request.LocksOp == ELocksOp::Commit || Request.LocksOp == ELocksOp::Rollback);
        }

        // Materialize (possibly empty) txs for all shards with locks (either commit or rollback)
        for (auto& [shardId, locksList] : locksMap) {
            YQL_ENSURE(!locksList.empty(), "unexpected empty locks list in DataShardLocks");
            NKikimrDataEvents::TKqpLocks* locks = nullptr;

            if (TxManager || ShardIdToTableInfo->Get(shardId).IsOlap) {
                if (auto it = evWriteTxs.find(shardId); it != evWriteTxs.end()) {
                    locks = it->second->MutableLocks();
                } else {
                    auto [eIt, success] = evWriteTxs.emplace(
                        shardId,
                        TasksGraph.GetMeta().Allocate<NKikimrDataEvents::TEvWrite>());
                    locks = eIt->second->MutableLocks();
                }
            } else {
                if (auto it = datashardTxs.find(shardId); it != datashardTxs.end()) {
                    locks = it->second->MutableLocks();
                } else {
                    auto [eIt, success] = datashardTxs.emplace(
                        shardId,
                        TasksGraph.GetMeta().Allocate<NKikimrTxDataShard::TKqpTransaction>());
                    locks = eIt->second->MutableLocks();
                }
            }

            switch (Request.LocksOp) {
                case ELocksOp::Commit:
                    locks->SetOp(NKikimrDataEvents::TKqpLocks::Commit);
                    break;
                case ELocksOp::Rollback:
                    locks->SetOp(NKikimrDataEvents::TKqpLocks::Rollback);
                    break;
                case ELocksOp::Unspecified:
                    break;
            }

            // Move lock descriptions to the datashard tx
            auto* protoLocks = locks->MutableLocks();
            protoLocks->Reserve(locksList.size());
            bool hasWrites = false;
            for (auto& lock : locksList) {
                hasWrites = hasWrites || lock.GetHasWrites();
                protoLocks->Add(std::move(lock));
            }
            locksList.clear();

            // When locks with writes are committed this commits accumulated effects
            if (Request.LocksOp == ELocksOp::Commit && hasWrites) {
                ShardsWithEffects.insert(shardId);
                YQL_ENSURE(!ReadOnlyTx);
            }
        }

        YQL_ENSURE(!TxManager);
        Request.TopicOperations.BuildTopicTxs(topicTxs);

        const bool needRollback = Request.LocksOp == ELocksOp::Rollback;

        VolatileTx = (
            // We want to use volatile transactions only when the feature is enabled
            AppData()->FeatureFlags.GetEnableDataShardVolatileTransactions() &&
            // We don't want volatile tx when acquiring locks (including write locks for uncommitted writes)
            !Request.AcquireLocksTxId &&
            // We don't want readonly volatile transactions
            !ReadOnlyTx &&
            // We only want to use volatile transactions with side-effects
            !ShardsWithEffects.empty() &&
            // We don't want to use volatile transactions when doing a rollback
            !needRollback &&
            // We cannot use volatile transactions with topics
            // TODO: add support in the future
            topicTxs.empty() &&
            // We only want to use volatile transactions for multiple shards
            (datashardTxs.size() + topicTxs.size()) > 1 &&
            // We cannot use volatile transactions with persistent channels
            // Note: currently persistent channels are never used
            !HasPersistentChannels &&
            // Can't use volatile transactions for EvWrite at current time
            evWriteTxs.empty());

        const bool useGenericReadSets = (
            // Use generic readsets when feature is explicitly enabled
            AppData()->FeatureFlags.GetEnableDataShardGenericReadSets() ||
            // Volatile transactions must always use generic readsets
            VolatileTx ||
            // Transactions with topics must always use generic readsets
            !topicTxs.empty() ||
            // HTAP transactions always use generic readsets
            !evWriteTxs.empty());

        if (!locksMap.empty() || VolatileTx || Request.TopicOperations.HasReadOperations()
            || Request.TopicOperations.HasWriteOperations())
        {
            YQL_ENSURE(Request.LocksOp == ELocksOp::Commit || Request.LocksOp == ELocksOp::Rollback || VolatileTx);

            bool needCommit = Request.LocksOp == ELocksOp::Commit || VolatileTx;

            absl::flat_hash_set<ui64> sendingShardsSet;
            absl::flat_hash_set<ui64> receivingShardsSet;
            absl::flat_hash_set<ui64> sendingColumnShardsSet;
            absl::flat_hash_set<ui64> receivingColumnShardsSet;
            ui64 arbiter = 0;
            std::optional<ui64> columnShardArbiter;

            // Gather shards that need to send/receive readsets (shards with effects)
            if (needCommit) {
                for (auto& [shardId, tx] : datashardTxs) {
                    if (tx->HasLocks()) {
                        // Locks may be broken so shards with locks need to send readsets
                        sendingShardsSet.insert(shardId);
                    }
                    if (ShardsWithEffects.contains(shardId)) {
                        // Volatile transactions may abort effects, so they send readsets
                        if (VolatileTx) {
                            sendingShardsSet.insert(shardId);
                        }
                        // Effects are only applied when all locks are valid
                        receivingShardsSet.insert(shardId);
                    }
                }

                for (auto& [shardId, tx] : evWriteTxs) {
                    if (tx->HasLocks()) {
                        // Locks may be broken so shards with locks need to send readsets
                        sendingShardsSet.insert(shardId);

                        if (ShardIdToTableInfo->Get(shardId).IsOlap) {
                            sendingColumnShardsSet.insert(shardId);
                        }
                    }
                    if (ShardsWithEffects.contains(shardId)) {
                        // Volatile transactions may abort effects, so they send readsets
                        if (VolatileTx) {
                            sendingShardsSet.insert(shardId);
                        }
                        // Effects are only applied when all locks are valid
                        receivingShardsSet.insert(shardId);

                        if (ShardIdToTableInfo->Get(shardId).IsOlap) {
                            receivingColumnShardsSet.insert(shardId);
                        }
                    }
                }

                if (auto tabletIds = Request.TopicOperations.GetSendingTabletIds()) {
                    sendingShardsSet.insert(tabletIds.begin(), tabletIds.end());
                    receivingShardsSet.insert(tabletIds.begin(), tabletIds.end());
                }

                if (auto tabletIds = Request.TopicOperations.GetReceivingTabletIds()) {
                    sendingShardsSet.insert(tabletIds.begin(), tabletIds.end());
                    receivingShardsSet.insert(tabletIds.begin(), tabletIds.end());
                }

                // The current value of 5 is arbitrary. Writing to 5 shards in
                // a single transaction is unusual enough, and having latency
                // regressions is unlikely. Full mesh readset count grows like
                // 2n(n-1), and arbiter reduces it to 4(n-1). Here's a readset
                // count table for various small `n`:
                //
                // n = 2: 4 -> 4
                // n = 3: 12 -> 8
                // n = 4: 24 -> 12
                // n = 5: 40 -> 16
                // n = 6: 60 -> 20
                // n = 7: 84 -> 24
                //
                // The ideal crossover is at n = 4, since the readset count
                // doesn't change when going from 3 to 4 shards, but the
                // increase in latency may not really be worth it. With n = 5
                // the readset count lowers from 24 to 16 readsets when going
                // from 4 to 5 shards. This makes 5 shards potentially cheaper
                // than 4 shards when readsets dominate the workload, but at
                // the price of possible increase in latency. Too many readsets
                // cause interconnect overload and reduce throughput however,
                // so we don't want to use a crossover value that is too high.
                const size_t minArbiterMeshSize = 5; // TODO: make configurable?
                if ((VolatileTx &&
                    receivingShardsSet.size() >= minArbiterMeshSize &&
                    AppData()->FeatureFlags.GetEnableVolatileTransactionArbiters()))
                {
                    std::vector<ui64> candidates;
                    candidates.reserve(receivingShardsSet.size());
                    for (ui64 candidate : receivingShardsSet) {
                        // Note: all receivers are also senders in volatile transactions
                        if (Y_LIKELY(sendingShardsSet.contains(candidate))) {
                            candidates.push_back(candidate);
                        }
                    }
                    if (candidates.size() >= minArbiterMeshSize) {
                        // Select a random arbiter
                        const ui32 index = RandomNumber<ui32>(candidates.size());
                        arbiter = candidates.at(index);
                    }
                }

                if (!receivingColumnShardsSet.empty() || !sendingColumnShardsSet.empty()) {
                    const auto& shards = receivingColumnShardsSet.empty()
                        ? sendingColumnShardsSet
                        : receivingColumnShardsSet;

                    const ui32 index = RandomNumber<ui32>(shards.size());
                    auto arbiterIterator = std::begin(shards);
                    std::advance(arbiterIterator, index);
                    columnShardArbiter = *arbiterIterator;
                    receivingShardsSet.insert(*columnShardArbiter);
                }
            }


            // Encode sending/receiving shards in tx bodies
            if (needCommit) {
                NProtoBuf::RepeatedField<ui64> sendingShards(sendingShardsSet.begin(), sendingShardsSet.end());
                NProtoBuf::RepeatedField<ui64> receivingShards(receivingShardsSet.begin(), receivingShardsSet.end());

                std::sort(sendingShards.begin(), sendingShards.end());
                std::sort(receivingShards.begin(), receivingShards.end());

                for (auto& [shardId, shardTx] : datashardTxs) {
                    shardTx->MutableLocks()->SetOp(NKikimrDataEvents::TKqpLocks::Commit);
                    if (!columnShardArbiter) {
                        *shardTx->MutableLocks()->MutableSendingShards() = sendingShards;
                        *shardTx->MutableLocks()->MutableReceivingShards() = receivingShards;
                        if (arbiter) {
                            shardTx->MutableLocks()->SetArbiterShard(arbiter);
                        }
                    } else if (!sendingShardsSet.empty() && !receivingShards.empty()) {
                        shardTx->MutableLocks()->AddSendingShards(*columnShardArbiter);
                        shardTx->MutableLocks()->AddReceivingShards(*columnShardArbiter);
                        if (sendingShardsSet.contains(shardId)) {
                            shardTx->MutableLocks()->AddSendingShards(shardId);
                        }
                        if (receivingShardsSet.contains(shardId)) {
                            shardTx->MutableLocks()->AddReceivingShards(shardId);
                        }
                        std::sort(
                            std::begin(*shardTx->MutableLocks()->MutableSendingShards()),
                            std::end(*shardTx->MutableLocks()->MutableSendingShards()));
                        std::sort(
                            std::begin(*shardTx->MutableLocks()->MutableReceivingShards()),
                            std::end(*shardTx->MutableLocks()->MutableReceivingShards()));
                        AFL_ENSURE(!arbiter);
                    }
                }

                for (auto& [shardId, tx] : evWriteTxs) {
                    tx->MutableLocks()->SetOp(NKikimrDataEvents::TKqpLocks::Commit);
                    if (!columnShardArbiter) {
                        *tx->MutableLocks()->MutableSendingShards() = sendingShards;
                        *tx->MutableLocks()->MutableReceivingShards() = receivingShards;
                        if (arbiter) {
                            tx->MutableLocks()->SetArbiterShard(arbiter);
                        }
                    } else if (*columnShardArbiter == shardId
                            && !sendingShardsSet.empty() && !receivingShardsSet.empty()) {
                        tx->MutableLocks()->SetArbiterColumnShard(*columnShardArbiter);
                        *tx->MutableLocks()->MutableSendingShards() = sendingShards;
                        *tx->MutableLocks()->MutableReceivingShards() = receivingShards;
                    } else if (!sendingShardsSet.empty() && !receivingShardsSet.empty()) {
                        tx->MutableLocks()->SetArbiterColumnShard(*columnShardArbiter);
                        tx->MutableLocks()->AddSendingShards(*columnShardArbiter);
                        tx->MutableLocks()->AddReceivingShards(*columnShardArbiter);
                        if (sendingShardsSet.contains(shardId)) {
                            tx->MutableLocks()->AddSendingShards(shardId);
                        }
                        if (receivingShardsSet.contains(shardId)) {
                            tx->MutableLocks()->AddReceivingShards(shardId);
                        }
                        std::sort(
                            std::begin(*tx->MutableLocks()->MutableSendingShards()),
                            std::end(*tx->MutableLocks()->MutableSendingShards()));
                        std::sort(
                            std::begin(*tx->MutableLocks()->MutableReceivingShards()),
                            std::end(*tx->MutableLocks()->MutableReceivingShards()));
                    }
                }

                for (auto& [shardId, t] : topicTxs) {
                    t.tx.SetOp(NKikimrPQ::TDataTransaction::Commit);
                    if (!columnShardArbiter) {
                        *t.tx.MutableSendingShards() = sendingShards;
                        *t.tx.MutableReceivingShards() = receivingShards;
                    } else if (!sendingShardsSet.empty() && !receivingShardsSet.empty()) {
                        t.tx.AddSendingShards(*columnShardArbiter);
                        t.tx.AddReceivingShards(*columnShardArbiter);
                        if (sendingShardsSet.contains(shardId)) {
                            t.tx.AddSendingShards(shardId);
                        }
                        if (receivingShardsSet.contains(shardId)) {
                            t.tx.AddReceivingShards(shardId);
                        }
                        std::sort(
                            std::begin(*t.tx.MutableSendingShards()),
                            std::end(*t.tx.MutableSendingShards()));
                        std::sort(
                            std::begin(*t.tx.MutableReceivingShards()),
                            std::end(*t.tx.MutableReceivingShards()));
                    }
                    YQL_ENSURE(!arbiter);
                }
            }
        }

        if (useGenericReadSets) {
            // Make sure datashards use generic readsets
            for (auto& pr : datashardTxs) {
                pr.second->SetUseGenericReadSets(true);
            }
        }
    }

    void ExecuteTasks() {
        auto lockTxId = Request.AcquireLocksTxId;
        if (lockTxId.Defined() && *lockTxId == 0) {
            lockTxId = TxId;
            LockHandle = TLockHandle(TxId, TActivationContext::ActorSystem());
        }

        LWTRACK(KqpDataExecuterStartTasksAndTxs, ResponseEv->Orbit, TxId, ComputeTasks.size(), DatashardTxs.size() + EvWriteTxs.size());

        for (auto& [shardId, tasks] : RemoteComputeTasks) {
            auto it = ShardIdToNodeId.find(shardId);
            YQL_ENSURE(it != ShardIdToNodeId.end());
            for (ui64 taskId : tasks) {
                auto& task = TasksGraph.GetTask(taskId);
                task.Meta.NodeId = it->second;
            }
        }

        TasksGraph.GetMeta().SinglePartitionOptAllowed = !HasOlapTable && !UnknownAffectedShardCount && !HasExternalSources && DatashardTxs.empty() && EvWriteTxs.empty();
        TasksGraph.GetMeta().LocalComputeTasks = !DatashardTxs.empty();
        TasksGraph.GetMeta().MayRunTasksLocally = !((HasExternalSources || HasOlapTable || HasDatashardSourceScan) && DatashardTxs.empty());

        bool isSubmitSuccessful = BuildPlannerAndSubmitTasks();
        if (!isSubmitSuccessful)
            return;

        // then start data tasks with known actor ids of compute tasks
        for (auto& [shardId, shardTx] : DatashardTxs) {
            shardTx->SetType(NKikimrTxDataShard::KQP_TX_TYPE_DATA);
            std::optional<bool> isOlap;
            for (auto& protoTask : *shardTx->MutableTasks()) {
                ui64 taskId = protoTask.GetId();
                auto& task = TasksGraph.GetTask(taskId);
                auto& stageInfo = TasksGraph.GetStageInfo(task.StageId);
                Y_ENSURE(!isOlap || *isOlap == stageInfo.Meta.IsOlap());
                isOlap = stageInfo.Meta.IsOlap();

                for (ui64 outputIndex = 0; outputIndex < task.Outputs.size(); ++outputIndex) {
                    auto& output = task.Outputs[outputIndex];
                    auto* protoOutput = protoTask.MutableOutputs(outputIndex);

                    for (ui64 outputChannelIndex = 0; outputChannelIndex < output.Channels.size(); ++outputChannelIndex) {
                        ui64 outputChannelId = output.Channels[outputChannelIndex];
                        auto* protoChannel = protoOutput->MutableChannels(outputChannelIndex);

                        ui64 dstTaskId = TasksGraph.GetChannel(outputChannelId).DstTask;

                        if (dstTaskId == 0) {
                            continue;
                        }

                        auto& dstTask = TasksGraph.GetTask(dstTaskId);
                        if (dstTask.ComputeActorId) {
                            protoChannel->MutableDstEndpoint()->Clear();
                            ActorIdToProto(dstTask.ComputeActorId, protoChannel->MutableDstEndpoint()->MutableActorId());
                        } else {
                            if (protoChannel->HasDstEndpoint() && protoChannel->GetDstEndpoint().HasTabletId()) {
                                if (protoChannel->GetDstEndpoint().GetTabletId() == shardId) {
                                    // inplace update
                                } else {
                                    // TODO: send data via executer?
                                    // but we don't have such examples...
                                    YQL_ENSURE(false, "not implemented yet: " << protoTask.DebugString());
                                }
                            } else {
                                YQL_ENSURE(!protoChannel->GetDstEndpoint().IsInitialized());
                                // effects-only stage
                            }
                        }
                    }
                }

                LOG_D("datashard task: " << taskId << ", proto: " << protoTask.ShortDebugString());
            }

            ExecuteDatashardTransaction(shardId, *shardTx, isOlap.value_or(false));
        }

        for (auto& [shardId, shardTx] : EvWriteTxs) {
            ExecuteEvWriteTransaction(shardId, *shardTx);
        }

        if (!TopicTxs.empty()) {
            ExecuteTopicTabletTransactions(TopicTxs);
        }

        LOG_I("Total tasks: " << TasksGraph.GetTasks().size()
            << ", readonly: " << ReadOnlyTx
            << ", datashardTxs: " << DatashardTxs.size()
            << ", evWriteTxs: " << EvWriteTxs.size()
            << ", topicTxs: " << Request.TopicOperations.GetSize()
            << ", volatile: " << VolatileTx
            << ", immediate: " << ImmediateTx
            << ", pending compute tasks" << (Planner ? Planner->GetPendingComputeTasks().size() : 0)
            << ", useFollowers: " << GetUseFollowers());

        // error
        LOG_T("Updating channels after the creation of compute actors");
        Y_ENSURE(Planner);
        THashMap<TActorId, THashSet<ui64>> updates;
        for (ui64 taskId : ComputeTasks) {
            auto& task = TasksGraph.GetTask(taskId);
            if (task.ComputeActorId)
                Planner->CollectTaskChannelsUpdates(task, updates);
        }
        Planner->PropagateChannelsUpdates(updates);
    }

    void ExecuteTopicTabletTransactions(TTopicTabletTxs& topicTxs) {
        YQL_ENSURE(!TxManager);
        TMaybe<ui64> writeId;

        if (Request.TopicOperations.HasWriteId()) {
            writeId = Request.TopicOperations.GetWriteId();
        }

        for (auto& [tabletId, t] : topicTxs) {
            auto& transaction = t.tx;

            auto ev = std::make_unique<TEvPersQueue::TEvProposeTransactionBuilder>();

            if (t.hasWrite && writeId.Defined()) {
                auto* w = transaction.MutableWriteId();
                w->SetNodeId(SelfId().NodeId());
                w->SetKeyId(*writeId);
            } else if (Request.TopicOperations.HasKafkaOperations() && t.hasWrite) {
                auto* w = transaction.MutableWriteId();
                w->SetKafkaTransaction(true);
                w->MutableKafkaProducerInstanceId()->SetId(Request.TopicOperations.GetKafkaProducerInstanceId().Id);
                w->MutableKafkaProducerInstanceId()->SetEpoch(Request.TopicOperations.GetKafkaProducerInstanceId().Epoch);
            }
            transaction.SetImmediate(ImmediateTx);

            ActorIdToProto(SelfId(), ev->Record.MutableSourceActor());
            ev->Record.MutableData()->Swap(&transaction);
            ev->Record.SetTxId(TxId);

            auto traceId = ExecuterSpan.GetTraceId();
            LOG_D("ExecuteTopicTabletTransaction traceId.verbosity: " << std::to_string(traceId.GetVerbosity()));

            LOG_D("Executing KQP transaction on topic tablet: " << tabletId
                  << ", writeId: " << writeId);

            Send(MakePipePerNodeCacheID(false),
                 new TEvPipeCache::TEvForward(ev.release(), tabletId, true),
                 0,
                 0,
                 std::move(traceId));

            TShardState state;
            state.State =
                ImmediateTx ? TShardState::EState::Executing : TShardState::EState::Preparing;
            state.DatashardState.ConstructInPlace();
            state.DatashardState->Follower = false;

            state.DatashardState->ShardReadLocks = Request.TopicOperations.TabletHasReadOperations(tabletId);

            auto result = ShardStates.emplace(tabletId, std::move(state));
            YQL_ENSURE(result.second);
        }
    }

    void Shutdown() override {
        if (Planner) {
            if (Planner->GetPendingComputeTasks().empty() && Planner->GetPendingComputeActors().empty()) {
                LOG_I("Shutdown immediately - nothing to wait");
                PassAway();
            } else {
                this->Become(&TThis::WaitShutdownState);
                LOG_I("Waiting for shutdown of " << Planner->GetPendingComputeTasks().size() << " tasks and "
                    << Planner->GetPendingComputeActors().size() << " compute actors");
                TActivationContext::Schedule(WaitCAStatsTimeout, new IEventHandle(SelfId(), SelfId(), new TEvents::TEvPoison));
            }
        } else {
            PassAway();
        }
    }

    void PassAway() override {
        auto totalTime = TInstant::Now() - StartTime;
        Counters->Counters->DataTxTotalTimeHistogram->Collect(totalTime.MilliSeconds());

        // TxProxyMon compatibility
        Counters->TxProxyMon->TxTotalTimeHgram->Collect(totalTime.MilliSeconds());
        Counters->TxProxyMon->TxExecuteTimeHgram->Collect(totalTime.MilliSeconds());

        Send(MakePipePerNodeCacheID(false), new TEvPipeCache::TEvUnlink(0));

        if (GetUseFollowers()) {
            Send(MakePipePerNodeCacheID(true), new TEvPipeCache::TEvUnlink(0));
        }

        TBase::PassAway();
    }

    STATEFN(WaitShutdownState) {
        switch(ev->GetTypeRewrite()) {
            hFunc(TEvDqCompute::TEvState, HandleShutdown);
            hFunc(TEvInterconnect::TEvNodeDisconnected, HandleShutdown);
            hFunc(TEvents::TEvPoison, HandleShutdown);
            hFunc(TEvDq::TEvAbortExecution, HandleShutdown);
            default:
                LOG_E("Unexpected event while waiting for shutdown: " << ev->GetTypeName()); // ignore all other events
        }
    }

    void HandleShutdown(TEvDqCompute::TEvState::TPtr& ev) {
        HandleComputeStats(ev);

        if (Planner->GetPendingComputeTasks().empty() && Planner->GetPendingComputeActors().empty()) {
            PassAway();
        }
    }

    void HandleShutdown(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
        const auto nodeId = ev->Get()->NodeId;
        LOG_N("Node has disconnected while shutdown: " << nodeId);

        YQL_ENSURE(Planner);

        for (const auto& task : TasksGraph.GetTasks()) {
            if (task.Meta.NodeId == nodeId && !task.Meta.Completed) {
                if (task.ComputeActorId) {
                    Planner->CompletedCA(task.Id, task.ComputeActorId);
                } else {
                    Planner->TaskNotStarted(task.Id);
                }
            }
        }

        if (Planner->GetPendingComputeTasks().empty() && Planner->GetPendingComputeActors().empty()) {
            PassAway();
        }
    }

    void HandleShutdown(TEvents::TEvPoison::TPtr& ev) {
        // Self-poison means timeout - don't wait anymore.
        LOG_I("Timed out on waiting for Compute Actors to finish - forcing shutdown. Sender: " << ev->Sender);

        if (ev->Sender == SelfId()) {
            PassAway();
        }
    }

    void HandleShutdown(TEvDq::TEvAbortExecution::TPtr& ev) {
        auto statusCode = NYql::NDq::DqStatusToYdbStatus(ev->Get()->Record.GetStatusCode());

        // TODO(ISSUE-12128): if wait stats timeout is less than 1% of the original query timeout, then still wait for stats.

        // In case of external timeout the response is already sent to the client - no need to wait for stats.
        if (statusCode == Ydb::StatusIds::TIMEOUT) {
            LOG_I("External timeout while waiting for Compute Actors to finish - forcing shutdown. Sender: " << ev->Sender);
            PassAway();
        }
    }

private:
    void ReplyTxStateUnknown(ui64 shardId) {
        auto message = TStringBuilder() << "Tx state unknown for shard " << shardId << ", txid " << TxId;
        if (ReadOnlyTx) {
            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
            issue.AddSubIssue(new TIssue(message));
            issue.GetSubIssues()[0]->SetCode(NKikimrIssues::TIssuesIds::TX_STATE_UNKNOWN, TSeverityIds::S_ERROR);
            ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
        } else {
            auto issue = YqlIssue({}, TIssuesIds::KIKIMR_OPERATION_STATE_UNKNOWN);
            issue.AddSubIssue(new TIssue(message));
            issue.GetSubIssues()[0]->SetCode(NKikimrIssues::TIssuesIds::TX_STATE_UNKNOWN, TSeverityIds::S_ERROR);
            ReplyErrorAndDie(Ydb::StatusIds::UNDETERMINED, issue);
        }
    }

    static bool HasMissingSnapshotError(const NKikimrTxDataShard::TEvProposeTransactionResult& result) {
        for (const auto& err : result.GetError()) {
            if (err.GetKind() == NKikimrTxDataShard::TError::SNAPSHOT_NOT_EXIST) {
                return true;
            }
        }
        return false;
    }

    static void AddDataShardErrors(const NKikimrTxDataShard::TEvProposeTransactionResult& result, TIssue& issue) {
        for (const auto& err : result.GetError()) {
            issue.AddSubIssue(new TIssue(TStringBuilder()
                << "[" << err.GetKind() << "] " << err.GetReason()));
        }
    }

    static void AddColumnShardErrors(const NKikimrTxColumnShard::TEvProposeTransactionResult& result, TIssue& issue) {
        issue.AddSubIssue(new TIssue(TStringBuilder() << result.GetStatusMessage()));
    }

    static std::string_view ToString(TShardState::EState state) {
        switch (state) {
            case TShardState::EState::Initial:   return "Initial"sv;
            case TShardState::EState::Preparing: return "Preparing"sv;
            case TShardState::EState::Prepared:  return "Prepared"sv;
            case TShardState::EState::Executing: return "Executing"sv;
            case TShardState::EState::Finished:  return "Finished"sv;
        }
    }

private:
    TShardIdToTableInfoPtr ShardIdToTableInfo;
    const bool AllowOlapDataQuery = false;

    bool HasExternalSources = false;
    bool SecretSnapshotRequired = false;
    bool ResourceSnapshotRequired = false;
    bool SaveScriptExternalEffectRequired = false;

    ui64 TxCoordinator = 0;
    THashMap<ui64, TShardState> ShardStates;
    TVector<NKikimrDataEvents::TLock> Locks;
    bool ReadOnlyTx = true;
    bool VolatileTx = false;
    bool ImmediateTx = false;
    bool TxPlanned = false;
    bool LocksBroken = false;

    TInstant FirstPrepareReply;
    TInstant LastPrepareReply;

    // Tracks which shards are expected to have effects
    THashSet<ui64> ShardsWithEffects;
    bool HasPersistentChannels = false;

    THashSet<ui64> SubscribedNodes;
    THashMap<ui64, TVector<ui64>> RemoteComputeTasks;

    TVector<ui64> ComputeTasks;
    TDatashardTxs DatashardTxs;
    TEvWriteTxs EvWriteTxs;
    TTopicTabletTxs TopicTxs;

    // Lock handle for a newly acquired lock
    TLockHandle LockHandle;
    ui64 LastShard = 0;

    const TDuration WaitCAStatsTimeout;
};

} // namespace

IActor* CreateKqpDataExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database, const TIntrusiveConstPtr<NACLib::TUserToken>& userToken,
    TKqpRequestCounters::TPtr counters, bool streamResult, const TExecuterConfig& executerConfig,
    NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory, const TActorId& creator,
    const TIntrusivePtr<TUserRequestContext>& userRequestContext, ui32 statementResultIndex,
    const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup, const TGUCSettings::TPtr& GUCSettings,
    TPartitionPruner::TConfig partitionPrunerConfig, const TShardIdToTableInfoPtr& shardIdToTableInfo,
    const IKqpTransactionManagerPtr& txManager, const TActorId bufferActorId,
    TMaybe<NBatchOperations::TSettings> batchOperationSettings)
{
    return new TKqpDataExecuter(std::move(request), database, userToken, counters, streamResult, executerConfig,
        std::move(asyncIoFactory), creator, userRequestContext, statementResultIndex, federatedQuerySetup, GUCSettings,
        std::move(partitionPrunerConfig), shardIdToTableInfo, txManager, bufferActorId, std::move(batchOperationSettings));
}

} // namespace NKqp
} // namespace NKikimr
