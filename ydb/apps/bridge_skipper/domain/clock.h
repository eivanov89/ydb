#pragma once

#include <library/cpp/time_provider/time_provider.h>

namespace NBridgeSkipper {

// Skipper owns its clock registration without changing the shared YDB API.
// Readers retain the selected provider throughout Now(), including replacement
// from another thread. Providers must support concurrent Now() calls; mutable
// test providers synchronize changes when used with background producers.
class TSkipperClock {
public:
    static void RegisterProvider(TIntrusivePtr<ITimeProvider> provider);
    static TInstant Now();
};

} // namespace NBridgeSkipper
