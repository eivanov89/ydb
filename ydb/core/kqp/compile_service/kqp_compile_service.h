#pragma once

#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/common/simple/temp_tables.h>
#include <ydb/core/kqp/gateway/kqp_gateway.h>
#include <ydb/core/kqp/federated_query/kqp_federated_query_helpers.h>

#include <library/cpp/lwtrace/rwspinlock.h>

namespace NKikimr {
namespace NKqp {

enum class ECompileActorAction {
    COMPILE,
    PARSE,
    SPLIT,
};

class TKqpQueryCache {
public:
    TKqpQueryCache(size_t size, TDuration ttl)
        : List(size)
        , Ttl(ttl) {}

    bool Insert(const TKqpCompileResult::TConstPtr& compileResult, bool isEnableAstCache, bool isPerStatementExecution) {
        TWriteSpinLockGuard guard(Lock);

        if (!isPerStatementExecution) {
            InsertQuery(compileResult);
        }
        if (isEnableAstCache && compileResult->Ast) {
            InsertAst(compileResult);
        }

        auto it = Index.emplace(compileResult->Uid, TCacheEntry{compileResult, TAppData::TimeProvider->Now() + Ttl});
        Y_ABORT_UNLESS(it.second);

        TItem* item = &const_cast<TItem&>(*it.first);
        auto removedItem = List.Insert(item);

        IncBytes(item->Value.CompileResult->PreparedQuery->ByteSize());

        if (removedItem) {
            DecBytes(removedItem->Value.CompileResult->PreparedQuery->ByteSize());

            auto queryId = *removedItem->Value.CompileResult->Query;
            QueryIndex.erase(queryId);
            if (removedItem->Value.CompileResult->Ast) {
                AstIndex.erase(GetQueryIdWithAst(queryId, *removedItem->Value.CompileResult->Ast));
            }
            auto indexIt = Index.find(*removedItem);
            if (indexIt != Index.end()) {
                Index.erase(indexIt);
            }
        }

        Y_ABORT_UNLESS(List.GetSize() == Index.size());

        return removedItem != nullptr;
    }

    void AttachReplayMessage(const TString uid, TString replayMessage) {
        TWriteSpinLockGuard guard(Lock);

        auto it = Index.find(TItem(uid));
        if (it != Index.end()) {
            TItem* item = &const_cast<TItem&>(*it);
            DecBytes(item->Value.ReplayMessage.size());
            item->Value.ReplayMessage = replayMessage;
            item->Value.LastReplayTime = TInstant::Now();
            IncBytes(replayMessage.size());
        }
    }

    TString ReplayMessageByUid(const TString uid, TDuration timeout) {
        TWriteSpinLockGuard guard(Lock);

        auto it = Index.find(TItem(uid));
        if (it != Index.end()) {
            TInstant& lastReplayTime = const_cast<TItem&>(*it).Value.LastReplayTime;
            TInstant now = TInstant::Now();
            if (lastReplayTime + timeout < now) {
                lastReplayTime = now;
                return it->Value.ReplayMessage;
            }
        }
        return "";
    }

    TKqpCompileResult::TConstPtr FindByUid(const TString& uid, bool /* promote */) {
        TReadSpinLockGuard guard(Lock);

        auto it = Index.find(TItem(uid));
        if (it != Index.end()) {
            TItem* item = &const_cast<TItem&>(*it);
            if (false) { // no promote to simplify locking
                item->Value.ExpiredAt = TAppData::TimeProvider->Now() + Ttl;
                List.Promote(item);
            }

            return item->Value.CompileResult;
        }

        return nullptr;
    }

    void Replace(const TKqpCompileResult::TConstPtr& compileResult) {
        TWriteSpinLockGuard guard(Lock);

        auto it = Index.find(TItem(compileResult->Uid));
        if (it != Index.end()) {
            TItem& item = const_cast<TItem&>(*it);
            item.Value.CompileResult = compileResult;
        }
    }

