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

// g_botai.c -- bot decision making: perception, aiming, combat and goals
//
// Each frame a bot:
//   1. makes sure it is on a team and queued to spawn
//   2. looks for enemies (line of sight, alien sense, helmet radar,
//      whoever just hurt it)
//   3. every 250ms picks a goal from a simple priority list:
//        fight > heal > shop (human) > evolve (alien) > hunt > role goal
//   4. moves (path following or direct combat movement)
//   5. aims with a skill dependent turn rate, reaction time and error,
//      leads projectiles, and uses the class abilities:
//        dretch:    bite (automatic on contact), wall walk to close in
//        basilisk:  claw + grab, adv. gas cloud
//        marauder:  claw, jumping around, adv. lightning zap
//        dragoon:   claw, charged pounce, adv. barbs
//        tyrant:    claw, trample charge
//        granger:   claw / spit to defend itself, otherwise builds
//        humans:    every weapon, lucifer charging, medkit, grenades,
//                   jetpack for tall climbs

#include "g_local.h"
#include "g_bot.h"

/*
===========================================================================

WEAPON KNOWLEDGE

===========================================================================
*/

typedef struct
{
  weapon_t  weapon;
  float     minRange;       // try to stay further than this
  float     idealRange;
  float     maxRange;       // no point firing beyond this
  float     projSpeed;      // 0 for hitscan / melee
  qboolean  gravity;
  qboolean  melee;
} botWeapon_t;

static const botWeapon_t botWeapons[ ] =
{
  { WP_ALEVEL0,         0,   0, LEVEL0_BITE_RANGE,  0, qfalse, qtrue },
  { WP_ALEVEL1,         0,   0, LEVEL1_CLAW_RANGE,  0, qfalse, qtrue },
  { WP_ALEVEL1_UPG,     0,   0, LEVEL1_CLAW_U_RANGE, 0, qfalse, qtrue },
  { WP_ALEVEL2,         0,   0, LEVEL2_CLAW_RANGE,  0, qfalse, qtrue },
  { WP_ALEVEL2_UPG,     0,   0, LEVEL2_CLAW_U_RANGE, 0, qfalse, qtrue },
  { WP_ALEVEL3,         0,   0, LEVEL3_CLAW_RANGE,  0, qfalse, qtrue },
  { WP_ALEVEL3_UPG,     0,   0, LEVEL3_CLAW_UPG_RANGE, 0, qfalse, qtrue },
  { WP_ALEVEL4,         0,   0, LEVEL4_CLAW_RANGE,  0, qfalse, qtrue },
  { WP_ABUILD,          0,   0, ABUILDER_CLAW_RANGE, 0, qfalse, qtrue },
  { WP_ABUILD2,         0,   0, ABUILDER_CLAW_RANGE, 0, qfalse, qtrue },

  { WP_BLASTER,         0, 250,  700, BLASTER_SPEED,  qfalse, qfalse },
  { WP_MACHINEGUN,      0, 300,  900, 0,              qfalse, qfalse },
  { WP_PAIN_SAW,        0,  24, PAINSAW_RANGE, 0,     qfalse, qtrue },
  { WP_SHOTGUN,         0, 140,  420, 0,              qfalse, qfalse },
  { WP_LAS_GUN,         0, 400, 1100, 0,              qfalse, qfalse },
  { WP_MASS_DRIVER,   120, 600, 2200, 0,              qfalse, qfalse },
  { WP_CHAINGUN,        0, 250,  750, 0,              qfalse, qfalse },
  { WP_FLAMER,          0, 140,  290, FLAMER_SPEED,   qfalse, qfalse },
  { WP_PULSE_RIFLE,     0, 300,  850, PRIFLE_SPEED,   qfalse, qfalse },
  { WP_LUCIFER_CANNON, 220, 450, 1400, LCANNON_SPEED, qfalse, qfalse },
  { WP_HBUILD,          0,   0,    0, 0,              qfalse, qtrue },

  { WP_NONE,            0,   0,    0, 0,              qfalse, qfalse }
};

static const botWeapon_t *Bot_WeaponInfo( weapon_t w )
{
  int i;

  for( i = 0; botWeapons[ i ].weapon != WP_NONE; i++ )
  {
    if( botWeapons[ i ].weapon == w )
      return &botWeapons[ i ];
  }

  return &botWeapons[ i ];
}

/*
===========================================================================

HELPERS

===========================================================================
*/

static gentity_t *Bot_Ent( bot_t *bot )
{
  return &g_entities[ bot->clientNum ];
}

static team_t Bot_Team( bot_t *bot )
{
  return Bot_Ent( bot )->client->pers.teamSelection;
}

static class_t Bot_Class( bot_t *bot )
{
  return Bot_Ent( bot )->client->ps.stats[ STAT_CLASS ];
}

static qboolean Bot_Alive( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );

  return ent->client->sess.spectatorState == SPECTATOR_NOT &&
         ent->health > 0 && ent->client->ps.pm_type != PM_DEAD;
}

static void Ent_Center( gentity_t *ent, vec3_t out )
{
  if( ent->client )
  {
    VectorCopy( ent->client->ps.origin, out );
    return;
  }

  VectorAdd( ent->r.absmin, ent->r.absmax, out );
  VectorScale( out, 0.5f, out );
}

static float Ent_Radius( gentity_t *ent )
{
  float r = ent->r.maxs[ 0 ];

  if( ent->r.maxs[ 1 ] > r )
    r = ent->r.maxs[ 1 ];

  return r;
}

static void Bot_Eye( gentity_t *ent, vec3_t out )
{
  BG_GetClientViewOrigin( &ent->client->ps, out );
}

static float Bot_FrameSec( void )
{
  float s = ( level.time - level.previousTime ) / 1000.0f;

  if( s <= 0.0f || s > 0.2f )
    s = 0.05f;

  return s;
}

static float Bot_HealthFrac( gentity_t *ent )
{
  int max = ent->client->ps.stats[ STAT_MAX_HEALTH ];

  if( max <= 0 )
    return 1.0f;

  return (float)ent->health / (float)max;
}

static qboolean Bot_IsEnemyClient( gentity_t *self, gentity_t *other )
{
  if( !other->inuse || !other->client || other == self )
    return qfalse;
  if( other->client->pers.connected != CON_CONNECTED )
    return qfalse;
  if( other->client->sess.spectatorState != SPECTATOR_NOT )
    return qfalse;
  if( other->health <= 0 || other->client->ps.pm_type == PM_DEAD )
    return qfalse;
  if( other->flags & FL_NOTARGET )
    return qfalse;
  if( other->client->pers.teamSelection == TEAM_NONE ||
      other->client->pers.teamSelection == self->client->pers.teamSelection )
    return qfalse;

  return qtrue;
}

static qboolean Bot_IsEnemyBuildable( bot_t *bot, gentity_t *other )
{
  if( !other->inuse || other->s.eType != ET_BUILDABLE )
    return qfalse;
  if( other->health <= 0 )
    return qfalse;
  if( other->buildableTeam == Bot_Team( bot ) || other->buildableTeam == TEAM_NONE )
    return qfalse;

  // dretches can only bite structures that are still being built
  if( Bot_Class( bot ) == PCL_ALIEN_LEVEL0 && other->spawned )
    return qfalse;

  // grangers and construction kits don't attack structures
  if( Bot_Ent( bot )->client->ps.weapon == WP_HBUILD )
    return qfalse;

  return qtrue;
}

static qboolean Bot_ValidEnemy( bot_t *bot, int entNum )
{
  gentity_t *e;

  if( entNum < 0 || entNum >= level.num_entities )
    return qfalse;

  e = &g_entities[ entNum ];

  if( e->client )
    return Bot_IsEnemyClient( Bot_Ent( bot ), e );

  return Bot_IsEnemyBuildable( bot, e );
}

// line of sight from the bot's eye to a target; returns the point to aim at
static qboolean Bot_CanSee( bot_t *bot, gentity_t *target, vec3_t aimPoint )
{
  gentity_t *ent = Bot_Ent( bot );
  vec3_t    eye, points[ 3 ];
  trace_t   tr;
  int       i, n = 0;

  Bot_Eye( ent, eye );
  Ent_Center( target, points[ 0 ] );

  if( !trap_InPVS( eye, points[ 0 ] ) )
    return qfalse;

  if( target->client )
  {
    // body, head, feet
    VectorCopy( points[ 0 ], points[ 1 ] );
    points[ 1 ][ 2 ] += target->r.maxs[ 2 ] * 0.7f;
    VectorCopy( points[ 0 ], points[ 2 ] );
    points[ 2 ][ 2 ] += target->r.mins[ 2 ] * 0.6f;
    n = 3;
  }
  else
  {
    VectorCopy( points[ 0 ], points[ 1 ] );
    points[ 1 ][ 2 ] = target->r.absmax[ 2 ] - 6.0f;
    n = 2;
  }

  for( i = 0; i < n; i++ )
  {
    trap_Trace( &tr, eye, NULL, NULL, points[ i ], ent->s.number, MASK_SHOT );
    if( tr.entityNum == target->s.number || tr.fraction >= 1.0f )
    {
      if( aimPoint )
        VectorCopy( points[ i ], aimPoint );
      return qtrue;
    }
  }

  return qfalse;
}

