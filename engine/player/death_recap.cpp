// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "death_recap.hpp"

#include "action/action.hpp"
#include "action/action_state.hpp"
#include "player.hpp"
#include "sim/sim.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <cmath>

void death_recap_t::reset()
{
  events.clear();
  waits.clear();
  baseline_health = -1;
  baseline_mana   = -1;
}

void death_recap_t::merge( const death_recap_t& other )
{
  for ( const auto& d : other.deaths )
  {
    if ( deaths.size() >= max_deaths )
      break;
    deaths.push_back( d );
  }
}

void death_recap_t::prune( timespan_t now )
{
  timespan_t start = now - window;

  while ( !events.empty() && events.front().time < start )
  {
    baseline_health = events.front().health;
    baseline_mana   = events.front().mana;
    events.pop_front();
  }

  while ( !waits.empty() && waits.front().second < start )
    waits.pop_front();
}

void death_recap_t::record( const player_t& p, event_t e )
{
  e.time   = p.sim->current_time();
  e.health = p.resources.current[ RESOURCE_HEALTH ];
  e.mana   = p.resources.current[ RESOURCE_MANA ];
  prune( e.time );
  events.push_back( e );
}

void death_recap_t::record_damage( const player_t& p, const action_state_t& s )
{
  double absorbed = std::max( 0.0, s.result_mitigated - s.result_amount );
  record( p, { timespan_t::zero(), s.action, false, s.result_amount, absorbed, 0.0, 0.0, 0.0 } );
}

void death_recap_t::record_heal( const player_t& p, const action_state_t& s )
{
  double overheal = std::max( 0.0, s.result_total - s.result_amount );
  record( p, { timespan_t::zero(), s.action, true, s.result_amount, 0.0, overheal, 0.0, 0.0 } );
}

void death_recap_t::record_wait( timespan_t start, timespan_t end )
{
  if ( end <= start )
    return;

  // Not pruned here: a wait can end in the future, and pruning happens on the next recorded event.
  waits.emplace_back( start, end );
}

void death_recap_t::record_death( const player_t& p, const action_state_t* killing_blow, timespan_t waiting_since )
{
  timespan_t now = p.sim->current_time();
  prune( now );

  if ( deaths.size() >= max_deaths )
    return;

  death_t d;
  d.time       = now.total_seconds();
  d.max_health = p.resources.max[ RESOURCE_HEALTH ];
  d.max_mana   = p.resources.max[ RESOURCE_MANA ];

  // Second k covers [now - (n - k), now - (n - k - 1)).
  auto n          = static_cast<size_t>( std::ceil( window.total_seconds() ) );
  double origin   = d.time - static_cast<double>( n );
  auto bucket_of  = [ origin, n ]( double t ) {
    auto i = static_cast<long long>( std::floor( t - origin ) );
    return static_cast<size_t>( std::clamp<long long>( i, 0, static_cast<long long>( n ) - 1 ) );
  };

  d.seconds.resize( n );
  std::vector<bool> has_resources( n, false );

  for ( const auto& e : events )
  {
    auto& sec = d.seconds[ bucket_of( e.time.total_seconds() ) ];
    if ( e.heal )
    {
      sec.healing += e.amount;
      sec.overheal += e.overheal;
    }
    else
    {
      sec.damage += e.amount;
      sec.absorbed += e.absorbed;

      if ( e.action )
      {
        std::string source = e.action->player->name_str;
        auto it = std::find_if( d.abilities.begin(), d.abilities.end(), [ & ]( const ability_t& a ) {
          return a.source == source && a.ability == e.action->name_str;
        } );
        if ( it == d.abilities.end() )
        {
          d.abilities.push_back( { source, e.action->name_str, util::school_type_string( e.action->get_school() ) } );
          it = d.abilities.end() - 1;
        }
        it->amount += e.amount;
        it->absorbed += e.absorbed;
        it->hits++;
      }
    }

    size_t i              = bucket_of( e.time.total_seconds() );
    d.seconds[ i ].health = e.health;
    d.seconds[ i ].mana   = e.mana;
    has_resources[ i ]    = true;
  }

  // Seconds without events keep the previous values; before the first event the player is assumed full
  // unless older events were pruned.
  double health = baseline_health >= 0 ? baseline_health : d.max_health;
  double mana   = baseline_mana >= 0 ? baseline_mana : d.max_mana;
  for ( size_t i = 0; i < n; ++i )
  {
    if ( has_resources[ i ] )
    {
      health = d.seconds[ i ].health;
      mana   = d.seconds[ i ].mana;
    }
    else
    {
      d.seconds[ i ].health = health;
      d.seconds[ i ].mana   = mana;
    }
  }

  auto add_idle = [ & ]( double a, double b ) {
    a = std::max( a, origin );
    b = std::min( b, d.time );
    for ( size_t i = 0; i < n && a < b; ++i )
    {
      double lo = origin + static_cast<double>( i ), hi = lo + 1.0;
      double overlap = std::min( b, hi ) - std::max( a, lo );
      if ( overlap > 0 )
        d.seconds[ i ].idle += overlap;
    }
  };
  for ( const auto& w : waits )
    add_idle( w.first.total_seconds(), w.second.total_seconds() );
  if ( waiting_since > timespan_t::min() )
    add_idle( waiting_since.total_seconds(), d.time );

  std::sort( d.abilities.begin(), d.abilities.end(),
             []( const ability_t& a, const ability_t& b ) { return a.amount > b.amount; } );

  if ( killing_blow && killing_blow->action )
  {
    d.killing_blow.source  = killing_blow->action->player->name_str;
    d.killing_blow.ability = killing_blow->action->name_str;
    d.killing_blow.school  = util::school_type_string( killing_blow->action->get_school() );
    d.killing_blow.amount  = killing_blow->result_amount;
    d.killing_blow.hits    = 1;
  }

  deaths.push_back( std::move( d ) );
}
