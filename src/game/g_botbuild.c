/*
===========================================================================
Copyright (C) 2026 Tremulous bot support contributors

This file is part of Tremulous.

Tremulous is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 3 of the License,
or (at your option) any later version.

Tremulous is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Tremulous; if not, see <https://www.gnu.org/licenses/>

===========================================================================
*/

// g_botbuild.c -- what bots know about their team, and the economic side
// of the game: human shopping, alien evolution and base building.
//
// Base building
//   Utility structures (spawns, armoury, medistation, DCC, booster) go on
//   random walkable spots 120-450 units from the HQ.
//   Defences (turrets, teslas, acid tubes, trappers, hives, barricades)
//   go on the rim of the base, at its choke points. These are found with
//   the navigation graph: flood outward from the HQ, then for a few
//   candidate radii count the graph nodes that sit on the boundary (nodes
//   inside the radius with a neighbour outside). The radius with the
//   fewest boundary nodes is where the base is most enclosed; each
//   connected group of boundary nodes there is one way into the base.
//   Narrow openings are true choke points and get their defences first,
//   wide ones get more of them.

#include "g_local.h"
#include "g_bot.h"

/*
===========================================================================

TEAM KNOWLEDGE

===========================================================================
*/

#define BOT_MAX_TEAM_BUILDABLES 512

static int      teamBuildables[ NUM_TEAMS ][ BOT_MAX_TEAM_BUILDABLES ];
static int      numTeamBuildables[ NUM_TEAMS ];
static int      teamHQ[ NUM_TEAMS ];
static vec3_t   teamBasePos[ NUM_TEAMS ];
static qboolean teamHaveBase[ NUM_TEAMS ];
static int      teamNextRefresh;

void BotTeam_Frame( void )
{
  int       i, t;
  gentity_t *ent;

  if( level.time < teamNextRefresh )
    return;

  teamNextRefresh = level.time + 400;

  for( t = 0; t < NUM_TEAMS; t++ )
  {
    numTeamBuildables[ t ] = 0;
    teamHQ[ t ] = -1;
  }

  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || ent->health <= 0 )
      continue;

    t = ent->buildableTeam;
    if( t <= TEAM_NONE || t >= NUM_TEAMS )
      continue;

    if( numTeamBuildables[ t ] < BOT_MAX_TEAM_BUILDABLES )
      teamBuildables[ t ][ numTeamBuildables[ t ]++ ] = i;

    if( ent->s.modelindex == BA_A_OVERMIND || ent->s.modelindex == BA_H_REACTOR )
      teamHQ[ t ] = i;
  }

  // remember where each base is, so it can be rebuilt in the same place
  for( t = TEAM_ALIENS; t < NUM_TEAMS; t++ )
  {
    if( teamHQ[ t ] >= 0 )
    {
      VectorCopy( g_entities[ teamHQ[ t ] ].r.currentOrigin, teamBasePos[ t ] );
      teamHaveBase[ t ] = qtrue;
    }
    else if( !teamHaveBase[ t ] )
    {
      for( i = 0; i < numTeamBuildables[ t ]; i++ )
      {
        ent = &g_entities[ teamBuildables[ t ][ i ] ];
        if( ent->s.modelindex == BA_A_SPAWN || ent->s.modelindex == BA_H_SPAWN )
        {
          VectorCopy( ent->r.currentOrigin, teamBasePos[ t ] );
          teamHaveBase[ t ] = qtrue;
          break;
        }
      }
    }
  }
}

int BotTeam_Buildables( team_t team, int **list )
{
  if( team <= TEAM_NONE || team >= NUM_TEAMS )
  {
    *list = NULL;
    return 0;
  }

  *list = teamBuildables[ team ];
  return numTeamBuildables[ team ];
}

gentity_t *BotTeam_HQ( team_t team )
{
  if( team <= TEAM_NONE || team >= NUM_TEAMS || teamHQ[ team ] < 0 )
    return NULL;

  if( !g_entities[ teamHQ[ team ] ].inuse || g_entities[ teamHQ[ team ] ].health <= 0 )
    return NULL;

  return &g_entities[ teamHQ[ team ] ];
}

qboolean BotTeam_HaveBaseInfo( team_t team, vec3_t basePos )
{
  if( team <= TEAM_NONE || team >= NUM_TEAMS || !teamHaveBase[ team ] )
    return qfalse;

  VectorCopy( teamBasePos[ team ], basePos );
  return qtrue;
}

int BotTeam_CountBuildable( team_t team, buildable_t b, qboolean spawnedOnly )
{
  int i, count = 0;
  gentity_t *ent;

  if( team <= TEAM_NONE || team >= NUM_TEAMS )
    return 0;

  for( i = 0; i < numTeamBuildables[ team ]; i++ )
  {
    ent = &g_entities[ teamBuildables[ team ][ i ] ];
    if( ent->s.modelindex != b || !ent->inuse || ent->health <= 0 )
      continue;
    if( spawnedOnly && !ent->spawned )
      continue;
    count++;
  }

  return count;
}

gentity_t *BotTeam_NearestBuildable( team_t team, buildable_t b, const vec3_t pos, float maxDist )
{
  int       i, best = -1;
  float     d, bestDist = maxDist > 0.0f ? maxDist : 999999.0f;
  gentity_t *ent;

  if( team <= TEAM_NONE || team >= NUM_TEAMS )
    return NULL;

  for( i = 0; i < numTeamBuildables[ team ]; i++ )
  {
    ent = &g_entities[ teamBuildables[ team ][ i ] ];
    if( ent->s.modelindex != b || !ent->inuse || ent->health <= 0 || !ent->spawned )
      continue;
    if( team == TEAM_HUMANS && !ent->powered && b != BA_H_REACTOR )
      continue;

    d = Distance( pos, ent->r.currentOrigin );
    if( d < bestDist )
    {
      bestDist = d;
      best = teamBuildables[ team ][ i ];
    }
  }

  return best >= 0 ? &g_entities[ best ] : NULL;
}

int BotTeam_Builders( team_t team, int exclude )
{
  int       i, count = 0;
  gclient_t *cl;

  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == exclude )
      continue;

    cl = &level.clients[ i ];
    if( cl->pers.connected != CON_CONNECTED || cl->pers.teamSelection != team )
      continue;

    if( g_bots[ i ].inuse && g_bots[ i ].role == BROLE_BUILDER )
    {
      count++;
      continue;
    }

    if( cl->sess.spectatorState != SPECTATOR_NOT )
      continue;

    if( cl->ps.stats[ STAT_CLASS ] == PCL_ALIEN_BUILDER0 ||
        cl->ps.stats[ STAT_CLASS ] == PCL_ALIEN_BUILDER0_UPG ||
        cl->ps.stats[ STAT_WEAPON ] == WP_HBUILD )
      count++;
  }

  return count;
}

/*
===========================================================================

HUMAN SHOPPING

===========================================================================
*/

#define BOT_NUM_WEAPON_PREFS 4

static const weapon_t humanWeaponPrefs[ BOT_NUM_WEAPON_PREFS ][ 7 ] =
{
  // rifleman
  { WP_PULSE_RIFLE, WP_CHAINGUN, WP_LAS_GUN, WP_SHOTGUN, WP_MACHINEGUN, WP_NONE },
  // marksman
  { WP_MASS_DRIVER, WP_LAS_GUN, WP_CHAINGUN, WP_SHOTGUN, WP_MACHINEGUN, WP_NONE },
  // close quarters
  { WP_FLAMER, WP_CHAINGUN, WP_SHOTGUN, WP_LAS_GUN, WP_MACHINEGUN, WP_NONE },
  // heavy weapons
  { WP_LUCIFER_CANNON, WP_PULSE_RIFLE, WP_CHAINGUN, WP_SHOTGUN, WP_MACHINEGUN, WP_NONE }
};

