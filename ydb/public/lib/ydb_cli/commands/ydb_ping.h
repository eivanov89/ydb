#pragma once

#pragma once

#include "ydb_command.h"

#include <ydb/public/lib/ydb_cli/common/format.h>
#include <ydb/public/lib/ydb_cli/common/interruptible.h>

namespace NYdb {

namespace NQuery {
    class TExecuteQueryIterator;
    class TQueryClient;
} // namespace NQuery

namespace NPing {
    class TPingClient;
};

namespace NConsoleClient {

class TCommandPing : public TYdbCommand, public TCommandWithFormat,
    public TInterruptibleCommand
{
public:
    TCommandPing();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;

private:
    int RunCommand(TConfig& config);
    int PrintResponse(NQuery::TExecuteQueryIterator& result);

    bool PingPlainGrpc(NPing::TPingClient& client);
    bool PingPlainKqp(NPing::TPingClient& client);
    bool PingGrpcProxy(NPing::TPingClient& client);

    bool PingKqp(NQuery::TQueryClient& client, const TString& query);

private:
    enum class EPingType {
        PlainGrpc,
        GrpcProxy,
        PlainKqp,
        Kqp,
        KqpSys,
    };

private:
    int Count = 0;
    TString PingTypeStr = "kqp";

    TDuration DelayBetween = TDuration::MilliSeconds(100);
    EPingType PingType = EPingType::Kqp;
};

} // NYdb::NConsoleClient
} // namespace NYdb
