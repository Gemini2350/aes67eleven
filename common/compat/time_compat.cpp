#include "time_compat.h"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#else
#include <sched.h>
#endif

using namespace pan::compat;

#ifdef __APPLE__

namespace
{
    mach_timebase_info_data_t Timebase()
    {
        static mach_timebase_info_data_t info = []
        {
            mach_timebase_info_data_t i;
            mach_timebase_info(&i);
            return i;
        }();
        return info;
    }

    uint64_t NsToMachTicks(uint64_t nNs)
    {
        auto tb = Timebase();
        return nNs * tb.denom / tb.numer;
    }
}

uint64_t pan::compat::HostNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t pan::compat::RealtimeNs()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
}

void pan::compat::SleepUntilHostNs(uint64_t nHostNs)
{
    mach_wait_until(NsToMachTicks(nHostNs));
}

void pan::compat::SetRealtimeThread(uint64_t nPeriodNs)
{
    //computation/constraint sized for "wake, build a packet, send" workloads
    thread_time_constraint_policy_data_t policy;
    policy.period = static_cast<uint32_t>(NsToMachTicks(nPeriodNs));
    policy.computation = static_cast<uint32_t>(NsToMachTicks(nPeriodNs / 5 + 50'000));
    policy.constraint = static_cast<uint32_t>(NsToMachTicks(nPeriodNs / 2 + 100'000));
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

#else   //linux

//CLOCK_MONOTONIC (not _RAW): clock_nanosleep can only sleep against this one,
//and ntp slews it without stepping - good enough as the local anchor clock
uint64_t pan::compat::HostNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
}

uint64_t pan::compat::RealtimeNs()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
}

void pan::compat::SleepUntilHostNs(uint64_t nHostNs)
{
    timespec ts{time_t(nHostNs / 1000000000ULL), long(nHostNs % 1000000000ULL)};
    while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

void pan::compat::SetRealtimeThread(uint64_t)
{
    //SCHED_FIFO needs CAP_SYS_NICE (docker: --cap-add SYS_NICE + rtprio ulimit);
    //warn once when refused - timing then rides on CFS and tx jitter grows
    sched_param param{};
    param.sched_priority = 70;
    if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
    {
        static bool bWarned = false;
        if(!bWarned)
        {
            bWarned = true;
            fprintf(stderr, "WRN [rt] SCHED_FIFO refused - run with --cap-add SYS_NICE and an rtprio ulimit for stable packet pacing\n");
        }
    }
}

#endif