static qboolean Bot_CanBuyWeapon( weapon_t w )
{
  const weaponAttributes_t *wa = BG_Weapon( w );

  return wa->purchasable && wa->team == TEAM_HUMANS && BG_WeaponIsAllowed( w ) &&
         BG_WeaponAllowedInStage( w, g_humanStage.integer );
}

static qboolean Bot_CanBuyUpgrade( upgrade_t u )
{
  const upgradeAttributes_t *ua = BG_Upgrade( u );

  return ua->purchasable && ua->team == TEAM_HUMANS && BG_UpgradeIsAllowed( u ) &&
         BG_UpgradeAllowedInStage( u, g_humanStage.integer );
}

// the decision tree for a human's equipment: armour first (cheap and it
// keeps you alive), then the best weapon the bot's style allows, then a
// helmet, then extras
static void Bot_ChooseLoadout( bot_t *bot, weapon_t *weaponOut, int *upgradesOut )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  weapon_t      cur = ps->stats[ STAT_WEAPON ];
  weapon_t      w = WP_MACHINEGUN;
  int           budget, i, ups = 0, armourRefund = 0;
  const weapon_t *prefs = humanWeaponPrefs[ bot->pers.weaponPref % BOT_NUM_WEAPON_PREFS ];

  budget = ent->client->pers.credit;
  if( cur != WP_NONE && BG_Weapon( cur )->purchasable )
    budget += BG_Weapon( cur )->price;

  if( BG_InventoryContainsUpgrade( UP_LIGHTARMOUR, ps->stats ) )
  {
    ups |= ( 1 << UP_LIGHTARMOUR );
    armourRefund += LIGHTARMOUR_PRICE;
  }
  if( BG_InventoryContainsUpgrade( UP_HELMET, ps->stats ) )
  {
    ups |= ( 1 << UP_HELMET );
    armourRefund += HELMET_PRICE;
  }
  if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, ps->stats ) )
    ups |= ( 1 << UP_BATTLESUIT );
  if( BG_InventoryContainsUpgrade( UP_BATTPACK, ps->stats ) )
    ups |= ( 1 << UP_BATTPACK );
  if( BG_InventoryContainsUpgrade( UP_JETPACK, ps->stats ) )
    ups |= ( 1 << UP_JETPACK );

  // battlesuit replaces armour and helmet
  if( !( ups & ( 1 << UP_BATTLESUIT ) ) && Bot_CanBuyUpgrade( UP_BATTLESUIT ) &&
      budget + armourRefund >= BSUIT_PRICE + 350 + (int)( bot->pers.saver * 150.0f ) &&
      !( ups & ( 1 << UP_JETPACK ) ) )
  {
    budget += armourRefund - BSUIT_PRICE;
    ups &= ~( ( 1 << UP_LIGHTARMOUR ) | ( 1 << UP_HELMET ) );
    ups |= ( 1 << UP_BATTLESUIT );
  }
  else if( !( ups & ( ( 1 << UP_LIGHTARMOUR ) | ( 1 << UP_BATTLESUIT ) ) ) &&
           Bot_CanBuyUpgrade( UP_LIGHTARMOUR ) && budget >= LIGHTARMOUR_PRICE )
  {
    budget -= LIGHTARMOUR_PRICE;
    ups |= ( 1 << UP_LIGHTARMOUR );
  }

  // best weapon in our style that we can afford
  for( i = 0; prefs[ i ] != WP_NONE; i++ )
  {
    if( !Bot_CanBuyWeapon( prefs[ i ] ) )
      continue;
    if( BG_Weapon( prefs[ i ] )->price > budget )
      continue;
    w = prefs[ i ];
    break;
  }

  budget -= BG_Weapon( w )->price;

  if( !( ups & ( ( 1 << UP_HELMET ) | ( 1 << UP_BATTLESUIT ) ) ) &&
      Bot_CanBuyUpgrade( UP_HELMET ) && budget >= HELMET_PRICE + 30 )
  {
    budget -= HELMET_PRICE;
    ups |= ( 1 << UP_HELMET );
  }

  if( BG_Weapon( w )->usesEnergy && !( ups & ( ( 1 << UP_BATTPACK ) | ( 1 << UP_JETPACK ) ) ) &&
      Bot_CanBuyUpgrade( UP_BATTPACK ) && budget >= BATTPACK_PRICE + 50 )
  {
    budget -= BATTPACK_PRICE;
    ups |= ( 1 << UP_BATTPACK );
  }
  else if( !( ups & ( ( 1 << UP_BATTPACK ) | ( 1 << UP_JETPACK ) | ( 1 << UP_BATTLESUIT ) ) ) &&
           bot->pers.aggression > 0.75f && Bot_CanBuyUpgrade( UP_JETPACK ) &&
           budget >= JETPACK_PRICE + 100 )
  {
    budget -= JETPACK_PRICE;
    ups |= ( 1 << UP_JETPACK );
  }

  if( !BG_InventoryContainsUpgrade( UP_GRENADE, ps->stats ) && Bot_CanBuyUpgrade( UP_GRENADE ) &&
      budget >= GRENADE_PRICE + 150 && bot->pers.aggression > 0.5f )
    ups |= ( 1 << UP_GRENADE );

  *weaponOut = w;
  *upgradesOut = ups;
}

static qboolean Bot_AmmoLow( playerState_t *ps )
{
  weapon_t                  w = ps->stats[ STAT_WEAPON ];
  const weaponAttributes_t  *wa = BG_Weapon( w );
  int                       have, max;

  if( w == WP_NONE || wa->infiniteAmmo )
    return qfalse;

  max = wa->maxAmmo * ( wa->maxClips + 1 );
  have = ps->ammo + ps->clips * wa->maxAmmo;

  return max > 0 && have < max * 0.35f;
}

qboolean BotShop_WantsToShop( bot_t *bot )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  gentity_t     *arm;
  weapon_t      w;
  int           ups, u;
  float         d;
  qboolean      ammoLow, outOfAmmo;

  // builders keep their construction kit
  if( bot->role == BROLE_BUILDER )
    return qfalse;

  arm = BotTeam_NearestBuildable( TEAM_HUMANS, BA_H_ARMOURY, ps->origin, 0 );
  if( !arm )
    return qfalse;

  d = Distance( ps->origin, arm->r.currentOrigin );
  ammoLow = Bot_AmmoLow( ps );
  outOfAmmo = ps->ammo <= 0 && ps->clips <= 0 &&
              !BG_Weapon( ps->stats[ STAT_WEAPON ] )->infiniteAmmo;

  if( level.time - bot->lastBuyTime < 12000 && !outOfAmmo )
    return qfalse;

  if( outOfAmmo || ( ammoLow && d < 2500.0f ) )
    return qtrue;

  Bot_ChooseLoadout( bot, &w, &ups );

  // a long trip back is only worth it for a real upgrade (or right after
  // spawning, when we're at the base anyway)
  if( d > 1800.0f && level.time - bot->spawnedAt > 8000 )
  {
    weapon_t cur = ps->stats[ STAT_WEAPON ];

    if( w != cur && BG_Weapon( w )->price >= BG_Weapon( cur )->price + 250 )
      return qtrue;
    if( ( ups & ( 1 << UP_BATTLESUIT ) ) && !BG_InventoryContainsUpgrade( UP_BATTLESUIT, ps->stats ) )
      return qtrue;
    return qfalse;
  }

  if( !BG_InventoryContainsUpgrade( UP_MEDKIT, ps->stats ) && d < 900.0f )
    return qtrue;

  if( w != ps->stats[ STAT_WEAPON ] )
    return qtrue;

  for( u = UP_NONE + 1; u < UP_NUM_UPGRADES; u++ )
  {
    if( ( ups & ( 1 << u ) ) && !BG_InventoryContainsUpgrade( u, ps->stats ) )
      return qtrue;
  }

  return qfalse;
}

