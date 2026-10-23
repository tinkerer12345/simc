// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_bosses.hpp"
#include "class_modules/bosses/sc_vashnik.hpp"

namespace bosses
{
player_t* create_encounter( sim_t* sim, util::string_view name )
{
  if ( util::str_compare_ci( name, "Vashnik" ) )
    return create_vashnik( sim );

  throw std::invalid_argument( fmt::format( "Unknown encounter '{}'.", name ) );
}
}  // namespace bosses