    TKqpCompileResult::TConstPtr FindByQuery(const TKqpQueryId& query, bool promote) {
        TReadSpinLockGuard guard(Lock);

        auto uid = QueryIndex.FindPtr(query);
        if (!uid) {
            return nullptr;
        }

        // we're holding read and assume it's recursive
        return FindByUid(*uid, promote);
    }

    TKqpCompileResult::TConstPtr FindByAst(const TKqpQueryId& query, const NYql::TAstParseResult& ast, bool promote) {
        TReadSpinLockGuard guard(Lock);

        auto uid = AstIndex.FindPtr(GetQueryIdWithAst(query, ast));
        if (!uid) {
            return nullptr;
        }

        return FindByUid(*uid, promote);
    }

    bool EraseByUid(const TString& uid) {
        TWriteSpinLockGuard guard(Lock);
        return EraseByUidImpl(uid);
    }

    size_t Size() const {
        TReadSpinLockGuard guard(Lock);
        return SizeImpl();
    }

    ui64 Bytes() const {
        TReadSpinLockGuard guard(Lock);
        return ByteSize;
    }

    size_t EraseExpiredQueries() {
        TWriteSpinLockGuard guard(Lock);

        auto prevSize = SizeImpl();

        auto now = TAppData::TimeProvider->Now();
        while (List.GetSize() && List.GetOldest()->Value.ExpiredAt <= now) {
            EraseByUidImpl(List.GetOldest()->Key);
        }

        Y_ABORT_UNLESS(List.GetSize() == Index.size());
        return prevSize - SizeImpl();
    }

    void Clear() {
        TWriteSpinLockGuard guard(Lock);

        List = TList(List.GetMaxSize());
        Index.clear();
        QueryIndex.clear();
        AstIndex.clear();
        ByteSize = 0;
    }

private:
    size_t SizeImpl() const {
        return Index.size();
    }

    void InsertQuery(const TKqpCompileResult::TConstPtr& compileResult) {
        Y_ENSURE(compileResult->Query);
        auto& query = *compileResult->Query;

        YQL_ENSURE(compileResult->PreparedQuery);

        auto queryIt = QueryIndex.emplace(query, compileResult->Uid);
        if (!queryIt.second) {
            EraseByUid(compileResult->Uid);
            QueryIndex.erase(query);
        }
        Y_ENSURE(queryIt.second);
    }

    void InsertAst(const TKqpCompileResult::TConstPtr& compileResult) {
        Y_ENSURE(compileResult->Query);
        Y_ENSURE(compileResult->Ast);

        AstIndex.emplace(GetQueryIdWithAst(*compileResult->Query, *compileResult->Ast), compileResult->Uid);
    }

    bool EraseByUidImpl(const TString& uid) {
        auto it = Index.find(TItem(uid));
        if (it == Index.end()) {
            return false;
        }

        TItem* item = &const_cast<TItem&>(*it);
        List.Erase(item);

        DecBytes(item->Value.CompileResult->PreparedQuery->ByteSize());
        DecBytes(item->Value.ReplayMessage.size());

        Y_ABORT_UNLESS(item->Value.CompileResult);
        Y_ABORT_UNLESS(item->Value.CompileResult->Query);
        auto queryId = *item->Value.CompileResult->Query;
        QueryIndex.erase(queryId);
        if (item->Value.CompileResult->Ast) {
            AstIndex.erase(GetQueryIdWithAst(queryId, *item->Value.CompileResult->Ast));
        }

        Index.erase(it);

        Y_ABORT_UNLESS(List.GetSize() == Index.size());
        return true;
    }

