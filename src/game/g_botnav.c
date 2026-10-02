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

// g_botnav.c -- navigation graph for bots
//
// The graph is generated straight from the level's collision model: we
// flood fill the map with the same box traces the player movement code
// uses, starting from spawn points, buildables and teleporter
// destinations. Every node is a point on walkable floor laid out on a
// regular grid (bot_navGridSize units apart); every edge records how a
// player gets from one node to the next:
//
//   walk / step      plain movement, including stairs and ramps
//   drop             walking off a ledge
//   climb            the way is blocked by a wall but there is floor on
//                    top of it: jumpable for short walls, wall-walkable
//                    for any height (dretch, basilisks, adv. granger),
//                    climbable with a ladder or a jetpack
//   teleport         trigger_teleport to its destination
//
// Each node and edge also stores which hull sizes fit, so tyrants are not
// sent through vents the dretches use.
//
// Generation is incremental (a few milliseconds per server frame) and the
// result is cached in bots/nav/<map>.nav so it only happens once per map.

#include "g_local.h"
#include "g_bot.h"

navNode_t       navNodes[ NAV_MAX_NODES ];
navEdge_t       navEdges[ NAV_MAX_EDGES ];
int             navNumNodes;
int             navNumEdges;
navState_t      navState;

static navExtraEdge_t navExtra[ NAV_MAX_EXTRA ];
static int      navNumExtra;
static int      navHash[ NAV_HASH_SIZE ];
static float    navGrid = 32.0f;
static int      navExpandNext;       // next node to expand during generation
static int      navGenStartTime;
static int      navGenMsec;
static qboolean navGenOverflow;
static char     navMapName[ MAX_QPATH ];
static int      navPendingGenerate;  // level.time to start generating at
static int      navLastSlice;        // real time the last generation slice ended
static int      navMapChecksum;

// per node hull placement offsets (bigger hulls may need to stand a bit
// away from walls); kept outside navNode_t to keep the hot struct small
// where each hull stands at each node: x/y nudges away from walls, and a
// raise for wide hulls on stairs (their box rests on the higher steps)
static signed char navHullOfs[ NAV_MAX_NODES ][ NAV_NUM_HULLS ][ 4 ];

#define NAV_MAGIC         0x56414e54  // "TNAV"
#define NAV_VERSION       10
#define NAV_EPS           1.0f
#define NAV_MAX_RAISE     40.0f     // wide hulls resting on higher steps
#define NAV_MASK          ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP )
#define NAV_Z_MATCH       12.0f
#define MIN_WALK_NORMAL   0.7f  // same as bg_local.h

// (the tyrant is 64 wide; its hull is a little narrower because the real
// movement code squeezes through openings the exact box test rejects)
static const float navHullHalf[ NAV_NUM_HULLS ]   = { 15.0f, 25.0f, 15.0f, 29.0f, 30.0f, 15.0f };
static const float navHullHeight[ NAV_NUM_HULLS ] = { 30.0f, 42.0f, 56.0f, 66.0f, 92.0f, 78.0f };

static const float navClimbLevels[ ] =
{
  24, 32, 40, 48, 56, 64, 72, 88, 104, 120, 144, 176, 208, 256, 320, 384, 448, 512
};
#define NAV_NUM_CLIMB_LEVELS ( sizeof( navClimbLevels ) / sizeof( navClimbLevels[ 0 ] ) )

