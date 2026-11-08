// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_PATCHWERK_HPP
#define SC_PATCHWERK_HPP

#include "simulationcraft.hpp"

namespace bosses
{
player_t* create_patchwerk( sim_t* sim );
// Patchwerk with phases shaped like Mythic Coiled Altar (encounter=CoiledAltar).
player_t* create_coiled_altar( sim_t* sim );
}

#endif
