#include "ydb_ping.h"

#include <ydb/core/grpc_services/service_ping.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/counters/counters.h>
#include <ydb/core/grpc_services/grpc_helper.h>

namespace NKikimr::NGRpcService {

TGRpcYdbPingService::TGRpcYdbPingService(NActors::TActorSystem *system,
                                         TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
                                         const NActors::TActorId& proxyId,
                                         bool rlAllowed,
                                         size_t handlersPerCompletionQueue)
    : TGrpcServiceBase(system, counters, proxyId, rlAllowed)
    , HandlersPerCompletionQueue(Max(size_t{1}, handlersPerCompletionQueue))
{
}

TGRpcYdbPingService::TGRpcYdbPingService(NActors::TActorSystem *system,
                                         TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
                                         const TVector<NActors::TActorId>& proxies,
                                         bool rlAllowed,
                                         size_t handlersPerCompletionQueue)
    : TGrpcServiceBase(system, counters, proxies, rlAllowed)
    , HandlersPerCompletionQueue(Max(size_t{1}, handlersPerCompletionQueue))
{
}

void TGRpcYdbPingService::SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger) {
    using namespace Ydb::Ping;

    auto getCounterBlock = CreateCounterCb(Counters_, ActorSystem_);
    size_t proxyCounter = 0;

    for (size_t i = 0; i < HandlersPerCompletionQueue; ++i) {
        for (auto* cq: CQS) {
            MakeIntrusive<TGRpcRequest<PlainGrpcRequest, PlainGrpcResponse, TGRpcYdbPingService>>(this, &Service_, cq,
                [this](NYdbGrpc::IRequestContextBase* ctx) {
                    NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer());
                    PlainGrpcResponse response;
                    ctx->Reply(&response, 0);
                }, &Ydb::Ping::V1::PingService::AsyncService::RequestPingPlainGrpc,
                "PingPlainGrpc", logger, getCounterBlock("ping", "PingPlainGrpc"))->Run();
        }
    }

#ifdef ADD_REQUEST
#error ADD_REQUEST macro already defined
#endif
#define ADD_REQUEST(NAME, IN, OUT, CB, REQUEST_TYPE) \
    for (size_t i = 0; i < HandlersPerCompletionQueue; ++i) {  \
        for (auto* cq: CQS) { \
            MakeIntrusive<TGRpcRequest<IN, OUT, TGRpcYdbPingService>>(this, &Service_, cq, \
                [this, proxyCounter](NYdbGrpc::IRequestContextBase* ctx) { \
                    NGRpcService::ReportGrpcReqToMon(*ActorSystem_, ctx->GetPeer()); \
                    ActorSystem_->Send(GRpcProxies_[proxyCounter % GRpcProxies_.size()], \
                        new TGrpcRequestNoOperationCall<IN, OUT> \
                            (ctx, &CB, TRequestAuxSettings { \
                                .RlMode = RLSWITCH(TRateLimiterMode::Rps), \
                                .RequestType = NJaegerTracing::ERequestType::PING_##REQUEST_TYPE, \
                            })); \
                }, &Ydb::Ping::V1::PingService::AsyncService::Request ## NAME, \
                #NAME, logger, getCounterBlock("query", #NAME))->Run(); \
            ++proxyCounter; \
        }  \
    }

    ADD_REQUEST(PingGrpcProxy, GrpcProxyRequest, GrpcProxyResponse, DoPing, PROXY);
    ADD_REQUEST(PingKqpProxy, KqpProxyRequest, KqpProxyResponse, DoPing, KQP);

#undef ADD_REQUEST

}

} // namespace NKikimr::NGRpcService
