#pragma once
#include <cstdint>

namespace pan::compat
{
    /** Monotonic host clock in nanoseconds (CLOCK_UPTIME_RAW == mach_absolute_time).
    *   This is the local timebase every mapping (PTP, driver clock) anchors to -
    *   unlike CLOCK_REALTIME it is never stepped by NTP.
    **/
    uint64_t HostNs();

    /** Wall clock (CLOCK_REALTIME) - only used to convert kernel SO_TIMESTAMP
    *   receive stamps onto the host clock. **/
    uint64_t RealtimeNs();

    /** Absolute sleep against the host clock (mach_wait_until). **/
    void SleepUntilHostNs(uint64_t nHostNs);

    /** Move the calling thread to the mach time-constraint (realtime) band.
    *   nPeriodNs is the expected wakeup period (e.g. the rtp packet time). **/
    void SetRealtimeThread(uint64_t nPeriodNs);
}
