# Bots for Tremulous (GPP / tremulous-em)

Server-side bots that play both teams: humans buy weapons and armour, aliens
evolve, both teams fight with every class ability and rebuild their base.
Bots are ordinary player slots driven by the game module, so they work on
dedicated servers and in local (listen server) games, in `game.so` and in
`game.qvm`. Bots need no client or UI changes: everything is controlled
from the server console (or `rcon`).

## Quick start

From the console of a local game (or a dedicated server):

```
/set sv_maxclients 16   a local game has 8 slots by default, you included
/map atcs               (or any map)
/addbot                 one bot on the team that needs it
/addbot aliens 7        a skill 7 alien
/addbot Ripley humans   a human called [BOT]Ripley
/botfill 6              keep both teams at 6 players (bots leave as people join)
/botlist                what every bot is doing
/kickbots               remove them all
```

On a dedicated server, type the commands without the `/`, or send them with
`rcon`.

The first time a map is played with bots, the navigation graph is built from
the map itself (half a second to a few seconds, spread over frames so the
server keeps running) and cached in `<homepath>/gpp/bots/nav/<map>.nav`.
Graphs for the eight stock maps are included, so those start instantly.

## Console commands

| Command | What it does |
|---|---|
| `addbot [name] [aliens\|humans\|auto] [skill 1-10]` | Add a bot. Arguments go in any order. Without a team it joins the smaller team. |
| `removebot <name\|slot\|all>` | Remove one bot (or all). `kickbot` is the same command. |
| `kickbots [aliens\|humans]` | Remove every bot, or every bot on one team. |
| `botlist` | Table of bots: skill, team, class, weapon, health, credits, role, current goal and personality. |
| `botskill <name\|slot\|all> <1-10>` | Change skill on the fly. |
| `botteam <name\|slot> <aliens\|humans\|spectate>` | Move a bot to another team. |
| `botfill <count> [aliens\|humans]` | Keep teams at `count` players by adding and removing bots as people come and go. `botfill 0` turns it off. |
| `botcmd <name\|slot> <command>` | Run any client command as the bot, e.g. `botcmd Ripley class level3`, `botcmd 5 say hello`. |
| `botdebug <name\|slot\|all\|off>` | Print a bot's decisions to the console. |
| `botgoto <name\|slot> <x y z \| player \| off>` | Send a bot somewhere and keep it there (handy with `bot_peaceful 1` to test navigation). `off` releases it. |
| `botplan` | Show where each team plans its base defences and which spots are filled. |
| `navgen` | Rebuild the navigation graph for the current map (and overwrite the cache). |
| `navinfo` | Graph statistics: nodes, edges, how many fit each body size, jump/drop/climb edges. |
| `navshow [radius] [seconds]` | Draw the graph as beams around the first non-bot player (default 600 units, 20 seconds). Climb and drop edges are drawn higher. `navshow off` clears it. |

Bot names can be shortened to any unique part of the name, or given as the
client slot number shown by `botlist`.

### botlist personality column

`botlist` shows a short personality code per bot, e.g. `21aS`:

* 1st digit: human weapon style: 0 rifleman (pulse rifle / chaingun), 1 marksman
  (mass driver / lasgun), 2 close quarters (flamer / chaingun / shotgun),
  3 heavy (lucifer cannon / pulse rifle)
* 2nd digit: alien line of evolution: 0 brawler (dragoon, tyrant),
  1 skirmisher (marauder), 2 support (basilisk, then the heavies)
* `A` / `a` / `-`: aggressive, average, careful
* `S` / `-`: saves credits for expensive upgrades / spends right away

