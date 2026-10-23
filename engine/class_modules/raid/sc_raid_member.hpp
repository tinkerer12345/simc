// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_RAID_MEMBER_HPP
#define SC_RAID_MEMBER_HPP

#include "simulationcraft.hpp"

namespace raid
{
// Creates count stand-in raid members of a kind ("tank", "healer", "ranged" or "melee"), named
// Tank_1, Healer_1, Ranged_1, Melee_1 and so on.
void create_raid_members( sim_t* sim, util::string_view kind, int count );
}  // namespace raid

#endif
