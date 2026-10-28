#include "hal_device.h"
#include <vector>
#include "log.h"

using namespace aes67vsc;

namespace
{
    AudioObjectPropertyAddress GlobalAddress(AudioObjectPropertySelector nSelector)
    {
        return {nSelector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    }
}

HalDevice::~HalDevice()
{
    Close();
}

bool HalDevice::Open(const std::string& sUid, uint32_t nBufferFrames)
{
    Close();

    CFStringRef cfUid = CFStringCreateWithCString(kCFAllocatorDefault, sUid.c_str(), kCFStringEncodingUTF8);
    AudioObjectID nDevice = kAudioObjectUnknown;
    UInt32 nSize = sizeof(nDevice);
    auto address = GlobalAddress(kAudioHardwarePropertyTranslateUIDToDevice);
    OSStatus nStatus = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                  sizeof(cfUid), &cfUid, &nSize, &nDevice);
    CFRelease(cfUid);
    if(nStatus != kAudioHardwareNoError || nDevice == kAudioObjectUnknown)
    {
        return false;
    }
    m_nDevice = nDevice;

    UInt32 nFrames = nBufferFrames;
    address = GlobalAddress(kAudioDevicePropertyBufferFrameSize);
    nStatus = AudioObjectSetPropertyData(m_nDevice, &address, 0, nullptr, sizeof(nFrames), &nFrames);
    if(nStatus != kAudioHardwareNoError)
    {
        LOG_WARN("hal") << sUid << ": buffer size " << nBufferFrames << " rejected (status " << nStatus << ")";
    }
    return true;
}

bool HalDevice::OpenOutput(const std::string& sSpec, uint32_t nBufferFrames)
{
    if(!sSpec.empty() && sSpec != "default" && Open(sSpec, nBufferFrames)) { return true; }
    Close();

    AudioObjectID nDevice = kAudioObjectUnknown;
    if(sSpec.empty() || sSpec == "default")
    {
        UInt32 nSize = sizeof(nDevice);
        auto address = GlobalAddress(kAudioHardwarePropertyDefaultOutputDevice);
        if(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                      &nSize, &nDevice) != kAudioHardwareNoError)
        {
            return false;
        }
    }
    else
    {
        //match by human readable name
        auto address = GlobalAddress(kAudioHardwarePropertyDevices);
        UInt32 nSize = 0;
        if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &nSize) != kAudioHardwareNoError)
        {
            return false;
        }
        std::vector<AudioObjectID> vDevices(nSize / sizeof(AudioObjectID));
        if(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                      &nSize, vDevices.data()) != kAudioHardwareNoError)
        {
            return false;
        }
        for(AudioObjectID nCandidate : vDevices)
        {
            CFStringRef cfName = nullptr;
            UInt32 nNameSize = sizeof(cfName);
            auto nameAddress = GlobalAddress(kAudioObjectPropertyName);
            if(AudioObjectGetPropertyData(nCandidate, &nameAddress, 0, nullptr,
                                          &nNameSize, &cfName) != kAudioHardwareNoError || !cfName)
            {
                continue;
            }
            char buffer[256] = {0};
            CFStringGetCString(cfName, buffer, sizeof(buffer), kCFStringEncodingUTF8);
            CFRelease(cfName);
            if(sSpec == buffer) { nDevice = nCandidate; break; }
        }
    }
    if(nDevice == kAudioObjectUnknown) { return false; }

    m_nDevice = nDevice;
    UInt32 nFrames = nBufferFrames;
    auto address = GlobalAddress(kAudioDevicePropertyBufferFrameSize);
    AudioObjectSetPropertyData(m_nDevice, &address, 0, nullptr, sizeof(nFrames), &nFrames);
    return true;
}

void HalDevice::Close()
{
    Stop();
    m_nDevice = kAudioObjectUnknown;
}

