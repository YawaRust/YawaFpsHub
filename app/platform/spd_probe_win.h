#pragma once

#include "metrics/spd.h"

#include <string>
#include <vector>

namespace yawa {

struct Ddr4SpdProbeResult
{
    SpdIdentity passport;
    std::vector<Ddr4XmpProfile> profiles;
};

// Explicit CLI-only probe. Never returns raw SPD, manufacturing data or a claim
// that a passport profile is active. Leaves result unchanged on failure.
bool readCezanneDdr4Spd(unsigned address, Ddr4SpdProbeResult& result, std::string& error);

} // namespace yawa
