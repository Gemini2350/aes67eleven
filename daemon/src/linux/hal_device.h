#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aes67vsc
{
    /** gateway build: there is no coreaudio - the virtual device pair does not
    *   exist and the engine loop paces itself (see kGatewayEngine in main.cpp).
    *   these stubs keep main.cpp identical between the mac daemon and the
    *   linux gateway: Open() reports success, the capture tap stays empty.
    **/
    struct AudioBuffer
    {
        uint32_t mNumberChannels = 0;
        uint32_t mDataByteSize = 0;
        void* mData = nullptr;
    };
    struct AudioBufferList
    {
        uint32_t mNumberBuffers = 0;
        AudioBuffer mBuffers[1];
    };

    class HalDevice
    {
        public:
            using IOFunc = std::function<void(const AudioBufferList* pInput, AudioBufferList* pOutput)>;

            bool Open(const std::string&, uint32_t = 128) { return true; }
            bool OpenOutput(const std::string&, uint32_t = 256) { return false; }
            void Close() {}
            bool IsOpen() const { return true; }
            bool Start(IOFunc, bool = false) { return true; }
            void Stop() {}
            std::vector<uint8_t> GetDataProperty(uint32_t) { return {}; }
            bool SetStringProperty(uint32_t, const std::string&) { return true; }
            std::string GetStringProperty(uint32_t) { return {}; }
    };

    inline void OnAudioServiceRestarted(std::function<void()>) {}
}
