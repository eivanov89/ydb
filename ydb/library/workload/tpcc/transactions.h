#pragma once

#include "task_queue.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>

#include <library/cpp/threading/future/core/coroutine_traits.h>

#include <memory>

class TLog;

namespace NYdb::NTPCC {

//-----------------------------------------------------------------------------

struct TTransactionContext {
    size_t TerminalID; // unrelated to TPC-C, part of implementation
    size_t WarehouseID;
    size_t WarehouseCount;
    ITaskQueue& TaskQueue;
    std::shared_ptr<NQuery::TQueryClient> Client;
    const TString Path;
    std::shared_ptr<TLog> Log;
};

struct TTransactionResult {
    enum EStatus {
        E_OK,
        E_ABORTED,
        E_ERROR
    };

    // intentionally use error as default value, since it's easy to spot in results
    TTransactionResult(EStatus status = E_ERROR)
        : Status(status)
    {
    }

    EStatus Status;
};

struct TUserAbortedException : public yexception {
};

using TTransactionTask = TTask<TTransactionResult>;

//-----------------------------------------------------------------------------

NThreading::TFuture<TStatus> GetNewOrderTask(
    TTransactionContext& context,
    NQuery::TSession session);

/*
TTransactionTask GetDeliveryTask(
    TTransactionContext& context,
    NThreading::TPromise<TStatus> promise,
    NQuery::TSession session);


TTransactionTask GetOrderStatusTask(
    TTransactionContext& context,
    NThreading::TPromise<TStatus> promise,
    NQuery::TSession session);


TTransactionTask GetPaymentTask(
    TTransactionContext& context,
    NThreading::TPromise<TStatus> promise,
    NQuery::TSession session);

TTransactionTask GetStockLevelTask(
    TTransactionContext& context,
    NThreading::TPromise<TStatus> promise,
    NQuery::TSession session);
*/

} // namespace NYdb::NTPCC