Personalities are random per bot and survive map changes.

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `bot_skill` | 5 | Skill for new bots (1-10): turn speed, reaction time, aim error, projectile leading, strafing. |
| `bot_aimSkillScale` | 1.0 | Multiplies aim error for every bot. `0` gives perfect aim, `2` doubles the error. |
| `bot_build` | 1 | Let bots build. |
| `bot_buildersPerTeam` | 1 | Builders per team (they also repair and rebuild a destroyed reactor / overmind). |
| `bot_buy` | 1 | Humans buy equipment. |
| `bot_evolve` | 1 | Aliens evolve. |
| `bot_chat` | 1 | 0 silent, 1 occasional chat, 2 also announce purchases, evolutions and buildings. |
| `bot_fillAliens`, `bot_fillHumans` | 0 | Team sizes kept by `botfill`. |
| `bot_pause` | 0 | Freeze all bots (they stand still and do nothing). |
| `bot_peaceful` | 0 | Bots don't attack anything (for testing movement). |
| `bot_debug` | 0 | 1: print every command bots send; 2: print every bot's decisions; 3: also print aiming / builder internals for bots selected with `botdebug`. |
| `bot_namePrefix` | `[BOT]` | Put in front of bot names. |
| `bot_navGridSize` | 32 | Spacing of graph nodes when generating. Bigger maps automatically retry with a coarser grid if they run out of nodes. |
| `bot_navAutoGenerate` | 1 | Build a graph automatically when a map has none. |
| `bot_navGenBudget` | 40 | Milliseconds per server frame spent generating a graph. |
| `sv_botsYieldSlots` | 1 | (engine) When a person connects to a full server, a bot leaves to make room. |
| `sv_botSupport` | 1 | (engine, read only) Tells the game module the engine can run bots. |

Bots, their teams, skills and personalities are kept across `map`,
`map_restart` and map rotation.

## How it works

### Navigation (`src/game/g_botnav.c`)

There is no hand-made waypoint or nav-mesh file: the graph is generated from
the map's collision model with the same box traces player movement uses.

* Starting from every spawn point, buildable and teleporter destination, a
  flood fill steps over a 32-unit grid. Every node is a spot of floor, and
  every edge says how to get to the neighbour: walk, step up, drop down,
  jump, climb (wall walk, jetpack or ladder) or teleport.
* Each node and edge records which of six body sizes fit (dretch;
  basilisk / granger / marauder; human; dragoon; tyrant; battlesuit), so a
  tyrant doesn't try to squeeze through a vent. Wide bodies on stairs are
  tested resting on the higher steps, the way player movement treats them.
* Wall walking aliens (dretch, basilisk, granger) get climb edges up any
  vertical surface the map has, so they reach ledges and ceilings other
  classes can't, and humans with a jetpack get their own (higher) climb edges.
* Lava, slime, no-drop areas, `trigger_hurt` volumes and kill zones (a
  trigger that fires a `target_kill`, like the pits in Uncreation) are marked
  as hazards and avoided. Nodes at the edge of a fatal fall cost more to
  path through and are never picked as places to go. Every frame, a last
  check stops a bot from stepping where there's no floor within a
  survivable fall.
* Paths are found with A* (bounded so it never stalls a frame), smoothed, and
  followed with stuck detection, recovery and replanning.

### Decisions (`src/game/g_botai.c`)

Every 250 ms a bot picks a goal from a priority list:

```
fight what I can see  >  heal (medistation / booster / creep)
  >  shop (human)  >  evolve (alien, when nobody is close)
  >  hunt what I know about  >  role: attack base / defend / build / roam
```

Roles are picked at each spawn and rebalanced every 15-25 seconds: about a
third of each team defends the base (careful bots are likelier to), the rest
push for the enemy base or hunt players, and one bot per team builds.

Combat is per class:

* **Humans** keep each weapon in its useful range (strafing, backing away from
  melee aliens), lead projectiles, charge the lucifer cannon, throw grenades,
  use medkits, swap to the blaster when out of ammo, and use a jetpack for
  tall climbs.
* **Dretch** circle-strafes and jumps at the head (bites lower down do half
  damage), and wall walks to come from above.
* **Basilisk** claws and grabs; the advanced one uses its gas cloud.
* **Marauder** jumps around its target; the advanced one zaps groups.
* **Dragoon** charges and releases pounce at range; the advanced one
  shoots barbs.
* **Tyrant** tramples (charge) when it has a run-up.
* **Granger** builds; it spits / claws only to defend itself.

### Buying and evolving (`src/game/g_botbuild.c`)

Human decision tree (re-evaluated after spawning, when passing an armoury,
when low on ammo, and when the bot can afford a clearly better weapon or a
battlesuit; only then is a long walk back to the armoury worth it):

```
battlesuit, if it can be afforded with plenty to spare
  else light armour
weapon: best one in the bot's style that's affordable and allowed in this stage
helmet, if there's money left
energy weapon? battery pack.  Aggressive? jetpack.
grenade (aggressive bots), medkit, ammo
```

