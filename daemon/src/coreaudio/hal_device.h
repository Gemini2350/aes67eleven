#pragma once
#include <CoreAudio/CoreAudio.h>
#include <functional>
#include <string>

namespace aes67vsc
{
    /** HAL client for one virtual device: resolve by uid, set the io buffer
    *   size, run an io proc. input/output buffer lists may be null depending
    *   on which streams the device has active for us.
    **/
    class HalDevice
    {
        public:
            using IOFunc = std::function<void(const AudioBufferList* pInput, AudioBufferList* pOutput)>;

            ~HalDevice();

            bool Open(const std::string& sUid, uint32_t nBufferFrames = 128);
            /** resolve an output device: "default"/"" = system default output,
            *   else device uid, else human-readable device name **/
            bool OpenOutput(const std::string& sSpec, uint32_t nBufferFrames = 256);
            void Close();
            bool IsOpen() const { return m_nDevice != kAudioObjectUnknown; }
            AudioObjectID Id() const { return m_nDevice; }

            /** bOutputOnly: enable only the output streams for our io proc -
            *   no input capture means no "microphone in use" indicator **/
            bool Start(IOFunc pIO, bool bOutputOnly = false);
            void Stop();

            /** custom property carrying raw bytes (CFData) **/
            std::vector<uint8_t> GetDataProperty(uint32_t nSelector);

            /** custom driver properties (CFString payloads) **/
            bool SetStringProperty(uint32_t nSelector, const std::string& sValue);
            std::string GetStringProperty(uint32_t nSelector);

        private:
            static OSStatus IOProc(AudioObjectID nDevice, const AudioTimeStamp* pNow,
                                   const AudioBufferList* pInputData, const AudioTimeStamp* pInputTime,
                                   AudioBufferList* pOutputData, const AudioTimeStamp* pOutputTime,
                                   void* pClientData);

            AudioObjectID m_nDevice = kAudioObjectUnknown;
            AudioDeviceIOProcID m_procId = nullptr;
            IOFunc m_pIO;
    };

    /** fires when coreaudiod restarts underneath us - every AudioObjectID is
    *   invalid afterwards, reopen everything **/
    void OnAudioServiceRestarted(std::function<void()> pCallback);
}
