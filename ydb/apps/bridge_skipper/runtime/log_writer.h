#pragma once

#include <util/generic/string.h>
#include <memory>
#include <functional>

namespace NBridgeSkipper {

// Bounded asynchronous stdout writer. Producers only enqueue owned JSON lines;
// the coordinator stops it. A stalled output retains only its own shared state.
class TAsyncLogWriter {
public:
    explicit TAsyncLogWriter(std::function<void()> dropped = {});
    ~TAsyncLogWriter();
    bool Write(TString line, bool transition);
    void Stop();

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};

} // namespace NBridgeSkipper