    TKqpQueryId GetQueryIdWithAst(const TKqpQueryId& query, const NYql::TAstParseResult& ast) {
        Y_ABORT_UNLESS(ast.Root);
        std::shared_ptr<std::map<TString, Ydb::Type>> astPgParams;
        if (query.QueryParameterTypes || ast.PgAutoParamValues) {
            astPgParams = std::make_shared<std::map<TString, Ydb::Type>>();
            if (query.QueryParameterTypes) {
                for (const auto& [name, param] : *query.QueryParameterTypes) {
                    astPgParams->insert({name, param});
                }
            }
            if (ast.PgAutoParamValues) {
                for (const auto& [name, param] : *ast.PgAutoParamValues) {
                    astPgParams->insert({name, param.Gettype()});
                }
            }
        }
        return TKqpQueryId{query.Cluster, query.Database, ast.Root->ToString(), query.Settings, astPgParams, query.GUCSettings};
    }

    void DecBytes(ui64 bytes) {
        if (bytes > ByteSize) {
            ByteSize = 0;
        } else {
            ByteSize -= bytes;
        }
    }

    void IncBytes(ui64 bytes) {
        ByteSize += bytes;
    }

private:
    struct TCacheEntry {
        TKqpCompileResult::TConstPtr CompileResult;
        TInstant ExpiredAt;
        TString ReplayMessage = "";
        TInstant LastReplayTime = TInstant::Zero();
    };

    using TList = TLRUList<TString, TCacheEntry>;
    using TItem = TList::TItem;

private:
    TList List;
    THashSet<TItem, TItem::THash> Index;
    THashMap<TKqpQueryId, TString, THash<TKqpQueryId>> QueryIndex;
    THashMap<TKqpQueryId, TString, THash<TKqpQueryId>> AstIndex;
    ui64 ByteSize = 0;
    TDuration Ttl;

    TRWSpinLock Lock;
};

IActor* CreateKqpCompileService(
    TKqpQueryCache& queryCache,
    const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
    const NKikimrConfig::TQueryServiceConfig& queryServiceConfig,
    const TKqpSettings::TConstPtr& kqpSettings, TIntrusivePtr<TModuleResolverState> moduleResolverState,
    TIntrusivePtr<TKqpCounters> counters, std::shared_ptr<IQueryReplayBackendFactory> queryReplayFactory,
    std::optional<TKqpFederatedQuerySetup> federatedQuerySetup
    );

IActor* CreateKqpCompileComputationPatternService(const NKikimrConfig::TTableServiceConfig& serviceConfig,
    TIntrusivePtr<TKqpCounters> counters);

IActor* CreateKqpCompileActor(const TActorId& owner, const TKqpSettings::TConstPtr& kqpSettings,
    const NKikimrConfig::TTableServiceConfig& tableServiceConfig,
    const NKikimrConfig::TQueryServiceConfig& queryServiceConfig,
    TIntrusivePtr<TModuleResolverState> moduleResolverState, TIntrusivePtr<TKqpCounters> counters,
    const TString& uid, const TKqpQueryId& query,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken,
    std::optional<TKqpFederatedQuerySetup> federatedQuerySetup,
    TKqpDbCountersPtr dbCounters, const TGUCSettings::TPtr& gUCSettings, const TMaybe<TString>& applicationName,
    const TIntrusivePtr<TUserRequestContext>& userRequestContext, NWilson::TTraceId traceId = {},
    TKqpTempTablesState::TConstPtr tempTablesState = nullptr,
    ECompileActorAction compileAction = ECompileActorAction::COMPILE,
    TMaybe<TQueryAst> queryAst = {},
    bool collectFullDiagnostics = false,
    bool PerStatementResult = false,
    NYql::TExprContext* ctx = nullptr,
    NYql::TExprNode::TPtr expr = nullptr);

IActor* CreateKqpCompileRequestActor(const TActorId& owner, const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, const TMaybe<TString>& uid,
    TMaybe<TKqpQueryId>&& query, bool keepInCache, const TInstant& deadline, TKqpDbCountersPtr dbCounters,
    ui64 cookie, NLWTrace::TOrbit orbit = {}, NWilson::TTraceId = {});

} // namespace NKqp
} // namespace NKikimr