static const int navDirs[ 8 ][ 2 ] =
{
  { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
  { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
};

// entities that block traces only while generating
#define NAV_MAX_UNLINKED 256
static int      navUnlinked[ NAV_MAX_UNLINKED ];
static int      navNumUnlinked;

// hurt volumes (marked as hazards)
#define NAV_MAX_HURT 128
static vec3_t   navHurtMins[ NAV_MAX_HURT ], navHurtMaxs[ NAV_MAX_HURT ];
static qboolean navHurtBelow[ NAV_MAX_HURT ];  // a kill trigger: whatever is under it is lost too
static int      navNumHurt;

/*
===========================================================================

BASIC HELPERS

===========================================================================
*/

void BotNav_HullBounds( navHull_t hull, vec3_t mins, vec3_t maxs )
{
  float h = navHullHalf[ hull ];

  VectorSet( mins, -h, -h, 0.0f );
  VectorSet( maxs, h, h, navHullHeight[ hull ] );
}

static void Nav_Trace( trace_t *tr, const vec3_t start, const vec3_t mins,
                       const vec3_t maxs, const vec3_t end )
{
  trap_Trace( tr, start, mins, maxs, end, ENTITYNUM_NONE, NAV_MASK );
}

static int Nav_HashCell( int cx, int cy )
{
  return ( ( cx * 73856093 ) ^ ( cy * 19349663 ) ) & ( NAV_HASH_SIZE - 1 );
}

static int Nav_CellCoord( float v )
{
  return (int)floor( v / navGrid + 0.5f );
}

static qboolean Nav_HullFits( navHull_t hull, const vec3_t floorPoint )
{
  vec3_t  mins, maxs, p;
  trace_t tr;

  BotNav_HullBounds( hull, mins, maxs );
  VectorSet( p, floorPoint[ 0 ], floorPoint[ 1 ], floorPoint[ 2 ] + NAV_EPS );
  Nav_Trace( &tr, p, mins, maxs, p );
  return !tr.startsolid && !tr.allsolid;
}

// drop a point to the floor with the given hull; returns qfalse if there
// is no walkable floor below
static qboolean Nav_DropToFloor( navHull_t hull, const vec3_t start, float maxDrop, vec3_t floorOut )
{
  vec3_t  mins, maxs, s, e;
  trace_t tr;

  BotNav_HullBounds( hull, mins, maxs );
  VectorCopy( start, s );
  VectorCopy( start, e );
  e[ 2 ] -= maxDrop;
  Nav_Trace( &tr, s, mins, maxs, e );

  if( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f )
    return qfalse;

  if( tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
    return qfalse;

  VectorCopy( tr.endpos, floorOut );
  floorOut[ 2 ] -= NAV_EPS;
  return qtrue;
}

static qboolean Nav_PointInHurt( const vec3_t p )
{
  int i;

  for( i = 0; i < navNumHurt; i++ )
  {
    if( p[ 0 ] >= navHurtMins[ i ][ 0 ] && p[ 0 ] <= navHurtMaxs[ i ][ 0 ] &&
        p[ 1 ] >= navHurtMins[ i ][ 1 ] && p[ 1 ] <= navHurtMaxs[ i ][ 1 ] &&
        ( p[ 2 ] + 8.0f >= navHurtMins[ i ][ 2 ] || navHurtBelow[ i ] ) &&
        p[ 2 ] <= navHurtMaxs[ i ][ 2 ] )
      return qtrue;
  }

  return qfalse;
}

/*
===========================================================================

MOVEMENT SIMULATION

These mirror what PM_StepSlideMove lets a player do.

===========================================================================
*/

typedef enum
{
  NAVMOVE_OK,
  NAVMOVE_DROP,
  NAVMOVE_BLOCKED,
  NAVMOVE_VOID
} navMoveResult_t;

// walk from a floor point toward a target xy; returns where we land
// is there floor right under the middle of a box standing at p (p is the
// bottom of the box)? A box can rest on the edge of a platform with most
// of it over the void; bots don't move precisely enough to walk there.
static qboolean Nav_CenterFloor( const vec3_t p, float maxDown )
{
  vec3_t  mins = { -4.0f, -4.0f, 0.0f }, maxs = { 4.0f, 4.0f, 4.0f };
  vec3_t  a, b;
  trace_t tr;

  VectorSet( a, p[ 0 ], p[ 1 ], p[ 2 ] + NAV_EPS );
  VectorSet( b, p[ 0 ], p[ 1 ], p[ 2 ] - maxDown );
  Nav_Trace( &tr, a, mins, maxs, b );
  if( tr.startsolid )
    return qtrue;   // the middle is inside the step we stand on

  return tr.fraction < 1.0f && tr.plane.normal[ 2 ] >= MIN_WALK_NORMAL;
}

static navMoveResult_t Nav_TestMove( navHull_t hull, const vec3_t from, float tx, float ty,
                                     float *landZ, int *blockSurf )
{
  vec3_t    mins, maxs, start, end, p, up, fwd;
  trace_t   tr;
  float     dx, dy, dist, f;
  int       steps, i;
  qboolean  dropped = qfalse;
  // wide bodies on stairs rest on the higher steps, so the step under
  // their middle can be further down
  float     stairSlack = ( navHullHalf[ hull ] > 20.0f ) ? NAV_MAX_RAISE : 0.0f;

  BotNav_HullBounds( hull, mins, maxs );
  VectorSet( start, from[ 0 ], from[ 1 ], from[ 2 ] + NAV_EPS );

  // fast path: step up, move, drop down
  VectorCopy( start, up );
  up[ 2 ] += NAV_STEPSIZE;
  Nav_Trace( &tr, start, mins, maxs, up );
  if( tr.startsolid )
    return NAVMOVE_BLOCKED;
  VectorCopy( tr.endpos, p );
  VectorSet( fwd, tx, ty, p[ 2 ] );
  Nav_Trace( &tr, p, mins, maxs, fwd );

  if( tr.fraction >= 1.0f )
  {
    VectorCopy( fwd, end );
    end[ 2 ] -= NAV_STEPSIZE + NAV_MAX_DROP;
    Nav_Trace( &tr, fwd, mins, maxs, end );

    if( tr.startsolid || tr.fraction >= 1.0f )
      return NAVMOVE_VOID;

    if( tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
    {
      // landed on something steep: fall back to the careful walk below
      // only if we didn't fall far (a ramp); otherwise it's a slide
      if( fwd[ 2 ] - tr.endpos[ 2 ] > NAV_STEPSIZE * 2.0f )
        return NAVMOVE_VOID;
    }
    else
    {
      *landZ = tr.endpos[ 2 ] - NAV_EPS;

      if( from[ 2 ] - *landZ > NAV_STEPSIZE + 2.0f )
        return NAVMOVE_DROP;

      // the straight move worked; check that it doesn't cut across the
      // corner of a platform, else take the careful path below
      {
        float   fdx = tx - from[ 0 ], fdy = ty - from[ 1 ];
        int     k, n = (int)( sqrt( fdx * fdx + fdy * fdy ) / 8.0f ) + 1;
        qboolean ok = qtrue;
        vec3_t  q;

        for( k = 1; k < n && ok; k++ )
        {
          float g = (float)k / (float)n;

          VectorSet( q, from[ 0 ] + fdx * g, from[ 1 ] + fdy * g,
                     from[ 2 ] + ( *landZ - from[ 2 ] ) * g + NAV_STEPSIZE );
          ok = Nav_CenterFloor( q, NAV_STEPSIZE * 2.5f + stairSlack );
        }

        if( ok )
          return NAVMOVE_OK;
      }
    }
  }

  // careful path: small steps so stairs and ramps work
  dx = tx - from[ 0 ];
  dy = ty - from[ 1 ];
  dist = sqrt( dx * dx + dy * dy );
  steps = (int)( dist / 10.0f ) + 1;
  if( steps < 2 )
    steps = 2;

  VectorCopy( start, p );

  for( i = 1; i <= steps; i++ )
  {
    f = (float)i / (float)steps;

    VectorCopy( p, up );
    up[ 2 ] += NAV_STEPSIZE;
    Nav_Trace( &tr, p, mins, maxs, up );
    VectorCopy( tr.endpos, up );

    VectorSet( fwd, from[ 0 ] + dx * f, from[ 1 ] + dy * f, up[ 2 ] );
    Nav_Trace( &tr, up, mins, maxs, fwd );

    if( tr.fraction < 1.0f )
    {
      if( blockSurf )
        *blockSurf = tr.surfaceFlags;
      return NAVMOVE_BLOCKED;
    }

    VectorCopy( fwd, end );
    end[ 2 ] -= NAV_STEPSIZE * 2.0f;
    Nav_Trace( &tr, fwd, mins, maxs, end );

    if( tr.fraction >= 1.0f )
    {
      // walked off a ledge
      VectorCopy( fwd, end );
      end[ 2 ] -= NAV_MAX_DROP;
      Nav_Trace( &tr, fwd, mins, maxs, end );

      if( tr.startsolid || tr.fraction >= 1.0f )
        return NAVMOVE_VOID;

      dropped = qtrue;
    }
    else if( !Nav_CenterFloor( tr.endpos, NAV_STEPSIZE * 1.5f + stairSlack ) )
    {
      // only the edge of the box is on something: too close to a drop
      // to walk reliably (a real ledge drop is found by the generator's
      // drop test instead)
      if( blockSurf )
        *blockSurf = 0;
      return NAVMOVE_BLOCKED;
    }

    if( tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
      return NAVMOVE_BLOCKED;

    VectorCopy( tr.endpos, p );
  }

  *landZ = p[ 2 ] - NAV_EPS;

  if( dropped || from[ 2 ] - *landZ > NAV_STEPSIZE + 2.0f )
    return NAVMOVE_DROP;

  return NAVMOVE_OK;
}

// the way is blocked: look for floor on top of (or beyond) the obstacle
static qboolean Nav_TestClimb( navHull_t hull, const vec3_t from, float tx, float ty,
                               float minClimb, float *landZ, float *climbH, int *surf )
{
  vec3_t  mins, maxs, start, end, p;
  trace_t tr;
  float   top, h;
  int     i;
  qboolean gotSurf = qfalse;

  BotNav_HullBounds( hull, mins, maxs );
  VectorSet( start, from[ 0 ], from[ 1 ], from[ 2 ] + NAV_EPS );
  VectorCopy( start, end );
  end[ 2 ] += NAV_MAX_CLIMB;
  Nav_Trace( &tr, start, mins, maxs, end );

  if( tr.startsolid )
    return qfalse;

  top = tr.endpos[ 2 ];

  for( i = 0; i < NAV_NUM_CLIMB_LEVELS; i++ )
  {
    h = navClimbLevels[ i ];

    if( h < minClimb )
      continue;

    if( start[ 2 ] + h > top )
      break;

    VectorSet( p, start[ 0 ], start[ 1 ], start[ 2 ] + h );
    VectorSet( end, tx, ty, p[ 2 ] );
    Nav_Trace( &tr, p, mins, maxs, end );

    if( tr.fraction < 1.0f )
    {
      if( !gotSurf && surf )
      {
        *surf = tr.surfaceFlags;
        gotSurf = qtrue;
      }
      continue;
    }

    // clear at this height: find the floor on the other side
    VectorCopy( end, p );
    end[ 2 ] -= h + NAV_MAX_DROP;
    Nav_Trace( &tr, p, mins, maxs, end );

    if( tr.startsolid || tr.fraction >= 1.0f )
      return qfalse;

    if( tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
      return qfalse;

    *landZ = tr.endpos[ 2 ] - NAV_EPS;
    *climbH = h;
    return qtrue;
  }

  return qfalse;
}

/*
===========================================================================

NODES AND EDGES

===========================================================================
*/

static int Nav_FindNode( int cx, int cy, float z, float tol )
{
  int   n = navHash[ Nav_HashCell( cx, cy ) ];
  int   best = -1;
  float bestDist = tol;
  float d;

  while( n >= 0 )
  {
    if( navNodes[ n ].cx == cx && navNodes[ n ].cy == cy )
    {
      d = fabs( navNodes[ n ].origin[ 2 ] - z );
      if( d <= bestDist )
      {
        bestDist = d;
        best = n;
      }
    }
    n = navNodes[ n ].hashNext;
  }

  return best;
}

static void Nav_HashInsert( int n )
{
  int h = Nav_HashCell( navNodes[ n ].cx, navNodes[ n ].cy );

  navNodes[ n ].hashNext = navHash[ h ];
  navHash[ h ] = n;
}

static void Nav_ComputeHullFits( int n )
{
  static const float ofs[ 8 ][ 2 ] =
  {
    { 12, 0 }, { -12, 0 }, { 0, 12 }, { 0, -12 },
    { 10, 10 }, { -10, 10 }, { -10, -10 }, { 10, -10 }
  };
  navNode_t *node = &navNodes[ n ];
  int       h, i;
  vec3_t    p;

  node->hulls = 0;

  for( h = 0; h < NAV_NUM_HULLS; h++ )
  {
    navHullOfs[ n ][ h ][ 0 ] = navHullOfs[ n ][ h ][ 1 ] = 0;
    navHullOfs[ n ][ h ][ 2 ] = navHullOfs[ n ][ h ][ 3 ] = 0;

    if( Nav_HullFits( h, node->origin ) )
    {
      node->hulls |= ( 1 << h );
      continue;
    }

    if( h == NAV_HULL_SMALL )
      continue;

    for( i = 0; i < 8; i++ )
    {
      VectorSet( p, node->origin[ 0 ] + ofs[ i ][ 0 ],
                 node->origin[ 1 ] + ofs[ i ][ 1 ], node->origin[ 2 ] );
      if( Nav_HullFits( h, p ) )
      {
        node->hulls |= ( 1 << h );
        navHullOfs[ n ][ h ][ 0 ] = (signed char)ofs[ i ][ 0 ];
        navHullOfs[ n ][ h ][ 1 ] = (signed char)ofs[ i ][ 1 ];
        break;
      }
    }
    if( node->hulls & ( 1 << h ) )
      continue;

    // on stairs a wide box overlaps the next steps up: let it rest on them
    {
      vec3_t  mins, maxs, top;
      trace_t tr;
      float   raise;

      BotNav_HullBounds( h, mins, maxs );
      VectorSet( top, node->origin[ 0 ], node->origin[ 1 ], node->origin[ 2 ] + NAV_MAX_RAISE );
      VectorSet( p, node->origin[ 0 ], node->origin[ 1 ], node->origin[ 2 ] + NAV_EPS );
      Nav_Trace( &tr, top, mins, maxs, p );

      if( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f ||
          tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
        continue;

      raise = tr.endpos[ 2 ] - node->origin[ 2 ] + 0.5f;
      if( raise < 1.0f || raise > NAV_MAX_RAISE )
        continue;

      node->hulls |= ( 1 << h );
      navHullOfs[ n ][ h ][ 2 ] = (signed char)ceil( raise );
    }
  }
}

static int Nav_AddNode( int cx, int cy, const vec3_t floorPoint, int flags )
{
  navNode_t *node;
  int       contents;
  vec3_t    p;
  int       n;

  if( navNumNodes >= NAV_MAX_NODES - 1 )
  {
    navGenOverflow = qtrue;
    return -1;
  }

  n = navNumNodes;
  node = &navNodes[ n ];
  memset( node, 0, sizeof( *node ) );
  VectorCopy( floorPoint, node->origin );
  node->cx = cx;
  node->cy = cy;
  node->firstEdge = 0;
  node->numEdges = 0;
  node->flags = flags;

  Nav_ComputeHullFits( n );

  if( !( node->hulls & ( 1 << NAV_HULL_SMALL ) ) )
    return -1;

  VectorSet( p, floorPoint[ 0 ], floorPoint[ 1 ], floorPoint[ 2 ] + 8.0f );
  contents = trap_PointContents( p, -1 );
  if( contents & ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP ) )
    node->flags |= NAVN_HAZARD;
  if( contents & CONTENTS_WATER )
    node->flags |= NAVN_WATER;
  if( Nav_PointInHurt( floorPoint ) )
    node->flags |= NAVN_HAZARD;

  navNumNodes++;
  Nav_HashInsert( n );
  return n;
}

// position inside a cell where the smallest hull fits
static void Nav_CellPoint( int cx, int cy, float z, vec3_t out )
{
  static const float jit[ 8 ][ 2 ] =
  {
    { 0.3f, 0 }, { -0.3f, 0 }, { 0, 0.3f }, { 0, -0.3f },
    { 0.3f, 0.3f }, { -0.3f, 0.3f }, { -0.3f, -0.3f }, { 0.3f, -0.3f }
  };
  vec3_t  p;
  int     i;

  VectorSet( out, cx * navGrid, cy * navGrid, z );
  VectorSet( p, out[ 0 ], out[ 1 ], z + NAV_STEPSIZE );

  if( Nav_HullFits( NAV_HULL_SMALL, p ) )
    return;

  for( i = 0; i < 8; i++ )
  {
    p[ 0 ] = out[ 0 ] + jit[ i ][ 0 ] * navGrid;
    p[ 1 ] = out[ 1 ] + jit[ i ][ 1 ] * navGrid;
    if( Nav_HullFits( NAV_HULL_SMALL, p ) )
    {
      out[ 0 ] = p[ 0 ];
      out[ 1 ] = p[ 1 ];
      return;
    }
  }
}

static void Nav_HullPoint( int n, navHull_t hull, vec3_t out )
{
  VectorCopy( navNodes[ n ].origin, out );
  out[ 0 ] += navHullOfs[ n ][ hull ][ 0 ];
  out[ 1 ] += navHullOfs[ n ][ hull ][ 1 ];
  out[ 2 ] += navHullOfs[ n ][ hull ][ 2 ];
}

// which bigger hulls can make the same move the small hull made
static int Nav_EdgeHulls( int from, int to, qboolean climb, float climbH )
{
  static const navHull_t order[ ] =
    { NAV_HULL_LARGE, NAV_HULL_DRAGOON, NAV_HULL_TALL, NAV_HULL_HUMAN, NAV_HULL_MEDIUM };
  int     hulls = ( 1 << NAV_HULL_SMALL );
  int     both = navNodes[ from ].hulls & navNodes[ to ].hulls;
  int     i, surf;
  navHull_t h;
  vec3_t  a, b;
  float   landZ, ch;
  navMoveResult_t r;
  qboolean ok;

  for( i = 0; i < sizeof( order ) / sizeof( order[ 0 ] ); i++ )
  {
    h = order[ i ];

    if( !( both & ( 1 << h ) ) )
      continue;

    // a hull that fits inside a hull that already passed passes too
    if( ( hulls & ( 1 << NAV_HULL_LARGE ) ) ||
        ( h == NAV_HULL_MEDIUM && ( hulls & ( 1 << NAV_HULL_DRAGOON ) ) ) ||
        ( h == NAV_HULL_HUMAN && ( hulls & ( ( 1 << NAV_HULL_DRAGOON ) | ( 1 << NAV_HULL_TALL ) ) ) ) )
    {
      hulls |= ( 1 << h );
      continue;
    }

    Nav_HullPoint( from, h, a );
    Nav_HullPoint( to, h, b );

    if( climb )
    {
      ok = Nav_TestClimb( h, a, b[ 0 ], b[ 1 ], climbH - 1.0f, &landZ, &ch, NULL ) &&
           fabs( landZ - b[ 2 ] ) <= NAV_STEPSIZE + 2.0f;
    }
    else
    {
      r = Nav_TestMove( h, a, b[ 0 ], b[ 1 ], &landZ, &surf );
      ok = ( r == NAVMOVE_OK || r == NAVMOVE_DROP ) &&
           fabs( landZ - b[ 2 ] ) <= NAV_STEPSIZE + 2.0f;
    }

    if( ok )
      hulls |= ( 1 << h );
  }

  return hulls;
}

static void Nav_AddEdge( int from, int to, int flags, int hulls, float climbH )
{
  navEdge_t *e;
  vec3_t    d;
  float     len;

  if( navNumEdges >= NAV_MAX_EDGES )
  {
    navGenOverflow = qtrue;
    return;
  }

  e = &navEdges[ navNumEdges++ ];
  e->target = (short)to;
  e->flags = (unsigned char)flags;
  e->hulls = (unsigned char)hulls;
  e->dz = (short)( navNodes[ to ].origin[ 2 ] - navNodes[ from ].origin[ 2 ] );
  e->climb = (short)climbH;

  VectorSubtract( navNodes[ to ].origin, navNodes[ from ].origin, d );
  len = VectorLength( d );
  if( flags & NAVE_CLIMB )
    len += climbH * 0.5f;
  if( len < 1.0f )
    len = 1.0f;
  if( len > 32000.0f )
    len = 32000.0f;
  e->cost = (short)len;
  e->pad = 0;
}

/*
===========================================================================

GENERATION

===========================================================================
*/

static void Nav_UnlinkMovers( void )
{
  int       i;
  gentity_t *ent;

  navNumUnlinked = 0;

  for( i = MAX_CLIENTS; i < level.num_entities && navNumUnlinked < NAV_MAX_UNLINKED; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse || !ent->r.linked || !ent->classname )
      continue;

    if( Q_stricmpn( ent->classname, "func_door", 9 ) &&
        Q_stricmp( ent->classname, "func_plat" ) )
      continue;

    trap_UnlinkEntity( ent );
    navUnlinked[ navNumUnlinked++ ] = i;
  }
}

static void Nav_RelinkMovers( void )
{
  int i;

  for( i = 0; i < navNumUnlinked; i++ )
    trap_LinkEntity( &g_entities[ navUnlinked[ i ] ] );

  navNumUnlinked = 0;
}

// does using this target name kill the activator?
static qboolean Nav_TargetsKill( const char *target, int depth )
{
  gentity_t *t = NULL;

  while( ( t = G_Find( t, FOFS( targetname ), target ) ) != NULL )
  {
    if( !t->classname )
      continue;
    if( !Q_stricmp( t->classname, "target_kill" ) )
      return qtrue;
    if( depth > 0 && t->target &&
        ( !Q_stricmp( t->classname, "target_relay" ) || !Q_stricmp( t->classname, "target_delay" ) ) &&
        Nav_TargetsKill( t->target, depth - 1 ) )
      return qtrue;
  }

  return qfalse;
}

static void Nav_CollectHurt( void )
{
  int       i;
  gentity_t *ent;

  navNumHurt = 0;

  for( i = MAX_CLIENTS; i < level.num_entities && navNumHurt < NAV_MAX_HURT; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse || !ent->classname )
      continue;

    if( Q_stricmp( ent->classname, "trigger_hurt" ) )
      continue;

    // only volumes that do real damage
    if( ent->damage > 0 && ent->damage < 20 )
      continue;

    VectorCopy( ent->r.absmin, navHurtMins[ navNumHurt ] );
    VectorCopy( ent->r.absmax, navHurtMaxs[ navNumHurt ] );
    navHurtBelow[ navNumHurt ] = qfalse;
    navNumHurt++;
  }

  // triggers that fire a target_kill (directly or through a relay / delay):
  // usually the bottom of a pit
  for( i = MAX_CLIENTS; i < level.num_entities && navNumHurt < NAV_MAX_HURT; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse || !ent->classname || !ent->target ||
        Q_stricmpn( ent->classname, "trigger_", 8 ) )
      continue;

    if( !Nav_TargetsKill( ent->target, 2 ) )
      continue;

    VectorCopy( ent->r.absmin, navHurtMins[ navNumHurt ] );
    VectorCopy( ent->r.absmax, navHurtMaxs[ navNumHurt ] );
    navHurtBelow[ navNumHurt ] = qtrue;
    navNumHurt++;
  }
}

static void Nav_AddSeed( const vec3_t origin )
{
  vec3_t  start, floorPoint, p;
  int     cx, cy, n, tries;

  VectorCopy( origin, start );
  start[ 2 ] += 8.0f;

  for( tries = 0; tries < 4; tries++ )
  {
    if( Nav_HullFits( NAV_HULL_SMALL, start ) )
      break;
    start[ 2 ] += 24.0f;
  }

  if( !Nav_DropToFloor( NAV_HULL_SMALL, start, 4096.0f, floorPoint ) )
    return;

  cx = Nav_CellCoord( floorPoint[ 0 ] );
  cy = Nav_CellCoord( floorPoint[ 1 ] );

  if( Nav_FindNode( cx, cy, floorPoint[ 2 ], NAV_Z_MATCH * 2.0f ) >= 0 )
    return;

  // snap to the cell point so neighbours line up
  Nav_CellPoint( cx, cy, floorPoint[ 2 ], p );
  p[ 2 ] = floorPoint[ 2 ] + NAV_STEPSIZE;
  if( !Nav_DropToFloor( NAV_HULL_SMALL, p, NAV_STEPSIZE * 3.0f, floorPoint ) )
    return;

  n = Nav_AddNode( cx, cy, floorPoint, NAVN_SEED );
  (void)n;
}

static void Nav_CollectSeeds( void )
{
  int       i;
  gentity_t *ent;
  static const char *seedClasses[ ] =
  {
    "info_player_deathmatch", "info_player_start", "info_player_intermission",
    "info_alien_intermission", "info_human_intermission", "misc_teleporter_dest",
    "target_position", "target_location", "info_notnull", NULL
  };
  int       c;

  for( i = 0; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse )
      continue;

    if( i < MAX_CLIENTS )
    {
      if( ent->client && ent->client->pers.connected == CON_CONNECTED &&
          ent->client->sess.spectatorState == SPECTATOR_NOT &&
          ent->health > 0 )
        Nav_AddSeed( ent->r.currentOrigin );
      continue;
    }

    if( ent->s.eType == ET_BUILDABLE )
    {
      vec3_t p;
      vec3_t forward;
      int    k;

      // seed around the structure, not inside it
      for( k = 0; k < 4; k++ )
      {
        float yaw = k * 90.0f;

        VectorSet( forward, cos( DEG2RAD( yaw ) ), sin( DEG2RAD( yaw ) ), 0.0f );
        VectorMA( ent->r.currentOrigin, ent->r.maxs[ 0 ] + 40.0f, forward, p );
        p[ 2 ] = ent->r.absmin[ 2 ] + 16.0f;
        Nav_AddSeed( p );
      }
      continue;
    }

    if( !ent->classname )
      continue;

    for( c = 0; seedClasses[ c ]; c++ )
    {
      if( !Q_stricmp( ent->classname, seedClasses[ c ] ) )
      {
        Nav_AddSeed( ent->s.origin );
        break;
      }
    }
  }
}

static void Nav_ExpandNode( int n )
{
  navNode_t *node = &navNodes[ n ];
  int       d, cx, cy, m, flags, hulls, surf;
  vec3_t    target, landPoint;
  float     landZ, climbH;
  navMoveResult_t r;

  node->firstEdge = navNumEdges;
  node->numEdges = 0;

  for( d = 0; d < 8; d++ )
  {
    cx = node->cx + navDirs[ d ][ 0 ];
    cy = node->cy + navDirs[ d ][ 1 ];

    Nav_CellPoint( cx, cy, node->origin[ 2 ], target );

    surf = 0;
    r = Nav_TestMove( NAV_HULL_SMALL, node->origin, target[ 0 ], target[ 1 ], &landZ, &surf );

    if( r == NAVMOVE_OK || r == NAVMOVE_DROP )
    {
      VectorSet( landPoint, target[ 0 ], target[ 1 ], landZ );
      m = Nav_FindNode( cx, cy, landZ, NAV_Z_MATCH );
      if( m < 0 )
        m = Nav_AddNode( cx, cy, landPoint, 0 );
      if( m < 0 || m == n )
        continue;

      flags = ( r == NAVMOVE_DROP ) ? NAVE_DROP : 0;
      hulls = Nav_EdgeHulls( n, m, qfalse, 0.0f );
      Nav_AddEdge( n, m, flags, hulls, 0.0f );
      if( navNodes[ m ].flags & NAVN_HAZARD )
        navNodes[ n ].flags |= NAVN_EDGE;
      continue;
    }

    if( r == NAVMOVE_VOID )
      navNodes[ n ].flags |= NAVN_EDGE;

    if( r != NAVMOVE_BLOCKED )
      continue;

    // diagonal moves around corners are covered by the cardinal moves
    if( d >= 4 )
      continue;

    if( !Nav_TestClimb( NAV_HULL_SMALL, node->origin, target[ 0 ], target[ 1 ], 0.0f,
                        &landZ, &climbH, &surf ) )
      continue;

    VectorSet( landPoint, target[ 0 ], target[ 1 ], landZ );
    m = Nav_FindNode( cx, cy, landZ, NAV_Z_MATCH );
    if( m < 0 )
      m = Nav_AddNode( cx, cy, landPoint, 0 );
    if( m < 0 || m == n )
      continue;

    flags = NAVE_CLIMB;
    if( surf & SURF_LADDER )
      flags |= NAVE_LADDER;
    if( landZ < node->origin[ 2 ] - NAV_STEPSIZE )
      flags |= NAVE_DROP;

    hulls = Nav_EdgeHulls( n, m, qtrue, climbH );
    Nav_AddEdge( n, m, flags, hulls, climbH );
  }

  node->numEdges = navNumEdges - node->firstEdge;
}

static void Nav_AddTeleporters( void )
{
  int       i, n, best;
  gentity_t *ent, *dest;
  vec3_t    floorPoint, start;
  float     d, bestDist;
  int       added;

  navNumExtra = 0;

  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];

    if( !ent->inuse || !ent->classname || Q_stricmp( ent->classname, "trigger_teleport" ) )
      continue;

    if( !ent->target )
      continue;

    dest = G_PickTarget( ent->target );
    if( !dest )
      continue;

    VectorCopy( dest->s.origin, start );
    start[ 2 ] += 8.0f;
    if( !Nav_DropToFloor( NAV_HULL_SMALL, start, 1024.0f, floorPoint ) )
      continue;

    best = -1;
    bestDist = 96.0f;
    for( n = 0; n < navNumNodes; n++ )
    {
      d = Distance( navNodes[ n ].origin, floorPoint );
      if( d < bestDist )
      {
        bestDist = d;
        best = n;
      }
    }

    if( best < 0 )
      continue;

    added = 0;
    for( n = 0; n < navNumNodes && navNumExtra < NAV_MAX_EXTRA && added < 6; n++ )
    {
      vec3_t p;

      VectorCopy( navNodes[ n ].origin, p );
      p[ 2 ] += 16.0f;

      if( p[ 0 ] < ent->r.absmin[ 0 ] - 8 || p[ 0 ] > ent->r.absmax[ 0 ] + 8 ||
          p[ 1 ] < ent->r.absmin[ 1 ] - 8 || p[ 1 ] > ent->r.absmax[ 1 ] + 8 ||
          p[ 2 ] < ent->r.absmin[ 2 ] - 24 || p[ 2 ] > ent->r.absmax[ 2 ] + 8 )
        continue;

      navExtra[ navNumExtra ].from = n;
      memset( &navExtra[ navNumExtra ].edge, 0, sizeof( navEdge_t ) );
      navExtra[ navNumExtra ].edge.target = (short)best;
      navExtra[ navNumExtra ].edge.flags = NAVE_TELEPORT;
      navExtra[ navNumExtra ].edge.hulls = 0xff;
      navExtra[ navNumExtra ].edge.dz =
        (short)( navNodes[ best ].origin[ 2 ] - navNodes[ n ].origin[ 2 ] );
      navExtra[ navNumExtra ].edge.cost = 64;
      navNodes[ n ].flags |= NAVN_EXTRA;
      navNumExtra++;
      added++;
    }
  }
}