static qboolean Bot_HasUpgrade( bot_t *bot, upgrade_t up )
{
  return BG_InventoryContainsUpgrade( up, Bot_Ent( bot )->client->ps.stats );
}

// distance between the two bounding boxes (roughly)
static float Bot_EdgeDistance( gentity_t *a, gentity_t *b )
{
  vec3_t  ca, cb;
  float   d;

  Ent_Center( a, ca );
  Ent_Center( b, cb );
  d = Distance( ca, cb ) - Ent_Radius( a ) - Ent_Radius( b );

  return d < 0.0f ? 0.0f : d;
}

/*
===========================================================================

AIMING

===========================================================================
*/

static void Bot_TurnToward( bot_t *bot, const vec3_t desired, float speedScale )
{
  float     maxTurn, diff;
  int       i;

  maxTurn = ( 160.0f + bot->skill * 55.0f ) * speedScale * Bot_FrameSec( );

  for( i = 0; i < 2; i++ )
  {
    diff = AngleSubtract( desired[ i ], bot->viewAngles[ i ] );

    // ease in: fast for big turns, gentle when nearly on target
    if( fabs( diff ) < maxTurn )
      bot->viewAngles[ i ] = desired[ i ];
    else
      bot->viewAngles[ i ] += ( diff > 0.0f ) ? maxTurn : -maxTurn;

    bot->viewAngles[ i ] = AngleNormalize180( bot->viewAngles[ i ] );
  }

  bot->viewAngles[ ROLL ] = 0.0f;
}

// remember where the enemy is, and return how far it has moved since
// 'delay' ms ago: aiming at the current point minus that is aiming where
// the enemy was, which is what a human's reaction lag looks like
static void Bot_AimLag( bot_t *bot, vec3_t lag )
{
  gentity_t *enemy;
  int       i, k, delay = G_BotAimDelay( bot ), want;

  VectorClear( lag );

  if( bot->enemy < 0 || bot->enemy >= MAX_GENTITIES )
  {
    bot->aimHistCount = 0;
    return;
  }

  enemy = &g_entities[ bot->enemy ];
  if( bot->aimHistEnt != bot->enemy )
  {
    bot->aimHistEnt = bot->enemy;
    bot->aimHistCount = 0;
  }

  // one sample per server frame
  if( !bot->aimHistCount ||
      bot->aimHistTime[ ( bot->aimHistHead + BOT_AIM_HISTORY - 1 ) % BOT_AIM_HISTORY ] != level.time )
  {
    VectorCopy( enemy->r.currentOrigin, bot->aimHist[ bot->aimHistHead ] );
    bot->aimHistTime[ bot->aimHistHead ] = level.time;
    bot->aimHistHead = ( bot->aimHistHead + 1 ) % BOT_AIM_HISTORY;
    if( bot->aimHistCount < BOT_AIM_HISTORY )
      bot->aimHistCount++;
  }

  if( delay <= 0 )
    return;

  // newest sample at least 'delay' old (or the oldest we have)
  want = level.time - delay;
  for( i = 1; i <= bot->aimHistCount; i++ )
  {
    k = ( bot->aimHistHead + BOT_AIM_HISTORY - i ) % BOT_AIM_HISTORY;
    if( bot->aimHistTime[ k ] <= want || i == bot->aimHistCount )
    {
      VectorSubtract( enemy->r.currentOrigin, bot->aimHist[ k ], lag );
      return;
    }
  }
}

// aim at a world point with skill based error (the aim cone) and lag (the
// aim delay); sets bot->aimLocked when the crosshair is within 'tolerance'
// degrees of where the bot thinks the target is
static void Bot_AimAt( bot_t *bot, const vec3_t point, float tolerance )
{
  gentity_t *ent = Bot_Ent( bot );
  vec3_t    eye, dir, desired, lag, lagged;
  float     err, dy, dp;

  Bot_Eye( ent, eye );
  Bot_AimLag( bot, lag );
  VectorSubtract( point, lag, lagged );
  VectorSubtract( lagged, eye, dir );
  vectoangles( dir, desired );

  if( level.time >= bot->nextAimErrorTime )
  {
    err = G_BotAimCone( bot );
    bot->aimError[ YAW ] = G_BotCrandom( ) * err;
    bot->aimError[ PITCH ] = G_BotCrandom( ) * err * 0.6f;
    bot->nextAimErrorTime = level.time + 250 + (int)( G_BotRandom( ) * 450.0f );
  }

  desired[ YAW ] += bot->aimError[ YAW ];
  desired[ PITCH ] += bot->aimError[ PITCH ];

  Bot_TurnToward( bot, desired, 1.0f );

  dy = fabs( AngleSubtract( desired[ YAW ] - bot->aimError[ YAW ], bot->viewAngles[ YAW ] ) );
  dp = fabs( AngleSubtract( desired[ PITCH ] - bot->aimError[ PITCH ], bot->viewAngles[ PITCH ] ) );
  bot->aimLocked = ( dy < tolerance && dp < tolerance * 1.3f );
}

// where to aim to hit 'target' with the current weapon
static void Bot_AimPoint( bot_t *bot, gentity_t *target, const vec3_t seenPoint, vec3_t out )
{
  gentity_t         *ent = Bot_Ent( bot );
  const botWeapon_t *wi = Bot_WeaponInfo( ent->client->ps.weapon );
  vec3_t            eye, vel;
  float             dist, t, lead;

  VectorCopy( seenPoint, out );

  if( target->client )
  {
    // aim a little high on humans (bigger hit box at the head end)
    if( target->client->pers.teamSelection == TEAM_HUMANS && !wi->melee )
      out[ 2 ] += 6.0f;

    VectorCopy( target->client->ps.velocity, vel );
  }
  else
    VectorClear( vel );

  if( wi->projSpeed > 0.0f )
  {
    Bot_Eye( ent, eye );
    dist = Distance( eye, out );
    t = dist / wi->projSpeed;

    // better bots lead their shots better
    lead = 0.35f + 0.065f * bot->skill;
    VectorMA( out, t * lead, vel, out );

    if( wi->gravity )
      out[ 2 ] += 0.5f * g_gravity.value * t * t;

    // flame balls inherit some of our own velocity
    if( wi->weapon == WP_FLAMER )
      VectorMA( out, -t * FLAMER_LAG, ent->client->ps.velocity, out );
  }
}

static float Bot_AimTolerance( bot_t *bot, gentity_t *target )
{
  vec3_t  eye, c;
  float   dist, r;

  Bot_Eye( Bot_Ent( bot ), eye );
  Ent_Center( target, c );
  dist = Distance( eye, c );
  r = Ent_Radius( target );
  if( dist < 1.0f )
    return 90.0f;

  return RAD2DEG( atan2( r, dist ) ) + 1.5f;
}

/*
===========================================================================

PERCEPTION

===========================================================================
*/

static void Bot_UpdateHurt( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  int       attacker;

  if( ent->health < bot->lastHealth )
  {
    bot->lastHurtTime = level.time;
    attacker = ent->client->ps.persistant[ PERS_ATTACKER ];
    if( attacker >= 0 && attacker < MAX_CLIENTS )
      bot->lastAttacker = attacker;
  }

  bot->lastHealth = ent->health;
}

static void Bot_FindEnemy( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  gentity_t *e;
  vec3_t    eye, c;
  int       i, best = -1, *list, num;
  float     d, score, bestScore = 999999.0f;
  qboolean  vis, sensed, helmet, bestVis = qfalse;
  team_t    team = Bot_Team( bot );
  int       interval;

  if( level.time < bot->nextEnemyScan )
    return;

  interval = 350 - bot->skill * 20;
  bot->nextEnemyScan = level.time + interval;

  if( bot_peaceful.integer )
  {
    bot->enemy = -1;
    return;
  }

  Bot_Eye( ent, eye );
  helmet = Bot_HasUpgrade( bot, UP_HELMET );

  for( i = 0; i < level.maxclients; i++ )
  {
    e = &g_entities[ i ];

    if( !Bot_IsEnemyClient( ent, e ) )
      continue;

    Ent_Center( e, c );
    d = Distance( eye, c );
    if( d > 2600.0f )
      continue;

    vis = Bot_CanSee( bot, e, NULL );
    sensed = qfalse;

    if( !vis )
    {
      if( team == TEAM_ALIENS && d < ALIENSENSE_RANGE )
        sensed = qtrue;
      else if( team == TEAM_HUMANS && helmet && d < HELMET_RANGE )
        sensed = qtrue;
      else if( i == bot->lastAttacker && level.time - bot->lastHurtTime < 3000 )
        sensed = qtrue;
      else if( i == bot->enemy && level.time - bot->enemySeenTime < 4000 )
        sensed = qtrue;
    }

    if( !vis && !sensed )
      continue;

    score = d;
    if( !vis )
      score += 700.0f;
    if( i == bot->enemy )
      score -= 200.0f;
    if( i == bot->lastAttacker && level.time - bot->lastHurtTime < 2000 )
      score -= 350.0f;
    // finish off the wounded
    score -= ( 1.0f - Bot_HealthFrac( e ) ) * 150.0f;

    if( score < bestScore )
    {
      bestScore = score;
      best = i;
      bestVis = vis;
    }
  }

  // structures, when no player is in sight
  if( !bestVis )
  {
    num = BotTeam_Buildables( team == TEAM_ALIENS ? TEAM_HUMANS : TEAM_ALIENS, &list );

    for( i = 0; i < num; i++ )
    {
      e = &g_entities[ list[ i ] ];

      if( !Bot_IsEnemyBuildable( bot, e ) )
        continue;

      Ent_Center( e, c );
      d = Distance( eye, c );
      if( d > 1400.0f )
        continue;

      if( !Bot_CanSee( bot, e, NULL ) )
        continue;

      score = d + ( best >= 0 ? 300.0f : 0.0f );

      // spawns and the HQ first; turrets that shoot us also matter
      if( e->s.modelindex == BA_A_SPAWN || e->s.modelindex == BA_H_SPAWN )
        score -= 250.0f;
      else if( e->s.modelindex == BA_A_OVERMIND || e->s.modelindex == BA_H_REACTOR )
        score -= 150.0f;
      else if( e->s.modelindex == BA_H_MGTURRET || e->s.modelindex == BA_H_TESLAGEN ||
               e->s.modelindex == BA_A_ACIDTUBE || e->s.modelindex == BA_A_HIVE )
        score -= 100.0f;

      if( score < bestScore )
      {
        bestScore = score;
        best = list[ i ];
        bestVis = qtrue;
      }
    }
  }

  if( best != bot->enemy )
  {
    bot->enemy = best;
    bot->enemyFirstSeen = level.time;
    G_BotDebug( bot, "new enemy %d", best );
  }

  if( best >= 0 && bestVis )
  {
    bot->enemySeenTime = level.time;
    Ent_Center( &g_entities[ best ], bot->enemyLastPos );
    if( g_entities[ best ].client )
      VectorCopy( g_entities[ best ].client->ps.velocity, bot->enemyLastVel );
    else
      VectorClear( bot->enemyLastVel );
  }
}

