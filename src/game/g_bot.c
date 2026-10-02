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

// g_bot.c -- bot management, console commands and the per-frame driver
//
// Bots are real clients: the engine reserves a client slot for them
// (trap_BotAllocateClient) and the game connects them like any player.
// Every server frame the AI fills in a usercmd_t which is handed to the
// engine (trap_BotUserCommand) and run through the normal ClientThink, so
// bots obey exactly the same movement and weapon rules as humans do.
// Gameplay actions (joining a team, spawning, buying, evolving, building)
// are issued as ordinary client commands through trap_BotClientCommand.

#include "g_local.h"
#include "g_bot.h"

bot_t     g_bots[ MAX_CLIENTS ];

vmCvar_t  bot_skill;
vmCvar_t  bot_debug;
vmCvar_t  bot_build;
vmCvar_t  bot_buildersPerTeam;
vmCvar_t  bot_chat;
vmCvar_t  bot_fillAliens;
vmCvar_t  bot_fillHumans;
vmCvar_t  bot_pause;
vmCvar_t  bot_peaceful;
vmCvar_t  bot_navGridSize;
vmCvar_t  bot_navAutoGenerate;
vmCvar_t  bot_navGenBudget;
vmCvar_t  bot_namePrefix;
vmCvar_t  bot_aimSkillScale;
vmCvar_t  bot_aimConeMax;
vmCvar_t  bot_aimConeMin;
vmCvar_t  bot_aimDelayMax;
vmCvar_t  bot_aimDelayMin;
vmCvar_t  bot_emote;
vmCvar_t  bot_evolve;
vmCvar_t  bot_buy;

typedef struct
{
  vmCvar_t  *vmCvar;
  char      *cvarName;
  char      *defaultString;
  int       cvarFlags;
} botCvarTable_t;

static botCvarTable_t botCvarTable[ ] =
{
  { &bot_skill, "bot_skill", "5", CVAR_ARCHIVE },   // a skill, or a range like "3-7"
  { &bot_debug, "bot_debug", "0", 0 },
  { &bot_build, "bot_build", "1", CVAR_ARCHIVE },
  { &bot_buildersPerTeam, "bot_buildersPerTeam", "1", CVAR_ARCHIVE },
  { &bot_chat, "bot_chat", "1", CVAR_ARCHIVE },
  { &bot_fillAliens, "bot_fillAliens", "0", CVAR_ARCHIVE },
  { &bot_fillHumans, "bot_fillHumans", "0", CVAR_ARCHIVE },
  { &bot_pause, "bot_pause", "0", 0 },
  { &bot_peaceful, "bot_peaceful", "0", 0 },
  { &bot_navGridSize, "bot_navGridSize", "32", CVAR_ARCHIVE },
  { &bot_navAutoGenerate, "bot_navAutoGenerate", "1", CVAR_ARCHIVE },
  { &bot_navGenBudget, "bot_navGenBudget", "40", CVAR_ARCHIVE },
  { &bot_namePrefix, "bot_namePrefix", "[BOT]", CVAR_ARCHIVE },
  { &bot_aimSkillScale, "bot_aimSkillScale", "1.0", CVAR_ARCHIVE },
  { &bot_aimConeMax, "bot_aimConeMax", "6", CVAR_ARCHIVE },      // degrees, skill 1
  { &bot_aimConeMin, "bot_aimConeMin", "0.5", CVAR_ARCHIVE },    // degrees, skill 10
  { &bot_aimDelayMax, "bot_aimDelayMax", "400", CVAR_ARCHIVE },  // ms, skill 1
  { &bot_aimDelayMin, "bot_aimDelayMin", "40", CVAR_ARCHIVE },   // ms, skill 10
  { &bot_emote, "bot_emote", "1", CVAR_ARCHIVE },
  { &bot_evolve, "bot_evolve", "1", CVAR_ARCHIVE },
  { &bot_buy, "bot_buy", "1", CVAR_ARCHIVE }
};

static const int botCvarTableSize = sizeof( botCvarTable ) / sizeof( botCvarTable[ 0 ] );

static qboolean botInitialized;
static int      botNextFillCheck;

static const char *botNames[ ] =
{
  "Ripley", "Hicks", "Vasquez", "Bishop", "Hudson", "Apone", "Drake", "Dietrich",
  "Ferro", "Spunkmeyer", "Gorman", "Frost", "Crowe", "Wierzbowski", "Burke", "Newt",
  "Dallas", "Kane", "Lambert", "Parker", "Brett", "Ash", "Mother", "Dutch",
  "Blain", "Mac", "Poncho", "Billy", "Hawkins", "Anna", "Rico", "Dizzy",
  "Ace", "Sugar", "Flores", "Zim", "Rasczak", "Jenkins", "Ibanez", "Watkins",
  "Granger", "Overmind", "Dretchy", "Basil", "Marauder", "Draggy", "Tyrant", "Hive",
  "Booster", "Acid", "Spike", "Creep", "Egg", "Trapper", "Barbs", "Pounce",
  "Zapper", "Lucy", "Chaingunner", "Medic", "Turret", "Tesla", "Reactor", "Node"
};
static const int numBotNames = sizeof( botNames ) / sizeof( botNames[ 0 ] );

/*
===========================================================================

UTILITIES

===========================================================================
*/

qboolean G_BotSupported( void )
{
  return trap_Cvar_VariableIntegerValue( "sv_botSupport" ) != 0;
}

bot_t *G_BotForClient( int clientNum )
{
  if( clientNum < 0 || clientNum >= MAX_CLIENTS )
    return NULL;

  if( !g_bots[ clientNum ].inuse )
    return NULL;

  return &g_bots[ clientNum ];
}