static void Nav_Reset( void )
{
  int i;

  navNumNodes = 0;
  navNumEdges = 0;
  navNumExtra = 0;
  navExpandNext = 0;
  navGenOverflow = qfalse;

  for( i = 0; i < NAV_HASH_SIZE; i++ )
    navHash[ i ] = -1;
}

static void Nav_MapInfo( void )
{
  char buf[ MAX_CVAR_VALUE_STRING ];

  trap_Cvar_VariableStringBuffer( "mapname", navMapName, sizeof( navMapName ) );
  trap_Cvar_VariableStringBuffer( "sv_mapChecksum", buf, sizeof( buf ) );
  navMapChecksum = atoi( buf );
}

static void BotNav_StartGenerationGrid( float grid )
{
  navPendingGenerate = 0;

  Nav_MapInfo( );
  Nav_Reset( );

  navGrid = grid;
  if( navGrid < 16.0f )
    navGrid = 16.0f;
  else if( navGrid > 96.0f )
    navGrid = 96.0f;

  Nav_CollectHurt( );

  Nav_UnlinkMovers( );
  Nav_CollectSeeds( );
  Nav_RelinkMovers( );

  navState = NAVSTATE_BUILDING;
  navGenStartTime = trap_Milliseconds( );
  navGenMsec = 0;

  G_Printf( "bot: generating navigation for %s (grid %.0f, %d seed nodes)...\n",
            navMapName, navGrid, navNumNodes );

  if( navNumNodes == 0 )
  {
    G_Printf( "bot: ^3no seed points found, navigation unavailable\n" );
    navState = NAVSTATE_NONE;
  }
}