/*
===========================================================================

MOVEMENT HELPERS

===========================================================================
*/

// move to a point, using the navigation graph when needed; returns qfalse
// when there is no known way there (the caller should pick another goal)
static qboolean Bot_GoTo( bot_t *bot, const vec3_t pos, qboolean sprint )
{
  gentity_t *ent = Bot_Ent( bot );
  qboolean  moving, reachable = qtrue;

  if( bot->moveMode != BMOVE_PATH ||
      Distance( pos, bot->goalPos ) > 96.0f ||
      level.time - bot->pathTime > 5000 )
  {
    // throttle path finding when it keeps failing
    if( level.time >= bot->pathFailTime || Distance( pos, bot->goalPos ) > 96.0f )
      reachable = BotMove_SetGoal( bot, pos, -1 );
    else if( bot->moveMode == BMOVE_DIRECT && level.time < bot->pathFailTime )
      reachable = qfalse;
  }

  if( BotMove_Unstick( bot ) )
    return qtrue;

  // no route and not close: don't walk into walls
  if( !reachable && BotNav_Ready( ) &&
      Distance( ent->client->ps.origin, pos ) > 160.0f )
    return qfalse;

  moving = BotMove_FollowPath( bot );

  if( !moving )
    BotMove_MoveTowardSafe( bot, pos );

  if( sprint && ent->client->pers.teamSelection == TEAM_HUMANS &&
      ent->client->ps.stats[ STAT_STAMINA ] > 200 )
    bot->buttons |= BUTTON_SPRINT;

  return qtrue;
}

// a random spot near 'center' that this bot can actually walk to: flood
// the graph outward from where we stand (cheap, bounded by distance) and
// pick one of the nodes it reaches near 'center'
static int Bot_ReachableNodeNear( bot_t *bot, const vec3_t center, float minDist, float maxDist )
{
  gentity_t *ent = Bot_Ent( bot );
  int       start;
  float     reach;

  if( !BotNav_Ready( ) )
    return -1;

  start = BotNav_NearestNode( ent->client->ps.origin, &bot->caps, qfalse );
  if( start < 0 )
    return -1;

  reach = Distance( ent->client->ps.origin, center ) * 1.6f + maxDist;
  if( reach > 6000.0f )
    reach = 6000.0f;

  return BotNav_RandomReachable( start, &bot->caps, 0.0f, reach, center, maxDist );
}

// a friendly telenode / egg puts newly spawned players at a fixed spot
// next to it and kills whoever stands there. If pos is in such a spot,
// returns qtrue and the direction to step out of it.
static qboolean Bot_OnSpawnExit( team_t team, const vec3_t pos, vec3_t away )
{
  int       i, num, *list;
  gentity_t *e;
  vec3_t    mins, maxs, exitPos, d;
  float     h;

  if( team == TEAM_HUMANS )
    num = BotTeam_Buildables( TEAM_HUMANS, &list );
  else if( team == TEAM_ALIENS )
    num = BotTeam_Buildables( TEAM_ALIENS, &list );
  else
    return qfalse;

  for( i = 0; i < num; i++ )
  {
    e = &g_entities[ list[ i ] ];
    if( e->s.modelindex != BA_H_SPAWN && e->s.modelindex != BA_A_SPAWN )
      continue;

    BG_BuildableBoundingBox( e->s.modelindex, mins, maxs );
    if( e->s.modelindex == BA_A_SPAWN )
      VectorMA( e->r.currentOrigin, ( maxs[ 2 ] + MAX_ALIEN_BBOX ) * M_ROOT3 + 1.0f,
                e->s.origin2, exitPos );
    else
    {
      VectorCopy( e->r.currentOrigin, exitPos );
      exitPos[ 2 ] += maxs[ 2 ] + 25.0f;
    }

    VectorSubtract( pos, exitPos, d );
    if( fabs( d[ 2 ] ) > 72.0f )
      continue;
    d[ 2 ] = 0.0f;
    h = VectorLength( d );
    if( h > 64.0f )
      continue;

    if( away )
    {
      if( h < 1.0f )
        VectorSet( d, G_BotCrandom( ), G_BotCrandom( ), 0.0f );
      VectorNormalize( d );
      VectorCopy( d, away );
    }
    return qtrue;
  }

  return qfalse;
}

// look where we are going when there's nothing to shoot at
static void Bot_LookAhead( bot_t *bot )
{
  vec3_t desired;

  if( bot->forceView )
    return;

  desired[ PITCH ] = 0.0f;
  desired[ YAW ] = bot->wantMove ? bot->moveYaw : bot->viewAngles[ YAW ];
  desired[ ROLL ] = 0.0f;

  Bot_TurnToward( bot, desired, 0.6f );
}

static void Bot_Strafe( bot_t *bot, float towardYaw, float forwardBias )
{
  float yaw;

  if( level.time >= bot->nextStrafeChange )
  {
    bot->strafeDir = ( G_BotRandom( ) < 0.5f ) ? -1 : 1;
    bot->nextStrafeChange = level.time + 350 + (int)( G_BotRandom( ) * ( 1300 - bot->skill * 70 ) );
  }

  yaw = towardYaw + bot->strafeDir * ( 90.0f - forwardBias * 60.0f );

  if( !BotMove_SafeDirection( bot, yaw ) )
  {
    bot->strafeDir = -bot->strafeDir;
    bot->nextStrafeChange = level.time + 600;
    yaw = towardYaw + bot->strafeDir * ( 90.0f - forwardBias * 60.0f );
    if( !BotMove_SafeDirection( bot, yaw ) )
      return;
  }

  bot->moveYaw = yaw;
  bot->wantMove = qtrue;
}

/*
===========================================================================

ITEMS (humans)

===========================================================================
*/

static void Bot_ItemCommands( bot_t *bot )
{
  gentity_t     *ent = Bot_Ent( bot );
  playerState_t *ps = &ent->client->ps;
  qboolean      jetActive;

  if( Bot_Team( bot ) != TEAM_HUMANS || level.time < bot->nextItemUse )
    return;

  // medkit when hurt (it also cures poison)
  if( BG_InventoryContainsUpgrade( UP_MEDKIT, ps->stats ) &&
      !( ps->stats[ STAT_STATE ] & SS_HEALING_2X ) &&
      ( Bot_HealthFrac( ent ) < 0.45f ||
        ( ( ps->stats[ STAT_STATE ] & SS_POISONED ) && Bot_HealthFrac( ent ) < 0.8f ) ) )
  {
    G_BotCommand( bot, "itemact medkit" );
    bot->nextItemUse = level.time + 1500;
    return;
  }

  // jetpack on/off for climbs
  if( BG_InventoryContainsUpgrade( UP_JETPACK, ps->stats ) )
  {
    jetActive = BG_UpgradeIsActive( UP_JETPACK, ps->stats );
    if( bot->jetpackUntil > level.time && !jetActive )
    {
      G_BotCommand( bot, "itemact jetpack" );
      bot->nextItemUse = level.time + 300;
      return;
    }
    if( bot->jetpackUntil <= level.time && jetActive &&
        ps->groundEntityNum != ENTITYNUM_NONE )
    {
      G_BotCommand( bot, "itemdeact jetpack" );
      bot->nextItemUse = level.time + 300;
      return;
    }
  }

  // out of ammo for the main weapon: pull the blaster; back again once
  // we have ammo
  if( ps->weapon != WP_HBUILD && ps->stats[ STAT_WEAPON ] != WP_NONE &&
      ps->stats[ STAT_WEAPON ] != WP_HBUILD &&
      !BG_Weapon( ps->stats[ STAT_WEAPON ] )->infiniteAmmo )
  {
    if( ps->weapon != WP_BLASTER && ps->ammo <= 0 && ps->clips <= 0 &&
        ps->weaponstate != WEAPON_RELOADING )
    {
      G_BotCommand( bot, "itemact blaster" );
      bot->nextItemUse = level.time + 1000;
      return;
    }
  }

  // (ps->ammo and ps->clips always belong to the main weapon)
  if( ps->weapon == WP_BLASTER && ps->stats[ STAT_WEAPON ] != WP_NONE &&
      ps->stats[ STAT_WEAPON ] != WP_BLASTER &&
      ( ps->ammo > 0 || ps->clips > 0 || BG_Weapon( ps->stats[ STAT_WEAPON ] )->infiniteAmmo ) &&
      !( ps->stats[ STAT_WEAPON ] == WP_HBUILD && bot->enemy >= 0 ) )
  {
    // the "weapon" pseudo item switches from the blaster to the main gun
    G_BotCommand( bot, "itemact weapon" );
    bot->nextItemUse = level.time + 1000;
  }
}