// the game's rand() is a small LCG whose low bits repeat every few calls,
// so bots use their own xorshift generator
static unsigned int botRandState = 0x12345678u;

static unsigned int G_BotRand32( void )
{
  unsigned int x = botRandState;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  botRandState = x;
  return x;
}

int G_BotRandInt( int n )
{
  if( n <= 1 )
    return 0;

  return (int)( ( G_BotRand32( ) >> 8 ) % (unsigned int)n );
}

float G_BotRandom( void )
{
  return ( G_BotRand32( ) >> 8 ) / 16777215.0f;
}

float G_BotCrandom( void )
{
  return 2.0f * ( G_BotRandom( ) - 0.5f );
}

void G_BotCommand( bot_t *bot, const char *fmt, ... )
{
  va_list argptr;
  char    text[ MAX_STRING_CHARS ];

  va_start( argptr, fmt );
  Q_vsnprintf( text, sizeof( text ), fmt, argptr );
  va_end( argptr );

  Q_strncpyz( bot->lastAction, text, sizeof( bot->lastAction ) );
  if( bot_debug.integer == 1 && !bot->debug )
    G_Printf( "^5bot %d %s^7: %s\n", bot->clientNum,
              level.clients[ bot->clientNum ].pers.netname, text );
  else
    G_BotDebug( bot, "cmd: %s", text );
  trap_BotClientCommand( bot->clientNum, text );
}

void G_BotSay( bot_t *bot, qboolean team, const char *text )
{
  if( !bot_chat.integer )
    return;

  if( level.time < bot->nextChat )
    return;

  bot->nextChat = level.time + 4000;
  G_Say( &g_entities[ bot->clientNum ], team ? SAY_TEAM : SAY_ALL, text );
}

void G_BotDebug( bot_t *bot, const char *fmt, ... )
{
  va_list argptr;
  char    text[ 1024 ];

  if( !bot->debug && bot_debug.integer < 2 )
    return;

  va_start( argptr, fmt );
  Q_vsnprintf( text, sizeof( text ), fmt, argptr );
  va_end( argptr );

  G_Printf( "^5bot %d %s^7: %s\n", bot->clientNum,
            level.clients[ bot->clientNum ].pers.netname, text );
}

static qboolean G_BotNameInUse( const char *name )
{
  int   i;
  char  clean[ MAX_NAME_LENGTH ], other[ MAX_NAME_LENGTH ];

  G_SanitiseString( (char *)name, clean, sizeof( clean ) );

  for( i = 0; i < level.maxclients; i++ )
  {
    if( level.clients[ i ].pers.connected == CON_DISCONNECTED )
      continue;
    G_SanitiseString( level.clients[ i ].pers.netname, other, sizeof( other ) );
    if( !strcmp( clean, other ) )
      return qtrue;
  }

  return qfalse;
}

static void G_BotPickName( char *out, int outSize )
{
  int   start = G_BotRandInt( numBotNames ), i;
  char  name[ MAX_NAME_LENGTH ];

  for( i = 0; i < numBotNames; i++ )
  {
    Com_sprintf( name, sizeof( name ), "%s%s", bot_namePrefix.string,
                 botNames[ ( start + i ) % numBotNames ] );
    if( !G_BotNameInUse( name ) )
    {
      Q_strncpyz( out, name, outSize );
      return;
    }
  }

  Com_sprintf( out, outSize, "%sBot%d", bot_namePrefix.string, G_BotRandInt( 1000 ) );
}

// find a bot by slot number or (partial, colour-insensitive) name
static int G_BotFind( const char *s )
{
  int   i, found = -1, matches = 0;
  char  clean[ MAX_NAME_LENGTH ], name[ MAX_NAME_LENGTH ];

  if( !s || !s[ 0 ] )
    return -1;

  if( s[ 0 ] >= '0' && s[ 0 ] <= '9' )
  {
    i = atoi( s );
    if( i >= 0 && i < level.maxclients && g_bots[ i ].inuse )
      return i;
  }

  G_SanitiseString( (char *)s, clean, sizeof( clean ) );

  for( i = 0; i < level.maxclients; i++ )
  {
    if( !g_bots[ i ].inuse )
      continue;
    G_SanitiseString( level.clients[ i ].pers.netname, name, sizeof( name ) );
    if( !strcmp( name, clean ) )
      return i;
    if( strstr( name, clean ) )
    {
      found = i;
      matches++;
    }
  }

  return matches == 1 ? found : -1;
}

static team_t G_BotTeamFromString( const char *s )
{
  if( !s || !s[ 0 ] )
    return TEAM_NONE;

  if( !Q_stricmpn( s, "a", 1 ) )
    return TEAM_ALIENS;
  if( !Q_stricmpn( s, "h", 1 ) )
    return TEAM_HUMANS;

  return TEAM_NONE;
}

/*
===========================================================================

CONNECTING / DISCONNECTING

===========================================================================
*/

/*
==================
Difficulty

Every bot has a skill from 1 to 10. Skill sets its aim cone (how far off
target its aim wanders, in degrees) and its aim delay (how far behind a
moving target its aim lags, which is also its reaction time before the
first shot). Both are interpolated between the bot_aim*Max (skill 1) and
bot_aim*Min (skill 10) cvars, and each bot gets its own +-15% spread so
two bots of the same skill don't play identically.
==================
*/
static float G_BotSkillLerp( const bot_t *bot, float atMin, float atMax )
{
  float f = ( bot->skill - 1 ) / (float)( BOT_MAX_SKILL - 1 );

  if( f < 0.0f )
    f = 0.0f;
  if( f > 1.0f )
    f = 1.0f;

  return ( atMin + ( atMax - atMin ) * f ) * ( bot->aimFactor > 0.0f ? bot->aimFactor : 1.0f );
}