void BotNav_StartGeneration( qboolean force )
{
  (void)force;
  BotNav_StartGenerationGrid( bot_navGridSize.value );
}

static void BotNav_StartGenerationGrid( float grid );

static void Nav_FinishGeneration( void )
{
  int n, walkable = 0, climb = 0, hazard = 0;

  // too big for the node limit at this grid size: start again coarser
  if( navGenOverflow && navGrid < 64.0f )
  {
    float next = navGrid + 8.0f;

    G_Printf( "bot: %s needs more than %d nodes at grid %.0f, retrying at grid %.0f\n",
              navMapName, NAV_MAX_NODES, navGrid, next );
    BotNav_StartGenerationGrid( next );
    return;
  }

  Nav_AddTeleporters( );

  for( n = 0; n < navNumNodes; n++ )
  {
    if( navNodes[ n ].flags & NAVN_HAZARD )
      hazard++;
  }
  for( n = 0; n < navNumEdges; n++ )
  {
    if( navEdges[ n ].flags & NAVE_CLIMB )
      climb++;
    else
      walkable++;
  }

  navState = NAVSTATE_READY;

  G_Printf( "bot: navigation ready: %d nodes, %d edges (%d climb), %d teleport, "
            "%d hazard nodes, %d ms CPU, %.1f s real%s\n",
            navNumNodes, navNumEdges, climb, navNumExtra, hazard, navGenMsec,
            ( trap_Milliseconds( ) - navGenStartTime ) / 1000.0f,
            navGenOverflow ? " ^3(limit reached, raise bot_navGridSize)" : "" );

  BotNav_Save( );
  BotAI_ResetPlans( );
}

void BotNav_Frame( void )
{
  int start, budget;

  if( navPendingGenerate && level.time >= navPendingGenerate )
  {
    navPendingGenerate = 0;
    BotNav_StartGeneration( qfalse );
  }

  if( navState != NAVSTATE_BUILDING )
    return;

  // budget real time, not game frames: when the server falls behind it
  // runs several game frames back to back, and generating in each of them
  // would only make it fall further behind
  start = trap_Milliseconds( );
  if( start - navLastSlice < 50 && start >= navLastSlice )
    return;

  budget = bot_navGenBudget.integer;
  if( budget < 5 )
    budget = 5;

  Nav_UnlinkMovers( );

  while( navExpandNext < navNumNodes )
  {
    Nav_ExpandNode( navExpandNext );
    navExpandNext++;

    if( trap_Milliseconds( ) - start >= budget )
      break;
  }

  Nav_RelinkMovers( );

  navLastSlice = trap_Milliseconds( );
  navGenMsec += navLastSlice - start;

  if( navExpandNext >= navNumNodes )
    Nav_FinishGeneration( );
}

qboolean BotNav_Ready( void )
{
  return navState == NAVSTATE_READY;
}

/*
===========================================================================

SAVE / LOAD

===========================================================================
*/

typedef struct
{
  int magic;
  int version;
  int nodeSize;
  int edgeSize;
  int checksum;
  int grid;
  int requestedGrid;
  int numNodes;
  int numEdges;
  int numExtra;
} navFileHeader_t;

static const char *Nav_FileName( void )
{
  return va( "bots/nav/%s.nav", navMapName );
}

qboolean BotNav_Save( void )
{
  fileHandle_t    f;
  navFileHeader_t h;

  if( navState != NAVSTATE_READY || !navMapName[ 0 ] )
    return qfalse;

  if( trap_FS_FOpenFile( Nav_FileName( ), &f, FS_WRITE ) < 0 || !f )
  {
    G_Printf( "bot: couldn't write %s\n", Nav_FileName( ) );
    return qfalse;
  }

  h.magic = NAV_MAGIC;
  h.version = NAV_VERSION;
  h.nodeSize = sizeof( navNode_t );
  h.edgeSize = sizeof( navEdge_t );
  h.checksum = navMapChecksum;
  h.grid = (int)navGrid;
  h.requestedGrid = (int)bot_navGridSize.value;
  h.numNodes = navNumNodes;
  h.numEdges = navNumEdges;
  h.numExtra = navNumExtra;

  trap_FS_Write( &h, sizeof( h ), f );
  trap_FS_Write( navNodes, sizeof( navNode_t ) * navNumNodes, f );
  trap_FS_Write( navEdges, sizeof( navEdge_t ) * navNumEdges, f );
  if( navNumExtra )
    trap_FS_Write( navExtra, sizeof( navExtraEdge_t ) * navNumExtra, f );
  trap_FS_Write( navHullOfs, sizeof( navHullOfs[ 0 ] ) * navNumNodes, f );
  trap_FS_FCloseFile( f );

  G_Printf( "bot: saved navigation to %s\n", Nav_FileName( ) );
  return qtrue;
}

static qboolean Nav_Load( void )
{
  fileHandle_t    f;
  navFileHeader_t h;
  int             len, expect, n;

  len = trap_FS_FOpenFile( Nav_FileName( ), &f, FS_READ );
  if( len <= 0 || !f )
    return qfalse;

  if( len < sizeof( h ) )
  {
    trap_FS_FCloseFile( f );
    return qfalse;
  }

  trap_FS_Read( &h, sizeof( h ), f );

  if( h.magic != NAV_MAGIC || h.version != NAV_VERSION ||
      h.nodeSize != sizeof( navNode_t ) || h.edgeSize != sizeof( navEdge_t ) ||
      h.numNodes < 1 || h.numNodes > NAV_MAX_NODES ||
      h.numEdges < 0 || h.numEdges > NAV_MAX_EDGES ||
      h.numExtra < 0 || h.numExtra > NAV_MAX_EXTRA ||
      h.requestedGrid != (int)bot_navGridSize.value )
  {
    G_Printf( "bot: %s is out of date, regenerating\n", Nav_FileName( ) );
    trap_FS_FCloseFile( f );
    return qfalse;
  }

  if( h.checksum != navMapChecksum )
  {
    G_Printf( "bot: %s was made for a different version of the map, regenerating\n",
              Nav_FileName( ) );
    trap_FS_FCloseFile( f );
    return qfalse;
  }

  expect = sizeof( h ) + h.numNodes * sizeof( navNode_t ) +
           h.numEdges * sizeof( navEdge_t ) + h.numExtra * sizeof( navExtraEdge_t ) +
           h.numNodes * sizeof( navHullOfs[ 0 ] );
  if( len != expect )
  {
    G_Printf( "bot: %s is truncated, regenerating\n", Nav_FileName( ) );
    trap_FS_FCloseFile( f );
    return qfalse;
  }

  Nav_Reset( );
  navGrid = (float)h.grid;
  navNumNodes = h.numNodes;
  navNumEdges = h.numEdges;
  navNumExtra = h.numExtra;

  trap_FS_Read( navNodes, sizeof( navNode_t ) * navNumNodes, f );
  trap_FS_Read( navEdges, sizeof( navEdge_t ) * navNumEdges, f );
  if( navNumExtra )
    trap_FS_Read( navExtra, sizeof( navExtraEdge_t ) * navNumExtra, f );
  trap_FS_Read( navHullOfs, sizeof( navHullOfs[ 0 ] ) * navNumNodes, f );
  trap_FS_FCloseFile( f );

  for( n = 0; n < navNumNodes; n++ )
    Nav_HashInsert( n );

  navExpandNext = navNumNodes;
  navState = NAVSTATE_READY;
  Nav_CollectHurt( );
  G_Printf( "bot: loaded navigation from %s (%d nodes, %d edges)\n",
            Nav_FileName( ), navNumNodes, navNumEdges );
  return qtrue;
}