void BotShop_Shop( bot_t *bot )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  weapon_t      w;
  int           ups, u;
  static const upgrade_t order[ ] =
  {
    UP_BATTLESUIT, UP_LIGHTARMOUR, UP_HELMET, UP_BATTPACK, UP_JETPACK, UP_GRENADE, UP_NONE
  };

  bot->lastBuyTime = level.time;
  bot->creditsAtLastBuy = ent->client->pers.credit;

  Bot_ChooseLoadout( bot, &w, &ups );

  if( w != ps->stats[ STAT_WEAPON ] )
    G_BotCommand( bot, "buy %s", BG_Weapon( w )->name );

  for( u = 0; order[ u ] != UP_NONE; u++ )
  {
    if( ( ups & ( 1 << order[ u ] ) ) && !BG_InventoryContainsUpgrade( order[ u ], ps->stats ) )
      G_BotCommand( bot, "buy %s", BG_Upgrade( order[ u ] )->name );
  }

  if( !BG_InventoryContainsUpgrade( UP_MEDKIT, ps->stats ) && Bot_CanBuyUpgrade( UP_MEDKIT ) )
    G_BotCommand( bot, "buy %s", BG_Upgrade( UP_MEDKIT )->name );

  G_BotCommand( bot, "buy %s", BG_Upgrade( UP_AMMO )->name );

  if( bot_chat.integer > 1 )
    G_BotSay( bot, qtrue, va( "Geared up with a %s", BG_Weapon( ps->stats[ STAT_WEAPON ] )->humanName ) );
}

const char *BotShop_SpawnItem( bot_t *bot )
{
  if( bot->role == BROLE_BUILDER && BG_WeaponIsAllowed( WP_HBUILD ) )
    return BG_Weapon( WP_HBUILD )->name;

  return BG_Weapon( WP_MACHINEGUN )->name;
}

/*
===========================================================================

ALIEN EVOLUTION

===========================================================================
*/

static int Bot_AlienTier( class_t c )
{
  switch( c )
  {
    case PCL_ALIEN_LEVEL0:      return 1;
    case PCL_ALIEN_LEVEL1:      return 2;
    case PCL_ALIEN_LEVEL1_UPG:  return 3;
    case PCL_ALIEN_LEVEL2:      return 4;
    case PCL_ALIEN_LEVEL2_UPG:  return 5;
    case PCL_ALIEN_LEVEL3:      return 6;
    case PCL_ALIEN_LEVEL3_UPG:  return 7;
    case PCL_ALIEN_LEVEL4:      return 8;
    default:                    return 0;
  }
}

// lines of evolution, best first
static const class_t alienPrefs[ 3 ][ 9 ] =
{
  // brawler: dragoons and tyrants
  { PCL_ALIEN_LEVEL4, PCL_ALIEN_LEVEL3_UPG, PCL_ALIEN_LEVEL3, PCL_ALIEN_LEVEL2_UPG,
    PCL_ALIEN_LEVEL2, PCL_ALIEN_LEVEL1_UPG, PCL_ALIEN_LEVEL1, PCL_NONE },
  // skirmisher: marauders
  { PCL_ALIEN_LEVEL2_UPG, PCL_ALIEN_LEVEL2, PCL_ALIEN_LEVEL4, PCL_ALIEN_LEVEL3_UPG,
    PCL_ALIEN_LEVEL3, PCL_ALIEN_LEVEL1_UPG, PCL_ALIEN_LEVEL1, PCL_NONE },
  // support: basilisks (heal aura, grab, gas), then the heavies
  { PCL_ALIEN_LEVEL1_UPG, PCL_ALIEN_LEVEL1, PCL_ALIEN_LEVEL3_UPG, PCL_ALIEN_LEVEL4,
    PCL_ALIEN_LEVEL3, PCL_ALIEN_LEVEL2_UPG, PCL_ALIEN_LEVEL2, PCL_NONE }
};

// the decision tree for evolving
static class_t Bot_ChooseEvolution( bot_t *bot )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  class_t   cur = ent->client->ps.stats[ STAT_CLASS ];
  int       credits = ent->client->pers.credit;
  int       stage = g_alienStage.integer;
  int       i, cost;
  const class_t *prefs;
  class_t   c;

  // builders only upgrade to the advanced granger
  if( cur == PCL_ALIEN_BUILDER0 || cur == PCL_ALIEN_BUILDER0_UPG )
  {
    if( bot->role == BROLE_BUILDER )
    {
      if( cur == PCL_ALIEN_BUILDER0 && BG_ClassIsAllowed( PCL_ALIEN_BUILDER0_UPG ) &&
          BG_ClassCanEvolveFromTo( cur, PCL_ALIEN_BUILDER0_UPG, credits, stage, 0 ) >= 0 )
        return PCL_ALIEN_BUILDER0_UPG;
      return PCL_NONE;
    }
    // a granger that is no longer needed as a builder becomes a fighter
  }

  prefs = alienPrefs[ bot->pers.alienPref % 3 ];

  for( i = 0; prefs[ i ] != PCL_NONE; i++ )
  {
    c = prefs[ i ];

    if( Bot_AlienTier( c ) <= Bot_AlienTier( cur ) )
      continue;
    if( !BG_ClassIsAllowed( c ) || !BG_ClassAllowedInStage( c, stage ) )
      continue;

    cost = BG_ClassCanEvolveFromTo( cur, c, credits, stage, 0 );
    if( cost < 0 )
      continue;

    // savers hold their evos for a dragoon or better, unless they're
    // about to hit the credit cap
    if( bot->pers.saver > 0.6f && Bot_AlienTier( c ) < 6 &&
        credits < ALIEN_MAX_CREDITS - ALIEN_CREDITS_PER_KILL )
      continue;

    return c;
  }

  // nothing in our line: take any upgrade when sitting on lots of evos
  if( credits >= ALIEN_CREDITS_PER_KILL * 3 )
  {
    for( c = PCL_ALIEN_LEVEL4; c >= PCL_ALIEN_LEVEL1; c-- )
    {
      if( Bot_AlienTier( c ) <= Bot_AlienTier( cur ) )
        continue;
      if( !BG_ClassIsAllowed( c ) || !BG_ClassAllowedInStage( c, stage ) )
        continue;
      if( BG_ClassCanEvolveFromTo( cur, c, credits, stage, 0 ) >= 0 )
        return c;
    }
  }

  return PCL_NONE;
}

