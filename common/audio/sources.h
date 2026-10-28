#pragma once
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"

namespace pan::audio
{
    /** Sample source: 48 kHz interleaved float, Channels() wide. Read fills
    *   exactly nFrames (looping or padding with silence) and returns false once
    *   a non-looping file has finished. The sender maps source channels onto
    *   the stream's channels (extra stream channels are silent).
    **/
    class Source
    {
        public:
            virtual ~Source() = default;
            virtual bool Read(float* pInterleaved, size_t nFrames) = 0;
            virtual std::string Describe() const = 0;
            virtual size_t Channels() const { return 2; }
    };

    std::unique_ptr<Source> CreateTone(double dFrequencyHz, double dLevelDb);
    std::unique_ptr<Source> CreateSweep(double dLevelDb);       //20..20k log sweep, 10s loop

    /** wav via dr_wav, mp3 via minimp3; resampled to 48k on load **/
    std::unique_ptr<Source> CreateFile(const std::string& sPath, bool bLoop);

    /** decode a wav/mp3 to interleaved stereo 48k float - empty on error **/
    std::vector<float> LoadFileStereo48k(const std::string& sPath);

    /** from the sender config object **/
    std::unique_ptr<Source> CreateFromConfig(const nlohmann::json& jsSender, const std::string& sFilesDir);
}
