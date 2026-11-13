// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_coiled_altar.hpp"
#include "class_modules/bosses/sc_patchwerk.hpp"

namespace bosses
{
namespace
{
/* Coiled Altar (Mythic), simulated: Patchwerk's engine (shares of the raid's health, one healer's share = 100%) on the
 * user's own kill (2026-11-11 21:23, 8:15, raw combat log): damage taken by ability per phase (amount + absorbed,
 * players only), healing absorbs, boss casts and Venom Rupture stacks. Rates per player per second = taken / 0.9 (the
 * stand-ins' 10% reduction) / (20 x 982k x seconds); tank parts scaled from a farm kill's (x0.55 for 0.238M/s).
 * - P1 (0:00-2:12) Zul'jan: steady = Coalesced Venom (67.8M); spot = Axegrinder, Mutagenic Venom, Noxious Ground,
 *   Volatile Venom, Widow's Touch, Guillotine (45.3M: the axes and pools are avoidable in game, but ticking damage on
 *   a few players, so spot; the engine's avoidable hit would be a one-shot at this size); tank = melee, Twinfang Toxin,
 *   Sever (21.5M). On the timeline: Fangs of the Coiled Altar at 0:01 and 1:25 (2.45 stacks each) and each Sever's
 *   Venom Rupture: one 10 sec wave, ticks every 2 sec at 55.7k unmitigated x stacks, its opening hit = +0.2 stack;
 *   one stack = 0.209 H per player (fits P1's 100.6M and P3's 57.3M): 0:22, 0:39, 0:59, 1:16 (4 stacks each),
 *   1:47 (7), 2:04 (3).
 * - P2 (2:12-5:06) Hex Lord Malacrass: steady = Dreadful Presence (234.9M, flat); spot = Retaliatory Malice,
 *   Gravebound, Axegrinder, Malevolent Resonance, Gloombomb (48.1M); tank = Soul Sever, Dread Bolt (23.2M). Suffocating
 *   Darkness, a healing absorb on the whole raid: 0.565 H at 3:25, 1.188 H at 4:49 (healing eaten per player).
 * - Intermission (5:06-5:47): Spirit Erasure soaks (64.6M), steady.
 * - P3 (5:47-8:15) both: steady = Coalesced Venom (127.9M); spot = Mutagenic and Volatile Venom, Gravebound,
 *   Retaliatory Malice, Grim Guillotine, Death's Whisper, Gloombomb (39.9M); tank = melee, Corrupted Toxin, Dread Bolt
 *   (36.4M). Blighted Sever's Venom Rupture at 6:22 (5 stacks), 6:53 (4), 7:27 (1), 8:03 (5). Healing absorbs (H per
 *   player): Defilement 0.716 at 5:48 and 0.964 at 7:30 on everyone, and on 7 players after Grim Guillotine at 6:09
 *   and 7:13 (avoidable; spread over the raid: 0.183); Suffocating Darkness 1.015 at 6:27 and 0.729 at 8:03.
 * Fight length: the kill's (8:16), set in fights\coiled_altar.simc.
 */
constexpr util::string_view coiled_altar_phases =
    "132:steady=0.905,spot=0.852,avoidable=0,tank=0.38,waves=10,stack=0.209,big_from=7,"
    "at=1+22+39+59+76+85+107+124,sizes=2.45+4.2+4.2+4.2+4.2+2.45+7.2+3.2/"
    "174:steady=2.38,spot=0.686,avoidable=0,tank=0.31,burst=0,absorb_at=73+157,absorbs=0.565+1.188/"
    "41:steady=2.78,spot=0,avoidable=0,tank=0.05,burst=0/"
    "148:steady=1.523,spot=0.669,avoidable=0,tank=0.57,waves=10,stack=0.209,big_from=5,at=35+66+100+136,"
    "sizes=5.2+4.2+1.2+5.2,absorb_at=1+22+40+86+103+136,absorbs=0.716+0.183+1.015+0.183+0.964+0.729";
}  // namespace

player_t* create_coiled_altar( sim_t* sim )
{
  return create_phased_patchwerk( sim, "Coiled Altar", coiled_altar_phases );
}
}  // namespace bosses