/*
===========================================================================

COMBAT

===========================================================================
*/

static void Bot_HumanCombat( bot_t *bot, gentity_t *enemy, qboolean visible, const vec3_t seen )
{
  gentity_t         *ent = Bot_Ent( bot );
  playerState_t     *ps = &ent->client->ps;
  const botWeapon_t *wi = Bot_WeaponInfo( ps->weapon );
  vec3_t            aim, myPos, dir;
  float             dist, edge, toward, tol;
  qboolean          canFire;
  qboolean          meleeEnemy = qfalse;

  VectorCopy( ps->origin, myPos );
  dist = Distance( myPos, seen );
  edge = Bot_EdgeDistance( ent, enemy );

  VectorSubtract( seen, myPos, dir );
  toward = vectoyaw( dir );

  if( enemy->client )
  {
    class_t ec = enemy->client->ps.stats[ STAT_CLASS ];
    meleeEnemy = ( ec != PCL_ALIEN_BUILDER0 && ec != PCL_ALIEN_BUILDER0_UPG );
  }

  // ---- movement
  if( !visible )
  {
    Bot_GoTo( bot, seen, qfalse );
  }
  else if( wi->melee )
  {
    // painsaw: get in there
    if( edge > 8.0f )
      Bot_GoTo( bot, seen, qtrue );
    else
      Bot_Strafe( bot, toward, 0.8f );
  }
  else if( meleeEnemy && dist < 260.0f && Bot_HealthFrac( ent ) < 0.9f &&
           BotMove_SafeDirection( bot, toward + 180.0f ) )
  {
    // keep melee aliens at arm's length: back off while shooting
    bot->moveYaw = toward + 180.0f + bot->strafeDir * 35.0f;
    bot->wantMove = qtrue;
    if( level.time >= bot->nextStrafeChange )
    {
      bot->strafeDir = -bot->strafeDir;
      bot->nextStrafeChange = level.time + 500 + (int)( G_BotRandom( ) * 600.0f );
    }
  }
  else if( dist > wi->maxRange * 0.85f )
  {
    Bot_GoTo( bot, seen, qfalse );
  }
  else if( dist < wi->minRange && BotMove_SafeDirection( bot, toward + 180.0f ) )
  {
    bot->moveYaw = toward + 180.0f;
    bot->wantMove = qtrue;
  }
  else if( dist > wi->idealRange * 1.4f )
  {
    // close in while dodging
    Bot_Strafe( bot, toward, 0.6f );
  }
  else
  {
    Bot_Strafe( bot, toward, 0.0f );
    // the occasional dodge jump
    if( ps->stats[ STAT_STAMINA ] > 600 && G_BotRandom( ) < 0.01f * bot->skill * 0.3f &&
        level.time - bot->lastJumpTime > 1500 )
    {
      bot->upmove = 127;
      bot->lastJumpTime = level.time;
    }
  }

  if( !visible )
  {
    Bot_LookAhead( bot );
    return;
  }

  // ---- aim
  Bot_AimPoint( bot, enemy, seen, aim );
  tol = Bot_AimTolerance( bot, enemy );
  Bot_AimAt( bot, aim, tol );

  canFire = bot->aimLocked && level.time - bot->enemyFirstSeen > G_BotAimDelay( bot ) + 150;

  // ---- fire
  switch( ps->weapon )
  {
    case WP_LUCIFER_CANNON:
      if( dist < 140.0f )
        break;   // would blow ourselves up

      if( dist < 300.0f )
      {
        // quick secondary shots up close
        if( canFire && ps->stats[ STAT_MISC ] == 0 )
          bot->buttons |= BUTTON_ATTACK2;
        break;
      }

      // hold primary to charge, release when charged and on target
      if( ps->stats[ STAT_MISC ] < 1200 + bot->skill * 120 && ps->ammo > 0 )
        bot->buttons |= BUTTON_ATTACK;
      else if( !canFire && ps->stats[ STAT_MISC ] < LCANNON_CHARGE_TIME_WARN )
        bot->buttons |= BUTTON_ATTACK;   // keep charging until we line up
      break;

    case WP_HBUILD:
      break;

    case WP_PAIN_SAW:
      if( edge < PAINSAW_RANGE && bot->aimLocked )
        bot->buttons |= BUTTON_ATTACK;
      break;

    default:
      if( canFire && dist < wi->maxRange * 1.15f )
        bot->buttons |= BUTTON_ATTACK;
      break;
  }

  // grenade at structures or groups
  if( Bot_HasUpgrade( bot, UP_GRENADE ) && canFire && level.time >= bot->nextItemUse &&
      dist > 160.0f && dist < 450.0f && ( !enemy->client || G_BotRandom( ) < 0.05f ) )
  {
    G_BotCommand( bot, "itemact gren" );
    bot->nextItemUse = level.time + 2000;
  }
}