qboolean BotEvolve_Think( bot_t *bot, qboolean safe )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  class_t       target;
  int           i;

  if( !safe || level.time < bot->nextEvolveTry )
    return qfalse;

  bot->nextEvolveTry = level.time + 2000;

  if( ps->groundEntityNum == ENTITYNUM_NONE || ( ps->eFlags & EF_WALLCLIMB ) ||
      ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
    return qfalse;

  if( !G_Overmind( ) )
    return qfalse;

  // the game refuses to evolve with humans or powered human structures in
  // a box around us (same test as the class command)
  {
    vec3_t  mins, maxs;
    int     list[ MAX_GENTITIES ], num;
    float   r = ( ALIENSENSE_RANGE * 0.5f ) / 1.7320508f + 8.0f;

    VectorSet( mins, ps->origin[ 0 ] - r, ps->origin[ 1 ] - r, ps->origin[ 2 ] - r );
    VectorSet( maxs, ps->origin[ 0 ] + r, ps->origin[ 1 ] + r, ps->origin[ 2 ] + r );
    num = trap_EntitiesInBox( mins, maxs, list, MAX_GENTITIES );
    for( i = 0; i < num; i++ )
    {
      gentity_t *e = &g_entities[ list[ i ] ];

      if( ( e->client && e->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS ) ||
          ( e->s.eType == ET_BUILDABLE && e->buildableTeam == TEAM_HUMANS && e->powered ) )
        return qfalse;
    }
  }

  target = Bot_ChooseEvolution( bot );
  if( target == PCL_NONE || target == ps->stats[ STAT_CLASS ] )
    return qfalse;

  // after a few failures (usually no room), move somewhere else first
  if( bot->evolveFailures >= 2 )
  {
    bot->evolveFailures = 0;
    bot->nextEvolveTry = level.time + 6000;
    return qfalse;
  }

  bot->evolveFailures++;
  G_BotCommand( bot, "class %s", BG_Class( target )->name );

  if( bot_chat.integer > 1 )
    G_BotSay( bot, qtrue, va( "Evolving to %s", BG_ClassConfig( target )->humanName ) );

  return qtrue;
}

/*
===========================================================================

BASE PLANNING

===========================================================================
*/

#define BOT_MAX_DEFENSE_SPOTS   24
#define BOT_MAX_BOUNDARY        1024

typedef struct
{
  qboolean  valid;
  vec3_t    hqPos;
  float     rimRadius;
  int       numDefense;
  vec3_t    defense[ BOT_MAX_DEFENSE_SPOTS ];
  int       defenseWidth[ BOT_MAX_DEFENSE_SPOTS ];
  int       defenseFailUntil[ BOT_MAX_DEFENSE_SPOTS ];
  int       nextPlanTime;
  int       openings;
} botBasePlan_t;

static botBasePlan_t  basePlans[ NUM_TEAMS ];
static float          planDists[ NUM_TEAMS ][ NAV_MAX_NODES ];
static float          *planDist = planDists[ 0 ];
static int            planBoundary[ BOT_MAX_BOUNDARY ];
static int            planCluster[ BOT_MAX_BOUNDARY ];
static int            planQueue[ BOT_MAX_BOUNDARY ];

void BotBuild_ResetPlans( void )
{
  memset( basePlans, 0, sizeof( basePlans ) );
}

// the movement abilities of whoever attacks this team's base
static void Bot_AttackerCaps( team_t team, navCaps_t *caps )
{
  if( team == TEAM_HUMANS )
    BotNav_CapsForClass( PCL_ALIEN_LEVEL2, TEAM_ALIENS, qfalse, caps );
  else
    BotNav_CapsForClass( PCL_HUMAN, TEAM_HUMANS, qfalse, caps );
}

static int Plan_Boundary( float radius, int *out, int max )
{
  int       n, i, count = 0, t;
  navNode_t *node;

  for( n = 0; n < navNumNodes; n++ )
  {
    if( planDist[ n ] < 0.0f || planDist[ n ] > radius )
      continue;

    node = &navNodes[ n ];
    for( i = 0; i < node->numEdges; i++ )
    {
      t = navEdges[ node->firstEdge + i ].target;
      if( planDist[ t ] < 0.0f || planDist[ t ] > radius )
        break;
    }

    if( i < node->numEdges )
    {
      if( out && count < max )
        out[ count ] = n;
      count++;
    }
  }

  return count;
}

static qboolean Plan_LOS( const vec3_t a, const vec3_t b )
{
  trace_t tr;
  vec3_t  p1, p2;

  VectorCopy( a, p1 );
  VectorCopy( b, p2 );
  p1[ 2 ] += 40.0f;
  p2[ 2 ] += 32.0f;
  trap_Trace( &tr, p1, NULL, NULL, p2, ENTITYNUM_NONE, MASK_SOLID );
  return tr.fraction >= 1.0f;
}