void BotNav_Init( void )
{
  navState = NAVSTATE_NONE;
  navPendingGenerate = 0;
  Nav_Reset( );
  Nav_MapInfo( );

  if( Nav_Load( ) )
    return;

  // map buildables only become real entities a few frames after the level
  // starts, and they make good seed points, so wait for them
  if( bot_navAutoGenerate.integer )
    navPendingGenerate = level.time + 1000;
}

void BotNav_Shutdown( void )
{
  BotNav_ClearShow( );

  if( navState == NAVSTATE_BUILDING )
    G_Printf( "bot: navigation generation interrupted by map change\n" );

  navState = NAVSTATE_NONE;
}

void BotNav_PrintInfo( void )
{
  int     n, e, hullCount[ NAV_NUM_HULLS ], flagCount[ 8 ], h, b;

  G_Printf( "navigation for %s: ", navMapName );
  switch( navState )
  {
    case NAVSTATE_NONE:
      G_Printf( "not available (use navgen)\n" );
      return;
    case NAVSTATE_BUILDING:
      G_Printf( "generating, %d of %d nodes expanded\n", navExpandNext, navNumNodes );
      return;
    default:
      break;
  }

  memset( hullCount, 0, sizeof( hullCount ) );
  memset( flagCount, 0, sizeof( flagCount ) );

  for( n = 0; n < navNumNodes; n++ )
    for( h = 0; h < NAV_NUM_HULLS; h++ )
      if( navNodes[ n ].hulls & ( 1 << h ) )
        hullCount[ h ]++;

  for( e = 0; e < navNumEdges; e++ )
    for( b = 0; b < 8; b++ )
      if( navEdges[ e ].flags & ( 1 << b ) )
        flagCount[ b ]++;

  G_Printf( "%d nodes, %d edges, grid %.0f\n", navNumNodes, navNumEdges, navGrid );
  G_Printf( "  nodes fitting hull: small %d, medium %d, human %d, dragoon %d, large %d, battlesuit %d\n",
            hullCount[ 0 ], hullCount[ 1 ], hullCount[ 2 ], hullCount[ 3 ], hullCount[ 4 ], hullCount[ 5 ] );
  G_Printf( "  edges: drop %d, climb %d, ladder %d, teleport %d\n",
            flagCount[ 0 ], flagCount[ 1 ], flagCount[ 2 ], navNumExtra );
}

/*
===========================================================================

QUERIES

===========================================================================
*/

void BotNav_CapsForClass( class_t pclass, team_t team, qboolean jetpack, navCaps_t *caps )
{
  const classAttributes_t *ca = BG_Class( pclass );
  float g = g_gravity.value > 1.0f ? g_gravity.value : 800.0f;
  float jump = ca->jumpMagnitude;

  memset( caps, 0, sizeof( *caps ) );
  caps->team = team;

  switch( pclass )
  {
    case PCL_ALIEN_LEVEL0:
      caps->hull = NAV_HULL_SMALL;
      break;
    case PCL_ALIEN_BUILDER0:
    case PCL_ALIEN_BUILDER0_UPG:
    case PCL_ALIEN_LEVEL1:
    case PCL_ALIEN_LEVEL1_UPG:
    case PCL_ALIEN_LEVEL2:
    case PCL_ALIEN_LEVEL2_UPG:
      caps->hull = NAV_HULL_MEDIUM;
      break;
    case PCL_ALIEN_LEVEL3:
    case PCL_ALIEN_LEVEL3_UPG:
      caps->hull = NAV_HULL_DRAGOON;
      break;
    case PCL_ALIEN_LEVEL4:
      caps->hull = NAV_HULL_LARGE;
      break;
    case PCL_HUMAN_BSUIT:
      caps->hull = NAV_HULL_TALL;
      break;
    default:
      caps->hull = NAV_HULL_HUMAN;
      break;
  }

  // apex of a standing jump plus the step-up the movement code gives
  // when landing on a ledge
  caps->maxClimb = ( jump * jump ) / ( 2.0f * g ) + NAV_STEPSIZE * 0.5f;

  if( BG_ClassHasAbility( pclass, SCA_WALLJUMPER ) )
    caps->maxClimb *= 1.4f;

  caps->wallClimber = BG_ClassHasAbility( pclass, SCA_WALLCLIMBER );
  caps->ladders = BG_ClassHasAbility( pclass, SCA_CANUSELADDERS ) || caps->wallClimber;
  caps->jetpack = jetpack;

  if( BG_ClassHasAbility( pclass, SCA_TAKESFALLDAMAGE ) && !jetpack )
    caps->maxDrop = 280.0f;
  else
    caps->maxDrop = NAV_MAX_DROP + 64.0f;
}

qboolean BotNav_EdgeUsable( const navEdge_t *e, const navCaps_t *caps )
{
  if( !( e->hulls & ( 1 << caps->hull ) ) )
    return qfalse;

  if( navNodes[ e->target ].flags & NAVN_HAZARD )
    return qfalse;

  if( e->flags & NAVE_CLIMB )
  {
    if( !caps->wallClimber && !caps->jetpack &&
        !( ( e->flags & NAVE_LADDER ) && caps->ladders ) &&
        e->climb > caps->maxClimb )
      return qfalse;
  }

  if( e->dz < -caps->maxDrop )
    return qfalse;

  return qtrue;
}

navEdge_t *BotNav_Edge( int edgeNum )
{
  if( edgeNum >= NAV_MAX_EDGES )
    return &navExtra[ edgeNum - NAV_MAX_EDGES ].edge;

  return &navEdges[ edgeNum ];
}

int BotNav_EdgeSource( int edgeNum )
{
  int lo, hi, mid;

  if( edgeNum >= NAV_MAX_EDGES )
    return navExtra[ edgeNum - NAV_MAX_EDGES ].from;

  // nodes were expanded in creation order, so firstEdge never decreases;
  // the owner of an edge is the last node whose firstEdge <= edgeNum
  // (nodes without edges share firstEdge with the node after them)
  lo = 0;
  hi = navNumNodes - 1;
  while( lo < hi )
  {
    mid = ( lo + hi + 1 ) / 2;
    if( navNodes[ mid ].firstEdge <= edgeNum )
      lo = mid;
    else
      hi = mid - 1;
  }

  return lo;
}

int BotNav_NearestNode( const vec3_t pos, const navCaps_t *caps, qboolean needVisible )
{
  int     cx, cy, x, y, n, best = -1, radius, maxRadius;
  float   d, dz, bestDist = 999999.0f;
  vec3_t  p, from;
  trace_t tr;
  navNode_t *node;

  if( navState != NAVSTATE_READY || navNumNodes <= 0 )
    return -1;

  cx = Nav_CellCoord( pos[ 0 ] );
  cy = Nav_CellCoord( pos[ 1 ] );
  maxRadius = (int)( 192.0f / navGrid ) + 1;

  for( radius = 2; radius <= maxRadius && best < 0; radius += 2 )
  {
    for( x = cx - radius; x <= cx + radius; x++ )
    {
      for( y = cy - radius; y <= cy + radius; y++ )
      {
        n = navHash[ Nav_HashCell( x, y ) ];
        while( n >= 0 )
        {
          node = &navNodes[ n ];
          if( node->cx == x && node->cy == y )
          {
            if( caps && !( node->hulls & ( 1 << caps->hull ) ) )
              goto next;
            if( node->flags & NAVN_HAZARD )
              goto next;

            dz = pos[ 2 ] - node->origin[ 2 ];
            // pos is usually the middle of a player box; prefer nodes
            // under us over ones above
            if( dz < -48.0f && !( caps && caps->wallClimber ) )
              goto next;
            if( dz > 160.0f && !( caps && caps->wallClimber ) )
              goto next;

            d = ( pos[ 0 ] - node->origin[ 0 ] ) * ( pos[ 0 ] - node->origin[ 0 ] ) +
                ( pos[ 1 ] - node->origin[ 1 ] ) * ( pos[ 1 ] - node->origin[ 1 ] );
            d = sqrt( d ) + fabs( dz - 24.0f ) * 1.5f;

            if( d < bestDist )
            {
              if( needVisible )
              {
                VectorCopy( pos, from );
                VectorCopy( node->origin, p );
                p[ 2 ] += 16.0f;
                trap_Trace( &tr, from, NULL, NULL, p, ENTITYNUM_NONE, MASK_SOLID );
                if( tr.fraction < 1.0f )
                  goto next;
              }
              bestDist = d;
              best = n;
            }
          }
next:
          n = node->hashNext;
        }
      }
    }
  }

  return best;
}

int BotNav_RandomNodeNear( const vec3_t pos, float minDist, float maxDist, const navCaps_t *caps )
{
  int   tries, cx, cy, n, r;
  float d;
  int   cells = (int)( maxDist / navGrid ) + 1;

  if( navState != NAVSTATE_READY )
    return -1;

  for( tries = 0; tries < 64; tries++ )
  {
    cx = Nav_CellCoord( pos[ 0 ] ) + (int)( G_BotCrandom( ) * cells );
    cy = Nav_CellCoord( pos[ 1 ] ) + (int)( G_BotCrandom( ) * cells );
    n = navHash[ Nav_HashCell( cx, cy ) ];
    r = 0;

    while( n >= 0 )
    {
      if( navNodes[ n ].cx == cx && navNodes[ n ].cy == cy &&
          !( navNodes[ n ].flags & ( NAVN_HAZARD | NAVN_EDGE ) ) &&
          ( !caps || ( navNodes[ n ].hulls & ( 1 << caps->hull ) ) ) )
      {
        d = Distance( navNodes[ n ].origin, pos );
        if( d >= minDist && d <= maxDist && fabs( navNodes[ n ].origin[ 2 ] - pos[ 2 ] ) < 256.0f )
          return n;
      }
      n = navNodes[ n ].hashNext;
      if( ++r > 8 )
        break;
    }
  }

  return -1;
}

/*
===========================================================================

A* / DIJKSTRA

===========================================================================
*/

static float    astarG[ NAV_MAX_NODES ];
static int      astarParent[ NAV_MAX_NODES ];   // edge number that reached the node
static int      astarStamp[ NAV_MAX_NODES ];
static int      astarClosed[ NAV_MAX_NODES ];
static int      astarHeap[ NAV_MAX_NODES ];
static float    astarHeapKey[ NAV_MAX_NODES ];
static int      astarHeapPos[ NAV_MAX_NODES ];
static int      astarHeapSize;
static int      astarSearch;

static void Heap_Swap( int a, int b )
{
  int   n = astarHeap[ a ];
  float k = astarHeapKey[ a ];

  astarHeap[ a ] = astarHeap[ b ];
  astarHeapKey[ a ] = astarHeapKey[ b ];
  astarHeap[ b ] = n;
  astarHeapKey[ b ] = k;
  astarHeapPos[ astarHeap[ a ] ] = a;
  astarHeapPos[ astarHeap[ b ] ] = b;
}

static void Heap_Up( int i )
{
  int parent;

  while( i > 0 )
  {
    parent = ( i - 1 ) / 2;
    if( astarHeapKey[ parent ] <= astarHeapKey[ i ] )
      break;
    Heap_Swap( i, parent );
    i = parent;
  }
}

