#include <CoreAudio/AudioServerPlugIn.h>
#include <aspl/Driver.hpp>
#include "LoopbackHandler.h"
#include "PtpClockedDevice.h"
#include "driver_iface.h"

using namespace aes67vsc;

namespace
{
    std::shared_ptr<aspl::Device> MakeDevice(const std::shared_ptr<aspl::Context>& context,
                                             const std::shared_ptr<DriverClock>& pClock,
                                             const std::shared_ptr<SharedRings>& pRings,
                                             const std::string& sName, const std::string& sUid,
                                             bool bIsBridge)
    {
        aspl::DeviceParameters params;
        params.Name = sName;
        params.Manufacturer = "aes67-vsc";
        params.DeviceUID = sUid;
        params.ModelUID = "aes67-vsc";
        params.SampleRate = kSampleRate;
        params.ChannelCount = kChannels;
        params.EnableMixing = true;
        params.ClockDomain = kClockDomain;
        params.ClockIsStable = true;
        params.ZeroTimeStampPeriod = 1024;      //identical on both - shared timeline
        params.CanBeDefault = !bIsBridge;
        params.CanBeDefaultForSystemSounds = false;     //no system beeps onto the air

        auto pDevice = std::make_shared<PtpClockedDevice>(context, params, pClock, pRings,
                                                          kAudioDeviceTransportTypeAVB);

        aspl::StreamParameters streamParams;
        streamParams.Format = {
            .mSampleRate = double(kSampleRate),
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                            kAudioFormatFlagIsPacked,
            .mBytesPerPacket = kChannels * sizeof(float),
            .mFramesPerPacket = 1,
            .mBytesPerFrame = kChannels * sizeof(float),
            .mChannelsPerFrame = kChannels,
            .mBitsPerChannel = 32,
        };
        streamParams.Direction = aspl::Direction::Output;
        pDevice->AddStreamWithControlsAsync(streamParams);
        streamParams.Direction = aspl::Direction::Input;
        pDevice->AddStreamWithControlsAsync(streamParams);

        pDevice->SetIOHandler(std::make_shared<CrossWireHandler>(pRings, bIsBridge));
        if(bIsBridge) { pDevice->SetIsHidden(true); }   //daemon only, invisible to apps
        return pDevice;
    }

    std::shared_ptr<aspl::Driver> CreateDriver()
    {
        auto context = std::make_shared<aspl::Context>();
        auto pClock = std::make_shared<DriverClock>();
        auto pRings = std::make_shared<SharedRings>();

        auto plugin = std::make_shared<aspl::Plugin>(context);
        plugin->AddDevice(MakeDevice(context, pClock, pRings, "AES67eleven", kDeviceUidMain, false));
        plugin->AddDevice(MakeDevice(context, pClock, pRings, "AES67 Bridge", kDeviceUidBridge, true));

        return std::make_shared<aspl::Driver>(context, plugin);
    }
}

extern "C" void* EntryPoint(CFAllocatorRef, CFUUIDRef typeUUID)
{
    if(!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) { return nullptr; }
    static std::shared_ptr<aspl::Driver> pDriver = CreateDriver();
    return pDriver->GetReference();
}