bool HalDevice::Start(IOFunc pIO, bool bOutputOnly)
{
    if(!IsOpen() || m_procId) { return false; }
    m_pIO = std::move(pIO);
    if(AudioDeviceCreateIOProcID(m_nDevice, &HalDevice::IOProc, this, &m_procId) != kAudioHardwareNoError)
    {
        m_procId = nullptr;
        return false;
    }
    //explicitly enable every stream of both directions for this io proc -
    //relying on the default left the input side unserved on newer macos
    for(AudioObjectPropertyScope nScope : {kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput})
    {
        AudioObjectPropertyAddress streamsAddress{kAudioDevicePropertyStreams, nScope, kAudioObjectPropertyElementMain};
        UInt32 nSize = 0;
        if(AudioObjectGetPropertyDataSize(m_nDevice, &streamsAddress, 0, nullptr, &nSize) != kAudioHardwareNoError)
        {
            continue;
        }
        UInt32 nStreams = nSize / sizeof(AudioObjectID);
        if(nStreams == 0) { continue; }

        std::vector<uint8_t> vUsage(sizeof(void*) + sizeof(UInt32) + nStreams*sizeof(UInt32));
        auto* pUsage = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(vUsage.data());
        pUsage->mIOProc = reinterpret_cast<void*>(m_procId);
        pUsage->mNumberStreams = nStreams;
        bool bOn = !(bOutputOnly && nScope == kAudioObjectPropertyScopeInput);
        for(UInt32 n = 0; n < nStreams; n++) { pUsage->mStreamIsOn[n] = bOn ? 1 : 0; }
        AudioObjectPropertyAddress usageAddress{kAudioDevicePropertyIOProcStreamUsage, nScope, kAudioObjectPropertyElementMain};
        OSStatus nStatus = AudioObjectSetPropertyData(m_nDevice, &usageAddress, 0, nullptr,
                                                      UInt32(vUsage.size()), pUsage);
        if(nStatus != kAudioHardwareNoError)
        {
            LOG_WARN("hal") << "stream usage (" << (nScope == kAudioObjectPropertyScopeInput ? "in" : "out")
                            << ") -> " << nStatus;
        }
    }

    if(AudioDeviceStart(m_nDevice, m_procId) != kAudioHardwareNoError)
    {
        AudioDeviceDestroyIOProcID(m_nDevice, m_procId);
        m_procId = nullptr;
        return false;
    }
    return true;
}

void HalDevice::Stop()
{
    if(m_procId && m_nDevice != kAudioObjectUnknown)
    {
        AudioDeviceStop(m_nDevice, m_procId);
        AudioDeviceDestroyIOProcID(m_nDevice, m_procId);
    }
    m_procId = nullptr;
}

OSStatus HalDevice::IOProc(AudioObjectID, const AudioTimeStamp*,
                           const AudioBufferList* pInputData, const AudioTimeStamp*,
                           AudioBufferList* pOutputData, const AudioTimeStamp*, void* pClientData)
{
    auto* pThis = static_cast<HalDevice*>(pClientData);
    if(pThis->m_pIO)
    {
        pThis->m_pIO(pInputData, pOutputData);
    }
    return kAudioHardwareNoError;
}

bool HalDevice::SetStringProperty(uint32_t nSelector, const std::string& sValue)
{
    if(!IsOpen()) { return false; }
    CFStringRef cfValue = CFStringCreateWithCString(kCFAllocatorDefault, sValue.c_str(), kCFStringEncodingUTF8);
    auto address = GlobalAddress(nSelector);
    OSStatus nStatus = AudioObjectSetPropertyData(m_nDevice, &address, 0, nullptr, sizeof(cfValue), &cfValue);
    CFRelease(cfValue);
    return nStatus == kAudioHardwareNoError;
}

std::vector<uint8_t> HalDevice::GetDataProperty(uint32_t nSelector)
{
    std::vector<uint8_t> vResult;
    if(!IsOpen()) { return vResult; }
    CFPropertyListRef cfValue = nullptr;
    UInt32 nSize = sizeof(cfValue);
    auto address = GlobalAddress(nSelector);
    if(AudioObjectGetPropertyData(m_nDevice, &address, 0, nullptr, &nSize, &cfValue) != kAudioHardwareNoError
       || !cfValue)
    {
        return vResult;
    }
    if(CFGetTypeID(cfValue) == CFDataGetTypeID())
    {
        auto cfData = static_cast<CFDataRef>(cfValue);
        vResult.assign(CFDataGetBytePtr(cfData), CFDataGetBytePtr(cfData) + CFDataGetLength(cfData));
    }
    CFRelease(cfValue);
    return vResult;
}

std::string HalDevice::GetStringProperty(uint32_t nSelector)
{
    if(!IsOpen()) { return ""; }
    CFStringRef cfValue = nullptr;
    UInt32 nSize = sizeof(cfValue);
    auto address = GlobalAddress(nSelector);
    if(AudioObjectGetPropertyData(m_nDevice, &address, 0, nullptr, &nSize, &cfValue) != kAudioHardwareNoError
       || !cfValue)
    {
        return "";
    }
    char buffer[256] = {0};
    CFStringGetCString(cfValue, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(cfValue);
    return buffer;
}

namespace
{
    std::function<void()>& RestartCallback()
    {
        static std::function<void()> s_pCallback;
        return s_pCallback;
    }

    OSStatus RestartListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*)
    {
        if(RestartCallback()) { RestartCallback()(); }
        return kAudioHardwareNoError;
    }
}

void aes67vsc::OnAudioServiceRestarted(std::function<void()> pCallback)
{
    RestartCallback() = std::move(pCallback);
    static AudioObjectPropertyAddress s_address = GlobalAddress(kAudioHardwarePropertyServiceRestarted);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &s_address, RestartListener, nullptr);
}