static void Bot_AlienCombat( bot_t *bot, gentity_t *enemy, qboolean visible, const vec3_t seen )
{
  gentity_t         *ent = Bot_Ent( bot );
  playerState_t     *ps = &ent->client->ps;
  const botWeapon_t *wi = Bot_WeaponInfo( ps->weapon );
  class_t           cls = ps->stats[ STAT_CLASS ];
  vec3_t            aim, myPos, dir, head;
  float             dist, edge, toward, tol, heightDiff;
  qboolean          onGround = ( ps->groundEntityNum != ENTITYNUM_NONE );
  qboolean          canAct;

  VectorCopy( ps->origin, myPos );
  dist = Distance( myPos, seen );
  edge = Bot_EdgeDistance( ent, enemy );
  VectorSubtract( seen, myPos, dir );
  toward = vectoyaw( dir );
  heightDiff = seen[ 2 ] - myPos[ 2 ];

  canAct = visible && level.time - bot->enemyFirstSeen > G_BotAimDelay( bot ) + 120;

  // ---- grangers mostly run, but bite back when cornered
  if( cls == PCL_ALIEN_BUILDER0 || cls == PCL_ALIEN_BUILDER0_UPG )
  {
    if( visible && cls == PCL_ALIEN_BUILDER0_UPG && dist > 120.0f && dist < 600.0f &&
        enemy->client )
    {
      Bot_AimPoint( bot, enemy, seen, aim );
      aim[ 2 ] += dist * 0.15f;
      Bot_AimAt( bot, aim, 4.0f );
      if( canAct && bot->aimLocked )
        bot->buttons |= BUTTON_USE_HOLDABLE;  // slowing spit
    }
    else if( visible && edge < ABUILDER_CLAW_RANGE + 16.0f )
    {
      Bot_AimAt( bot, seen, Bot_AimTolerance( bot, enemy ) );
      if( bot->aimLocked )
        bot->buttons |= BUTTON_ATTACK2;  // claw
    }
    else
      Bot_LookAhead( bot );

    if( visible && dist < 500.0f && enemy->client )
    {
      // back off toward our base
      gentity_t *hq = BotTeam_HQ( TEAM_ALIENS );
      if( hq )
        Bot_GoTo( bot, hq->r.currentOrigin, qfalse );
      else if( BotMove_SafeDirection( bot, toward + 180.0f ) )
      {
        bot->moveYaw = toward + 180.0f;
        bot->wantMove = qtrue;
      }
    }
    else if( !visible )
      Bot_GoTo( bot, seen, qfalse );
    return;
  }

  // ---- movement: close the distance, dodging on the way
  if( !visible )
  {
    // (the path itself says where to wall walk: holding it all the time
    // walks a dretch over the edge of a platform and down its side)
    Bot_GoTo( bot, seen, qfalse );
  }
  else
  {
    qboolean direct = dist < 700.0f && fabs( heightDiff ) < 64.0f &&
                      BotMove_SafeDirection( bot, toward );
    qboolean small = ( cls == PCL_ALIEN_LEVEL0 || cls == PCL_ALIEN_LEVEL1 ||
                       cls == PCL_ALIEN_LEVEL1_UPG || cls == PCL_ALIEN_LEVEL2 ||
                       cls == PCL_ALIEN_LEVEL2_UPG );
    float    reach = wi->maxRange;

    if( level.time >= bot->nextStrafeChange )
    {
      bot->strafeDir = ( G_BotRandom( ) < 0.5f ) ? -1 : 1;
      bot->nextStrafeChange = level.time + 250 + (int)( G_BotRandom( ) * 450.0f );
    }

    if( small && enemy->client && edge < reach * 0.55f )
    {
      // too close to hit anything but legs: circle around them
      bot->moveYaw = toward + bot->strafeDir * 75.0f;
      bot->wantMove = qtrue;
      if( !BotMove_SafeDirection( bot, bot->moveYaw ) )
      {
        bot->strafeDir = -bot->strafeDir;
        bot->moveYaw = toward + bot->strafeDir * 75.0f;
      }
      // and hop up to their head
      if( onGround && level.time - bot->lastJumpTime > 600 )
      {
        bot->upmove = 127;
        bot->lastJumpTime = level.time;
      }
    }
    else if( direct || edge < 48.0f )
    {
      bot->moveYaw = toward;
      bot->wantMove = qtrue;

      // zig-zag on the way in, harder the closer we get to their guns
      if( dist > 120.0f && dist < 700.0f && cls != PCL_ALIEN_LEVEL4 &&
          !( ps->stats[ STAT_STATE ] & SS_CHARGING ) )
        bot->moveYaw += bot->strafeDir * ( dist < 350.0f ? 45.0f : 30.0f );

      // small aliens leap at the head from just outside melee range
      if( small && enemy->client && onGround && edge < reach + 110.0f &&
          edge > reach * 0.5f && level.time - bot->lastJumpTime > 700 )
      {
        bot->moveYaw = toward;
        bot->upmove = 127;
        bot->lastJumpTime = level.time;
      }
    }
    else
      Bot_GoTo( bot, seen, qfalse );

    // target above us and close: wall walkers climb, others hop
    if( heightDiff > 48.0f && bot->caps.wallClimber && dist < 400.0f )
      bot->wantWallclimb = qtrue;

    // agile classes also jump to dodge on the way in
    if( small && onGround && level.time - bot->lastJumpTime > 900 &&
        dist > 160.0f && dist < 500.0f && G_BotRandom( ) < 0.08f )
    {
      bot->upmove = 127;
      bot->lastJumpTime = level.time;
    }
  }

  if( !visible )
  {
    if( !bot->forceView )
      Bot_LookAhead( bot );
    return;
  }

  // ---- aim
  VectorCopy( seen, head );
  if( enemy->client )
  {
    // go for the head: higher hits do far more damage
    Ent_Center( enemy, head );
    head[ 2 ] = enemy->r.currentOrigin[ 2 ] + enemy->r.maxs[ 2 ] * 0.85f;
  }

  tol = Bot_AimTolerance( bot, enemy ) + 2.0f;

  switch( cls )
  {
    case PCL_ALIEN_LEVEL3:
    case PCL_ALIEN_LEVEL3_UPG:
    {
      int maxCharge = ( cls == PCL_ALIEN_LEVEL3 ) ? LEVEL3_POUNCE_TIME : LEVEL3_POUNCE_TIME_UPG;
      int jumpMag = ( cls == PCL_ALIEN_LEVEL3 ) ? LEVEL3_POUNCE_JUMP_MAG : LEVEL3_POUNCE_JUMP_MAG_UPG;

      // barbs from range
      if( cls == PCL_ALIEN_LEVEL3_UPG && ps->ammo > 0 && dist > 220.0f && dist < 900.0f &&
          !bot->chargeStart )
      {
        vec3_t eye;
        float  t;

        Bot_Eye( ent, eye );
        t = Distance( eye, seen ) / LEVEL3_BOUNCEBALL_SPEED;
        VectorCopy( seen, aim );
        if( enemy->client )
          VectorMA( aim, t * 0.6f, enemy->client->ps.velocity, aim );
        aim[ 2 ] += 0.5f * g_gravity.value * t * t;
        Bot_AimAt( bot, aim, tol );
        if( canAct && bot->aimLocked && ps->weaponTime <= 0 )
          bot->buttons |= BUTTON_USE_HOLDABLE;
        break;
      }

      // pounce from mid range: hold to charge, release to leap
      if( ( bot->chargeStart || ( dist > 110.0f && dist < 650.0f && onGround ) ) &&
          !( ps->pm_flags & PMF_CHARGE ) )
      {
        float  t, needed;
        vec3_t eye;

        if( !bot->chargeStart )
        {
          bot->chargeStart = level.time;
          // charge proportional to the distance to cover
          needed = dist / (float)jumpMag * 1.35f;
          bot->chargeFor = (int)( needed * maxCharge );
          if( bot->chargeFor < LEVEL3_POUNCE_TIME_MIN + 80 )
            bot->chargeFor = LEVEL3_POUNCE_TIME_MIN + 80;
          if( bot->chargeFor > maxCharge )
            bot->chargeFor = maxCharge;
        }

        // aim over the target so the arc lands on it
        Bot_Eye( ent, eye );
        t = Distance( eye, seen ) / jumpMag;
        VectorCopy( seen, aim );
        if( enemy->client )
          VectorMA( aim, t * 0.8f, enemy->client->ps.velocity, aim );
        aim[ 2 ] += 0.5f * g_gravity.value * t * t * 0.9f;
        Bot_AimAt( bot, aim, tol * 1.5f );

        if( ps->stats[ STAT_MISC ] < bot->chargeFor ||
            ( !bot->aimLocked && level.time - bot->chargeStart < 1600 ) )
          bot->buttons |= BUTTON_ATTACK2;    // keep charging
        else
          bot->chargeStart = 0;              // release: pounce

        if( level.time - bot->chargeStart > 2500 )
          bot->chargeStart = 0;
        break;
      }

      bot->chargeStart = 0;
      Bot_AimAt( bot, head, tol );
      if( canAct && bot->aimLocked && edge < wi->maxRange )
        bot->buttons |= BUTTON_ATTACK;
      break;
    }

    case PCL_ALIEN_LEVEL4:
      Bot_AimAt( bot, head, tol );

      // trample: build up a charge while running at them, then let go
      if( !( ps->stats[ STAT_STATE ] & SS_CHARGING ) && dist > 250.0f && dist < 1000.0f &&
          fabs( heightDiff ) < 64.0f && bot->aimLocked && visible )
      {
        if( !bot->chargeStart )
          bot->chargeStart = level.time;

        if( ps->stats[ STAT_MISC ] < LEVEL4_TRAMPLE_CHARGE_MAX * 0.85f &&
            level.time - bot->chargeStart < 2500 )
        {
          bot->buttons |= BUTTON_ATTACK2;
          bot->moveYaw = toward;
          bot->wantMove = qtrue;
        }
        else
          bot->chargeStart = 0;
      }
      else
        bot->chargeStart = 0;

      // keep running straight while charging
      if( ps->stats[ STAT_STATE ] & SS_CHARGING )
      {
        bot->moveYaw = toward;
        bot->wantMove = qtrue;
      }

      if( canAct && bot->aimLocked && edge < LEVEL4_CLAW_RANGE )
        bot->buttons |= BUTTON_ATTACK;
      break;

    case PCL_ALIEN_LEVEL2_UPG:
      Bot_AimAt( bot, head, tol );
      if( canAct && bot->aimLocked )
      {
        if( edge < LEVEL2_CLAW_U_RANGE )
          bot->buttons |= BUTTON_ATTACK;
        else if( dist < LEVEL2_AREAZAP_RANGE - 10.0f && ps->weaponTime <= 0 )
          bot->buttons |= BUTTON_ATTACK2;    // zap
      }
      break;

    case PCL_ALIEN_LEVEL1_UPG:
      Bot_AimAt( bot, head, tol );
      if( canAct && bot->aimLocked )
      {
        if( dist < LEVEL1_PCLOUD_RANGE - 20.0f && enemy->client &&
            !( enemy->client->ps.eFlags & EF_POISONCLOUDED ) && ps->weaponTime <= 0 &&
            G_BotRandom( ) < 0.3f )
          bot->buttons |= BUTTON_ATTACK2;    // gas
        else if( edge < LEVEL1_CLAW_U_RANGE )
          bot->buttons |= BUTTON_ATTACK;
      }
      break;

    case PCL_ALIEN_LEVEL0:
      // the bite is automatic on contact: just point at them and run in
      Bot_AimAt( bot, head, tol + 4.0f );
      break;

    default:
      Bot_AimAt( bot, head, tol );
      if( canAct && bot->aimLocked && edge < wi->maxRange + 4.0f )
        bot->buttons |= BUTTON_ATTACK;
      break;
  }
}

/*
===========================================================================

SPAWNING

===========================================================================
*/

static void Bot_PickTeam( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  team_t    team = bot->wantTeam;

  if( level.time < bot->nextTeamTry )
    return;

  bot->nextTeamTry = level.time + 2000;

  if( team == TEAM_NONE )
  {
    if( level.numAlienClients < level.numHumanClients )
      team = TEAM_ALIENS;
    else if( level.numHumanClients < level.numAlienClients )
      team = TEAM_HUMANS;
    else
      team = G_BotRandInt( 2 ) ? TEAM_ALIENS : TEAM_HUMANS;
  }

  if( team == TEAM_ALIENS && level.alienTeamLocked )
    return;
  if( team == TEAM_HUMANS && level.humanTeamLocked )
    return;

  G_ChangeTeam( ent, team );
  G_BotDebug( bot, "joined %s", BG_TeamName( team ) );
}

