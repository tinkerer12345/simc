// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "raid_frames.hpp"

#include "action/action.hpp"
#include "action/action_state.hpp"
#include "action/dot.hpp"
#include "buff/buff.hpp"
#include "player.hpp"
#include "sim/event.hpp"
#include "sim/sim.hpp"
#include "util/util.hpp"

#include <algorithm>

// Same rule as player_t::sequence_add, so the frames come from the same fight as the cast sequence.
bool raid_frames_t::recording( const player_t& owner ) const
{
  const sim_t& sim = *owner.sim;
  return ( sim.iterations <= 1 && sim.current_iteration == 0 ) || ( sim.iterations > 1 && owner.nth_iteration() == 1 );
}

void raid_frames_t::combat_begin( player_t& owner )
{
  if ( !recording( owner ) )
    return;

  take_snapshot( owner );
  schedule_snapshot( owner );
}

void raid_frames_t::schedule_snapshot( player_t& owner )
{
  make_event( *owner.sim, timespan_t::from_seconds( 1 ), [ this, &owner ] {
    take_snapshot( owner );
    schedule_snapshot( owner );
  } );
}

void raid_frames_t::take_snapshot( const player_t& owner )
{
  const sim_t& sim = *owner.sim;

  if ( units.empty() )
  {
    for ( const player_t* p : sim.player_no_pet_list )
    {
      // p->role is the profile's role=; primary_role() can differ (a Restoration Druid reports "attack").
      units.push_back( { p->name_str, util::role_type_string( p->role ), util::position_type_string( p->position() ),
                         p->resources.max[ RESOURCE_HEALTH ], p->resources.max[ RESOURCE_MANA ] } );
    }
  }

  snapshot_t snap;
  snap.time = sim.current_time().total_seconds();

  for ( player_t* p : sim.player_no_pet_list )
  {
    double absorb = 0;
    for ( const absorb_buff_t* b : p->absorb_buff_list )
      if ( b->check() )
        absorb += b->current_value;

    bool alive = !p->is_sleeping() && p->resources.current[ RESOURCE_HEALTH ] > 0;
    snap.health.push_back( std::max( 0.0, p->resources.current[ RESOURCE_HEALTH ] ) );
    snap.absorb.push_back( alive ? absorb : 0.0 );
    snap.mana.push_back( std::max( 0.0, p->resources.current[ RESOURCE_MANA ] ) );
    snap.alive.push_back( alive );

    std::vector<effect_t> effects;

    // Nothing to show on the dead.
    if ( !alive )
    {
      snap.effects.push_back( std::move( effects ) );
      continue;
    }

    // HoTs from the owner's actions.
    std::vector<const dot_t*> seen;
    for ( const action_t* a : owner.action_list )
    {
      // Only HoTs from the owner's class spells; item, enchant and rune procs (no class family) are skipped.
      if ( a->data().class_family() == 0 )
        continue;

      const dot_t* d = a->find_dot( p );
      if ( !d || !d->is_ticking() || std::find( seen.begin(), seen.end(), d ) != seen.end() )
        continue;
      seen.push_back( d );
      effects.push_back( { d->name_str, d->remains().total_seconds(), d->current_stack(), true } );
    }

    snap.effects.push_back( std::move( effects ) );
  }

  snapshots.push_back( std::move( snap ) );
}

void raid_frames_t::record_death( player_t& owner, const player_t& dead, const action_state_t* killing_blow )
{
  if ( !recording( owner ) )
    return;

  take_snapshot( owner );

  death_t d;
  d.time     = owner.sim->current_time().total_seconds();
  d.name     = dead.name_str;
  d.snapshot = snapshots.size() - 1;
  if ( killing_blow && killing_blow->action )
  {
    d.ability = killing_blow->action->name_str;
    d.source  = killing_blow->action->player->name_str;
    d.amount  = killing_blow->result_amount;
  }
  deaths.push_back( std::move( d ) );
}

// Every thread records its own first fight; keep one (this sim's, if it has one).
void raid_frames_t::merge( const raid_frames_t& other )
{
  if ( snapshots.empty() && !other.snapshots.empty() )
  {
    units     = other.units;
    snapshots = other.snapshots;
    deaths    = other.deaths;
  }
}
