#pragma once
#include <aspl/Device.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include "LoopbackHandler.h"
#include "driver_iface.h"

namespace aes67vsc
{
    /** Clock shared by the visible device and the bridge: one rate target
    *   (fed by the daemon via the custom property) AND one zero-timestamp
    *   timeline. Identical sample clocks on both devices are what makes the
    *   cross-wired timeline rings line up. The tiny mutex only guards the
    *   per-period advance - two callers per ~21ms period, negligible. **/
    struct DriverClock
    {
        std::atomic<int64_t> nTargetRatePpb{0};     //(rate-1)*1e9
        std::atomic<bool> bLocked{false};
        std::atomic<uint64_t> nUpdates{0};

        //shared zero timestamp state (guarded by mutex)
        std::mutex mutex;
        double dHostTicksPerSec = 0.0;
        uint64_t nAnchorHostTicks = 0;
        uint64_t nPeriodHostTicks = 0;
        uint64_t nPeriodCounter = 0;
        double dRate = 1.0;
    };

    /** Device whose zero timestamps tick at the ptp rate. The daemon measures
    *   d(ptp)/d(host) with its ptp slave and publishes it; the device slews its
    *   host-ticks-per-frame accordingly, so the sample clock every hal client
    *   sees IS the ptp media clock (rate-wise). Without updates the last rate
    *   holds (holdover) and the device keeps running - apps never notice a
    *   daemon restart.
    **/
    class PtpClockedDevice : public aspl::Device
    {
        public:
            PtpClockedDevice(const std::shared_ptr<const aspl::Context>& context,
                             const aspl::DeviceParameters& params,
                             std::shared_ptr<DriverClock> pClock,
                             std::shared_ptr<SharedRings> pRings,
                             UInt32 nTransportType = kAudioDeviceTransportTypeVirtual);

            //network audio devices (avb) are treated as pro audio interfaces by
            //macos - no orange "microphone in use" indicator, no voice isolation
            UInt32 GetTransportType() const override { return m_nTransportType; }

        protected:
            OSStatus GetZeroTimeStampImpl(UInt32 nClientID,
                                          Float64* pOutSampleTime,
                                          UInt64* pOutHostTime,
                                          UInt64* pOutSeed) override;

        private:
            CFStringRef GetClockSyncProperty() const;
            void SetClockSyncProperty(CFStringRef value);
            CFPropertyListRef GetCaptureTap() const;

            std::shared_ptr<DriverClock> m_pClock;
            std::shared_ptr<SharedRings> m_pRings;
            UInt32 m_nTransportType;
    };
}
