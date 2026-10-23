// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_BOSSES_HPP
#define SC_BOSSES_HPP

#include "simulationcraft.hpp"

namespace bosses
{
player_t* create_encounter( sim_t* sim, util::string_view name );
}

#endif