// how many of our team's bots (not counting this one or builders) there
// are, and how many of them defend
static void Bot_TeamRoles( bot_t *bot, int *members, int *defenders )
{
  team_t  team = Bot_Team( bot );
  int     i;

  *members = *defenders = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == bot->clientNum || !g_bots[ i ].inuse || g_bots[ i ].role == BROLE_BUILDER ||
        level.clients[ i ].pers.connected != CON_CONNECTED ||
        level.clients[ i ].pers.teamSelection != team )
      continue;

    ( *members )++;
    if( g_bots[ i ].role == BROLE_DEFEND )
      ( *defenders )++;
  }
}

static int Bot_MaxDefenders( int teamBots )
{
  int n = (int)( teamBots * 0.34f + 0.5f );

  return n < 1 ? 1 : n;
}

static void Bot_ChooseRole( bot_t *bot )
{
  int members, defenders;

  if( BotBuild_WantBuilderRole( bot ) )
  {
    bot->role = BROLE_BUILDER;
    return;
  }

  Bot_TeamRoles( bot, &members, &defenders );

  // about a third of the team defends (careful bots are likelier to),
  // the rest split between pushing the enemy base and hunting players
  if( defenders < Bot_MaxDefenders( members + 1 ) &&
      ( ( defenders == 0 && members >= 2 ) ||
        G_BotRandom( ) < 0.25f + ( 1.0f - bot->pers.aggression ) * 0.3f ) )
    bot->role = BROLE_DEFEND;
  else if( G_BotRandom( ) < 0.4f )
    bot->role = BROLE_ROAM;
  else
    bot->role = BROLE_ATTACK;

  bot->nextRoleCheck = level.time + 15000 + (int)( G_BotRandom( ) * 10000.0f );
}

// roles are picked at each respawn, and defenders rarely die, so left
// alone a team slowly turns into a crowd of base campers: every so often
// keep the number of defenders to about a third of the team
static void Bot_BalanceRole( bot_t *bot )
{
  int members, defenders;

  if( level.time < bot->nextRoleCheck || bot->role == BROLE_BUILDER )
    return;

  bot->nextRoleCheck = level.time + 15000 + (int)( G_BotRandom( ) * 10000.0f );

  Bot_TeamRoles( bot, &members, &defenders );

  if( bot->role == BROLE_DEFEND && defenders + 1 > Bot_MaxDefenders( members + 1 ) )
    bot->role = ( G_BotRandom( ) < 0.5f + bot->pers.aggression * 0.3f ) ? BROLE_ATTACK : BROLE_ROAM;
  else if( bot->role != BROLE_DEFEND && defenders == 0 && members >= 2 )
    bot->role = BROLE_DEFEND;
  else if( bot->role != BROLE_DEFEND && G_BotRandom( ) < 0.3f )
    bot->role = ( bot->role == BROLE_ATTACK ) ? BROLE_ROAM : BROLE_ATTACK;
}

static void Bot_SpectatorThink( bot_t *bot )
{
  gentity_t   *ent = Bot_Ent( bot );
  team_t      team = ent->client->pers.teamSelection;
  qboolean    queued;
  const char  *what;

  // never press attack here: it toggles the spawn queue
  bot->buttons = 0;
  bot->enemy = -1;
  BotMove_Reset( bot );

  if( team == TEAM_NONE )
  {
    Bot_PickTeam( bot );
    return;
  }

  if( team == TEAM_ALIENS )
    queued = G_SearchSpawnQueue( &level.alienSpawnQueue, bot->clientNum );
  else
    queued = G_SearchSpawnQueue( &level.humanSpawnQueue, bot->clientNum );

  if( queued || level.time < bot->nextClassTry )
    return;

  bot->nextClassTry = level.time + 1500 + (int)( G_BotRandom( ) * 1000.0f );

  Bot_ChooseRole( bot );

  if( team == TEAM_ALIENS )
  {
    if( bot->role == BROLE_BUILDER )
    {
      if( BG_ClassIsAllowed( PCL_ALIEN_BUILDER0_UPG ) &&
          BG_ClassAllowedInStage( PCL_ALIEN_BUILDER0_UPG, g_alienStage.integer ) )
        what = BG_Class( PCL_ALIEN_BUILDER0_UPG )->name;
      else
        what = BG_Class( PCL_ALIEN_BUILDER0 )->name;
    }
    else
      what = BG_Class( PCL_ALIEN_LEVEL0 )->name;
  }
  else
    what = BotShop_SpawnItem( bot );

  G_BotCommand( bot, "class %s", what );
}

/*
===========================================================================

GOALS

===========================================================================
*/

static qboolean Bot_NeedsHealing( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  float     frac = Bot_HealthFrac( ent );

  if( bot->goal == BGOAL_HEAL )
    return frac < 0.92f;

  if( Bot_Team( bot ) == TEAM_HUMANS )
  {
    if( Bot_HasUpgrade( bot, UP_MEDKIT ) )
      return qfalse;
    return frac < 0.35f && BotTeam_CountBuildable( TEAM_HUMANS, BA_H_MEDISTAT, qtrue ) > 0;
  }

  // aliens heal anywhere slowly, much faster on creep / near boosters
  return frac < 0.3f + ( 1.0f - bot->pers.aggression ) * 0.15f;
}

static gentity_t *Bot_HealSource( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  gentity_t *src;

  if( Bot_Team( bot ) == TEAM_HUMANS )
    return BotTeam_NearestBuildable( TEAM_HUMANS, BA_H_MEDISTAT, ent->client->ps.origin, 0 );

  src = BotTeam_NearestBuildable( TEAM_ALIENS, BA_A_BOOSTER, ent->client->ps.origin, 0 );
  if( src )
    return src;
  src = BotTeam_NearestBuildable( TEAM_ALIENS, BA_A_OVERMIND, ent->client->ps.origin, 0 );
  if( src )
    return src;
  return BotTeam_NearestBuildable( TEAM_ALIENS, BA_A_SPAWN, ent->client->ps.origin, 0 );
}

static void Bot_SetGoal( bot_t *bot, botGoal_t goal, int entNum, const vec3_t pos, int timeout )
{
  if( bot->goal != goal || bot->goalEnt != entNum )
  {
    bot->goalSetTime = level.time;
    G_BotDebug( bot, "goal %d ent %d", goal, entNum );

    // heading out from our base to hit theirs: a battle cry on the way
    if( goal == BGOAL_ATTACKBASE && bot->goal != BGOAL_ATTACKBASE )
    {
      gentity_t *hq = BotTeam_HQ( Bot_Team( bot ) );

      if( !hq || Distance( hq->r.currentOrigin, Bot_Ent( bot )->r.currentOrigin ) < 1500.0f )
        G_BotEmote( bot, 0.6f, 300 + (int)( G_BotRandom( ) * 900.0f ) );
    }
  }

  bot->goal = goal;
  bot->goalEnt = entNum;
  if( pos )
    VectorCopy( pos, bot->goalPos );
  bot->goalTimeout = timeout ? level.time + timeout : 0;
}

// choose an enemy structure to push toward
static gentity_t *Bot_PickEnemyStructure( bot_t *bot )
{
  team_t    enemyTeam = Bot_Team( bot ) == TEAM_ALIENS ? TEAM_HUMANS : TEAM_ALIENS;
  gentity_t *ent = Bot_Ent( bot );
  gentity_t *e;
  int       *list, num, i, best = -1;
  float     d, score, bestScore = 999999.0f;

  num = BotTeam_Buildables( enemyTeam, &list );

  for( i = 0; i < num; i++ )
  {
    e = &g_entities[ list[ i ] ];
    if( !Bot_IsEnemyBuildable( bot, e ) )
      continue;

    d = Distance( ent->client->ps.origin, e->r.currentOrigin );
    score = d;

    switch( e->s.modelindex )
    {
      case BA_A_SPAWN:
      case BA_H_SPAWN:
        score -= 600.0f;
        break;
      case BA_A_OVERMIND:
      case BA_H_REACTOR:
        score -= 400.0f;
        break;
      default:
        break;
    }

    // spread the team out over different targets
    score += ( ( bot->clientNum * 7 + list[ i ] * 13 ) % 5 ) * 120.0f;

    if( score < bestScore )
    {
      bestScore = score;
      best = list[ i ];
    }
  }

  return best >= 0 ? &g_entities[ best ] : NULL;
}

qboolean BotAI_PathToEntity( bot_t *bot, gentity_t *target )
{
  vec3_t pos;

  Ent_Center( target, pos );
  if( !target->client )
    pos[ 2 ] = target->r.absmin[ 2 ] + 24.0f;

  return Bot_GoTo( bot, pos, qfalse );
}

