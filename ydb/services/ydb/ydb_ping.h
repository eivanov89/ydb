#pragma once

#include <ydb/core/grpc_services/base/base_service.h>
#include <ydb/public/api/grpc/ydb_ping_v1.grpc.pb.h>

namespace NKikimr::NGRpcService {

class TGRpcYdbPingService
    : public TGrpcServiceBase<Ydb::Ping::V1::PingService>
{
public:
    using TGrpcServiceBase<Ydb::Ping::V1::PingService>::TGrpcServiceBase;

    TGRpcYdbPingService(
        NActors::TActorSystem *system,
        TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
        const NActors::TActorId& proxyId,
        bool rlAllowed,
        size_t handlersPerCompletionQueue = 1);

    TGRpcYdbPingService(
        NActors::TActorSystem *system,
        TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
        const TVector<NActors::TActorId>& proxies,
        bool rlAllowed,
        size_t handlersPerCompletionQueue);

private:
    void SetupIncomingRequests(NYdbGrpc::TLoggerPtr logger);

private:
    const size_t HandlersPerCompletionQueue;
};

} // namespace NKikimr::NGRpcService