float G_BotAimCone( const bot_t *bot )
{
  return G_BotSkillLerp( bot, bot_aimConeMax.value, bot_aimConeMin.value ) *
         bot_aimSkillScale.value;
}

int G_BotAimDelay( const bot_t *bot )
{
  int d = (int)G_BotSkillLerp( bot, bot_aimDelayMax.value, bot_aimDelayMin.value );

  return d < 0 ? 0 : d;
}

// "7" -> 7, "3-7" -> a random skill from 3 to 7; fallback when unreadable
int G_BotPickSkill( const char *s, int fallback )
{
  int         lo, hi, t;
  const char  *dash;

  if( !s || !s[ 0 ] || s[ 0 ] < '0' || s[ 0 ] > '9' )
    return fallback;

  lo = hi = atoi( s );
  dash = strchr( s, '-' );
  if( dash && dash[ 1 ] >= '0' && dash[ 1 ] <= '9' )
    hi = atoi( dash + 1 );

  if( hi < lo )
  {
    t = lo;
    lo = hi;
    hi = t;
  }
  if( lo < 1 )
    lo = 1;
  if( hi > BOT_MAX_SKILL )
    hi = BOT_MAX_SKILL;
  if( lo > hi )
    return fallback;

  return lo + G_BotRandInt( hi - lo + 1 );
}

static int G_BotDefaultSkill( void )
{
  return G_BotPickSkill( bot_skill.string, 5 );
}

// queue the gesture button ("Come on!" for humans, a roar for aliens).
// Kept rare: at most one every 10 s per bot and one every 2 s per team.
void G_BotEmote( bot_t *bot, float chance, int delay )
{
  static int  teamNext[ NUM_TEAMS ];
  team_t      team;

  if( !bot_emote.integer || level.time < bot->nextEmote || bot->emoteAt )
    return;

  team = level.clients[ bot->clientNum ].pers.teamSelection;
  if( team <= TEAM_NONE || team >= NUM_TEAMS || level.time < teamNext[ team ] )
    return;

  if( G_BotRandom( ) >= chance )
    return;

  bot->emoteAt = level.time + delay;
  bot->nextEmote = level.time + 10000 + (int)( G_BotRandom( ) * 10000.0f );
  teamNext[ team ] = level.time + 2000;
}

static void G_BotRandomPersonality( botPersonality_t *p )
{
  // whole percentages, so the values survive being stored in the userinfo
  p->aggression = ( 25 + G_BotRandInt( 76 ) ) / 100.0f;
  p->saver = G_BotRandInt( 101 ) / 100.0f;
  p->weaponPref = G_BotRandInt( 4 );
  p->alienPref = G_BotRandInt( 3 );
}

/*
==================
G_AddBot

Allocates a client slot and connects a bot to it. Returns the client
number or -1.
==================
*/
int G_AddBot( const char *name, team_t team, int skill, qboolean announce )
{
  int       clientNum, i;
  char      userinfo[ MAX_INFO_STRING ];
  char      botName[ MAX_NAME_LENGTH ];
  char      guid[ 33 ];
  const char *denied;
  bot_t     *bot;

  if( !G_BotSupported( ) )
  {
    G_Printf( "This server engine does not support bots (sv_botSupport is not set)\n" );
    return -1;
  }

  if( level.intermissiontime )
  {
    G_Printf( "Can't add bots during intermission\n" );
    return -1;
  }

  clientNum = trap_BotAllocateClient( );
  if( clientNum < 0 )
  {
    G_Printf( "Can't add bot: the server is full (sv_maxclients %d)\n", level.maxclients );
    return -1;
  }

  if( clientNum >= level.maxclients )
  {
    trap_BotFreeClient( clientNum );
    return -1;
  }

  if( skill < 1 )
    skill = 1;
  if( skill > BOT_MAX_SKILL )
    skill = BOT_MAX_SKILL;

  if( name && name[ 0 ] )
    Q_strncpyz( botName, name, sizeof( botName ) );
  else
    G_BotPickName( botName, sizeof( botName ) );

  // a valid 32 digit hex guid so the namelog/admin code is happy
  for( i = 0; i < 32; i++ )
    guid[ i ] = "0123456789ABCDEF"[ G_BotRandInt( 16 ) ];
  guid[ 32 ] = '\0';

  userinfo[ 0 ] = '\0';
  Info_SetValueForKey( userinfo, "name", botName );
  Info_SetValueForKey( userinfo, "rate", "25000" );
  Info_SetValueForKey( userinfo, "snaps", "20" );
  Info_SetValueForKey( userinfo, "cl_guid", guid );
  Info_SetValueForKey( userinfo, "ip", "bot" );
  Info_SetValueForKey( userinfo, "cg_unlagged", "1" );
  Info_SetValueForKey( userinfo, "botskill", va( "%d", skill ) );
  Info_SetValueForKey( userinfo, "botteam",
                       team == TEAM_ALIENS ? "aliens" : team == TEAM_HUMANS ? "humans" : "auto" );
  trap_SetUserinfo( clientNum, userinfo );

  denied = ClientConnect( clientNum, qtrue, qtrue );
  if( denied )
  {
    G_Printf( "Bot connection refused: %s\n", denied );
    trap_DropClient( clientNum, "refused" );
    return -1;
  }

  ClientBegin( clientNum );

  bot = &g_bots[ clientNum ];
  if( announce )
    G_Printf( "Added bot %s^7 (slot %d, skill %d, team %s)\n", level.clients[ clientNum ].pers.netname,
              clientNum, bot->skill,
              team == TEAM_NONE ? "auto" : BG_TeamName( team ) );

  return clientNum;
}

