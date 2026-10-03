# GPRL Geode mod (`gmo12.gprl`)

So basically this is a Geometry Dash mod that measures how precise your inputs are. It records presses, releases, deaths, all that stuff, and sends it to the GPRL API so you can get rated. The mod doesn't do the rating itself - the server handles that part.

Built for Geode SDK 5.6.1 on GD 2.2081 (Windows only, sorry). Uses [Argon](https://github.com/GlobedGD/argon) 1.5.1 for login stuff. If you have `syzzi.click_between_frames` installed it'll record sub-tick inputs. Eclipse's noclip/bot/TPS bypass menus get picked up automatically.

## How to connect your account

No accounts to make. Just hook your GD account through the in-game popup (pause menu, hit the GPRL button). We use Argon to prove you own the account, so your GD password never leaves your computer.

The popup shows:
- **Connect** - takes a few seconds while we verify with Argon, then you're in
- **Disconnect** - forgets your device token locally  
- **Open my profile** - opens the website (checks the origin to avoid phishing)
- **Website code** - shows a signin code you can copy and paste into the website instead

If you switch GD accounts, or your token expires, it'll ask you to connect again.

## The menu (v0.13.0)

Two ways to open it: pause menu button or circle on the main menu.

**Shell stuff:**
- Sidebar with 5 tabs: Home, Ranks, Board, Level, Account
- Status chips at the top showing connection state, whether you're recording, etc
- 480 x 300 popup with a dark theme
- Everything updates every 0.5 seconds

**Tabs:**
- **Home** - Shows your rank badge, progress to the next rank, current sigma/s (or calibration % if locked). Has a "what is sigma/s?" info button.
- **Ranks** - The full ladder. Your rank gets highlighted. Shows what you need to rank up.
- **Board** - Top 25 players. Shows your position if you're on it.
- **Level** - Stats for the current level: attempts, deaths, jumps, practice mode stuff, and timing window info. Hit Details for the raw data dump.
- **Account** - Connection status, website signin code, clipping stuff if you have it, mod settings.

Fonts are sized to actually be readable (9px for body text minimum, we're not animals).

## Timing windows (v0.4.0+)

The mod runs a "frame perfect counter" clone engine to measure exactly how tight your inputs are. Here's what it does:

- Creates hidden player clones that mirror your real inputs
- Tests the clones against slight timing variations to find the exact window where you could've hit
- Only reports windows that it's 100% sure about (controls matched perfectly)
- Runs in the background at 0.1-0.4 ms per frame overhead

Every input gets checked. Misses (where you died) get recorded. The result goes in the calibration samples on the server.

### What to look for in the logs

Set `solver-debug` to `verbose` and play a level. Look for lines starting with `GPRL solver:`:
- `ready - pool lazy (max 160)` = things are set up
- `job N pass 0 spawned 21 clones` = testing an input
- `window 4.17 ms [-2.08,+2.08]` = we found the timing window
- `DROPPED mismatch` = something didn't match, we skipped this one
- `5 s summary` = overall stats on what we measured

## Building this

```powershell
# Host tests (pure C++, no GD needed): 32 test suites
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\tests\run_tests.ps1

# Build the mod (MSVC + Geode CLI)
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\build.ps1
```

Needs:
- Portable MSVC from `D:\GeodeMods\_msvc\msvc-env.ps1`
- Geode SDK at `C:\Users\gacue\Desktop\GeodeSDK`
- CPM cache at `D:\GeodeMods\_cpm`

Output goes to `geode/build/gmo12.gprl.geode`.

### MSVC quirk with Argon

MSVC 14.44 crashes with an internal compiler error when Argon's coroutines get precompiled (happens in `src/Connect.cpp` at `arc/future/Pollable.hpp`). Fixed by disabling the precompiled header for the argon target and `src/Connect.cpp`. Keep all `<argon/argon.hpp>` includes in that one file.

## Directory layout

```
mod.json, CMakeLists.txt, build.ps1, about.md
core/                  Pure C++20, no dependencies
  vocab.hpp              Game state enums (gamemode, speed, trust, etc)
  json.hpp/.cpp          JSON parsing & writing, no third-party libs
  telemetry.hpp/.cpp     Event structs and batch validation
  snapshot.hpp           Player state snapshot (mirrors schema.ts)
  crypto.hpp/.cpp        SHA-256, HMAC, base64 (RFC tested)
  ringbuffer.hpp         Single-producer ring buffer
  fingerprint.hpp/.cpp   Timing fingerprint (matches TS engine)
  calibration.hpp/.cpp   Calibration progress tracker
  identity.hpp/.cpp      Account connection state
  ranks.hpp/.cpp         Ladder parsing, sigma -> rank mapping
  classify.hpp/.cpp      Event classification (gamemode, speed, trust, etc)
  geometry_hash.hpp/.cpp Spatial hash of nearby objects
  solver/                Timing window measurement engine
    local_window.hpp     Basic window solver
    pass_planner.hpp     Batch pass orchestration
    sequence.hpp         Sequence-aware windows (advanced)
    window_event.hpp     Payload builder & server validation
    activation.hpp       One-shot activation slot logic
    budget.hpp           Job/clone budgets & adaptive throttling
    diagnostics.hpp      Drop reasons & mismatch tracking
    timing_result_event.hpp  Result struct & validator
src/                   Geode code (game-facing)
  main.cpp              Mod entry
  Hooks.cpp             Game state hooks
  Tracker.cpp           Event tracking
  Telemetry.cpp         Worker & batching
  Api.cpp               API calls
  Connect.cpp           Login (Argon)
  Hud.cpp               Status line drawing
  Popup.cpp             In-game menu (old v0.3.0 version)
  Clipper.cpp           Clip buffer orchestration
  solver/CloneEngine.cpp  The clone engine that does the measuring
tests/                 32 host test suites
```

## What actually gets sent

Events (all timestamped):
- `environment` - loaded mods, trust state, noclip status (sent when things change mid-level)
- `attempt_start` - level starts, practice mode, start position
- `input` - every jump press/release with sub-tick timing
- `state_sample` - player state every 24 ticks
- `gamemode_change` - portal transitions
- `death` - where you died, what object killed you
- `progress` - per-percent progress updates
- `attempt_end` - how the attempt ended
- `timing_window` - measured input timing (from v0.4.0+)

Batches are signed with HMAC-SHA256 and sent every 2 seconds or 500 events (max 4000 events, 1 MB per batch).

Trust state runs on a priority system:
1. Bot playback = don't count (level-only evidence)
2. Physics modded = don't count
3. Noclip on = don't count
4. Unknown mod loaded = warning (might not count)
5. Clean = good to go

Unknown modules are tracked as file name + size hash (actual contents never read).

## Stub features (not finished yet)

- Half-tick input placement without Click Between Frames (windows are tick-resolution only right now)
- Sequence windows exist and are host-tested but haven't run in-game yet
- Local calibration progress shows but doesn't unlock anything - server state takes over when connected
- Clipping buffer exists but the server doesn't accept uploads yet
- Device token saved as plain Geode value (encryption is Phase 2)

## Notes for the other packages

Connect API expects this shape:
```json
{
  "accountId": 12345,
  "argonToken": "...",
  "clientBuild": "gprl-geode 0.2.0+win",
  "modList": [{ "id": "...", "version": "..." }],
  "userId": 12345,
  "username": "Your GD Name"
}
```

Response should be:
```json
{
  "deviceToken": "...",
  "playerId": "...",
  "username": "...",
  "displayName": "...",
  "identityVerified": true
}
```

Website signin code endpoint needs to return a 20-char base32 code + expiry time.

## v0.5.1 additions

- HUD history panel (last 8 inputs, middle-right)
- Level analysis coverage display (from the server, per MASTER §20)
- Bot playback now tagged as "level-only evidence" (not counted for your rank)
- Attempt timing capture (active/practice/startpos milliseconds)

## v0.6.0 additions  

Clipping buffer that captures the last 2 minutes of gameplay and lets you save/share clips. Design doc at `docs/CLIPPING.md`.

- Rolling buffer at 60 fps -> 1-second H.264 segments
- Preserve clips on legit completions or manually with "Clip last attempt"
- Save to computer or send to moderators
- Microphone + game audio capture