static void Bot_Decide( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  team_t    team = Bot_Team( bot );
  gentity_t *target, *own;
  vec3_t    pos;
  qboolean  enemyVisible = bot->enemy >= 0 && level.time - bot->enemySeenTime < 600;
  qboolean  enemyIsPlayer = bot->enemy >= 0 && bot->enemy < MAX_CLIENTS;
  float     enemyDist = 99999.0f;

  if( level.time < bot->nextDecision )
    return;

  bot->nextDecision = level.time + 250;

  // an operator sent us somewhere (botgoto): go there and stay unless
  // attacked
  if( level.time < bot->forcedGoalUntil &&
      !( bot->enemy >= 0 && level.time - bot->lastHurtTime < 1500 ) )
  {
    if( bot->goal != BGOAL_ROAM )
      Bot_SetGoal( bot, BGOAL_ROAM, -1, bot->forcedGoal, 0 );

    if( !bot->forcedReached && Distance( ent->client->ps.origin, bot->forcedGoal ) < 48.0f )
    {
      bot->forcedReached = qtrue;
      G_Printf( "%s^7 reached %s\n", ent->client->pers.netname, vtos( bot->forcedGoal ) );
    }
    return;
  }

  if( bot->enemy >= 0 )
    enemyDist = Distance( ent->client->ps.origin, bot->enemyLastPos );

  // aliens evolve whenever it's safe to (alien sense always "knows" about
  // humans somewhere, so only a nearby one stops us)
  if( team == TEAM_ALIENS && bot_evolve.integer &&
      !( enemyVisible && enemyDist < 450.0f ) && enemyDist > 350.0f &&
      BotEvolve_Think( bot, qtrue ) )
    return;

  // fight what we can see (players first, then structures)
  if( bot->enemy >= 0 && ( enemyVisible || enemyDist < 500.0f ) )
  {
    // badly hurt aliens break off unless the kill is close
    if( Bot_NeedsHealing( bot ) && team == TEAM_ALIENS && enemyIsPlayer &&
        Bot_HealthFrac( ent ) < 0.25f && enemyDist > 150.0f && Bot_HealSource( bot ) )
    {
      target = Bot_HealSource( bot );
      Bot_SetGoal( bot, BGOAL_HEAL, target - g_entities, target->r.currentOrigin, 20000 );
      return;
    }

    Bot_SetGoal( bot, BGOAL_FIGHT, bot->enemy, bot->enemyLastPos, 0 );
    return;
  }

  if( Bot_NeedsHealing( bot ) && ( target = Bot_HealSource( bot ) ) )
  {
    Bot_SetGoal( bot, BGOAL_HEAL, target - g_entities, target->r.currentOrigin, 30000 );
    return;
  }

  if( team == TEAM_HUMANS && bot_buy.integer && BotShop_WantsToShop( bot ) )
  {
    target = BotTeam_NearestBuildable( TEAM_HUMANS, BA_H_ARMOURY, ent->client->ps.origin, 0 );
    if( target )
    {
      Bot_SetGoal( bot, BGOAL_ARMOURY, target - g_entities, target->r.currentOrigin, 25000 );
      return;
    }
  }

  // an enemy we know about but can't see: go get them
  if( bot->enemy >= 0 && enemyIsPlayer )
  {
    Bot_SetGoal( bot, BGOAL_HUNT, bot->enemy, bot->enemyLastPos, 0 );
    return;
  }

  // keep the current long-term goal until it expires
  if( ( bot->goal == BGOAL_ATTACKBASE || bot->goal == BGOAL_DEFEND || bot->goal == BGOAL_ROAM ) &&
      ( !bot->goalTimeout || level.time < bot->goalTimeout ) &&
      ( bot->goal != BGOAL_ATTACKBASE || Bot_ValidEnemy( bot, bot->goalEnt ) ) )
    return;

  Bot_BalanceRole( bot );

  switch( bot->role )
  {
    case BROLE_DEFEND:
    case BROLE_BUILDER:
      own = BotTeam_HQ( team );
      if( own )
      {
        int n = -1, tries;

        for( tries = 0; tries < 4; tries++ )
        {
          n = Bot_ReachableNodeNear( bot, own->r.currentOrigin, 150.0f, 700.0f );
          if( n < 0 || !Bot_OnSpawnExit( team, navNodes[ n ].origin, NULL ) )
            break;
          n = -1;
        }
        if( n >= 0 )
        {
          Bot_SetGoal( bot, BGOAL_DEFEND, -1, navNodes[ n ].origin, 8000 + (int)( G_BotRandom( ) * 8000 ) );
          return;
        }
      }
      // fall through - nothing to defend
    case BROLE_ATTACK:
      target = Bot_PickEnemyStructure( bot );
      if( target )
      {
        Bot_SetGoal( bot, BGOAL_ATTACKBASE, target - g_entities, target->r.currentOrigin, 60000 );
        return;
      }
      // fall through - no known structure
    case BROLE_ROAM:
    default:
    {
      int n = -1;

      // roam toward the enemy base area if we know it, else anywhere
      if( BotTeam_HaveBaseInfo( team == TEAM_ALIENS ? TEAM_HUMANS : TEAM_ALIENS, pos ) &&
          G_BotRandom( ) < 0.6f )
        n = Bot_ReachableNodeNear( bot, pos, 0.0f, 900.0f );
      if( n < 0 )
        n = Bot_ReachableNodeNear( bot, ent->client->ps.origin, 400.0f, 2000.0f );

      if( n >= 0 )
        Bot_SetGoal( bot, BGOAL_ROAM, -1, navNodes[ n ].origin, 15000 );
      else
        Bot_SetGoal( bot, BGOAL_ROAM, -1, ent->client->ps.origin, 3000 );
      return;
    }
  }
}

static void Bot_ExecuteGoal( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  gentity_t *target = NULL;
  vec3_t    seen;
  qboolean  visible;
  float     d;

  if( bot->goalEnt >= 0 && bot->goalEnt < level.num_entities )
    target = &g_entities[ bot->goalEnt ];

  switch( bot->goal )
  {
    case BGOAL_FIGHT:
    case BGOAL_HUNT:
      if( !target || !Bot_ValidEnemy( bot, bot->goalEnt ) )
      {
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
        Bot_LookAhead( bot );
        return;
      }

      visible = Bot_CanSee( bot, target, seen );
      if( !visible )
        VectorCopy( bot->enemyLastPos, seen );

      // arrived where we last saw them and they're gone: give up
      if( !visible && Distance( ent->client->ps.origin, bot->enemyLastPos ) < 64.0f &&
          level.time - bot->enemySeenTime > 2500 )
      {
        bot->enemy = -1;
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
        return;
      }

      if( Bot_Team( bot ) == TEAM_HUMANS )
        Bot_HumanCombat( bot, target, visible, seen );
      else
        Bot_AlienCombat( bot, target, visible, seen );
      return;

    case BGOAL_HEAL:
      if( !target || !target->inuse || target->health <= 0 )
      {
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
        return;
      }

      d = Distance( ent->client->ps.origin, target->r.currentOrigin );
      if( Bot_Team( bot ) == TEAM_HUMANS )
      {
        // stand right on the medistation
        if( d > 40.0f )
          Bot_GoTo( bot, target->r.currentOrigin, d > 300.0f );
        else
          BotMove_MoveTowardSafe( bot, target->r.currentOrigin );
      }
      else if( d > 140.0f )
        Bot_GoTo( bot, target->r.currentOrigin, qfalse );

      Bot_LookAhead( bot );

      if( Bot_HealthFrac( ent ) > 0.95f )
      {
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
      }
      return;

    case BGOAL_ARMOURY:
      if( !target || !target->inuse || target->health <= 0 || !target->spawned )
      {
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
        return;
      }

      if( G_BuildableRange( ent->client->ps.origin, 90, BA_H_ARMOURY ) )
      {
        BotShop_Shop( bot );
        bot->goal = BGOAL_NONE;
        bot->nextDecision = level.time + 500;
        return;
      }

      if( !Bot_GoTo( bot, target->r.currentOrigin, qtrue ) )
      {
        // can't get to that armoury: don't try again for a while
        bot->lastBuyTime = level.time;
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
      }
      Bot_LookAhead( bot );
      return;

    case BGOAL_ATTACKBASE:
      if( !target || !Bot_ValidEnemy( bot, bot->goalEnt ) ||
          !BotAI_PathToEntity( bot, target ) )
      {
        bot->goal = BGOAL_NONE;
        bot->goalTimeout = 0;
        bot->nextDecision = 0;
        if( target && bot->role == BROLE_ATTACK )
          bot->role = BROLE_ROAM;    // that structure is out of reach for us
        return;
      }
      Bot_LookAhead( bot );
      return;

    case BGOAL_DEFEND:
    case BGOAL_ROAM:
      d = Distance( ent->client->ps.origin, bot->goalPos );
      if( d > 48.0f )
      {
        if( !Bot_GoTo( bot, bot->goalPos, bot->goal == BGOAL_ROAM && d > 800.0f ) )
        {
          if( bot->forcedGoalUntil > level.time )
            G_Printf( "%s^7 can't find a way to %s\n", ent->client->pers.netname,
                      vtos( bot->goalPos ) );
          bot->forcedGoalUntil = 0;
          bot->goal = BGOAL_NONE;
          bot->nextDecision = 0;

          // nowhere we pick is reachable from here (off the graph, or
          // standing somewhere our class doesn't fit): wander a bit
          if( level.time - bot->lastGoalFail > 5000 )
            bot->goalFailCount = 0;
          bot->lastGoalFail = level.time;
          if( ++bot->goalFailCount >= 3 )
          {
            bot->goalFailCount = 0;
            bot->unstickYaw = G_BotRandom( ) * 360.0f;
            BotMove_FindSafeYaw( bot, &bot->unstickYaw );
            bot->unstickUntil = level.time + 1200;
            bot->nextDecision = level.time + 1200;
            BotMove_ClearPath( bot );
          }
        }
      }
      else if( bot->goal == BGOAL_ROAM && bot->forcedGoalUntil <= level.time )
      {
        bot->goal = BGOAL_NONE;
        bot->nextDecision = 0;
      }
      Bot_LookAhead( bot );
      return;

    default:
      BotMove_Unstick( bot );
      Bot_LookAhead( bot );
      return;
  }
}