void G_RemoveBot( int clientNum, const char *reason )
{
  if( clientNum < 0 || clientNum >= level.maxclients || !g_bots[ clientNum ].inuse )
    return;

  trap_DropClient( clientNum, reason ? reason : "was removed" );
}

/*
==================
G_BotConnect

Called from ClientConnect for bots (also after every map change)
==================
*/
void G_BotConnect( int clientNum, qboolean firstTime )
{
  bot_t *bot = &g_bots[ clientNum ];
  char  userinfo[ MAX_INFO_STRING ];
  int   skill;

  trap_GetUserinfo( clientNum, userinfo, sizeof( userinfo ) );

  if( firstTime || !bot->inuse )
  {
    char *p;

    memset( bot, 0, sizeof( *bot ) );
    G_BotRandomPersonality( &bot->pers );
    bot->aimFactor = ( 85 + G_BotRandInt( 31 ) ) / 100.0f;

    // keep the same personality across map changes ("a,s,w,al"; parsed
    // by hand because the QVM's sscanf doesn't match literal characters)
    p = Info_ValueForKey( userinfo, "botstyle" );
    if( p[ 0 ] )
    {
      int v[ 5 ], n = 0;

      while( n < 5 && *p )
      {
        v[ n++ ] = atoi( p );
        while( *p && *p != ',' )
          p++;
        if( *p == ',' )
          p++;
      }

      if( n == 5 && v[ 4 ] >= 50 && v[ 4 ] <= 150 )
        bot->aimFactor = v[ 4 ] / 100.0f;
      if( n >= 4 )
      {
        bot->pers.aggression = v[ 0 ] / 100.0f;
        bot->pers.saver = v[ 1 ] / 100.0f;
        bot->pers.weaponPref = v[ 2 ];
        bot->pers.alienPref = v[ 3 ];
      }
    }
  }

  bot->inuse = qtrue;
  bot->clientNum = clientNum;

  skill = atoi( Info_ValueForKey( userinfo, "botskill" ) );
  if( skill < 1 || skill > BOT_MAX_SKILL )
    skill = G_BotDefaultSkill( );
  if( skill < 1 )
    skill = 1;
  if( skill > BOT_MAX_SKILL )
    skill = BOT_MAX_SKILL;
  bot->skill = skill;

  bot->wantTeam = G_BotTeamFromString( Info_ValueForKey( userinfo, "botteam" ) );
}

void G_BotBegin( int clientNum )
{
  bot_t *bot = G_BotForClient( clientNum );

  if( !bot )
    return;

  BotAI_Begin( bot );
}

void G_BotDisconnect( int clientNum )
{
  if( clientNum < 0 || clientNum >= MAX_CLIENTS )
    return;

  memset( &g_bots[ clientNum ], 0, sizeof( g_bots[ 0 ] ) );
}

// keep the desired team in the userinfo so it survives map changes
static void G_BotStoreTeam( bot_t *bot )
{
  char userinfo[ MAX_INFO_STRING ];

  trap_GetUserinfo( bot->clientNum, userinfo, sizeof( userinfo ) );
  Info_SetValueForKey( userinfo, "botteam",
                       bot->wantTeam == TEAM_ALIENS ? "aliens" :
                       bot->wantTeam == TEAM_HUMANS ? "humans" : "auto" );
  Info_SetValueForKey( userinfo, "botskill", va( "%d", bot->skill ) );
  Info_SetValueForKey( userinfo, "botstyle", va( "%d,%d,%d,%d,%d",
                       (int)( bot->pers.aggression * 100.0f + 0.5f ), (int)( bot->pers.saver * 100.0f + 0.5f ),
                       bot->pers.weaponPref, bot->pers.alienPref,
                       (int)( bot->aimFactor * 100.0f + 0.5f ) ) );
  trap_SetUserinfo( bot->clientNum, userinfo );
}

/*
===========================================================================

TEAM FILL

bot_fillAliens / bot_fillHumans keep a team at N players by adding bots
when humans are missing and removing them when humans join.

===========================================================================
*/

static int G_TeamHumans( team_t team )
{
  int i, count = 0;

  for( i = 0; i < level.maxclients; i++ )
  {
    if( level.clients[ i ].pers.connected == CON_DISCONNECTED )
      continue;
    if( level.clients[ i ].pers.isBot )
      continue;
    if( level.clients[ i ].pers.teamSelection == team )
      count++;
  }

  return count;
}

static int G_TeamBots( team_t team, int *worst )
{
  int i, count = 0, worstScore = 999999;

  if( worst )
    *worst = -1;

  for( i = 0; i < level.maxclients; i++ )
  {
    if( !g_bots[ i ].inuse || level.clients[ i ].pers.connected == CON_DISCONNECTED )
      continue;

    if( g_bots[ i ].wantTeam != team &&
        level.clients[ i ].pers.teamSelection != team )
      continue;

    count++;

    // prefer removing a dead bot with a low score
    if( worst )
    {
      int score = level.clients[ i ].ps.persistant[ PERS_SCORE ];
      if( level.clients[ i ].sess.spectatorState == SPECTATOR_NOT )
        score += 10000;
      if( score < worstScore )
      {
        worstScore = score;
        *worst = i;
      }
    }
  }

  return count;
}