static void Heap_Down( int i )
{
  int l, r, s;

  for( ;; )
  {
    l = i * 2 + 1;
    r = l + 1;
    s = i;
    if( l < astarHeapSize && astarHeapKey[ l ] < astarHeapKey[ s ] )
      s = l;
    if( r < astarHeapSize && astarHeapKey[ r ] < astarHeapKey[ s ] )
      s = r;
    if( s == i )
      break;
    Heap_Swap( i, s );
    i = s;
  }
}

static void Heap_Push( int node, float key )
{
  int i = astarHeapSize++;

  astarHeap[ i ] = node;
  astarHeapKey[ i ] = key;
  astarHeapPos[ node ] = i;
  Heap_Up( i );
}

static void Heap_Decrease( int node, float key )
{
  int i = astarHeapPos[ node ];

  astarHeapKey[ i ] = key;
  Heap_Up( i );
}

static int Heap_Pop( void )
{
  int top = astarHeap[ 0 ];

  astarHeapSize--;
  if( astarHeapSize > 0 )
  {
    astarHeap[ 0 ] = astarHeap[ astarHeapSize ];
    astarHeapKey[ 0 ] = astarHeapKey[ astarHeapSize ];
    astarHeapPos[ astarHeap[ 0 ] ] = 0;
    Heap_Down( 0 );
  }

  return top;
}

static void Search_Begin( void )
{
  astarSearch++;
  if( astarSearch <= 0 )
  {
    // wrapped: clear stamps
    memset( astarStamp, 0, sizeof( astarStamp ) );
    memset( astarClosed, 0, sizeof( astarClosed ) );
    astarSearch = 1;
  }
  astarHeapSize = 0;
}

static float Edge_Cost( const navEdge_t *e, const navCaps_t *caps )
{
  float cost = e->cost;

  if( e->flags & NAVE_CLIMB )
  {
    if( e->climb > caps->maxClimb && !caps->jetpack )
      cost += e->climb * 1.5f;   // slow wall climb / ladder
    else
      cost += 24.0f;             // jump
  }

  if( e->dz < -NAV_STEPSIZE && caps->maxDrop < NAV_MAX_DROP )
    cost += -e->dz * 0.75f;      // falls hurt humans

  if( navNodes[ e->target ].flags & NAVN_WATER )
    cost += 64.0f;

  // keep away from the edge of a deadly fall; dropping onto a ledge that
  // borders one is how bots overshoot into the void
  if( navNodes[ e->target ].flags & NAVN_EDGE )
    cost += ( e->flags & NAVE_DROP ) ? 1500.0f : 96.0f;

  return cost;
}

// relax every usable edge leaving node n; returns nothing
static void Search_Expand( int n, int goal, const navCaps_t *caps, int badEdge, qboolean astar )
{
  navNode_t *node = &navNodes[ n ];
  int       i, e, t, total;
  navEdge_t *edge;
  float     g, h;
  vec3_t    d;

  total = node->numEdges + ( ( node->flags & NAVN_EXTRA ) ? navNumExtra : 0 );

  for( i = 0; i < total; i++ )
  {
    if( i < node->numEdges )
    {
      e = node->firstEdge + i;
      edge = &navEdges[ e ];
    }
    else
    {
      int x = i - node->numEdges;
      if( navExtra[ x ].from != n )
        continue;
      e = NAV_MAX_EDGES + x;
      edge = &navExtra[ x ].edge;
    }

    if( !BotNav_EdgeUsable( edge, caps ) )
      continue;

    t = edge->target;
    if( astarClosed[ t ] == astarSearch )
      continue;

    g = astarG[ n ] + Edge_Cost( edge, caps );
    if( e == badEdge )
      g += 5000.0f;

    if( astarStamp[ t ] != astarSearch )
    {
      astarStamp[ t ] = astarSearch;
      astarG[ t ] = g;
      astarParent[ t ] = e;
      h = 0.0f;
      if( astar )
      {
        VectorSubtract( navNodes[ goal ].origin, navNodes[ t ].origin, d );
        h = VectorLength( d );
      }
      Heap_Push( t, g + h );
    }
    else if( g < astarG[ t ] )
    {
      float oldKey = astarHeapKey[ astarHeapPos[ t ] ];
      h = oldKey - astarG[ t ];
      astarG[ t ] = g;
      astarParent[ t ] = e;
      Heap_Decrease( t, g + h );
    }
  }
}

static int Search_BuildPath( int start, int end, int *pathEdges, int maxLen, int *pathLen )
{
  static int  rev[ NAV_MAX_NODES ];
  int         len = 0, n = end, i, keep;

  while( n != start && len < NAV_MAX_NODES )
  {
    if( astarParent[ n ] < 0 )
      break;
    rev[ len++ ] = astarParent[ n ];
    n = BotNav_EdgeSource( astarParent[ n ] );
  }

  // long paths keep their first part; the bot re-paths when it gets there
  keep = len < maxLen ? len : maxLen;
  for( i = 0; i < keep; i++ )
    pathEdges[ i ] = rev[ len - 1 - i ];

  *pathLen = keep;
  return keep > 0;
}

int BotNav_FindPath( int start, int goal, const navCaps_t *caps,
                     int *pathEdges, int maxLen, int *pathLen )
{
  int n, expanded = 0;
  vec3_t d;

  *pathLen = 0;

  if( navState != NAVSTATE_READY || start < 0 || goal < 0 ||
      start >= navNumNodes || goal >= navNumNodes )
    return 0;

  if( start == goal )
    return 1;

  Search_Begin( );
  astarStamp[ start ] = astarSearch;
  astarG[ start ] = 0.0f;
  astarParent[ start ] = -1;
  VectorSubtract( navNodes[ goal ].origin, navNodes[ start ].origin, d );
  Heap_Push( start, VectorLength( d ) );

  while( astarHeapSize > 0 )
  {
    n = Heap_Pop( );

    if( n == goal )
      return Search_BuildPath( start, goal, pathEdges, maxLen, pathLen );

    astarClosed[ n ] = astarSearch;

    // unreachable goals would otherwise explore everything we can reach
    if( ++expanded > 12000 )
      break;

    Search_Expand( n, goal, caps, -1, qtrue );
  }

  return 0;
}

int BotNav_FindPathToNearest( int start, const navCaps_t *caps,
                              qboolean ( *isGoal )( int node, void *data ), void *data,
                              int maxDist, int *pathEdges, int maxLen, int *pathLen )
{
  int n;

  *pathLen = 0;

  if( navState != NAVSTATE_READY || start < 0 || start >= navNumNodes )
    return -1;

  Search_Begin( );
  astarStamp[ start ] = astarSearch;
  astarG[ start ] = 0.0f;
  astarParent[ start ] = -1;
  Heap_Push( start, 0.0f );

  while( astarHeapSize > 0 )
  {
    n = Heap_Pop( );

    if( astarG[ n ] > maxDist )
      break;

    if( isGoal( n, data ) )
    {
      if( n != start )
        Search_BuildPath( start, n, pathEdges, maxLen, pathLen );
      return n;
    }

    astarClosed[ n ] = astarSearch;
    Search_Expand( n, -1, caps, -1, qfalse );
  }

  return -1;
}

int BotNav_RandomReachable( int start, const navCaps_t *caps, float minDist, float maxDist,
                            const vec3_t near, float nearRadius )
{
  int   n, seen = 0, pick = -1;

  if( navState != NAVSTATE_READY || start < 0 || start >= navNumNodes )
    return -1;

  Search_Begin( );
  astarStamp[ start ] = astarSearch;
  astarG[ start ] = 0.0f;
  astarParent[ start ] = -1;
  Heap_Push( start, 0.0f );

  while( astarHeapSize > 0 )
  {
    n = Heap_Pop( );

    if( astarG[ n ] > maxDist )
      break;

    astarClosed[ n ] = astarSearch;

    // reservoir sample a node in the distance band
    if( astarG[ n ] >= minDist && !( navNodes[ n ].flags & ( NAVN_HAZARD | NAVN_EDGE ) ) &&
        ( navNodes[ n ].hulls & ( 1 << caps->hull ) ) &&
        ( !near || Distance( navNodes[ n ].origin, near ) <= nearRadius ) )
    {
      seen++;
      if( G_BotRandInt( seen ) == 0 )
        pick = n;
    }

    Search_Expand( n, -1, caps, -1, qfalse );
  }

  return pick;
}

void BotNav_Flood( int start, const navCaps_t *caps, float maxDist, float *distOut )
{
  int n;

  for( n = 0; n < navNumNodes; n++ )
    distOut[ n ] = -1.0f;

  if( navState != NAVSTATE_READY || start < 0 || start >= navNumNodes )
    return;

  Search_Begin( );
  astarStamp[ start ] = astarSearch;
  astarG[ start ] = 0.0f;
  astarParent[ start ] = -1;
  Heap_Push( start, 0.0f );

  while( astarHeapSize > 0 )
  {
    n = Heap_Pop( );

    if( astarG[ n ] > maxDist )
      break;

    distOut[ n ] = astarG[ n ];
    astarClosed[ n ] = astarSearch;
    Search_Expand( n, -1, caps, -1, qfalse );
  }
}

/*
===========================================================================

NAVIGATION DEBUG DISPLAY

Draws the graph around a player with beam entities (navshow command).

===========================================================================
*/

#define NAV_MAX_SHOW 200
static int navShowEnts[ NAV_MAX_SHOW ];
static int navNumShow;

static void Nav_ShowThink( gentity_t *self )
{
  G_FreeEntity( self );
}

void BotNav_ClearShow( void )
{
  int i;

  for( i = 0; i < navNumShow; i++ )
  {
    gentity_t *ent = &g_entities[ navShowEnts[ i ] ];
    if( ent->inuse && ent->classname && !strcmp( ent->classname, "navshow" ) )
      G_FreeEntity( ent );
  }
  navNumShow = 0;
}

void BotNav_ShowFor( gentity_t *viewer, float radius, int seconds )
{
  int       n, i;
  gentity_t *beam;
  navNode_t *node;
  navEdge_t *e;
  vec3_t    org;

  BotNav_ClearShow( );

  if( !viewer || !viewer->client || navState != NAVSTATE_READY )
    return;

  VectorCopy( viewer->client->ps.origin, org );

  for( n = 0; n < navNumNodes && navNumShow < NAV_MAX_SHOW; n++ )
  {
    node = &navNodes[ n ];

    if( Distance( node->origin, org ) > radius )
      continue;

    for( i = 0; i < node->numEdges && navNumShow < NAV_MAX_SHOW; i++ )
    {
      e = &navEdges[ node->firstEdge + i ];

      // draw each walk edge once, but every special edge
      if( !( e->flags & ( NAVE_CLIMB | NAVE_DROP ) ) && e->target < n )
        continue;

      beam = G_Spawn( );
      beam->classname = "navshow";
      beam->s.eType = ET_BEAM;
      VectorCopy( node->origin, beam->s.pos.trBase );
      beam->s.pos.trBase[ 2 ] += ( e->flags & NAVE_CLIMB ) ? 12.0f : 4.0f;
      VectorCopy( navNodes[ e->target ].origin, beam->s.origin2 );
      beam->s.origin2[ 2 ] += ( e->flags & NAVE_CLIMB ) ? 12.0f : 4.0f;
      G_SetOrigin( beam, beam->s.pos.trBase );
      beam->r.svFlags = SVF_BROADCAST;
      beam->think = Nav_ShowThink;
      beam->nextthink = level.time + seconds * 1000;
      trap_LinkEntity( beam );
      navShowEnts[ navNumShow++ ] = beam - g_entities;
    }
  }

  G_Printf( "navshow: drawing %d edges within %.0f units for %d seconds\n",
            navNumShow, radius, seconds );
}

