#include "log_writer.h"
#include <util/system/yassert.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace NBridgeSkipper {

struct TAsyncLogWriter::TImpl {
    struct TState {
        std::mutex Mutex;
        std::condition_variable Ready;
        std::deque<std::pair<TString, bool>> Queue;
        bool Stopping = false;
        int Descriptor = dup(STDOUT_FILENO);
        std::promise<void> Finished;
        std::atomic<bool> Failed{false};
        std::function<void()> Dropped;

        ~TState() {
            if (Descriptor >= 0) {
                close(Descriptor);
            }
        }
    };
    std::shared_ptr<TState> State = std::make_shared<TState>();
    std::future<void> Finished = State->Finished.get_future();
    std::thread Thread;
};

TAsyncLogWriter::TAsyncLogWriter(std::function<void()> dropped)
    : Impl(std::make_unique<TImpl>())
{
    Impl->State->Dropped = std::move(dropped);
    Impl->Thread = std::thread([state = Impl->State] {
        for (;;) {
            TString line;
            {
                std::unique_lock lock(state->Mutex);
                state->Ready.wait(lock, [&] {
                    return state->Stopping || !state->Queue.empty();
                });
                if (state->Queue.empty()) {
                    break;
                }
                line = std::move(state->Queue.front().first);
                state->Queue.pop_front();
            }
            line += '\n';
            if (state->Failed.load()) {
                if (state->Dropped) {
                    state->Dropped();
                }
                continue;
            }
            size_t offset = 0;
            while (offset < line.size() && state->Descriptor >= 0) {
                const auto count = write(state->Descriptor, line.data() + offset, line.size() - offset);
                if (count > 0) {
                    offset += count;
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else {
                    state->Failed.store(true);
                    if (state->Dropped) {
                        state->Dropped();
                    }
                    break;
                }
            }
        }
        state->Finished.set_value();
    });
}

TAsyncLogWriter::~TAsyncLogWriter() {
    // The coordinator must stop producers before releasing process resources.
    Y_ABORT_UNLESS(!Impl->Thread.joinable());
}

bool TAsyncLogWriter::Write(TString line, bool transition) {
    std::lock_guard guard(Impl->State->Mutex);
    auto& state = *Impl->State;
    if (state.Stopping || state.Failed.load() || state.Descriptor < 0 || line.size() > 8192) {
        return false;
    }
    if (state.Queue.size() == 512) {
        if (!transition) {
            return false;
        }
        const auto diagnostic = std::find_if(state.Queue.begin(), state.Queue.end(), [](const auto& item) {
            return !item.second;
        });
        if (diagnostic == state.Queue.end()) {
            return false;
        }
        state.Queue.erase(diagnostic);
        if (state.Dropped) {
            state.Dropped();
        }
    }
    state.Queue.emplace_back(std::move(line), transition);
    Impl->State->Ready.notify_one();
    return true;
}

void TAsyncLogWriter::Stop() {
    {
        std::lock_guard guard(Impl->State->Mutex);
        Impl->State->Stopping = true;
    }
    Impl->State->Ready.notify_one();
    if (Impl->Thread.joinable()) {
        if (Impl->Finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready) {
            Impl->Thread.join();
        } else {
            // A blocked stdout cannot block shutdown. The detached producer
            // owns its queue, descriptor and completion object, never actors.
            Impl->Thread.detach();
        }
    }
}

} // namespace NBridgeSkipper