static void G_BotFillTeam( team_t team, int wanted )
{
  int humans, bots, worst;

  if( wanted < 0 )
    return;

  humans = G_TeamHumans( team );
  bots = G_TeamBots( team, &worst );

  if( humans + bots < wanted )
  {
    static int fullWarnTime = -999999;
    int        i, used = 0;

    for( i = 0; i < level.maxclients; i++ )
      if( level.clients[ i ].pers.connected != CON_DISCONNECTED )
        used++;

    // no free slot: say so once a minute instead of every check
    if( used >= level.maxclients )
    {
      if( level.time - fullWarnTime > 60000 || level.time < fullWarnTime )
      {
        G_Printf( "botfill: the server is full (sv_maxclients %d), raise sv_maxclients "
                  "for more bots\n", level.maxclients );
        fullWarnTime = level.time;
      }
      return;
    }

    G_AddBot( NULL, team, G_BotDefaultSkill( ), qtrue );
  }
  else if( humans + bots > wanted && bots > 0 && worst >= 0 )
    G_RemoveBot( worst, "was removed to balance teams" );
}

static void G_BotFillCheck( void )
{
  if( level.time < botNextFillCheck || level.intermissiontime )
    return;

  botNextFillCheck = level.time + 1500;

  // nothing to do until someone asks for it
  if( bot_fillAliens.integer <= 0 && bot_fillHumans.integer <= 0 &&
      !G_TeamBots( TEAM_ALIENS, NULL ) && !G_TeamBots( TEAM_HUMANS, NULL ) )
    return;

  if( bot_fillAliens.integer > 0 || G_TeamBots( TEAM_ALIENS, NULL ) )
    G_BotFillTeam( TEAM_ALIENS, bot_fillAliens.integer > 0 ? bot_fillAliens.integer : -1 );
  if( bot_fillHumans.integer > 0 || G_TeamBots( TEAM_HUMANS, NULL ) )
    G_BotFillTeam( TEAM_HUMANS, bot_fillHumans.integer > 0 ? bot_fillHumans.integer : -1 );
}

/*
===========================================================================

USERCMD

===========================================================================
*/

