#pragma once

#include "uring_operation.h"

#include <util/generic/string.h>
#include <util/system/types.h>

namespace NKikimr::NPDisk {

struct TUringRouterConfig {
    // Target SQ ring size (number of submission slots). The kernel creates a
    // CQ of twice this size by default. Typical devices have hardware queue
    // depth around 128; using 256 entries gives additional headroom to reduce
    // the risk of SQ exhaustion and improve device utilization. Submissions
    // beyond this cap are absorbed by the submit queue.
    ui32 QueueDepth = 256;

    // How long (in microseconds) the dedicated I/O thread busy-polls the
    // submission queue and completion ring before parking when idle. Lower
    // values trade CPU for submit-wakeup latency.
    ui32 IdleSpinUs = 200;

    TString ToString() const;
};

// Submit-only view of a TUringRouter. DDisk and PersistentBuffer hold this so
// they can enqueue I/O without access to lifecycle or registration methods.
//
// Read() and Write() are thread-safe. Publishing transfers the operation's
// lifetime to the I/O thread, which may invoke its terminal callback before
// the call returns. A caller transferring a smart pointer must therefore
// release it before the call and restore it only if false is returned. Every
// accepted operation receives exactly one OnComplete() or OnDrop() callback.
// False means the router has not been started or is stopping/stopped and no
// callback will be delivered.
class IUringRouterClient {
public:
    virtual ~IUringRouterClient() = default;

    virtual bool Read(TUringOperationBase* op) = 0;
    virtual bool Write(TUringOperationBase* op) = 0;
    virtual const TUringRouterConfig& GetConfig() const = 0;
};

} // namespace NKikimr::NPDisk
