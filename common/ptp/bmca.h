#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace pan::ptp
{
    using ClockIdentity = std::array<uint8_t, 8>;
    std::string IdentityToString(const ClockIdentity& id);

    struct PortIdentity
    {
        ClockIdentity clockIdentity{};
        uint16_t nPort = 0;
        auto operator<=>(const PortIdentity&) const = default;
    };

    /** The fields of an Announce message that BMCA compares (IEEE 1588-2008 9.3.4) **/
    struct AnnounceDataset
    {
        PortIdentity source;
        uint8_t nPriority1 = 255;
        uint8_t nClockClass = 255;
        uint8_t nClockAccuracy = 0xFE;
        uint16_t nOffsetScaledLogVariance = 0xFFFF;
        uint8_t nPriority2 = 255;
        ClockIdentity grandmasterIdentity{};
        uint16_t nStepsRemoved = 0;
        uint8_t nTimeSource = 0xA0;
        int16_t nUtcOffset = 37;
        bool bLeap61 = false;
        bool bLeap59 = false;
        bool bUtcValid = false;
        bool bTimeTraceable = false;
        bool bFreqTraceable = false;
    };

    /** Result of comparing two datasets: which wins and on which field the decision fell. **/
    struct Comparison
    {
        int nResult = 0;            // <0: a wins, >0: b wins, 0: identical
        std::string sDecidingField; // "priority1", "clockClass", ...
    };

    Comparison CompareDatasets(const AnnounceDataset& a, const AnnounceDataset& b);
}
