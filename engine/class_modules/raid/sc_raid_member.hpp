// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_RAID_MEMBER_HPP
#define SC_RAID_MEMBER_HPP

#include "simulationcraft.hpp"

namespace raid
{
// Creates count stand-in raid members of a kind ("tank", "healer", "ranged" or "melee"), named after specs
// (Protection_Warrior, Holy_Priest, Balance_Druid, Havoc_Demon_Hunter...). Healers are names only: they don't heal.
void create_raid_members( sim_t* sim, util::string_view kind, int count );
}  // namespace raid

#endif