static void BotBuild_PlanBase( team_t team )
{
  botBasePlan_t *plan = &basePlans[ team ];
  navCaps_t     caps;
  vec3_t        hqPos, centroid, p;
  int           start, numB, i, j, k, c, numClusters, n, best, s;
  int           clusterSize[ BOT_MAX_DEFENSE_SPOTS ];
  vec3_t        clusterCenter[ BOT_MAX_DEFENSE_SPOTS ];
  int           clusterOrder[ BOT_MAX_DEFENSE_SPOTS ];
  int           bestCount = 999999;
  float         bestR = 600.0f, r, d, bestScore, score;
  static const float alienRadii[ ] = { 300.0f, 400.0f, 500.0f, 600.0f };
  static const float humanRadii[ ] = { 450.0f, 600.0f, 750.0f, 900.0f };
  const float   *radii = ( team == TEAM_ALIENS ) ? alienRadii : humanRadii;
  qboolean      clear;

  plan->nextPlanTime = level.time + 30000;
  planDist = planDists[ team ];

  if( !BotTeam_HaveBaseInfo( team, hqPos ) || !BotNav_Ready( ) )
    return;

  Bot_AttackerCaps( team, &caps );
  // plan with the smallest hull so narrow passages count as ways in too
  caps.hull = NAV_HULL_SMALL;

  start = BotNav_NearestNode( hqPos, NULL, qfalse );
  if( start < 0 )
    return;

  BotNav_Flood( start, &caps, radii[ 3 ] + 400.0f, planDist );

  // the radius at which the base is most enclosed
  for( i = 0; i < 4; i++ )
  {
    c = Plan_Boundary( radii[ i ], NULL, 0 );
    if( c > 0 && c < bestCount )
    {
      bestCount = c;
      bestR = radii[ i ];
    }
  }

  numB = Plan_Boundary( bestR, planBoundary, BOT_MAX_BOUNDARY );
  if( numB > BOT_MAX_BOUNDARY )
    numB = BOT_MAX_BOUNDARY;

  // group boundary nodes that touch into openings (breadth first)
  for( i = 0; i < numB; i++ )
    planCluster[ i ] = -1;

  numClusters = 0;
  for( i = 0; i < numB && numClusters < BOT_MAX_DEFENSE_SPOTS; i++ )
  {
    int head = 0, tail = 0;

    if( planCluster[ i ] >= 0 )
      continue;

    planCluster[ i ] = numClusters;
    clusterSize[ numClusters ] = 1;
    planQueue[ tail++ ] = i;

    while( head < tail )
    {
      j = planQueue[ head++ ];
      for( k = 0; k < numB; k++ )
      {
        if( planCluster[ k ] >= 0 )
          continue;
        if( Distance( navNodes[ planBoundary[ j ] ].origin,
                      navNodes[ planBoundary[ k ] ].origin ) < 80.0f )
        {
          planCluster[ k ] = numClusters;
          clusterSize[ numClusters ]++;
          planQueue[ tail++ ] = k;
        }
      }
    }
    numClusters++;
  }

  // centre of each opening
  for( c = 0; c < numClusters; c++ )
  {
    VectorClear( centroid );
    for( i = 0, n = 0; i < numB; i++ )
    {
      if( planCluster[ i ] != c )
        continue;
      VectorAdd( centroid, navNodes[ planBoundary[ i ] ].origin, centroid );
      n++;
    }
    if( n )
      VectorScale( centroid, 1.0f / n, centroid );
    VectorCopy( centroid, clusterCenter[ c ] );
    clusterOrder[ c ] = c;
  }

  // narrow openings (real choke points) first
  for( i = 0; i < numClusters; i++ )
    for( j = i + 1; j < numClusters; j++ )
      if( clusterSize[ clusterOrder[ j ] ] < clusterSize[ clusterOrder[ i ] ] )
      {
        k = clusterOrder[ i ];
        clusterOrder[ i ] = clusterOrder[ j ];
        clusterOrder[ j ] = k;
      }

  plan->numDefense = 0;
  plan->rimRadius = bestR;
  plan->openings = numClusters;
  VectorCopy( hqPos, plan->hqPos );

  // one defence per opening, then more for the wide ones
  for( s = 0; s < 3; s++ )
  {
    for( i = 0; i < numClusters && plan->numDefense < BOT_MAX_DEFENSE_SPOTS; i++ )
    {
      c = clusterOrder[ i ];
      if( s == 1 && clusterSize[ c ] < 5 )
        continue;
      if( s == 2 && clusterSize[ c ] < 10 )
        continue;

      best = -1;
      bestScore = 999999.0f;

      // a spot inside the base that sees the opening
      for( n = 0; n < navNumNodes; n++ )
      {
        if( planDist[ n ] < bestR - 280.0f || planDist[ n ] > bestR - 50.0f )
          continue;

        d = Distance( navNodes[ n ].origin, clusterCenter[ c ] );
        if( d > 300.0f || fabs( navNodes[ n ].origin[ 2 ] - clusterCenter[ c ][ 2 ] ) > 128.0f )
          continue;

        // keep away from the spots already chosen
        clear = qtrue;
        for( k = 0; k < plan->numDefense; k++ )
          if( Distance( plan->defense[ k ], navNodes[ n ].origin ) < 110.0f )
            clear = qfalse;
        if( !clear )
          continue;

        // structures need some room around them
        if( !( navNodes[ n ].hulls & ( 1 << NAV_HULL_DRAGOON ) ) )
          continue;

        score = fabs( planDist[ n ] - ( bestR - 150.0f ) ) + d * 0.3f;
        if( score >= bestScore )
          continue;

        if( !Plan_LOS( navNodes[ n ].origin, clusterCenter[ c ] ) )
          continue;

        bestScore = score;
        best = n;
      }

      if( best >= 0 )
      {
        VectorCopy( navNodes[ best ].origin, plan->defense[ plan->numDefense ] );
        plan->defenseWidth[ plan->numDefense ] = clusterSize[ c ];
        plan->defenseFailUntil[ plan->numDefense ] = 0;
        plan->numDefense++;
      }
    }
  }

  // if the base has no clear openings, ring it at the rim instead
  for( i = 0; plan->numDefense < 4 && i < 64; i++ )
  {
    n = BotNav_RandomNodeNear( hqPos, bestR * 0.5f, bestR, NULL );
    if( n < 0 || planDist[ n ] < 0.0f )
      continue;
    VectorCopy( navNodes[ n ].origin, p );
    clear = qtrue;
    for( k = 0; k < plan->numDefense; k++ )
      if( Distance( plan->defense[ k ], p ) < 150.0f )
        clear = qfalse;
    if( !clear )
      continue;
    VectorCopy( p, plan->defense[ plan->numDefense ] );
    plan->defenseWidth[ plan->numDefense ] = 99;
    plan->defenseFailUntil[ plan->numDefense ] = 0;
    plan->numDefense++;
  }

  plan->valid = qtrue;

  r = bestR;
  G_Printf( "bot: planned %s base: rim at %.0f units, %d way%s in, %d defence spots\n",
            BG_TeamName( team ), r, numClusters, numClusters == 1 ? "" : "s", plan->numDefense );
}

static botBasePlan_t *Bot_BasePlan( team_t team )
{
  botBasePlan_t *plan = &basePlans[ team ];
  vec3_t        hqPos;

  if( !BotTeam_HaveBaseInfo( team, hqPos ) )
    return NULL;

  if( ( !plan->valid || Distance( plan->hqPos, hqPos ) > 200.0f ) &&
      level.time >= plan->nextPlanTime )
    BotBuild_PlanBase( team );

  return plan->valid ? plan : NULL;
}

/*
===========================================================================

BUILD ORDERS

===========================================================================
*/

typedef struct
{
  buildable_t b;
  int         count;
} buildOrder_t;

static const buildOrder_t humanBuildOrder[ ] =
{
  { BA_H_SPAWN, 1 }, { BA_H_ARMOURY, 1 }, { BA_H_MGTURRET, 1 }, { BA_H_SPAWN, 2 },
  { BA_H_MEDISTAT, 1 }, { BA_H_MGTURRET, 2 }, { BA_H_DCC, 1 }, { BA_H_MGTURRET, 4 },
  { BA_H_TESLAGEN, 2 }, { BA_H_SPAWN, 3 }, { BA_H_MGTURRET, 6 }, { BA_H_TESLAGEN, 4 },
  { BA_H_SPAWN, 4 }, { BA_NONE, 0 }
};

static const buildOrder_t alienBuildOrder[ ] =
{
  { BA_A_SPAWN, 1 }, { BA_A_ACIDTUBE, 1 }, { BA_A_SPAWN, 2 }, { BA_A_BOOSTER, 1 },
  { BA_A_ACIDTUBE, 2 }, { BA_A_TRAPPER, 2 }, { BA_A_SPAWN, 3 }, { BA_A_HIVE, 1 },
  { BA_A_ACIDTUBE, 4 }, { BA_A_BARRICADE, 2 }, { BA_A_SPAWN, 4 }, { BA_A_TRAPPER, 4 },
  { BA_NONE, 0 }
};

static qboolean Bot_IsDefence( buildable_t b )
{
  switch( b )
  {
    case BA_H_MGTURRET:
    case BA_H_TESLAGEN:
    case BA_A_ACIDTUBE:
    case BA_A_TRAPPER:
    case BA_A_HIVE:
    case BA_A_BARRICADE:
      return qtrue;
    default:
      return qfalse;
  }
}

static qboolean Bot_BuildAllowed( team_t team, buildable_t b )
{
  int stage = ( team == TEAM_ALIENS ) ? g_alienStage.integer : g_humanStage.integer;

  return BG_BuildableIsAllowed( b ) && BG_BuildableAllowedInStage( b, stage );
}

