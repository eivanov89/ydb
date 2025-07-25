#include "uptime.h"

#if defined(_win_)
    #include <util/system/winint.h>
#elif defined(_linux_)
    #include <util/stream/file.h>
    #include <util/string/cast.h>
#elif defined(_darwin_) || defined(_freebsd_)
    #include <sys/sysctl.h>
#endif

#if defined(_darwin_) || defined(_freebsd_)
namespace {
    TInstant GetBootTime() {
        struct timeval timeSinceBoot;
        size_t len = sizeof(timeSinceBoot);
        int request[2] = {CTL_KERN, KERN_BOOTTIME};
        if (sysctl(request, 2, &timeSinceBoot, &len, nullptr, 0) < 0) {
            ythrow yexception() << "cannot get kern.boottime from sysctl";
        }
        return TInstant::MicroSeconds(timeSinceBoot.tv_sec * 1'000'000 + timeSinceBoot.tv_usec);
    }

    TDuration GetBSDUptime() {
        TInstant beforeNow;
        TInstant afterNow;
        TInstant now;

        // avoid race when NTP changes machine time between getting Now() and uptime
        afterNow = GetBootTime();
        do {
            beforeNow = afterNow;
            now = Now();
            afterNow = GetBootTime();
        } while (afterNow != beforeNow);

        return now - beforeNow;
    }
} // namespace
#endif // _darwin_

TDuration Uptime() {
#if defined(_win_)
    return TDuration::MilliSeconds(GetTickCount64());
#elif defined(_linux_)
    struct timespec ts;
    int ret = clock_gettime(CLOCK_BOOTTIME, &ts);
    Y_ENSURE_EX(ret != -1, TSystemError() << "Failed to read the CLOCK_BOOTTIME timer");
    return TDuration::Seconds(ts.tv_sec) + TDuration::MicroSeconds(ts.tv_nsec / 1000);
#elif defined(_darwin_) || defined(_freebsd_)
    return GetBSDUptime();
#elif defined(_emscripten_)
    ythrow yexception() << "unimplemented";
#else
    #error "Implement this method"
#endif
}