/*
===========================================================================

ENTRY POINTS

===========================================================================
*/

void BotAI_ForceGoal( bot_t *bot, const vec3_t pos )
{
  if( !pos )
  {
    bot->forcedGoalUntil = 0;
    bot->goal = BGOAL_NONE;
    bot->nextDecision = 0;
    return;
  }

  VectorCopy( pos, bot->forcedGoal );
  bot->forcedReached = qfalse;
  Bot_SetGoal( bot, BGOAL_ROAM, -1, pos, 0 );
  bot->forcedGoalUntil = level.time + 300000;
  bot->nextDecision = 0;
  BotMove_ClearPath( bot );
  bot->pathFailTime = 0;
}

void BotAI_Begin( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );

  bot->enemy = -1;
  bot->goal = BGOAL_NONE;
  bot->goalEnt = -1;
  bot->lastAttacker = -1;
  bot->strafeDir = 1;
  bot->nextTeamTry = level.time + 500 + (int)( G_BotRandom( ) * 1500.0f );
  bot->nextClassTry = 0;
  bot->lastHealth = ent->health;
  bot->repairEnt = -1;
  VectorCopy( ent->client->ps.viewangles, bot->viewAngles );
  BotMove_Reset( bot );
}

// emotes: after a kill now and then, and when a queued one is due (not
// in the middle of a fight)
static void Bot_EmoteFrame( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  int       score = ent->client->ps.persistant[ PERS_SCORE ];
  qboolean  busy;

  if( score > bot->lastScore && bot->enemy < 0 )
    G_BotEmote( bot, 0.35f, 300 + (int)( G_BotRandom( ) * 500.0f ) );
  else if( score > bot->lastScore )
    G_BotEmote( bot, 0.25f, 800 );
  bot->lastScore = score;

  if( !bot->emoteAt || level.time < bot->emoteAt )
    return;

  busy = bot->enemy >= 0 && level.time - bot->enemySeenTime < 600 &&
         Distance( bot->enemyLastPos, ent->r.currentOrigin ) < 500.0f;

  if( busy )
  {
    if( level.time - bot->emoteAt > 3000 )
      bot->emoteAt = 0;   // never mind
    return;
  }

  bot->buttons |= BUTTON_GESTURE;
  bot->emoteAt = 0;
  G_BotDebug( bot, "emote" );
}

void BotAI_Spawned( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );

  bot->spawnedAt = level.time;
  bot->emoteAt = 0;
  bot->lastScore = ent->client->ps.persistant[ PERS_SCORE ];
  bot->enemy = -1;
  bot->goal = BGOAL_NONE;
  bot->goalEnt = -1;
  bot->chargeStart = 0;
  bot->lastHealth = ent->health;
  bot->nextDecision = level.time + 300;
  bot->buildType = BA_NONE;
  bot->buildTries = 0;
  bot->evolveFailures = 0;
  VectorCopy( ent->client->ps.viewangles, bot->viewAngles );
  BotMove_Reset( bot );
  BotMove_UpdateCaps( bot );
}

void BotAI_TeamFrame( void )
{
  BotTeam_Frame( );
}

void BotAI_ResetPlans( void )
{
  BotBuild_ResetPlans( );
}

// standing still where a teammate is about to spawn gets us killed
static void Bot_ClearSpawnExit( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );
  vec3_t    away;

  if( bot->wantMove )
    return;

  if( !Bot_OnSpawnExit( Bot_Team( bot ), ent->client->ps.origin, away ) )
    return;

  bot->moveYaw = vectoyaw( away );
  if( BotMove_FindSafeYaw( bot, &bot->moveYaw ) )
    bot->wantMove = qtrue;
}

void BotAI_Think( bot_t *bot )
{
  gentity_t     *ent = Bot_Ent( bot );
  playerState_t *ps = &ent->client->ps;
  static int    lastSpawnCount[ MAX_CLIENTS ];

  // waiting to spawn, or not on a team yet
  if( ent->client->sess.spectatorState != SPECTATOR_NOT )
  {
    Bot_SpectatorThink( bot );
    return;
  }

  if( !Bot_Alive( bot ) )
  {
    bot->buttons = 0;
    return;
  }

  // freshly (re)spawned or evolved
  if( lastSpawnCount[ bot->clientNum ] != ps->persistant[ PERS_SPAWN_COUNT ] ||
      bot->caps.team != Bot_Team( bot ) )
  {
    lastSpawnCount[ bot->clientNum ] = ps->persistant[ PERS_SPAWN_COUNT ];
    BotAI_Spawned( bot );
  }

  // the class can change (evolve, battlesuit): keep the movement caps right
  if( ( level.framenum & 7 ) == ( bot->clientNum & 7 ) )
    BotMove_UpdateCaps( bot );

  // just spawned: let go of the buttons so we're allowed to act
  if( ps->pm_flags & PMF_RESPAWNED )
  {
    bot->buttons = 0;
    bot->upmove = 0;
  }

  Bot_UpdateHurt( bot );
  Bot_FindEnemy( bot );

  if( bot->enemy >= 0 && !Bot_ValidEnemy( bot, bot->enemy ) )
  {
    bot->enemy = -1;
    if( bot->goal == BGOAL_FIGHT || bot->goal == BGOAL_HUNT )
    {
      bot->goal = BGOAL_NONE;
      bot->nextDecision = 0;
    }
  }

  Bot_ItemCommands( bot );

  // builders have their own routine (which hands back control for fights)
  if( bot->role == BROLE_BUILDER && BotBuild_Think( bot ) )
  {
    Bot_ClearSpawnExit( bot );
    BotMove_CheckStuck( bot );
    return;
  }

  Bot_Decide( bot );
  Bot_ExecuteGoal( bot );
  Bot_ClearSpawnExit( bot );
  BotMove_CheckStuck( bot );

  // sprint when travelling far with nobody around
  if( Bot_Team( bot ) == TEAM_HUMANS && bot->wantMove && bot->enemy < 0 &&
      ps->stats[ STAT_STAMINA ] > 500 && bot->goal != BGOAL_DEFEND )
    bot->buttons |= BUTTON_SPRINT;

  Bot_EmoteFrame( bot );

  if( level.time - bot->spawnedAt < 400 )
  {
    bot->buttons = 0;
    bot->upmove = 0;
  }

  if( bot->debug && level.time >= bot->nextDebugPrint )
  {
    bot->nextDebugPrint = level.time + ( bot_debug.integer >= 3 ? 250 : 2000 );
    BotAI_PrintState( bot );

    if( bot_debug.integer >= 3 && bot->enemy >= 0 )
    {
      gentity_t *e = &g_entities[ bot->enemy ];
      G_Printf( "   combat: enemy %d dist %.0f edge %.0f locked %d view %.0f %.0f buttons %d "
                "move %d yaw %.0f up %d vel %.0f ground %d wc %d\n",
                bot->enemy, Distance( ps->origin, e->r.currentOrigin ),
                Bot_EdgeDistance( ent, e ), bot->aimLocked, ps->viewangles[ PITCH ],
                ps->viewangles[ YAW ], bot->buttons, bot->wantMove, bot->moveYaw, bot->upmove,
                VectorLength( ps->velocity ), ps->groundEntityNum != ENTITYNUM_NONE,
                ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) ? 1 : 0 );
    }
  }
}

void BotAI_PrintState( bot_t *bot )
{
  gentity_t *ent = Bot_Ent( bot );

  G_Printf( "^5bot %d^7 %s^7: team %s class %s hp %d credits %d role %d goal %d "
            "goalEnt %d enemy %d path %d/%d mode %d node %d pos %s stuck %d last '%s' "
            "pers %.2f/%.2f/%d/%d items 0x%x/0x%x jet %d pm %d cmdup %d jetuntil %d\n",
            bot->clientNum, ent->client->pers.netname,
            BG_TeamName( ent->client->pers.teamSelection ),
            BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->name,
            ent->health, ent->client->pers.credit, bot->role, bot->goal, bot->goalEnt,
            bot->enemy, bot->pathPos, bot->pathLen, bot->moveMode, bot->curNode,
            vtos( ent->client->ps.origin ), bot->stuckCount, bot->lastAction,
            bot->pers.aggression, bot->pers.saver, bot->pers.weaponPref, bot->pers.alienPref,
            ent->client->ps.stats[ STAT_ITEMS ], ent->client->ps.stats[ STAT_ACTIVEITEMS ],
            bot->caps.jetpack, ent->client->ps.pm_type, bot->cmd.upmove,
            bot->jetpackUntil - level.time );
}