static qboolean Bot_SpotOccupied( team_t team, const vec3_t spot, float radius )
{
  int       i, *list, num;
  gentity_t *e;
  float     dxy;

  num = BotTeam_Buildables( team, &list );
  for( i = 0; i < num; i++ )
  {
    e = &g_entities[ list[ i ] ];
    if( Distance( e->r.currentOrigin, spot ) < radius )
      return qtrue;

    // spawns need their exit kept clear (they destroy whatever blocks it)
    if( e->s.modelindex == BA_A_SPAWN || e->s.modelindex == BA_H_SPAWN )
    {
      dxy = sqrt( ( e->r.currentOrigin[ 0 ] - spot[ 0 ] ) * ( e->r.currentOrigin[ 0 ] - spot[ 0 ] ) +
                  ( e->r.currentOrigin[ 1 ] - spot[ 1 ] ) * ( e->r.currentOrigin[ 1 ] - spot[ 1 ] ) );
      if( dxy < 130.0f && fabs( e->r.currentOrigin[ 2 ] - spot[ 2 ] ) < 160.0f )
        return qtrue;
    }
  }

  return qfalse;
}

static qboolean Bot_PickUtilitySpot( bot_t *bot, team_t team, const vec3_t hqPos, vec3_t out )
{
  botBasePlan_t *plan = Bot_BasePlan( team );
  int           tries, n, k;
  qboolean      ok;
  float         maxR = ( team == TEAM_ALIENS ) ? 420.0f : 480.0f;
  vec3_t        p;

  planDist = planDists[ team ];

  for( tries = 0; tries < 64; tries++ )
  {
    if( BotNav_Ready( ) )
    {
      n = BotNav_RandomNodeNear( hqPos, 110.0f, maxR, NULL );
      if( n < 0 )
        continue;
      // keep corridors clear: only open floor
      if( !( navNodes[ n ].hulls & ( 1 << NAV_HULL_LARGE ) ) )
        continue;
      if( plan && ( planDist[ n ] < 0.0f || planDist[ n ] > maxR * 1.3f ) )
        continue;
      // the HQ's own floor first, other levels only if that is full
      if( tries < 40 && fabs( navNodes[ n ].origin[ 2 ] - hqPos[ 2 ] ) > 64.0f )
        continue;
      VectorCopy( navNodes[ n ].origin, p );
    }
    else
    {
      float a = G_BotRandom( ) * 2.0f * M_PI, r = 120.0f + G_BotRandom( ) * ( maxR - 120.0f );
      VectorSet( p, hqPos[ 0 ] + cos( a ) * r, hqPos[ 1 ] + sin( a ) * r, hqPos[ 2 ] );
    }

    if( Bot_SpotOccupied( team, p, 100.0f ) )
      continue;

    ok = qtrue;
    if( plan )
    {
      for( k = 0; k < plan->numDefense; k++ )
        if( Distance( plan->defense[ k ], p ) < 90.0f )
          ok = qfalse;
    }
    if( !ok )
      continue;

    VectorCopy( p, out );
    return qtrue;
  }

  return qfalse;
}

static int Bot_PickDefenceSpot( team_t team, vec3_t out )
{
  botBasePlan_t *plan = Bot_BasePlan( team );
  int           i;

  if( !plan )
    return -1;

  for( i = 0; i < plan->numDefense; i++ )
  {
    if( level.time < plan->defenseFailUntil[ i ] )
      continue;
    if( Bot_SpotOccupied( team, plan->defense[ i ], 90.0f ) )
      continue;
    VectorCopy( plan->defense[ i ], out );
    return i;
  }

  return -1;
}

static qboolean Bot_EnoughBP( team_t team, buildable_t b, const vec3_t pos )
{
  return G_GetBuildPoints( pos, team ) >= BG_Buildable( b )->buildPoints;
}

// decide what to build next and where; qfalse when the base is complete
static qboolean Bot_PlanNextBuild( bot_t *bot )
{
  gentity_t         *ent = &g_entities[ bot->clientNum ];
  team_t            team = ent->client->pers.teamSelection;
  gentity_t         *hq = BotTeam_HQ( team );
  const buildOrder_t *order = ( team == TEAM_ALIENS ) ? alienBuildOrder : humanBuildOrder;
  buildable_t       hqType = ( team == TEAM_ALIENS ) ? BA_A_OVERMIND : BA_H_REACTOR;
  vec3_t            hqPos, spot;
  int               i, idx;

  bot->buildSpotIndex = -1;

  if( !hq )
  {
    // rebuild the heart of the base where it was, or next to us
    if( !BotTeam_HaveBaseInfo( team, hqPos ) )
      VectorCopy( ent->client->ps.origin, hqPos );

    bot->buildType = hqType;
    VectorCopy( hqPos, bot->buildSpot );
    bot->buildTries = 0;
    bot->buildPhase = 0;
    return qtrue;
  }

  VectorCopy( hq->r.currentOrigin, hqPos );

  for( i = 0; order[ i ].b != BA_NONE; i++ )
  {
    if( BotTeam_CountBuildable( team, order[ i ].b, qfalse ) >= order[ i ].count )
      continue;
    if( !Bot_BuildAllowed( team, order[ i ].b ) )
      continue;
    if( !Bot_EnoughBP( team, order[ i ].b, hqPos ) )
    {
      G_BotDebug( bot, "not enough build points for %s (%d)", BG_Buildable( order[ i ].b )->name,
                  G_GetBuildPoints( hqPos, team ) );
      continue;
    }

    if( Bot_IsDefence( order[ i ].b ) )
    {
      idx = Bot_PickDefenceSpot( team, spot );
      if( idx < 0 && !Bot_PickUtilitySpot( bot, team, hqPos, spot ) )
      {
        G_BotDebug( bot, "no spot for %s", BG_Buildable( order[ i ].b )->name );
        continue;
      }
      bot->buildSpotIndex = idx;
    }
    else if( !Bot_PickUtilitySpot( bot, team, hqPos, spot ) )
    {
      G_BotDebug( bot, "no spot for %s", BG_Buildable( order[ i ].b )->name );
      continue;
    }

    bot->buildType = order[ i ].b;
    VectorCopy( spot, bot->buildSpot );
    bot->buildTries = 0;
    bot->buildPhase = 0;
    G_BotDebug( bot, "plan: %s at %s", BG_Buildable( bot->buildType )->name, vtos( spot ) );
    return qtrue;
  }

  bot->buildType = BA_NONE;
  return qfalse;
}

static void Bot_BuildFailed( bot_t *bot, itemBuildError_t err )
{
  team_t        team = g_entities[ bot->clientNum ].client->pers.teamSelection;
  botBasePlan_t *plan = &basePlans[ team ];

  G_BotDebug( bot, "can't build %s here (error %d)", BG_Buildable( bot->buildType )->name, err );

  if( bot->buildSpotIndex >= 0 && bot->buildSpotIndex < plan->numDefense )
    plan->defenseFailUntil[ bot->buildSpotIndex ] = level.time + 90000;

  bot->buildTries++;
  bot->buildPhase = 0;

  // out of build points / power / creep: give this one up for now
  if( err == IBE_NOALIENBP || err == IBE_NOHUMANBP || err == IBE_NODCC ||
      err == IBE_NOOVERMIND || err == IBE_ONEOVERMIND || err == IBE_ONEREACTOR ||
      bot->buildTries >= 4 )
  {
    bot->buildType = BA_NONE;
    bot->nextBuildPlan = level.time + 4000;
    return;
  }

  // try another spot for the same thing
  if( bot->buildType == BA_A_OVERMIND || bot->buildType == BA_H_REACTOR )
  {
    vec3_t p;
    int n = BotNav_RandomNodeNear( bot->buildSpot, 64.0f, 300.0f, NULL );
    if( n >= 0 )
    {
      VectorCopy( navNodes[ n ].origin, p );
      VectorCopy( p, bot->buildSpot );
    }
    return;
  }

  {
    gentity_t *hq = BotTeam_HQ( team );
    vec3_t    spot;

    if( hq && Bot_PickUtilitySpot( bot, team, hq->r.currentOrigin, spot ) )
    {
      VectorCopy( spot, bot->buildSpot );
      bot->buildSpotIndex = -1;
    }
  }
}