Alien decision tree (evolves on the ground, off the walls, with no humans or
powered human structures close by: the same test the game applies):

```
granger builder: advanced granger when available, otherwise stays a builder
otherwise: the best class in the bot's line of evolution that the
  credits and stage allow
  (savers keep their evos for a dragoon or better, unless near the cap)
lots of unused evos? take any upgrade
```

### Base building (`src/game/g_botbuild.c`)

One bot per team (`bot_buildersPerTeam`) spawns as a granger or with a
construction kit.

* **Choke points**: flooding the graph outward from the reactor/overmind, the
  bot looks at a few candidate radii and counts the graph nodes on the edge
  of each circle that lead further out. The radius where that boundary is
  smallest is where the base is most enclosed; each connected group of
  boundary nodes is one way into the base, and the narrow ones are the
  choke points. `botplan` prints the result.
* **Defences** (machine gun turrets, tesla generators, acid tubes, trappers,
  hives, barricades) go on those spots first, then around the rim.
* **Utility buildings** (telenodes/eggs, armoury, medistation, DCC, booster)
  go on random open floor 110-480 units from the reactor (110-420 from the
  overmind), preferring the HQ's own floor, kept clear of spawn exits, of the
  defence spots and of each other.
* Build order follows build points and stage (humans: telenode, armoury,
  turret, telenode, medistation, turrets, DCC, teslas...; aliens: egg, acid
  tube, egg, booster, acid tube, trapper, egg, hive...). If the
  reactor/overmind is destroyed, rebuilding it comes first. Human builders
  repair damaged structures between builds.

## Engine changes

Bots need a few engine hooks (`src/server`, `src/qcommon`):

* a `NA_BOT` network address type, so bot clients have no network channel and
  never time out;
* game syscalls to allocate and free a bot client slot, and to feed it user
  commands and client commands;
* bots are reconnected (with team and skill) across map changes and restarts;
* `sv_botsYieldSlots` frees a bot slot for a real player on a full server.

Because of these, a patched game module (QVM or .so) needs the patched
server; with an unpatched server the bot commands just report that bots are
unsupported, and the rest of the game works normally.

### Other fixes in this tree

Not about bots, but needed to build or run it:

* `src/sdl/sdl_glimp.cpp`: the video mode list was allocated one entry short
  and the "automatic" mode written past its end. On displays that report each
  resolution once (single refresh rate) this corrupts the heap and the client
  aborts at start-up ("malloc(): corrupted top size").
* `src/qcommon/net_ip.cpp`: Windows builds use `ioctlsocket` instead of
  `fcntl` for non-blocking sockets, and SOCKS `send`/`recv` buffers are
  `char *`, so the engine cross-compiles with MinGW again.
* `Makefile`: the OpenGL 2 renderer DLL links the JSON helpers it uses
  (only Windows' linker noticed they were missing).

## Files

```
src/game/g_bot.h        shared definitions
src/game/g_bot.c        bot slots, console commands, cvars, per-frame driver
src/game/g_botnav.c     navigation graph generation, A*, path following
src/game/g_botai.c      perception, aiming, combat, goals
src/game/g_botbuild.c   team knowledge, shopping, evolving, base building
```

Plus small hooks in `g_main.c`, `g_client.c`, `g_active.c`, `g_svcmds.c`,
`g_local.h`, `g_public.h`, `g_syscalls.c/.asm`, the two build files and the
server sources listed above.

## Limits

* Bots don't use voice chat, don't vote, and don't deconstruct or move
  structures.
* Builders follow a fixed build order; they don't adapt it to what the enemy
  is doing.
* The graph is generated, not hand-tuned: odd geometry (moving platforms,
  doors that need a button, very narrow ledges) can confuse bots. `navgen`
  with a smaller `bot_navGridSize` (e.g. 24) helps tight maps.
* Teleporter and elevator handling is basic.
* On maps with walkways over a void (Uncreation), bots still fall off now
  and then, mostly when pushed around in a fight.
* Generating a graph for a big map takes a few seconds (it is spread over
  frames, and saved for next time). Until it is ready, bots walk in
  straight lines toward what they want and get stuck easily.
