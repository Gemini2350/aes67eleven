#include "bmca.h"
#include <cstdio>

using namespace pan::ptp;

std::string pan::ptp::IdentityToString(const ClockIdentity& id)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    return buffer;
}

Comparison pan::ptp::CompareDatasets(const AnnounceDataset& a, const AnnounceDataset& b)
{
    //IEEE 1588-2008 figure 27/28 - compare grandmaster attributes in order.
    //if both datasets describe the SAME grandmaster the tiebreak is topological
    //(stepsRemoved, then sender identity)
    if(a.grandmasterIdentity != b.grandmasterIdentity)
    {
        if(a.nPriority1 != b.nPriority1)
        {
            return {a.nPriority1 < b.nPriority1 ? -1 : 1, "priority1"};
        }
        if(a.nClockClass != b.nClockClass)
        {
            return {a.nClockClass < b.nClockClass ? -1 : 1, "clockClass"};
        }
        if(a.nClockAccuracy != b.nClockAccuracy)
        {
            return {a.nClockAccuracy < b.nClockAccuracy ? -1 : 1, "clockAccuracy"};
        }
        if(a.nOffsetScaledLogVariance != b.nOffsetScaledLogVariance)
        {
            return {a.nOffsetScaledLogVariance < b.nOffsetScaledLogVariance ? -1 : 1, "offsetScaledLogVariance"};
        }
        if(a.nPriority2 != b.nPriority2)
        {
            return {a.nPriority2 < b.nPriority2 ? -1 : 1, "priority2"};
        }
        return {a.grandmasterIdentity < b.grandmasterIdentity ? -1 : 1, "grandmasterIdentity"};
    }

    if(a.nStepsRemoved != b.nStepsRemoved)
    {
        return {a.nStepsRemoved < b.nStepsRemoved ? -1 : 1, "stepsRemoved"};
    }
    if(a.source.clockIdentity != b.source.clockIdentity)
    {
        return {a.source.clockIdentity < b.source.clockIdentity ? -1 : 1, "senderIdentity"};
    }
    if(a.source.nPort != b.source.nPort)
    {
        return {a.source.nPort < b.source.nPort ? -1 : 1, "senderPort"};
    }
    return {0, "identical"};
}