// a damaged structure a human builder should fix
static gentity_t *Bot_FindRepair( bot_t *bot )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  int       i, *list, num;
  gentity_t *e, *best = NULL;
  float     d, bestDist = 900.0f;

  num = BotTeam_Buildables( TEAM_HUMANS, &list );
  for( i = 0; i < num; i++ )
  {
    e = &g_entities[ list[ i ] ];
    if( !e->spawned || e->health <= 0 )
      continue;
    if( e->health >= BG_Buildable( e->s.modelindex )->health * 0.8f )
      continue;
    d = Distance( e->r.currentOrigin, ent->client->ps.origin );
    if( d < bestDist )
    {
      bestDist = d;
      best = e;
    }
  }

  return best;
}

qboolean BotBuild_WantBuilderRole( bot_t *bot )
{
  team_t team = g_entities[ bot->clientNum ].client->pers.teamSelection;

  if( !bot_build.integer || bot_buildersPerTeam.integer <= 0 )
    return qfalse;

  if( team == TEAM_HUMANS && !BG_WeaponIsAllowed( WP_HBUILD ) )
    return qfalse;
  if( team == TEAM_ALIENS && !BG_ClassIsAllowed( PCL_ALIEN_BUILDER0 ) )
    return qfalse;

  return BotTeam_Builders( team, bot->clientNum ) < bot_buildersPerTeam.integer;
}

