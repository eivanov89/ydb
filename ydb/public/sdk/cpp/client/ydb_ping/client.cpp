#include "client.h"

#include <ydb/public/api/grpc/ydb_ping_v1.grpc.pb.h>
#include <ydb/public/api/grpc/ydb_ping_v1.pb.h>
#include <ydb/public/api/protos/ydb_ping.pb.h>

#define INCLUDE_YDB_INTERNAL_H
#include <ydb/public/sdk/cpp/client/impl/ydb_internal/make_request/make.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <ydb/public/sdk/cpp/client/ydb_common_client/impl/client.h>

namespace NYdb::NPing {

using namespace NThreading;

class TPingClient::TImpl: public TClientImplCommon<TPingClient::TImpl> {
public:
    TImpl(std::shared_ptr<TGRpcConnectionsImpl>&& connections, const TClientSettings& settings)
        : TClientImplCommon(std::move(connections), settings)
    {}

    TAsyncPlainGrpcPingResult PlainGrpcPing(const TPlainGrpcPingSettings& settings) {
        auto pingPromise = NewPromise<TPlainGrpcPingResult>();
        auto responseCb = [pingPromise] (Ydb::Ping::PlainGrpcResponse*, TPlainStatus status) mutable {
            TPlainGrpcPingResult val(TStatus(std::move(status)));
            pingPromise.SetValue(std::move(val));
        };

        Connections_->Run<Ydb::Ping::V1::PingService, Ydb::Ping::PlainGrpcRequest, Ydb::Ping::PlainGrpcResponse>(
            Ydb::Ping::PlainGrpcRequest(),
            responseCb,
            &Ydb::Ping::V1::PingService::Stub::AsyncPingPlainGrpc,
            DbDriverState_,
            TRpcRequestSettings::Make(settings));

        return pingPromise;
    }

    TAsyncGrpcProxyPingResult GrpcProxyPing(const TGrpcProxyPingSettings& settings) {
        auto pingPromise = NewPromise<TGrpcProxyPingResult>();
        auto responseCb = [pingPromise] (Ydb::Ping::GrpcProxyResponse*, TPlainStatus status) mutable {
            TGrpcProxyPingResult val(TStatus(std::move(status)));
            pingPromise.SetValue(std::move(val));
        };

        Connections_->Run<Ydb::Ping::V1::PingService, Ydb::Ping::GrpcProxyRequest, Ydb::Ping::GrpcProxyResponse>(
            Ydb::Ping::GrpcProxyRequest(),
            responseCb,
            &Ydb::Ping::V1::PingService::Stub::AsyncPingGrpcProxy,
            DbDriverState_,
            TRpcRequestSettings::Make(settings));

        return pingPromise;
    }

    TAsyncKqpProxyPingResult KqpProxyPing(const TKqpProxyPingSettings& settings) {
        auto pingPromise = NewPromise<TKqpProxyPingResult>();
        auto responseCb = [pingPromise] (Ydb::Ping::KqpProxyResponse*, TPlainStatus status) mutable {
            TKqpProxyPingResult val(TStatus(std::move(status)));
            pingPromise.SetValue(std::move(val));
        };

        Connections_->Run<Ydb::Ping::V1::PingService, Ydb::Ping::KqpProxyRequest, Ydb::Ping::KqpProxyResponse>(
            Ydb::Ping::KqpProxyRequest(),
            responseCb,
            &Ydb::Ping::V1::PingService::Stub::AsyncPingKqpProxy,
            DbDriverState_,
            TRpcRequestSettings::Make(settings));

        return pingPromise;
    }

    ~TImpl() = default;
};

TPingClient::TPingClient(const TDriver& driver, const TClientSettings& settings)
    : Impl_(new TImpl(CreateInternalInterface(driver), settings))
{
}

TAsyncPlainGrpcPingResult TPingClient::PlainGrpcPing(const TPlainGrpcPingSettings& settings) {
    return Impl_->PlainGrpcPing(settings);
}

TAsyncGrpcProxyPingResult TPingClient::GrpcProxyPing(const TGrpcProxyPingSettings& settings) {
    return Impl_->GrpcProxyPing(settings);
}

TAsyncKqpProxyPingResult TPingClient::KqpProxyPing(const TKqpProxyPingSettings& settings) {
    return Impl_->KqpProxyPing(settings);
}

} // namespace NYdb::NPing