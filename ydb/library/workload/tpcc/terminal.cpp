#include "terminal.h"

#include "log.h"

//-----------------------------------------------------------------------------

using namespace NYdb::NQuery;

//-----------------------------------------------------------------------------

namespace NYdb::NTPCC {

TTerminal::TTerminal(size_t terminalID,
                     size_t warehouseID,
                     size_t warehouseCount,
                     ITaskQueue& taskQueue,
                     TDriver& driver,
                     const TString& path,
                     std::stop_token stopToken,
                     std::atomic<bool>& stopWarmup,
                     std::shared_ptr<TLog>& log)
    : TaskQueue(taskQueue)
    , Driver(driver)
    , Context(terminalID, warehouseID, warehouseCount, TaskQueue, std::make_shared<NQuery::TQueryClient>(Driver), path, log)
    , StopToken(stopToken)
    , StopWarmup(stopWarmup)
    , Task(Run())
{
    TaskQueue.TaskReady(Task.Handle, Context.TerminalID);
}

TTerminalTask TTerminal::Run() {
    auto& Log = Context.Log; // to make LOG_* macros working

    LOG_D("Terminal " << Context.TerminalID << " has started");

    Y_UNUSED(StopWarmup);

    while (!StopToken.stop_requested()) {
        try {
            // TODO: disable autocommit

            LOG_T("Terminal " << Context.TerminalID << " starting transaction");

            size_t execCount = 0;

            auto future = Context.Client->RetryQuery([this, &execCount](TSession session) mutable {
                auto& Log = Context.Log;
                LOG_T("Terminal " << Context.TerminalID << " started RetryQuery");
                ++execCount;
                return GetNewOrderTask(Context, session);
            });

            auto result = co_await TSuspendWithFuture(future, Context.TaskQueue, Context.TerminalID);
            (void) result;
            LOG_T("Terminal " << Context.TerminalID << " transaction finished in " << execCount << " execution(s)");
        } catch (const TUserAbortedException& ex) {
            // it's OK, inc statistics and ignore
            LOG_T("Terminal " << Context.TerminalID << " transaction aborted by user");
        } catch (const yexception& ex) {
            TStringStream ss;
            ss << "Terminal " << Context.TerminalID << " got exception while transaction execution: "
                << ex.what();
            const auto* backtrace = ex.BackTrace();
            if (backtrace) {
                ss << ", stacktrace: " << ex.BackTrace()->PrintToString();
            }
            LOG_E(ss.Str());
            std::quick_exit(1);
        }

        LOG_T("Terminal " << " is going to sleep");
        co_await TSuspend(TaskQueue, Context.TerminalID, std::chrono::milliseconds(50));
    }

    LOG_D("Terminal " << Context.TerminalID << " stopped");

    co_return;
}

bool TTerminal::IsDone() const {
    if (!Task.Handle) {
        return true;
    }

    return Task.Handle.done();
}

} // namespace NYdb::NTPCC
