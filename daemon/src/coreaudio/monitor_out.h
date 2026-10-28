#pragma once
#include <atomic>
#include <string>
#include "hal_device.h"
#include "json.hpp"
#include "spsc_ring.h"

namespace aes67vsc
{
    /** Local monitoring: a stereo pick of the received device channels is
    *   rendered onto a normal output device (speakers/headphones). The monitor
    *   device runs on its own crystal, not on ptp - the consumer-side trim and
    *   silence padding absorb that drift (an occasional inaudible skip is fine
    *   for a monitor path, this is not the synchronous output).
    **/
    class MonitorOut
    {
        public:
            /** sDevice: "default" (system output), device name, or device uid **/
            bool Start(const std::string& sDevice, double dGainDb);
            void Stop();
            bool IsRunning() const { return m_bRun.load(); }

            /** which device channels feed left/right (from receiver.monitor_*) **/
            void SetChannels(int nLeft, int nRight);
            void SetGainDb(double dGainDb);

            /** tee from the rx bridge: interleaved device-channel frames **/
            void Feed(const float* pInterleaved, size_t nChannels, size_t nFrames);

            nlohmann::json GetStatusJson() const;

        private:
            HalDevice m_device;
            SpscRing m_ring{4096, 2};       //~85 ms cap, trimmed to ~21 ms
            std::atomic<bool> m_bRun{false};
            std::atomic<float> m_fGain{1.f};
            std::atomic<int> m_nLeft{0};
            std::atomic<int> m_nRight{1};
            std::atomic<uint64_t> m_nUnderruns{0};
            std::atomic<uint64_t> m_nTrimmed{0};
            std::atomic<uint64_t> m_nLastFeedNs{0};
            std::string m_sDevice;
    };
}
