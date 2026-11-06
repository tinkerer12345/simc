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

// A stand-in's max health (a tank's if tank): raid_member_health / raid_tank_health, or with raid_health_from_healer=1
// the healer's own max health (x raid_tank_health_factor for tanks). Encounters size their damage from it.
double member_health( const sim_t* sim, bool tank );
}  // namespace raid

#endif
