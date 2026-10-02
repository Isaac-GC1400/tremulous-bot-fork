![Wtfbbqhax/Tremulous/tremulous-banner.jpg](misc/tremulous-banner.jpg)

[![Travis branch](https://travis-ci.org/GrangerHub/tremulous.svg?branch=master)](https://travis-ci.org/GrangerHub/tremulous)
[![Coverity Scan](https://img.shields.io/coverity/scan/9866.svg?maxAge=3600)](https://scan.coverity.com/projects/wtfbbqhax-tremulous)

# What's in this fork: bots

This fork of GrangerHub's Tremulous adds server-side bots that play both
teams. Full details are in [BOTS.md](BOTS.md). In short:

**Bots**
- Bots fill player slots on either team, on dedicated servers and in local
  games. They need no client changes and run in both `game.qvm` and the
  native game library.
- Navigation is built automatically from each map's own geometry the first
  time it's played with bots, then saved to `gpp/bots/nav/<map>.nav`. It
  covers walking, jumping, drops, ladders, alien wall walking and jetpack
  climbs, with separate routes for each body size (a tyrant won't try a
  dretch vent). Kill zones and fatal drops are avoided.
- Humans buy equipment from a simple decision tree (armour, then the best
  weapon their style and the stage allow, then helmet, battery pack,
  jetpack, grenades and medkit). Aliens evolve along one of three lines
  (dragoon/tyrant, marauder, basilisk) and only where the game allows it.
- Every class fights its own way: pounce, trample, barbs, zap, gas, dretch
  head bites and wall walking, lucifer cannon charging, leading slow
  projectiles, medkits, grenades and blaster swapping.
- A builder bot on each team finds the ways into its base and puts turrets,
  teslas, acid tubes and hives there first, places the other structures
  around the reactor or overmind, repairs damage and rebuilds a lost
  reactor or overmind.
- Roles (attack, defend, roam, build) are balanced across each team.
- **Difficulty:** each bot's skill (1-10) sets its aim cone (how far its aim
  wanders) and aim delay (how far behind a moving target it aims, also its
  reaction time). Skills can be given as a range like `3-7`, and every bot
  gets its own small spread on top.
- **Emotes:** bots shout or roar when heading out to attack and sometimes
  after a kill.
- Bots keep their team, skill and personality across map changes and
  restarts, and leave to make room when a person joins a full server.

**Gameplay changes for big teams** (these apply to real players too; set a
cvar to 0 to turn its change off)
- Build points grow with team size: 5 more per player beyond the first 8
  (`g_buildPointsPerPlayer`, `g_buildPointsFreePlayers`).
- Spawns rest less between spawns when more players are queued than there
  are telenodes or eggs, from 10 s down to 2.5 s (`g_spawnQueueBoost`).
- Bot builders put up extra spawns first while players are queueing, about
  one per three players, and big teams get a second builder.

**Fixes**
- The Windows client no longer crashes silently at start-up (a console
  buffer was too big for the Windows stack).
- A heap overflow in the video mode list that could abort the client at
  start-up is fixed.
- The granger launcher builds with current MinGW.

**Console commands added** (server console, or `rcon`; in a local game put
`/` in front)

| Command | What it does |
|---|---|
| `addbot [name] [aliens\|humans\|auto] [skill]` | Add a bot. Skill is 1-10 or a range like `3-7`. |
| `removebot <name\|slot\|all>` (or `kickbot`) | Remove a bot. |
| `kickbots [aliens\|humans]` | Remove every bot, or every bot on one team. |
| `botfill <count> [aliens\|humans]` | Keep teams at `count` players, adding and removing bots as people come and go. `botfill 0` turns it off. |
| `botlist` | What every bot is doing: skill, aim, team, class, weapon, credits, role, goal. |
| `botskill <name\|slot\|all> <1-10 \| 3-7>` | Change skill; a range gives each bot its own skill in it. |
| `botteam <name\|slot> <aliens\|humans\|spectate>` | Move a bot to another team. |
| `botcmd <name\|slot> <command>` | Run any client command as the bot (`botcmd Ripley class level3`). |
| `botdebug <name\|slot\|all\|off>` | Print a bot's decisions to the console. |
| `botgoto <name\|slot> <x y z \| player \| off>` | Send a bot somewhere and keep it there. |
| `botplan` | Show where each team plans its base defences. |
| `navgen` | Rebuild the navigation for the current map. |
| `navinfo` | Navigation statistics. |
| `navshow [radius] [seconds]` | Draw the navigation around you. |

Main settings: `bot_skill` (default 5, or a range), `bot_aimConeMax/Min`,
`bot_aimDelayMax/Min`, `bot_build`, `bot_buildersPerTeam`, `bot_buy`,
`bot_evolve`, `bot_emote`, `bot_chat`, `bot_peaceful`, `bot_pause`. See
[BOTS.md](BOTS.md) for all of them.

# Get started with bots

In a local game, open the console with `~` and type:

```
/set sv_maxclients 16
/map atcs
/botfill 6
```

That gives each team 6 players. Pick a team from the menu as usual. A few
more things to try:

```
/addbot aliens 8          add a skill 8 alien
/addbot Ripley humans     add a human called [BOT]Ripley
/set bot_skill 3-7        new bots get a random skill from 3 to 7
/botskill all 2-9         re-roll every bot's skill
/botlist                  see what every bot is doing
/kickbots                 remove all bots
```

`sv_maxclients` has to be set before `/map`; a local game has only 8 slots
by default, you included. The first time a map is played with bots, the
game takes a few seconds to work out the map's navigation.

For a dedicated server with bots from the start:

```
tremded +set sv_maxclients 16 +map atcs +botfill 6
```

# Installing

This is a fork from the hard work over at GrangerHub's Tremulous. Please refer there for installation/dependancies.
https://github.com/GrangerHub/tremulous/

# Acknowledgments

In loving memory of *<{dGr8LookinSparky, who was a great community builder and developer for Tremulous.