/*
===========================================================================

PATH FOLLOWING

===========================================================================
*/

void BotMove_UpdateCaps( bot_t *bot )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  qboolean  jet;

  jet = BG_InventoryContainsUpgrade( UP_JETPACK, ent->client->ps.stats );
  BotNav_CapsForClass( ent->client->ps.stats[ STAT_CLASS ],
                       ent->client->ps.stats[ STAT_TEAM ], jet, &bot->caps );
}

void BotMove_ClearPath( bot_t *bot )
{
  bot->pathLen = 0;
  bot->pathPos = 0;
  bot->pathGoalNode = -1;
  bot->moveMode = BMOVE_NONE;
}

void BotMove_Reset( bot_t *bot )
{
  BotMove_ClearPath( bot );
  bot->curNode = -1;
  bot->stuckCount = 0;
  bot->unstickUntil = 0;
  bot->badEdge = -1;
  bot->pathFailTime = 0;
  VectorClear( bot->stuckPos );
  bot->stuckCheckTime = level.time + 1000;
}

// node position adjusted for the bot's hull
static void Nav_BotNodePoint( bot_t *bot, int n, vec3_t out )
{
  Nav_HullPoint( n, bot->caps.hull, out );
}

qboolean BotMove_SetGoal( bot_t *bot, const vec3_t pos, int goalNode )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  int       start, len;

  VectorCopy( pos, bot->goalPos );

  if( navState != NAVSTATE_READY )
  {
    bot->moveMode = BMOVE_DIRECT;
    return qtrue;
  }

  if( goalNode < 0 )
    goalNode = BotNav_NearestNode( pos, &bot->caps, qfalse );

  if( goalNode < 0 )
  {
    bot->moveMode = BMOVE_DIRECT;
    return qfalse;
  }

  // keep following the current path if it still leads there
  if( bot->moveMode == BMOVE_PATH && bot->pathGoalNode == goalNode &&
      bot->pathPos < bot->pathLen && level.time - bot->pathTime < 6000 )
    return qtrue;

  if( bot->pathGoalNode == goalNode && level.time < bot->pathFailTime )
    return qfalse;

  start = BotNav_NearestNode( ent->client->ps.origin, &bot->caps, qtrue );
  if( start < 0 )
    start = BotNav_NearestNode( ent->client->ps.origin, &bot->caps, qfalse );

  if( start < 0 )
  {
    bot->moveMode = BMOVE_DIRECT;
    return qfalse;
  }

  bot->curNode = start;

  if( start == goalNode )
  {
    bot->pathLen = 0;
    bot->pathPos = 0;
    bot->pathGoalNode = goalNode;
    bot->moveMode = BMOVE_DIRECT;
    bot->pathTime = level.time;
    return qtrue;
  }

  if( !BotNav_FindPath( start, goalNode, &bot->caps, bot->path, NAV_MAX_PATH, &len ) )
  {
    bot->pathGoalNode = goalNode;
    bot->pathFailTime = level.time + 3000;
    bot->pathLen = 0;
    bot->moveMode = BMOVE_DIRECT;
    G_BotDebug( bot, "no path from node %d to %d", start, goalNode );
    return qfalse;
  }

  bot->pathLen = len;
  bot->pathPos = 0;
  bot->pathStart = start;
  bot->pathGoalNode = goalNode;
  bot->pathTime = level.time;
  bot->moveMode = BMOVE_PATH;
  return qtrue;
}

static int Path_NodeAt( bot_t *bot, int pos )
{
  return BotNav_Edge( bot->path[ pos ] )->target;
}

// can the bot walk straight to this point without hitting anything or
// falling into a hole?
static qboolean Nav_CanWalkDirect( bot_t *bot, const vec3_t floorPoint )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  vec3_t    start, end, mid, down;
  trace_t   tr;
  int       i;

  VectorCopy( ent->client->ps.origin, start );
  VectorCopy( floorPoint, end );
  end[ 2 ] += -ent->r.mins[ 2 ] + NAV_STEPSIZE * 0.5f;

  if( fabs( end[ 2 ] - start[ 2 ] ) > NAV_STEPSIZE * 1.5f )
    return qfalse;

  trap_Trace( &tr, start, ent->r.mins, ent->r.maxs, end, ent->s.number, MASK_PLAYERSOLID & ~CONTENTS_BODY );
  if( tr.fraction < 1.0f )
    return qfalse;

  // make sure there is walkable floor along the way, close under our feet:
  // a shortcut across the corner of a platform must not cut over the void
  {
    vec3_t  fmins, fmaxs, side;
    float   len = Distance( start, end );
    int     steps = (int)( len / 16.0f ) + 2;
    int     o;

    VectorSet( fmins, -4.0f, -4.0f, 0.0f );
    VectorSet( fmaxs, 4.0f, 4.0f, 4.0f );

    // floor on a strip either side of the line too, so drifting a little
    // while walking it doesn't take us over an edge
    VectorSet( side, -( end[ 1 ] - start[ 1 ] ), end[ 0 ] - start[ 0 ], 0.0f );
    VectorNormalize( side );
    VectorScale( side, 20.0f, side );

    for( i = 1; i < steps; i++ )
    {
      float f = (float)i / (float)steps;

      for( o = -1; o <= 1; o++ )
      {
        mid[ 0 ] = start[ 0 ] + ( end[ 0 ] - start[ 0 ] ) * f + side[ 0 ] * o;
        mid[ 1 ] = start[ 1 ] + ( end[ 1 ] - start[ 1 ] ) * f + side[ 1 ] * o;
        mid[ 2 ] = start[ 2 ] + ( end[ 2 ] - start[ 2 ] ) * f;
        VectorCopy( mid, down );
        down[ 2 ] += ent->r.mins[ 2 ] - NAV_STEPSIZE * 1.5f;
        trap_Trace( &tr, mid, fmins, fmaxs, down, ent->s.number, MASK_PLAYERSOLID & ~CONTENTS_BODY );
        if( tr.startsolid && o != 0 )
          continue;   // a wall beside the line is fine
        if( tr.fraction >= 1.0f || tr.plane.normal[ 2 ] < MIN_WALK_NORMAL )
          return qfalse;
        if( Nav_PointInHurt( tr.endpos ) )
          return qfalse;
      }
    }
  }

  return qtrue;
}

static void Path_Smooth( bot_t *bot )
{
  int       k, j, furthest;
  vec3_t    p;
  navEdge_t *e;

  if( level.time < bot->nextSmooth )
    return;

  bot->nextSmooth = level.time + 300;

  furthest = bot->pathPos + 5;
  if( furthest > bot->pathLen - 1 )
    furthest = bot->pathLen - 1;

  for( k = furthest; k > bot->pathPos; k-- )
  {
    // only skip over plain walking, and never along the edge of a deadly
    // fall: there we follow the nodes exactly
    for( j = bot->pathPos; j <= k; j++ )
    {
      e = BotNav_Edge( bot->path[ j ] );
      if( e->flags & ( NAVE_CLIMB | NAVE_TELEPORT | NAVE_DROP ) )
        break;
      if( navNodes[ e->target ].flags & NAVN_EDGE )
        break;
    }
    if( j <= k )
      continue;

    Nav_BotNodePoint( bot, Path_NodeAt( bot, k ), p );
    if( Nav_CanWalkDirect( bot, p ) )
    {
      bot->pathPos = k;
      return;
    }
  }
}

void BotMove_MoveToward( bot_t *bot, const vec3_t pos )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  vec3_t    dir;

  VectorSubtract( pos, ent->client->ps.origin, dir );
  dir[ 2 ] = 0.0f;

  if( VectorLength( dir ) < 1.0f )
    return;

  bot->moveYaw = vectoyaw( dir );
  bot->wantMove = qtrue;
}

