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
// Patchwerk's engine with a phase table (phases=..., see sc_patchwerk.cpp): bosses shaped like a real fight reuse it
// (e.g. sc_coiled_altar.cpp). The table is the boss's default; a phases= option after encounter= overrides it.
player_t* create_phased_patchwerk( sim_t* sim, util::string_view name, util::string_view phases );
}

#endif