// turn the AI's wishes (view angles, move direction, buttons) into a usercmd
static void G_BotBuildUsercmd( bot_t *bot, usercmd_t *cmd )
{
  gentity_t     *ent = &g_entities[ bot->clientNum ];
  playerState_t *ps = &ent->client->ps;
  vec3_t        local, axis[ 3 ], rotaxis[ 3 ];
  float         delta, f, r;
  int           i;

  memset( cmd, 0, sizeof( *cmd ) );
  cmd->serverTime = level.time;
  cmd->weapon = ps->weapon;

  // view angles: when wall walking the command angles are relative to the
  // surface, so undo the rotation the movement code applies
  VectorCopy( bot->viewAngles, local );
  if( bot->viewAngles[ PITCH ] > 85.0f )
    local[ PITCH ] = 85.0f;
  else if( bot->viewAngles[ PITCH ] < -85.0f )
    local[ PITCH ] = -85.0f;

  if( ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) && ps->pm_type != PM_DEAD )
  {
    AnglesToAxis( local, axis );
    if( BG_RotateAxis( ps->grapplePoint, axis, rotaxis, qtrue,
                       ps->eFlags & EF_WALLCLIMBCEILING ) )
      AxisToAngles( rotaxis, local );
  }

  for( i = 0; i < 3; i++ )
  {
    if( i == ROLL )
      continue;
    cmd->angles[ i ] = ANGLE2SHORT( local[ i ] ) - ps->delta_angles[ i ];
  }

  // movement relative to where we look
  if( bot->wantMove )
  {
    if( bot->wantWallclimb && ( ps->stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
    {
      // on a wall we look where we go
      f = 127.0f;
      r = 0.0f;
    }
    else
    {
      delta = DEG2RAD( AngleSubtract( bot->moveYaw, bot->viewAngles[ YAW ] ) );
      f = cos( delta ) * 127.0f;
      r = -sin( delta ) * 127.0f;
    }

    if( bot->walk )
    {
      f *= 0.5f;
      r *= 0.5f;
    }

    cmd->forwardmove = (signed char)f;
    cmd->rightmove = (signed char)r;
  }

  cmd->upmove = (signed char)bot->upmove;
  if( bot->wantWallclimb && bot->upmove <= 0 &&
      BG_ClassHasAbility( ps->stats[ STAT_CLASS ], SCA_WALLCLIMBER ) )
    cmd->upmove = -127;

  cmd->buttons = bot->buttons;
  if( bot->walk )
    cmd->buttons |= BUTTON_WALKING;

  // jump needs to be released between jumps
  if( bot->upmove > 0 && ( ps->pm_flags & PMF_JUMP_HELD ) &&
      ps->groundEntityNum != ENTITYNUM_NONE )
    cmd->upmove = 0;
}

/*
===========================================================================

FRAME

===========================================================================
*/

void G_BotInit( void )
{
  int i;

  for( i = 0; i < botCvarTableSize; i++ )
    trap_Cvar_Register( botCvarTable[ i ].vmCvar, botCvarTable[ i ].cvarName,
                        botCvarTable[ i ].defaultString, botCvarTable[ i ].cvarFlags );

  botInitialized = qtrue;
  botNextFillCheck = level.time + 3000;

  botRandState ^= (unsigned int)trap_Milliseconds( ) * 2654435761u;
  if( !botRandState )
    botRandState = 0x9e3779b9u;

  if( !G_BotSupported( ) )
    return;

  BotNav_Init( );
  BotAI_ResetPlans( );
}

void G_BotShutdown( void )
{
  int i;

  BotNav_Shutdown( );

  // remember each bot's team for the next map
  for( i = 0; i < MAX_CLIENTS; i++ )
  {
    if( g_bots[ i ].inuse && level.clients &&
        level.clients[ i ].pers.connected != CON_DISCONNECTED )
    {
      if( g_bots[ i ].wantTeam == TEAM_NONE )
        g_bots[ i ].wantTeam = level.clients[ i ].pers.teamSelection;
      G_BotStoreTeam( &g_bots[ i ] );
    }
  }
}

void G_BotRunFrame( void )
{
  int       i;
  bot_t     *bot;
  gentity_t *ent;

  if( !botInitialized )
    return;

  for( i = 0; i < botCvarTableSize; i++ )
    trap_Cvar_Update( botCvarTable[ i ].vmCvar );

  if( !G_BotSupported( ) )
    return;

  BotNav_Frame( );
  G_BotFillCheck( );
  BotAI_TeamFrame( );

  for( i = 0; i < level.maxclients; i++ )
  {
    bot = &g_bots[ i ];
    if( !bot->inuse )
      continue;

    ent = &g_entities[ i ];
    if( !ent->client || ent->client->pers.connected != CON_CONNECTED )
      continue;

    // reset per-frame wishes
    bot->wantMove = qfalse;
    bot->walk = qfalse;
    bot->upmove = 0;
    bot->buttons = 0;
    bot->wantWallclimb = qfalse;
    bot->forceView = qfalse;

    if( !bot_pause.integer && !level.intermissiontime )
      BotAI_Think( bot );
    else if( level.intermissiontime )
      bot->buttons = ( level.time & 1024 ) ? BUTTON_ATTACK : 0;   // ready up

    if( !bot_pause.integer && !level.intermissiontime )
      BotMove_EdgeGuard( bot );

    G_BotBuildUsercmd( bot, &bot->cmd );
    bot->lastButtons = bot->cmd.buttons;

    trap_BotUserCommand( i, &bot->cmd );
    ClientThink( i );
  }
}

/*
===========================================================================

CONSOLE COMMANDS

===========================================================================
*/

static void Bot_Usage( void )
{
  G_Printf( "bot commands:\n"
            "  addbot [name] [aliens|humans|auto] [skill 1-10 or range 3-7]\n"
            "  removebot <name|slot|all>        (alias kickbot)\n"
            "  kickbots [aliens|humans]         remove every bot (on a team)\n"
            "  botlist                          list bots and what they are doing\n"
            "  botskill <name|slot|all> <1-10|3-7>  change skill (a range picks one per bot)\n"
            "  botteam <name|slot> <aliens|humans|spectate>\n"
            "  botfill <count> [aliens|humans]  keep teams at <count> players with bots\n"
            "  botcmd <name|slot> <command>     run a client command as the bot\n"
            "  botdebug <name|slot|off>         print a bot's decisions\n"
            "  botgoto <name|slot> <x y z|player>  send a bot somewhere\n"
            "  botplan                          show where bots plan base defences\n"
            "  navgen                           regenerate the navigation graph\n"
            "  navinfo                          navigation statistics\n"
            "  navshow [radius] [seconds]       draw the graph near the first player\n" );
}

void Svcmd_AddBot_f( void )
{
  char    name[ MAX_NAME_LENGTH ] = "";
  char    arg[ MAX_TOKEN_CHARS ];
  team_t  team = TEAM_NONE;
  int     skill = G_BotDefaultSkill( );
  int     i, argc = trap_Argc( );

  // arguments in any order: a team keyword, a number (skill) or a name
  for( i = 1; i < argc; i++ )
  {
    trap_Argv( i, arg, sizeof( arg ) );

    if( !Q_stricmp( arg, "aliens" ) || !Q_stricmp( arg, "alien" ) || !Q_stricmp( arg, "a" ) )
      team = TEAM_ALIENS;
    else if( !Q_stricmp( arg, "humans" ) || !Q_stricmp( arg, "human" ) || !Q_stricmp( arg, "h" ) )
      team = TEAM_HUMANS;
    else if( !Q_stricmp( arg, "auto" ) )
      team = TEAM_NONE;
    else if( arg[ 0 ] >= '0' && arg[ 0 ] <= '9' && atoi( arg ) > 0 && atoi( arg ) <= BOT_MAX_SKILL &&
             strlen( arg ) <= 5 )
      skill = G_BotPickSkill( arg, skill );   // "7" or a range like "3-7"
    else if( !Q_stricmp( arg, "help" ) || !Q_stricmp( arg, "?" ) )
    {
      Bot_Usage( );
      return;
    }
    else
      Q_strncpyz( name, arg, sizeof( name ) );
  }

  G_AddBot( name[ 0 ] ? va( "%s%s", bot_namePrefix.string, name ) : NULL, team, skill, qtrue );
}

void Svcmd_RemoveBot_f( void )
{
  char  arg[ MAX_TOKEN_CHARS ];
  int   i, n;

  if( trap_Argc( ) < 2 )
  {
    G_Printf( "usage: removebot <name|slot|all>\n" );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );

  if( !Q_stricmp( arg, "all" ) )
  {
    for( i = 0; i < level.maxclients; i++ )
      if( g_bots[ i ].inuse )
        G_RemoveBot( i, "was removed" );
    return;
  }

  n = G_BotFind( arg );
  if( n < 0 )
  {
    G_Printf( "removebot: no unique bot matches '%s'\n", arg );
    return;
  }

  G_RemoveBot( n, "was removed" );
}

void Svcmd_KickBots_f( void )
{
  char    arg[ MAX_TOKEN_CHARS ];
  team_t  team = TEAM_NONE;
  int     i, removed = 0;

  if( trap_Argc( ) > 1 )
  {
    trap_Argv( 1, arg, sizeof( arg ) );
    team = G_BotTeamFromString( arg );
  }

  // stop the fill logic re-adding them
  if( team == TEAM_NONE || team == TEAM_ALIENS )
    trap_Cvar_Set( "bot_fillAliens", "0" );
  if( team == TEAM_NONE || team == TEAM_HUMANS )
    trap_Cvar_Set( "bot_fillHumans", "0" );

  for( i = 0; i < level.maxclients; i++ )
  {
    if( !g_bots[ i ].inuse )
      continue;
    if( team != TEAM_NONE && level.clients[ i ].pers.teamSelection != team &&
        g_bots[ i ].wantTeam != team )
      continue;
    G_RemoveBot( i, "was removed" );
    removed++;
  }

  G_Printf( "removed %d bot%s\n", removed, removed == 1 ? "" : "s" );
}

void Svcmd_BotList_f( void )
{
  int       i, count = 0;
  gentity_t *ent;
  bot_t     *bot;
  static const char *goalNames[ BGOAL_NUM ] =
  {
    "idle", "fight", "hunt", "attack base", "defend", "shop", "heal", "build",
    "repair", "evolve", "flee", "roam"
  };
  static const char *roleNames[ ] = { "attack", "defend", "builder", "roam" };

  G_Printf( "slot skill  aim       team    class        weapon     hp  credits role    goal         style name\n" );

  for( i = 0; i < level.maxclients; i++ )
  {
    bot = &g_bots[ i ];
    if( !bot->inuse )
      continue;

    ent = &g_entities[ i ];
    count++;

    G_Printf( "%4d %5d  %4.1f/%-4d %-7s %-12s %-10s %3d %8d %-7s %-12s %d%d%c%c  %s\n", i, bot->skill,
              G_BotAimCone( bot ), G_BotAimDelay( bot ),
              BG_TeamName( ent->client->pers.teamSelection ),
              ent->client->sess.spectatorState == SPECTATOR_NOT ?
                BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->name : "-",
              ent->client->sess.spectatorState == SPECTATOR_NOT ?
                BG_Weapon( ent->client->ps.stats[ STAT_WEAPON ] )->name : "-",
              ent->client->sess.spectatorState == SPECTATOR_NOT ? ent->health : 0,
              ent->client->pers.credit,
              roleNames[ bot->role ], goalNames[ bot->goal ],
              bot->pers.weaponPref, bot->pers.alienPref,
              bot->pers.aggression > 0.66f ? 'A' : bot->pers.aggression > 0.4f ? 'a' : '-',
              bot->pers.saver > 0.6f ? 'S' : '-',
              ent->client->pers.netname );
  }

  G_Printf( "%d bot%s, navigation: ", count, count == 1 ? "" : "s" );
  BotNav_PrintInfo( );
}

void Svcmd_BotSkill_f( void )
{
  char  arg[ MAX_TOKEN_CHARS ], range[ MAX_TOKEN_CHARS ];
  int   i, n, skill, changed = 0;

  if( trap_Argc( ) < 3 )
  {
    G_Printf( "usage: botskill <name|slot|all> <1-10 | range like 3-7>\n"
              "  a range gives each bot its own random skill in it\n" );
    return;
  }

  trap_Argv( 2, range, sizeof( range ) );
  if( G_BotPickSkill( range, -1 ) < 0 )
  {
    G_Printf( "skill must be between 1 and %d, or a range like 3-7\n", BOT_MAX_SKILL );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );

  for( i = 0; i < level.maxclients; i++ )
  {
    if( !g_bots[ i ].inuse )
      continue;

    if( Q_stricmp( arg, "all" ) )
    {
      n = G_BotFind( arg );
      if( n != i )
        continue;
    }

    skill = G_BotPickSkill( range, g_bots[ i ].skill );
    g_bots[ i ].skill = skill;
    G_BotStoreTeam( &g_bots[ i ] );
    G_Printf( "%s^7 is now skill %d (aim cone %.1f deg, aim delay %d ms)\n",
              level.clients[ i ].pers.netname, skill,
              G_BotAimCone( &g_bots[ i ] ), G_BotAimDelay( &g_bots[ i ] ) );
    changed++;
  }

  if( !changed )
    G_Printf( "botskill: no bot matches '%s'\n", arg );
}

void Svcmd_BotTeam_f( void )
{
  char    arg[ MAX_TOKEN_CHARS ];
  int     n;
  team_t  team;

  if( trap_Argc( ) < 3 )
  {
    G_Printf( "usage: botteam <name|slot> <aliens|humans|spectate>\n" );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );
  n = G_BotFind( arg );
  if( n < 0 )
  {
    G_Printf( "botteam: no unique bot matches '%s'\n", arg );
    return;
  }

  trap_Argv( 2, arg, sizeof( arg ) );
  team = G_BotTeamFromString( arg );
  g_bots[ n ].wantTeam = team;
  G_BotStoreTeam( &g_bots[ n ] );
  G_ChangeTeam( &g_entities[ n ], team );
}

void Svcmd_BotFill_f( void )
{
  char    arg[ MAX_TOKEN_CHARS ];
  int     count;
  team_t  team = TEAM_NONE;

  if( trap_Argc( ) < 2 )
  {
    G_Printf( "usage: botfill <count> [aliens|humans]\n"
              "  keeps each team (or just one) at <count> players, adding bots when\n"
              "  humans are missing and removing them as humans join. 0 disables.\n"
              "  currently: aliens %d, humans %d\n",
              bot_fillAliens.integer, bot_fillHumans.integer );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );
  count = atoi( arg );
  if( count < 0 )
    count = 0;

  if( trap_Argc( ) > 2 )
  {
    trap_Argv( 2, arg, sizeof( arg ) );
    team = G_BotTeamFromString( arg );
  }

  if( team == TEAM_NONE || team == TEAM_ALIENS )
    trap_Cvar_Set( "bot_fillAliens", va( "%d", count ) );
  if( team == TEAM_NONE || team == TEAM_HUMANS )
    trap_Cvar_Set( "bot_fillHumans", va( "%d", count ) );

  botNextFillCheck = 0;
}

void Svcmd_BotCmd_f( void )
{
  char  arg[ MAX_TOKEN_CHARS ];
  char  command[ MAX_STRING_CHARS ];
  int   n;

  if( trap_Argc( ) < 3 )
  {
    G_Printf( "usage: botcmd <name|slot> <command>\n"
              "  e.g. botcmd 5 say hello, botcmd Ripley class level0\n" );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );
  n = G_BotFind( arg );
  if( n < 0 )
  {
    G_Printf( "botcmd: no unique bot matches '%s'\n", arg );
    return;
  }

  // copy the rest before the client command re-tokenises the arguments
  Q_strncpyz( command, ConcatArgs( 2 ), sizeof( command ) );
  trap_BotClientCommand( n, command );
}

void Svcmd_BotDebug_f( void )
{
  char  arg[ MAX_TOKEN_CHARS ];
  int   i, n;

  if( trap_Argc( ) < 2 )
  {
    G_Printf( "usage: botdebug <name|slot|all|off>\n" );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );

  if( !Q_stricmp( arg, "off" ) || !Q_stricmp( arg, "all" ) )
  {
    for( i = 0; i < MAX_CLIENTS; i++ )
      g_bots[ i ].debug = !Q_stricmp( arg, "all" );
    return;
  }

  n = G_BotFind( arg );
  if( n < 0 )
  {
    G_Printf( "botdebug: no unique bot matches '%s'\n", arg );
    return;
  }

  g_bots[ n ].debug = !g_bots[ n ].debug;
  G_Printf( "debugging %s^7 %s\n", level.clients[ n ].pers.netname,
            g_bots[ n ].debug ? "on" : "off" );
  if( g_bots[ n ].debug )
    BotAI_PrintState( &g_bots[ n ] );
}

void Svcmd_BotGoto_f( void )
{
  char    arg[ MAX_TOKEN_CHARS ];
  int     n, i, target;
  vec3_t  pos;

  if( trap_Argc( ) < 3 )
  {
    G_Printf( "usage: botgoto <bot> <x y z | player | off>\n"
              "  sends a bot to a spot and keeps it there (handy with bot_peaceful 1\n"
              "  to test navigation); 'off' lets it go back to normal\n" );
    return;
  }

  trap_Argv( 1, arg, sizeof( arg ) );
  n = G_BotFind( arg );
  if( n < 0 )
  {
    G_Printf( "botgoto: no unique bot matches '%s'\n", arg );
    return;
  }

  trap_Argv( 2, arg, sizeof( arg ) );
  if( !Q_stricmp( arg, "off" ) )
  {
    BotAI_ForceGoal( &g_bots[ n ], NULL );
    G_Printf( "%s^7 is free to roam again\n", level.clients[ n ].pers.netname );
    return;
  }

  if( trap_Argc( ) >= 5 )
  {
    for( i = 0; i < 3; i++ )
    {
      trap_Argv( 2 + i, arg, sizeof( arg ) );
      pos[ i ] = atof( arg );
    }
  }
  else
  {
    char err[ MAX_STRING_CHARS ];

    trap_Argv( 2, arg, sizeof( arg ) );
    target = G_ClientNumberFromString( arg, err, sizeof( err ) );
    if( target < 0 )
    {
      G_Printf( "botgoto: %s", err );
      return;
    }
    VectorCopy( g_entities[ target ].r.currentOrigin, pos );
  }

  BotAI_ForceGoal( &g_bots[ n ], pos );
  G_Printf( "%s^7 heading to %s\n", level.clients[ n ].pers.netname, vtos( pos ) );
}

void Svcmd_NavGen_f( void )
{
  if( !G_BotSupported( ) )
  {
    G_Printf( "bots are not supported by this server engine\n" );
    return;
  }

  BotNav_StartGeneration( qtrue );
}

void Svcmd_NavInfo_f( void )
{
  BotNav_PrintInfo( );
}

void Svcmd_BotPlan_f( void )
{
  BotBuild_PrintPlans( );
}

void Svcmd_NavShow_f( void )
{
  char      arg[ MAX_TOKEN_CHARS ];
  float     radius = 600.0f;
  int       seconds = 20, i;
  gentity_t *viewer = NULL;

  if( trap_Argc( ) > 1 )
  {
    trap_Argv( 1, arg, sizeof( arg ) );
    if( !Q_stricmp( arg, "off" ) )
    {
      BotNav_ClearShow( );
      return;
    }
    radius = atof( arg );
    if( radius < 64.0f )
      radius = 64.0f;
  }

  if( trap_Argc( ) > 2 )
  {
    trap_Argv( 2, arg, sizeof( arg ) );
    seconds = atoi( arg );
    if( seconds < 1 )
      seconds = 1;
  }

  // the first human player that is in the game
  for( i = 0; i < level.maxclients; i++ )
  {
    if( level.clients[ i ].pers.connected == CON_CONNECTED && !level.clients[ i ].pers.isBot )
    {
      viewer = &g_entities[ i ];
      break;
    }
  }

  if( !viewer )
  {
    G_Printf( "navshow: no player to show the graph to\n" );
    return;
  }

  BotNav_ShowFor( viewer, radius, seconds );
}