// straight at a point, off the graph: don't walk over an edge on the way
void BotMove_MoveTowardSafe( bot_t *bot, const vec3_t pos )
{
  playerState_t *ps = &g_entities[ bot->clientNum ].client->ps;

  BotMove_MoveToward( bot, pos );

  if( !bot->wantMove || ps->groundEntityNum == ENTITYNUM_NONE ||
      ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
    return;

  if( !BotMove_SafeDirection( bot, bot->moveYaw ) )
    bot->wantMove = qfalse;
}

// returns qfalse when there's nothing left to follow
qboolean BotMove_FollowPath( bot_t *bot )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  navEdge_t *e;
  vec3_t    tpos, feet, dir;
  float     dxy = 0.0f, dz = 0.0f, reach;
  int       node, guard = 0;
  qboolean  onGround = ( ps->groundEntityNum != ENTITYNUM_NONE );

  if( bot->moveMode == BMOVE_DIRECT || navState != NAVSTATE_READY )
  {
    BotMove_MoveTowardSafe( bot, bot->goalPos );
    return Distance( ps->origin, bot->goalPos ) > 32.0f;
  }

  if( bot->moveMode != BMOVE_PATH )
    return qfalse;

  VectorCopy( ps->origin, feet );
  feet[ 2 ] += ent->r.mins[ 2 ];

  reach = navHullHalf[ bot->caps.hull ] + 8.0f;
  if( reach < 24.0f )
    reach = 24.0f;

  // advance over waypoints we have reached
  while( bot->pathPos < bot->pathLen && guard++ < 8 )
  {
    node = Path_NodeAt( bot, bot->pathPos );
    Nav_BotNodePoint( bot, node, tpos );

    dxy = sqrt( ( tpos[ 0 ] - feet[ 0 ] ) * ( tpos[ 0 ] - feet[ 0 ] ) +
                ( tpos[ 1 ] - feet[ 1 ] ) * ( tpos[ 1 ] - feet[ 1 ] ) );
    dz = feet[ 2 ] - tpos[ 2 ];

    e = BotNav_Edge( bot->path[ bot->pathPos ] );

    if( dxy < reach && dz > -( NAV_STEPSIZE + 6.0f ) && dz < 56.0f )
    {
      bot->curNode = node;
      bot->pathPos++;
      continue;
    }

    // teleported or fell further along the path
    if( ( e->flags & NAVE_TELEPORT ) && dxy < 96.0f && fabs( dz ) < 64.0f )
    {
      bot->curNode = node;
      bot->pathPos++;
      continue;
    }
    break;
  }

  if( bot->pathPos >= bot->pathLen )
  {
    // final approach to the exact goal position
    bot->moveMode = BMOVE_DIRECT;
    BotMove_MoveTowardSafe( bot, bot->goalPos );
    return Distance( ps->origin, bot->goalPos ) > 32.0f;
  }

  // way off the path (knocked back, fell): find a new one. Measure from
  // the edge we are on, so tall climbs don't count as being lost.
  {
    vec3_t  spos;
    int     src = ( bot->pathPos == 0 ) ? bot->pathStart : Path_NodeAt( bot, bot->pathPos - 1 );
    float   sxy, sdz;

    Nav_BotNodePoint( bot, src, spos );
    sxy = sqrt( ( spos[ 0 ] - feet[ 0 ] ) * ( spos[ 0 ] - feet[ 0 ] ) +
                ( spos[ 1 ] - feet[ 1 ] ) * ( spos[ 1 ] - feet[ 1 ] ) );
    sdz = feet[ 2 ] - spos[ 2 ];

    if( ( dxy > 400.0f && sxy > 400.0f ) ||
        ( sdz < -160.0f && dz < -160.0f ) ||
        ( dz > 300.0f && sdz > 300.0f && !bot->caps.wallClimber ) )
    {
      BotMove_ClearPath( bot );
      bot->pathTime = 0;
      BotMove_SetGoal( bot, bot->goalPos, -1 );
      return qtrue;
    }
  }

  Path_Smooth( bot );

  node = Path_NodeAt( bot, bot->pathPos );
  Nav_BotNodePoint( bot, node, tpos );
  e = BotNav_Edge( bot->path[ bot->pathPos ] );

  BotMove_MoveToward( bot, tpos );

  if( e->flags & NAVE_CLIMB )
  {
    float need = e->climb;

    dz = tpos[ 2 ] - feet[ 2 ];

    if( need <= bot->caps.maxClimb && !( e->flags & NAVE_LADDER ) )
    {
      // jump up when we are close to the ledge
      if( onGround && dxy < reach + 40.0f && dz > NAV_STEPSIZE * 0.5f &&
          level.time - bot->lastJumpTime > 500 )
      {
        bot->upmove = 127;
        bot->lastJumpTime = level.time;
      }
    }
    else if( bot->caps.wallClimber )
    {
      // wall walk straight up: look where we are going and walk into the wall
      if( dz > -8.0f )
      {
        bot->wantWallclimb = qtrue;
        VectorSubtract( tpos, ps->origin, dir );
        dir[ 2 ] += 32.0f;
        vectoangles( dir, bot->viewAngles );
        bot->moveYaw = bot->viewAngles[ YAW ];
        bot->forceView = qtrue;
      }
    }
    else if( bot->caps.jetpack && !( e->flags & NAVE_LADDER ) )
    {
      // the generator checked a clear shaft above the source node only:
      // get onto it, fly straight up, then across
      vec3_t  spos;
      int     src = ( bot->pathPos == 0 ) ? bot->pathStart : Path_NodeAt( bot, bot->pathPos - 1 );
      float   sxy;

      Nav_BotNodePoint( bot, src, spos );
      sxy = sqrt( ( spos[ 0 ] - feet[ 0 ] ) * ( spos[ 0 ] - feet[ 0 ] ) +
                  ( spos[ 1 ] - feet[ 1 ] ) * ( spos[ 1 ] - feet[ 1 ] ) );

      if( feet[ 2 ] < tpos[ 2 ] + 12.0f )
      {
        if( sxy > 20.0f && feet[ 2 ] < spos[ 2 ] + 24.0f && onGround )
          BotMove_MoveToward( bot, spos );
        else
        {
          bot->jetpackUntil = level.time + 800;
          bot->upmove = 127;
          if( sxy > 12.0f )
            BotMove_MoveToward( bot, spos );
          else
            bot->wantMove = qfalse;
        }
      }
      else
      {
        // high enough: keep altitude and cross over
        bot->jetpackUntil = level.time + 500;
        bot->upmove = 127;
      }
    }
    else
    {
      // ladder: look up the ladder and keep walking into it
      VectorSubtract( tpos, ps->origin, dir );
      dir[ 2 ] += 48.0f;
      vectoangles( dir, bot->viewAngles );
      bot->moveYaw = bot->viewAngles[ YAW ];
      bot->forceView = qtrue;
    }
  }

  return qtrue;
}

void BotMove_CheckStuck( bot_t *bot )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  float     moved;

  if( level.time < bot->stuckCheckTime )
    return;

  bot->stuckCheckTime = level.time + 700;
  moved = Distance( ent->client->ps.origin, bot->stuckPos );
  VectorCopy( ent->client->ps.origin, bot->stuckPos );

  if( !bot->wantMove || level.time < bot->unstickUntil )
    return;

  if( moved > 40.0f )
  {
    bot->stuckCount = 0;
    return;
  }

  bot->stuckCount++;
  G_BotDebug( bot, "stuck (%d), moved %.0f", bot->stuckCount, moved );

  if( bot->stuckCount == 1 )
  {
    bot->upmove = 127;
    bot->lastJumpTime = level.time;
  }
  else if( bot->stuckCount == 2 || bot->stuckCount == 4 )
  {
    bot->unstickYaw = bot->moveYaw + ( G_BotRandom( ) < 0.5f ? 90.0f : -90.0f ) +
                      G_BotCrandom( ) * 30.0f;
    BotMove_FindSafeYaw( bot, &bot->unstickYaw );
    bot->unstickUntil = level.time + 600 + (int)( G_BotRandom( ) * 600.0f );
    bot->upmove = 127;
  }
  else if( bot->stuckCount == 3 || bot->stuckCount >= 5 )
  {
    // give up on this edge for a while and find another way
    if( bot->moveMode == BMOVE_PATH && bot->pathPos < bot->pathLen )
    {
      bot->badEdge = bot->path[ bot->pathPos ];
      bot->badEdgeUntil = level.time + 20000;
    }
    BotMove_ClearPath( bot );
    bot->pathTime = 0;
    bot->unstickYaw = G_BotRandom( ) * 360.0f;
    BotMove_FindSafeYaw( bot, &bot->unstickYaw );
    bot->unstickUntil = level.time + 800;

    if( bot->stuckCount >= 7 )
      bot->stuckCount = 0;
  }
}

// last line of defence against walking off into the void: whatever the
// bot decided, don't take a step that leaves no floor under us within a
// survivable fall (or that lands in a kill zone). Deliberate drops along
// the path are left alone.
void BotMove_EdgeGuard( bot_t *bot )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  vec3_t        dir, start, ahead, down, mins = { -4.0f, -4.0f, 0.0f }, maxs = { 4.0f, 4.0f, 4.0f };
  trace_t       tr;
  float         look, speed;

  if( !bot->wantMove || ps->groundEntityNum == ENTITYNUM_NONE ||
      ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) || ps->pm_type != PM_NORMAL )
    return;

  if( bot->moveMode == BMOVE_PATH && bot->pathPos < bot->pathLen &&
      ( BotNav_Edge( bot->path[ bot->pathPos ] )->flags & ( NAVE_DROP | NAVE_TELEPORT ) ) )
    return;

  speed = sqrt( ps->velocity[ 0 ] * ps->velocity[ 0 ] + ps->velocity[ 1 ] * ps->velocity[ 1 ] );
  look = 24.0f + speed * 0.12f;

  VectorSet( dir, cos( DEG2RAD( bot->moveYaw ) ), sin( DEG2RAD( bot->moveYaw ) ), 0.0f );
  VectorCopy( ps->origin, start );
  VectorMA( start, look, dir, ahead );
  trap_Trace( &tr, start, ent->r.mins, ent->r.maxs, ahead, ent->s.number, MASK_PLAYERSOLID );
  VectorCopy( tr.endpos, ahead );
  ahead[ 2 ] += ent->r.mins[ 2 ];

  VectorCopy( ahead, down );
  down[ 2 ] -= NAV_MAX_DROP + 64.0f;
  trap_Trace( &tr, ahead, mins, maxs, down, ent->s.number, MASK_PLAYERSOLID & ~CONTENTS_BODY );

  if( tr.startsolid )
    return;

  if( tr.fraction < 1.0f && !Nav_PointInHurt( tr.endpos ) &&
      !( trap_PointContents( tr.endpos, -1 ) & ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP ) ) )
    return;

  // deadly: slide along the edge if we can, else stop
  if( !BotMove_FindSafeYaw( bot, &bot->moveYaw ) )
    bot->wantMove = qfalse;
  else if( fabs( AngleSubtract( bot->moveYaw, vectoyaw( dir ) ) ) > 95.0f )
    bot->wantMove = qfalse;
}

// the side-step that gets a stuck bot free: re-checked every frame,
// because a step that started safe can run off a ledge a second later
qboolean BotMove_Unstick( bot_t *bot )
{
  playerState_t *ps = &g_entities[ bot->clientNum ].client->ps;

  if( level.time >= bot->unstickUntil )
    return qfalse;

  if( ps->groundEntityNum != ENTITYNUM_NONE &&
      !( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) &&
      !BotMove_SafeDirection( bot, bot->unstickYaw ) &&
      !BotMove_FindSafeYaw( bot, &bot->unstickYaw ) )
  {
    bot->unstickUntil = 0;
    return qfalse;
  }

  bot->moveYaw = bot->unstickYaw;
  bot->wantMove = qtrue;
  if( bot->caps.wallClimber )
    bot->wantWallclimb = qtrue;
  return qtrue;
}

// turn *yaw to the nearest direction that is safe to walk in; qfalse when
// there is none
qboolean BotMove_FindSafeYaw( bot_t *bot, float *yaw )
{
  static const float turns[ ] = { 0, 45, -45, 90, -90, 135, -135, 180 };
  int i;

  for( i = 0; i < 8; i++ )
  {
    if( BotMove_SafeDirection( bot, *yaw + turns[ i ] ) )
    {
      *yaw = AngleNormalize360( *yaw + turns[ i ] );
      return qtrue;
    }
  }

  return qfalse;
}

qboolean BotMove_SafeDirection( bot_t *bot, float yaw )
{
  gentity_t *ent = &g_entities[ bot->clientNum ];
  vec3_t    dir, start, end, down;
  trace_t   tr;

  VectorSet( dir, cos( DEG2RAD( yaw ) ), sin( DEG2RAD( yaw ) ), 0.0f );
  VectorCopy( ent->client->ps.origin, start );
  VectorMA( start, 56.0f, dir, end );

  trap_Trace( &tr, start, ent->r.mins, ent->r.maxs, end, ent->s.number, MASK_PLAYERSOLID );
  if( tr.fraction < 0.5f )
    return qfalse;

  VectorCopy( tr.endpos, end );
  VectorCopy( end, down );
  down[ 2 ] += ent->r.mins[ 2 ] - 72.0f;
  trap_Trace( &tr, end, NULL, NULL, down, ent->s.number, MASK_PLAYERSOLID );
  if( tr.fraction >= 1.0f )
    return qfalse;

  if( trap_PointContents( tr.endpos, -1 ) & ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP ) )
    return qfalse;

  if( Nav_PointInHurt( tr.endpos ) )
    return qfalse;

  return qtrue;
}