/*
==================
BotBuild_Think

Runs a builder bot. Returns qfalse to let the normal fighting logic take
over (there's an enemy close by, or this bot isn't a builder any more).
==================
*/
qboolean BotBuild_Think( bot_t *bot )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  team_t        team = ent->client->pers.teamSelection;
  class_t       cls = ps->stats[ STAT_CLASS ];
  float         dist, d;
  gentity_t     *repair, *enemy;
  vec3_t        dir, angles, savedAngles, org, normal, bangles;
  int           groundEnt, k;
  itemBuildError_t err = IBE_NONE, firstErr = IBE_NONE;
  qboolean      found;
  float         buildDist, yaw, baseYaw;

  if( team == TEAM_HUMANS && ps->stats[ STAT_WEAPON ] != WP_HBUILD )
  {
    bot->role = BROLE_ATTACK;   // sold the kit or bought a gun
    return qfalse;
  }
  if( team == TEAM_ALIENS && cls != PCL_ALIEN_BUILDER0 && cls != PCL_ALIEN_BUILDER0_UPG )
  {
    bot->role = BROLE_ATTACK;   // evolved into a fighter
    return qfalse;
  }

  if( !bot_build.integer )
    return qfalse;

  if( bot->debug && bot_debug.integer >= 3 && level.time >= bot->nextDebugPrint )
    G_BotDebug( bot, "builder: type %d phase %d timer %d nextplan %d weapon %d",
                bot->buildType, bot->buildPhase, ps->stats[ STAT_MISC ],
                bot->nextBuildPlan - level.time, ps->weapon );

  // enemies nearby: defend ourselves / run
  if( bot->enemy >= 0 && bot->enemy < MAX_CLIENTS )
  {
    enemy = &g_entities[ bot->enemy ];
    if( Distance( enemy->r.currentOrigin, ps->origin ) < 700.0f &&
        level.time - bot->enemySeenTime < 2000 )
    {
      if( team == TEAM_HUMANS && ps->weapon == WP_HBUILD && level.time >= bot->nextBlasterSwap )
      {
        G_BotCommand( bot, "itemact blaster" );
        bot->nextBlasterSwap = level.time + 1500;
      }
      bot->buildPhase = 0;
      return qfalse;
    }
  }

  // humans need the construction kit in hand
  if( team == TEAM_HUMANS && ps->weapon != WP_HBUILD )
  {
    if( level.time >= bot->nextBlasterSwap && ps->weaponstate == WEAPON_READY )
    {
      G_BotCommand( bot, "itemact weapon" );
      bot->nextBlasterSwap = level.time + 1500;
    }
    return qtrue;
  }

  // waiting for the build timer: repair or guard
  if( ps->stats[ STAT_MISC ] > 0 && bot->buildPhase == 0 )
  {
    if( team == TEAM_HUMANS && ( repair = Bot_FindRepair( bot ) ) )
    {
      vec3_t c;

      VectorAdd( repair->r.absmin, repair->r.absmax, c );
      VectorScale( c, 0.5f, c );
      d = Distance( ps->origin, c );
      if( d > 90.0f )
        BotAI_PathToEntity( bot, repair );

      // look at it: the kit repairs whatever it points at
      VectorSubtract( c, ps->origin, dir );
      dir[ 2 ] -= ps->viewheight;
      vectoangles( dir, bot->viewAngles );
      bot->forceView = qtrue;
      return qtrue;
    }
    return qfalse;   // nothing to do: behave like a defender meanwhile
  }

  if( bot->buildType == BA_NONE )
  {
    qboolean planned = qfalse;

    if( level.time >= bot->nextBuildPlan )
    {
      planned = Bot_PlanNextBuild( bot );
      if( !planned )
        bot->nextBuildPlan = level.time + 3000;
    }

    if( !planned )
    {

      // nothing to build: repair, or fall back to normal behaviour
      if( team == TEAM_HUMANS && ( repair = Bot_FindRepair( bot ) ) )
      {
        vec3_t c;

        VectorAdd( repair->r.absmin, repair->r.absmax, c );
        VectorScale( c, 0.5f, c );
        if( Distance( ps->origin, c ) > 90.0f )
          BotAI_PathToEntity( bot, repair );
        VectorSubtract( c, ps->origin, dir );
        dir[ 2 ] -= ps->viewheight;
        vectoangles( dir, bot->viewAngles );
        bot->forceView = qtrue;
        return qtrue;
      }
      return qfalse;
    }
  }

  // a plan may have been overtaken (someone else built it)
  if( bot->buildType != BA_A_OVERMIND && bot->buildType != BA_H_REACTOR &&
      !BotTeam_HQ( team ) )
  {
    bot->buildType = BA_NONE;
    return qtrue;
  }
  if( ( bot->buildType == BA_A_OVERMIND || bot->buildType == BA_H_REACTOR ) && BotTeam_HQ( team ) )
  {
    bot->buildType = BA_NONE;
    return qtrue;
  }

  dist = Distance( ps->origin, bot->buildSpot );
  buildDist = BG_Class( cls )->buildDist;

  // phase 0: walk to where the structure lands on the spot: the structure
  // is placed buildDist in front of us
  if( bot->buildPhase == 0 )
  {
    qboolean  inPlace = qfalse;
    float     dxy, dzs;
    trace_t   tr;
    vec3_t    eye, target;

    dxy = sqrt( ( bot->buildSpot[ 0 ] - ps->origin[ 0 ] ) * ( bot->buildSpot[ 0 ] - ps->origin[ 0 ] ) +
                ( bot->buildSpot[ 1 ] - ps->origin[ 1 ] ) * ( bot->buildSpot[ 1 ] - ps->origin[ 1 ] ) );
    dzs = ps->origin[ 2 ] + ent->r.mins[ 2 ] - bot->buildSpot[ 2 ];

    if( dxy < buildDist + 40.0f && fabs( dzs ) < 48.0f )
    {
      VectorCopy( ps->origin, eye );
      VectorCopy( bot->buildSpot, target );
      target[ 2 ] += 24.0f;
      trap_Trace( &tr, eye, NULL, NULL, target, ent->s.number, MASK_SOLID );
      inPlace = ( tr.fraction >= 1.0f );
    }

    // too close: step back so the structure doesn't land past the spot
    if( inPlace && dxy < buildDist - 35.0f && level.time - bot->buildSettle < 1500 )
    {
      VectorSubtract( ps->origin, bot->buildSpot, dir );
      dir[ 2 ] = 0.0f;
      if( VectorLength( dir ) > 1.0f && BotMove_SafeDirection( bot, vectoyaw( dir ) ) )
      {
        bot->moveYaw = vectoyaw( dir );
        bot->wantMove = qtrue;
        bot->viewAngles[ PITCH ] = 0.0f;
        bot->viewAngles[ YAW ] = AngleNormalize180( bot->moveYaw + 180.0f );
        bot->forceView = qtrue;
        return qtrue;
      }
    }

    if( !inPlace )
    {
      if( !bot->buildSettle )
        bot->buildSettle = level.time;

      BotMove_SetGoal( bot, bot->buildSpot, -1 );
      if( BotMove_Unstick( bot ) )
        ;
      else if( !BotMove_FollowPath( bot ) )
        BotMove_MoveTowardSafe( bot, bot->buildSpot );

      if( !bot->forceView )
      {
        bot->viewAngles[ PITCH ] = 0.0f;
        bot->viewAngles[ YAW ] = bot->moveYaw;
      }

      // walking for too long: pick something else
      if( level.time - bot->buildSettle > 45000 && bot->buildSettle )
      {
        Bot_BuildFailed( bot, IBE_NOROOM );
        bot->buildSettle = level.time;
      }
      return qtrue;
    }

    // there: find a direction to face where the structure fits, toward
    // the spot first
    VectorCopy( ps->viewangles, savedAngles );

    VectorSubtract( bot->buildSpot, ps->origin, dir );
    dir[ 2 ] = 0.0f;
    baseYaw = ( VectorLength( dir ) > 8.0f ) ? vectoyaw( dir ) : G_BotRandom( ) * 360.0f;

    found = qfalse;
    yaw = baseYaw;
    for( k = 0; k < 8; k++ )
    {
      yaw = baseYaw + ( ( k & 1 ) ? -1.0f : 1.0f ) * ( ( k + 1 ) / 2 ) * 45.0f;
      VectorSet( angles, 0.0f, yaw, 0.0f );
      VectorCopy( angles, ps->viewangles );
      err = G_CanBuild( ent, bot->buildType, buildDist, org, normal, bangles, &groundEnt );
      if( k == 0 )
        firstErr = err;
      if( err == IBE_NONE || err == IBE_TNODEWARN || err == IBE_RPTNOREAC ||
          err == IBE_SPWNWARN )
      {
        found = qtrue;
        break;
      }
      // these don't depend on direction
      if( err == IBE_NOALIENBP || err == IBE_NOHUMANBP || err == IBE_NODCC ||
          err == IBE_NOOVERMIND || err == IBE_ONEOVERMIND || err == IBE_ONEREACTOR )
        break;
    }

    VectorCopy( savedAngles, ps->viewangles );
    level.numBuildablesForRemoval = 0;

    if( !found )
    {
      Bot_BuildFailed( bot, err != IBE_NONE ? err : firstErr );
      return qtrue;
    }

    bot->buildYaw = AngleNormalize180( yaw );
    VectorCopy( org, bot->buildOrigin );
    bot->buildPhase = 1;
    bot->buildPhaseTime = level.time;
    G_BotCommand( bot, "build %s", BG_Buildable( bot->buildType )->name );
  }

  // phase 1: face the right way, then place it
  bot->viewAngles[ PITCH ] = 10.0f;
  bot->viewAngles[ YAW ] = bot->buildYaw;
  bot->forceView = qtrue;

  // the build command didn't take (wrong stage, sudden death, ...)
  if( bot->buildPhase == 1 && level.time - bot->buildPhaseTime > 600 &&
      ( ps->stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK ) != bot->buildType )
  {
    Bot_BuildFailed( bot, IBE_PERMISSION );
    return qtrue;
  }

  if( ( ps->stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK ) == bot->buildType &&
      fabs( AngleSubtract( ps->viewangles[ YAW ], bot->buildYaw ) ) < 3.0f &&
      level.time - bot->buildPhaseTime > 150 )
  {
    if( !( bot->lastButtons & BUTTON_ATTACK ) )
    {
      bot->buttons |= BUTTON_ATTACK;
      bot->buildPhase = 2;
    }
  }

  // did it work? (placing the structure clears the blueprint)
  if( bot->buildPhase == 2 &&
      ( ps->stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK ) == BA_NONE )
  {
    G_BotDebug( bot, "built %s at %s", BG_Buildable( bot->buildType )->name, vtos( bot->buildOrigin ) );

    // it went up somewhere else than planned: that defence spot is now
    // covered by this structure
    {
      botBasePlan_t *plan = &basePlans[ team ];

      if( bot->buildSpotIndex >= 0 && bot->buildSpotIndex < plan->numDefense &&
          Distance( bot->buildOrigin, plan->defense[ bot->buildSpotIndex ] ) > 80.0f )
        VectorCopy( bot->buildOrigin, plan->defense[ bot->buildSpotIndex ] );
    }

    if( bot_chat.integer > 1 )
      G_BotSay( bot, qtrue, va( "Built a %s", BG_Buildable( bot->buildType )->humanName ) );
    bot->buildType = BA_NONE;
    bot->buildPhase = 0;
    bot->buildSettle = 0;
    bot->nextBuildPlan = level.time + 500;
    return qtrue;
  }

  if( level.time - bot->buildPhaseTime > 2500 )
  {
    // something blocked it at the last moment (a player walked in)
    Bot_BuildFailed( bot, IBE_NOROOM );
  }

  return qtrue;
}

void BotBuild_PrintPlans( void )
{
  int           t, i;
  botBasePlan_t *plan;

  for( t = TEAM_ALIENS; t < NUM_TEAMS; t++ )
  {
    plan = Bot_BasePlan( t );
    if( !plan )
    {
      G_Printf( "%s base: no plan (no base or no navigation)\n", BG_TeamName( t ) );
      continue;
    }

    G_Printf( "%s base at %s: rim %.0f units, %d way%s in, %d defence spots\n",
              BG_TeamName( t ), vtos( plan->hqPos ), plan->rimRadius, plan->openings,
              plan->openings == 1 ? "" : "s", plan->numDefense );
    for( i = 0; i < plan->numDefense; i++ )
      G_Printf( "  spot %2d: %s  opening width %d%s%s\n", i, vtos( plan->defense[ i ] ),
                plan->defenseWidth[ i ],
                Bot_SpotOccupied( t, plan->defense[ i ], 90.0f ) ? "  (built)" : "",
                level.time < plan->defenseFailUntil[ i ] ? "  (blocked)" : "" );
  }
}
