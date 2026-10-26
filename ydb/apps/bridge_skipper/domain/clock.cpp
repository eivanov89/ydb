#include "clock.h"

#include <atomic>
#include <memory>

namespace NBridgeSkipper {
namespace {

struct TProviderHolder {
    TIntrusivePtr<ITimeProvider> Provider;
};

std::shared_ptr<const TProviderHolder>& RegisteredProvider() {
    static std::shared_ptr<const TProviderHolder> provider = std::make_shared<const TProviderHolder>(
        TProviderHolder{CreateDefaultTimeProvider()});
    return provider;
}

} // namespace

void TSkipperClock::RegisterProvider(TIntrusivePtr<ITimeProvider> provider) {
    if (!provider) {
        provider = CreateDefaultTimeProvider();
    }
    auto holder = std::make_shared<const TProviderHolder>(TProviderHolder{std::move(provider)});
    std::atomic_store_explicit(&RegisteredProvider(), std::move(holder), std::memory_order_release);
}

TInstant TSkipperClock::Now() {
    const auto holder = std::atomic_load_explicit(&RegisteredProvider(), std::memory_order_acquire);
    return holder->Provider->Now();
}

} // namespace NBridgeSkipper
