# GPRL Geode mod (`gmo12.gprl`)

The Geometry Dash client of GPRL (Geometry Precision Ranking List). It records inputs, deaths,
progress, gamemode changes and player-state samples as reproducible `gprl.telemetry/1` evidence,
shows calibration progress, and sends signed batches to the GPRL API (or keeps them on disk). The
server computes every rating; this mod never does (ARCHITECTURE §3).

Targets: Geode SDK 5.6.1, GD 2.2081 (Windows). Optional dependencies: `syzzi.click_between_frames`
(sub-tick inputs are recorded when it is active), `eclipse.eclipse-menu` (noclip / bot / TPS bypass
state is read through its public API). Linked library: [Argon](https://github.com/GlobedGD/argon)
1.5.1 (CPM, static) for the GD-account Connect. The optional clipping buffer (v0.6.0, off by
default, "v0.6.0" below) needs an `ffmpeg.exe` and links `opengl32 shell32 ole32 avrt`.

## Connecting (v0.2.0): the GD account is the identity

Players never make a website account (no email, no password). In the pause-menu GPRL popup:

- **Connect** (shown while not connected): needs a logged-in GD account
  (`GJAccountManager::m_accountID > 0`, else the notification "Log in to your Geometry Dash account
  first (Settings > Account)"). `argon::startAuth()` proves ownership of that account (Argon sends
  a GD message / profile field challenge; the GD password / GJP never leaves the game; Argon caches
  its token globally for all mods). It runs on Geode's async runtime and can take several seconds;
  the popup shows `Connecting... (<Argon step>)`. Then the telemetry worker sends
  `POST /v1/client/connect { accountId, userId, username, argonToken, clientBuild, modList }`;
  the server validates the token with Argon, finds or creates the player for that GD account and
  answers `{ deviceToken, playerId, username, displayName, identityVerified }`. The device token is
  saved (`device-token`) together with the GD account id it was issued for (`gd-account-id`), plus
  `player-id`, `username`, `display-name`, `identity-verified`. Success: notification
  `Connected as <displayName>`. Argon failure: a dialog with Argon's message and Retry. Server
  `401` with `details.reason` `gd_identity_invalid`: `argon::clearToken(accountId)` (drops a stale / mismatched Argon token)
  and a Retry dialog. No dialog is ever shown over an unpaused level (a notification instead).
  Reconnecting (same GD account, same or another PC) returns a new device token for the same
  player; the GD name is refreshed on every verified connect.
- **Disconnect** (shown while connected, asks first): forgets the device token locally.
- **Open my profile**: `POST /v1/client/web-login` (Bearer device token) -> `{ url, expiresAt, code }`,
  a one-time sign-in link (single use, 300 s). The mod opens it with
  `geode::utils::web::openLinkInBrowser` only when `core/identity profileUrlAllowed(url, origin)`
  accepts it: `url` must start with exactly `<origin>/`, where origin is the normalised `site-url`
  setting (default `https://gprl.pages.dev`); look-alike hosts, other ports, `user@host`, spaces,
  control characters, quotes and backslashes are refused. The URL is never logged (it carries the
  one-time code).
- **Website code** (v0.2.1, shown while connected, next to Open my profile): the same
  `POST /v1/client/web-login`, but instead of opening the browser the popup shows the response's
  `code` in a "Website sign-in code" panel: the code (`XXXXX-XXXXX-XXXXX-XXXXX`, 20 Crockford
  base32 characters, chatFont, fitted left of the buttons), a **Copy** button
  (`geode::utils::clipboard::write`, notification "Copied - paste it into the website's sign-in
  box"), a countdown `expires in 4:59` driven by the popup's 0.5 s tick, and the hint
  `Paste it at gprl.pages.dev (Sign in)` (the `site-url` host). At 0 the code turns grey with
  `expired - press Website code again` and Copy is disabled. **Hide** restores the session /
  counter lines the panel covers. The panel keeps the code while the popup is closed and reopened
  (it lives in `Connect.cpp`, main thread; Disconnect forgets it). The request is asynchronous
  (telemetry worker, never the game thread); one web-login request at a time, and
  `s_webLoginUse` remembers whether Open my profile or Website code asked. The mod shows / copies
  only a value that `core/identity formatWebLoginCode` accepts (a server from before `code`
  existed is handled through the url's `#code=` fragment; the old 43-character format shows
  "the server did not send a usable code"), and never logs it. Pure rules in `core/identity`
  (host-tested): `normalizeWebLoginCode` / `formatWebLoginCode` (uppercase, ASCII spaces and
  dashes dropped, `O -> 0`, `I`/`L -> 1`, exactly 20 alphabet characters), `webLoginCodeFromResponse`,
  `parseIsoUtcSeconds` + `webLoginCodeSecondsLeft` (clamped to 0..300 so a skewed clock never
  shows more than the TTL; unparseable = 300 from now), `formatCountdown` (`m:ss`),
  `webLoginCodeStatus`, `webLoginCodeHint`.
- **Account switch**: a token is only used while the GD account it was issued for is logged in
  (`core/identity connectionState`, host-tested). Another GD account, a GD logout or a v0.1.x
  link-code token (no `gd-account-id` saved) means "not connected": levels are spooled as
  `unsent`, the popup explains why and offers Connect. The server calibration of the old account
  is not shown either.
- Link codes are gone from the mod (the `link-code` setting was removed; the API still accepts
  `POST /v1/client/link` for old clients).

## The in-game menu (v0.13.0 redesign)

`src/ui/` replaces the v0.3.0 tabbed popup (`src/Popup.cpp`, removed; its v0.12.2 Patreon row
lives in `PageAccount.cpp`'s Connection card). Same two entry points (pause-menu `GPRL` button, main-menu circle
button, `Hooks.cpp` -> `gprl::ui::open()`), same data sources and the same actions behind every
button (`Connect.cpp`, `Clipper.cpp`, `client::requestSiteData`); only what the player sees is new.
Design: a new player should understand what GPRL is and what to do next without reading the docs,
and every number should look like a number, not a log line.

**Shell** (`Menu.cpp`, sizes in `Theme.hpp`, drawing pieces in `Widgets.hpp`): a 480 x 300 popup on
GD's blue square with two dark navy panels - a **sidebar** of five icon tiles (Home, Ranks, Board,
Level, Account; the selected tile is tinted cyan with a left accent bar; the mod version sits
under them) and the **content area** (380 x 252) under a header strip with the screen title and
the **status chips**: `Connected` / `Not connected` / `Connecting...`, a red `LIVE` chip exactly
when `display::liveNow()` is true (the same rule as the HUD tag), `Offline`, `Local-only` /
`API not set`, `Record-Safe`, `Mod disabled`. The popup reopens on the screen it was closed on
(remembered while the game runs); the first open lands on Home. Every screen is rebuilt only when
the text it would show changed (a per-screen key compared on the 0.5 s tick), so buttons stay put
under the cursor and the scroll lists keep their position; the sign-in code countdown is updated
in place.

- **Home** (`PageHome.cpp`). Not connected: a **welcome screen** - the sigma/s logo, one sentence
  on what GPRL measures, three step cards (`1. Connect` / `2. Play` / `3. Get ranked`), one big
  green **Connect with my GD account** button, the connection state (or the last `Connect failed`
  reason, or the local-only / API warning) in one line, and an info button that opens **What is
  sigma/s?** (plain-words explanation; the mod measures, only the server rates; LOCKED until the
  calibration completes, rank at the ladder's confidence minimum). Connected: the **rank badge**
  (92 px) with the band name in the rank colour and the **progress bar to the next band**
  (`to Master I  47%`, server `rankProgress` or the ladder), the GD name with a `GD verified`
  chip, then either the **sigma/s hero** (the headline figure in 0.95 bigFont, a `94% confidence`
  chip, the verification chip - `Auto-Verified` green / `Pending Manual Verification` gold /
  `Unverified` orange / `Invalid` red - the provisional label, `raw / practical`, the private
  estimate line, leaderboard place + verified runs) or, while locked, a **calibration ring**
  (cyan arc, percent in the middle) with `N of 700 effective timings - N of 3 gamemodes`, the
  private estimate when the server sends one, and a **What to play next** card: the server's own
  requirement words through `calibrationHint` (effective samples, gamemodes) plus plain advice on
  releases and playing near the limit. Along the bottom the **Gamemodes strip**: the eight garage
  icons (lit once the gamemode has a verified sigma/s), the figure or the calibration percent, a
  bar (green = sigma / 300, blue = calibration progress).
- **Ranks** (`PageRanks.cpp`): the owner's disclaimer as a gold strip on top (`What the level
  ranges mean` + one line; the info button opens the full wording: beatable in about 10,000
  attempts if the level were 100 seconds long, not a fast or first-try beat) and the ladder as the
  server lists it: 56 px rows (38 for Ascendant tiers) with the badge, the name in the rank colour,
  `III 0-20  II 20-40  I 40-60 sigma/s`, `Needs: ...`, the description; the player's row is tinted
  in its rank colour with a left accent bar and a `YOU ARE HERE` chip; unreached Ascendant tiers
  are dimmed with an `Unreached` chip; `n players` on the right when the server counts them.
- **Board** (`PageBoard.cpp`): 24 px rows - `#1` with GD's `rankIcon_1_001.png` medal, `#2` /
  `#3` in silver / bronze, the badge, the name (green on the player's own tinted row), the band
  name in the rank colour, the sigma/s - and a footer `Your position: #n` / "Not on the board
  yet ..." / "Connect (Account) to see your own place here".
- **Level** (`PageLevel.cpp`): the level card (name, `ID n - playing now / last played`, chips:
  `LIVE` / `Offline: kept on disk` / `Local-only`, `On the levels list` / `Feeds calibration only`
  for Remote sessions, `Practice`, the trust word when not allowed) with a **Details** button;
  eight **stat tiles** (attempts, deaths, jumps, best %, completions, time playing, in practice,
  noclip would-be deaths or the GD save's attempt count); the **Timing windows** card (the
  solver's status line, `Inputs with a window: 76%` + bar, `n measured  n misses  n dropped
  (kinds)  n local samples`); the **Level analysis** card (analyzer status, the server's
  `Level Analysis Coverage` line, the family / job line). **Details** opens `DetailsPopup`: every
  raw line the old Session + Account tabs printed (jumps / releases, attempts, presses per
  gamemode, practice / percent / noclip / active time, the hook check, trust, session, batches,
  server ratable + level counts, last error, calibration counters + source, coverage, the solver's
  three lines, the trace line, the analyzer's lines, the analysis mode, API / site, spool) in a
  scrollable list that follows the game every 0.5 s.
- **Account** (`PageAccount.cpp`), four cards: **Connection** (state dot + `identity::describe`
  line, the failure reason / API + site / local-only warning under it, Connect or Disconnect;
  while connected the v0.12.2 plan line `Plan: GPRL Pro (Patreon)   Last synchronized: 2 min ago`
  with **Connect Patreon** + **Enter Patreon code**, or **Sync Patreon** once linked, at its right end),
  **Website** (one sentence, `Open my profile` / `Website code` / `Visit website`; with a code
  requested the card becomes **Website sign-in code**: the code in 0.72 chatFont, the countdown +
  `Paste it at gprl.pages.dev (Sign in)` hint updated in place, `Copy` / `Hide`), **Clipping**
  (buffer line, the newest clip's state with the upload progress, the clip buttons that apply:
  `YouTube link`, `Clip last attempt`, `Clip choice`, `Retry upload`, `Clips folder`; a hint when
  clipping is off), **Mod** (analysis mode line, session + batch counters, last error or the spool
  count; `Settings`, `Flush`, `Reset data` (connected only)). Nothing here networks on the game
  thread; every action is the v0.2.1 - v0.10.0 one.

**HUD** (`Hud.cpp`, same texts and rules as v0.12.0): the top-right sigma/s panel, the `LIVE` tag,
the middle-right timing-window history, the trace panel, the verification toast and the family
notice are now rounded dark nine-slice panels (`square02b_001.png`) instead of flat rectangles.

**Widgets** (`Widgets.cpp`): `text` (clipped label), `panel` / `card` (tinted nine-slice),
`badge` (the v0.3.0 badge drawing, moved), `ring` (CCDrawNode arc gauge), `bar`, `chip`, `tile`,
`paragraph` (`SimpleTextArea` word wrap), `button` / `wideButton` / `iconButton`, `gamemodeIcon`
(GD's `gj_*Btn_off/on_001.png` garage frames), `frameSprite` / `icon` (never
`createWithSpriteFrameName` on an unknown frame). Fonts: `goldFont.fnt` titles, `bigFont.fnt`
numbers / names, `chatFont.fnt` body; every label goes through `limitLabelWidth`.

## The in-game menu (v0.3.0)

Opened from the pause-menu `GPRL` button and from a new **main-menu button** (`MenuLayer`: a cyan
circle with `gprl_icon.png` added to the `bottom-menu` node Geode's own MenuLayer hook creates;
a "GPRL" label when the sprite is missing; an own `CCMenu` at (30, 30) when there is no
`bottom-menu`). Both open the same tabbed `GprlPopup` (`src/Popup.cpp`), 460 x 300, replacing the
v0.2.x status popup. Everything shown comes from cached state; the popup never networks on the
game thread. It re-reads on a 0.5 s tick, which also asks the telemetry worker for stale site
data and drives the website code countdown.

Layout (m_mainLayer coordinates, y up, 460 wide): title at ~280; the **tab row** at y = 254, five
78 px `ButtonSprite`s at x = 24 + 84 i (green `GJ_button_01` for the selected tab, grey
`GJ_button_04` for the others); the **content panel** from y = 40 to 240 (x 14..446); scroll
lists (`ScrollLayer`, `createDefaultListLayout(3)`) at (18, 44) with 424 x 194; the bottom row at
y = 22 holds the Account buttons (Account tab) or a one-line grey hint (other tabs). Fonts:
`bigFont.fnt` for the GD name / headers, `goldFont.fnt` for rank names and the jumps line,
`chatFont.fnt` for everything else; every label goes through `limitLabelWidth`. GD's bitmap
fonts have no Greek glyphs or en dashes: the menu writes `sigma/s` and `0-20`.

- **Profile**: `GET /api/players/<username>` (the connected GPRL username, cached 60 s; "Loading..."
  until it arrives; a 404 shows "No GPRL profile for X yet"). GD name (bigFont 0.55 at (166, 224),
  clipped to 190 px) with `GD verified` / `GD not verified` next to it (`identityVerified`); the
  **rank badge** at 120 px centred on (86, 146) with the division numeral (I / II / III) or the
  Ascendant tier numeral drawn on a dark pill at its bottom, or `rank_locked.png` + "Unranked";
  then lines from y = 208: the band name in the rank colour (`Master II`), `Verified sigma/s:
  LOCKED` + `Calibration 12%` while `ratings.availability == "locked"` (SPEC §10: never a figure
  while locked, even if a summary field carries one), else `Verified sigma/s 159.7   confidence
  94%` and `raw / practical`; `Progress to Master I: 47%` (the server's `rankProgress
  {nextName, percent}` when present, else computed from the ladder and the verified sigma/s per
  docs/RANKS.md); `Unverified equivalent: <rank>` (`unverifiedRankEquivalent`, string or
  `{rankId, division}`); `Competitive Verified` / `Not yet verified` (`competitiveVerified`,
  `verificationStatus` or `verification.status`, all optional - the deployed API sends none of
  them today); `Leaderboard #n` / `Not on the leaderboard yet` + verified runs. Below, the **8
  gamemode columns** (cube..swing, x from 166 across 274 px): the verified sigma or `-`, a 6 px bar
  (green = sigma / 300, blue = `calibrationProgress` while locked) and the gamemode name. Not
  connected: "Not connected" + the reason + a **Go to Account** button.
- **Ranks**: the ladder from `GET /api/ranks` (cached 10 min), **in the response order, nothing
  hardcoded** (docs/RANKS.md: thresholds are server configuration). `core/ranks parseRankList`
  accepts the current shape (`RankDefinition[]` with `divisionThresholds[{division,
  minVerifiedSigma, maxVerifiedSigma}]`) and the announced next shape (`{ ranks: [{ kind:
  'divisions' | 'single' | 'ascendant', divisions?: [{division, minSigma, maxSigma}], minSigma,
  maxSigma, tier?, reached?, requirements }], eligibility }`); a missing `kind` is `divisions`
  when division thresholds are listed, else `single`; an id / iconKey starting with `ascendant`
  or `kind: 'ascendant'` is an Ascendant tier (shared `rank_ascendant.png`, tier numeral from
  `tier`, the id's number or the position among the tiers; the name gets the numeral appended
  when it is just "Ascendant"). Rows (fixed heights, scrollable): 58 px for division / single
  ranks - badge at 40 px, name in the rank colour (goldFont 0.45), the bands line `III 0-20  II
  20-40  I 40-60 sigma/s` (or `175-210 sigma/s` for a single band, `175+` open-ended),
  `Needs: min confidence 90%, 4 gamemodes rated, 30 verified runs`, the description; 42 px for
  Ascendant tiers - `Ascendant IV   402-442 sigma/s` + the requirements. The player's rank row is
  tinted with the rank colour and tagged `YOU ARE HERE - Master II` (the official rank from the
  profile); an Ascendant tier with `reached: false` is dimmed (opacity 110 / 90) and tagged
  `Unreached`; other rows show `n players` when the server counts them. The list is rebuilt only
  when the data generation changes, so scrolling is not reset by the tick.
- **Board**: `GET /api/leaderboard?limit=25` (cached 60 s). 26 px rows: `#pos` (goldFont), the
  badge at 24 px (x 62), the display name (bigFont 0.35, clipped to 200 px), the band name in the
  rank colour and the sigma/s right-aligned; the connected player's row is tinted green. Empty
  state (the live board today): `No verified ratings yet - the first players are still
  calibrating.`
- **Session** (what "counting" looks like): live counters of the current / last level from
  `tracker::session()` (`Tracker.hpp SessionCounters`, updated on the game thread, kept after the
  level is left, reset on the next entry): `Jumps (presses) N    releases N` (goldFont), attempts
  / deaths / would-be deaths / completions, presses per gamemode (of the pressing player's mode),
  practice / percent / best / noclip now / noclip seen, a **hook check** line `handleButton N /
  pushButton (P1) N presses` (the mod records inputs from `GJBaseGameLayer::handleButton`; a new
  diagnostic hook on `PlayerObject::pushButton` / `releaseButton` counts the game's own path for
  player 1 only - one handleButton press pushes both players in dual mode - so "pushButton > 0
  while handleButton = 0" is shown in red as `INPUT HOOK NOT FIRING, please report` instead of
  silently not counting), trust, the session / batch counters, and the honest
  line `Timing windows: not measured yet (solver coming in a later update) - sigma/s stays locked
  until then`.
- **Account**: the v0.2.1 content unchanged in behaviour - calibration + source line, connection
  line, API + site line, session / batch / ratable / last error / spool lines, the **Website
  sign-in code** panel over the lower 70 px (58..128) with Copy / Hide and the countdown, and the
  button row Connect | Disconnect / Open my profile / Website code (connected only) / Flush /
  Settings at y = 22 (scaled down if the font metrics would overflow). The popup opens on
  Profile when connected and on Account otherwise.

**Badges**: `resources/ranks/rank_<iconKey>.png` (bronze, silver, gold, platinum, diamond,
master, grandmaster, elite, apex, one `rank_ascendant.png` for every Ascendant tier),
`rank_locked.png` (no rank) and `gprl_icon.png` (64 px main-menu icon), declared as mod.json
`resources.sprites` and loaded through `Mod::expandSpriteName` (`CCSprite::create` of the
resource file, or a sprite frame of that name when a sheet provides one). A missing sprite falls
back to a `CCDrawNode` hexagon in the rank colour (grey with a `?` when there is no rank); the
numeral is drawn either way.

**Networking**: `api::getRanks`, `getLeaderboard(limit)`, `getPlayer(username)` (no
Authorization header) run on the telemetry worker like web-login: the popup calls
`client::requestSiteData(kind[, username])` on its tick; the worker fetches when the per-endpoint
cache is older than its TTL (ranks 10 min, board 60 s, profile 60 s) and never more than once per
10 s per endpoint (errors included), results land in `client::siteData()` (mutex copy with a
generation counter per endpoint). Local-only mode / a placeholder API URL fetch nothing and the
tabs say so. Disconnect drops the cached profile.

**HUD** (`Hud.cpp`): the bottom-left line is now `GPRL: jumps 42 | attempts 7 | sigma/s LOCKED
(calibrating 12%)` (+ ` (local)` until the server calibration arrives, ` | trust: ...`,
` | local-only` / ` | not connected` / ` | offline`), so it visibly counts; the `show-hud` setting
still hides it.

**core/ranks** (pure, host-tested by `tests/menu_tests.cpp`, 414 checks incl. hostile-JSON
suites: wrong types, +-inf / 1e300, missing arrays, empty strings, repeated divisions): `parseHexColor`,
`scaled`, `parseRankList` (both shapes, tolerant of malformed entries), `placeSigma` (sigma ->
rank / division over the runtime ladder, `[lower, upper)` bands), `findRank`, `bandOf`,
`progressOf` (docs/RANKS.md formula: to the next band in ladder order, Ascendant tiers included,
nullopt at the top), `bandName`, `numeral` (Roman, up to 3999), `divisionNumeral`,
`badgeNumeral`, `formatSigma` (one decimal, `68.3`), `formatRange` (`60-85`, `300+`),
`formatBands`, `formatRequirements`, `parseProfile` (the deployed LOCKED answer and the optional
next-shape fields), `profileProgress`, `parseLeaderboard` (flat `PlayerSummary + position` rows
and nested `{ player }` rows), `percentText`.

## The timing-window solver (v0.4.0, Phase 3)

Design: `docs/SOLVER_DESIGN.md` (decisions D1-D9), physics facts: `docs/GD_PHYSICS_NOTES.md`.
Before v0.4.0 `src/solver/GdOracle` was a stub returning `Invalid`, so no `timing_window` event
ever existed, the calibration had no samples and sigma/s stayed LOCKED ("no samples are coming in").

**What runs now.** `src/solver/CloneEngine` is the frame-perfect-counter engine (same author,
proven in-game: zero shadow mismatches when the model is right) ported without its HUD, manual
tags, macro player, auto-bot and physics lock:

- **Timeline**: `GJBaseGameLayer::processCommands` (pre-hook) is one step of 1 or 0.5 frames
  (0 when physics did not run); player 1 is snapshotted field by field (`PlayerFields.inc` +
  `CopyFields.inc`, verbatim FPC, plus the hitbox block) into a ring of 1024 steps (~2 s) BEFORE
  the step's inputs are processed. `PlayerObject::update` deltas (1/60 s units, 0.25 per tick)
  give each step's real delta; a GD half tick is 0.5 frames; Click Between Frames splits appear as
  fractional input frames.
- **Inputs**: every player-1 jump press / release is logged in `PlayerObject::pushButton` /
  `releaseButton` (pre-hook, before the button state changes; FPC-proven: every input path ends
  there) with a pre-input `PlayerStateSnapshot`; the telemetry `input` event still comes from
  `handleButton`, whose post-hook links the job to that event's `seq` (`bindLastInput`). An input
  that never reaches `handleButton` (a bot calling the player directly) is dropped as `unbound`.
- **Clones**: hidden `PlayerObject`s (pool of at most 160, created lazily) stepped inside the game's
  own loop in the game's own order - inputs, `resetTouchedRings(false)`, collision-log reset,
  `updateInternalActions`, `update(dt)`, `checkCollisions`, `updateRotation` and the
  end-of-step bookkeeping - with per-clone activation flags of orbs / pads / portals swapped in
  and out (`beginSwap` / `endSwap`), the anti-cheat spike ignored, deaths claimed in
  `PlayLayer::destroyPlayer` (`Priority::First`, before any noclip menu), and every side effect
  guarded (`src/solver/CloneHooks.cpp`: triggers, pickups, sounds, streaks, checkpoints, level
  complete, dual / teleport portals make the clone `Invalid`).
- **Search**: per input a job runs the pure `core/solver/pass_planner` (BoundarySearch as batched
  passes): pass 0 spawns a control clone (shift 0) plus shifts of +-1..10 ticks on each side
  (aligned to whole-tick boundaries when the input sat on a half tick; clipped to the history and
  to the previous / next same-channel input minus 0.01 ms, the late neighbour applied as it is
  logged); a shifted clone passes when it re-synchronises with the control for 16 steps or is alive
  at the horizon (0.5 s + 10 ticks), fails when it dies; clones more than 2 ticks beyond the first
  fail on a side are cancelled (BoundarySearch island scan). With CBF active (`solver-subtick`),
  1 or 2 refinement passes place 7 shifts inside each bracket (1/8 tick = 0.52 ms, 1/64 tick =
  0.065 ms). The window uses midpoint edges (a frame perfect = 4.17 ms [-2.08, +2.08]),
  `resolutionMs` = the widest bracket.
- **Only exact windows are emitted** (D7): the control clone is compared with the real player every
  step (`statesMatch`: position 1e-3, y velocity 1e-6, flags, size, speed, held buttons, rings);
  any difference drops the window with the first differing field in the log. A real death: the
  control must die on the same object at the same frame (then the input is a **miss** and the
  surviving shifted clones finish during the death pause with synthetic whole ticks, tagged
  `[ext]`); under noclip a would-be death counts the same way and the world keeps running.
- **Shadow clone**: replays every logged input from the previous step's snapshot and is compared
  with the real player every step; up to 6 `GPRL shadow mismatch` warnings per attempt name the
  step and field. Zero mismatches = the model reproduces that attempt exactly.
- **Emission**: `core/fingerprint_build` (gamemode, speed, gravity, mini, yVelocity = raw * 60
  gravity-normalised, trajectory / horizontal state, gaps to the previous / next input, hold, first
  portal within the horizon, `core/geometry_hash` of the non-decoration objects in [x-60, x+300]
  quantised to 15 units on a 30-unit anchor) + `core/solver/window_event` (payload, hold fields
  for releases, `solverVersion` `gprl-clone/1` or `gprl-clone/1-cbf` when a sub-tick pass
  refined it) -> `client::push` like any event (a `timing_window` may follow `attempt_end`) ->
  a local `CalibrationSample` (HUD `samples N/700 (local)`, Session tab counters). The payload is
  checked against a C++ mirror of the server's `windows.ts` gate before it leaves.
- **Gate** (`refreshGate`, at every attempt start and 0.5 s environment poll): paused under Eclipse
  Physics Bypass at a TPS other than 240 and speedhack (the session is unratable anyway); bot
  playback measures since v0.5.1 (level-only evidence, see "v0.5.1" below); noclip and unknown
  mods measure; platformer levels are unsupported; dual mode, a dead
  player, a slow frame (dt > 1/30 s), sim load above 4 ms/frame (until it drops below 2) and more
  than 8 open jobs skip the input (logged + counted). The ring keeps running while paused.
- **Settings** (`mod.json`): `measure-windows` (on; REMOVED in v0.12.0 - measuring cannot be turned off, only
  Record-Safe Mode stops the live solver), `solver-subtick` (`off` / `1/8 tick` /
  `1/64 tick`), `solver-debug` (`off` / `windows` / `verbose`; `debug-log` forces verbose),
  `max-shift-ticks` (10), `horizon-seconds` (0.5).
- **Server**: `api/src/processing/config.ts` allowlists `gprl-clone/1` and `gprl-clone/1-cbf` for
  `clientBuild` `gprl-geode 0.4+win` (resolution 0.01-4.2 ms), so `timing_samples.rated` follows
  the usual trust rules for these windows (SECURITY.md §4, §9: still not re-derived server side).

**In-game log checklist** (set `solver-debug` to `verbose` or turn on `debug-log`, play a level,
then send the Geode log): every line is prefixed `GPRL solver:`.

- [ ] at level entry: `snapshot roundtrip OK` then `ready - pool lazy (max 160), history 1024
      steps, cbf=.. subtick=.., N level objects, gate=measuring, snapshot roundtrip OK`. A
      `snapshot roundtrip FAILED (<field>)` line means the ring cannot be trusted: nothing is
      measured on that level (Session tab says `failed: ...`) - report the field.
- [ ] after the first jumps (verbose): `input #k press t=.. tick=.. frame=.. step=.. x=..: job N
      (seq S, early limit .., late pending)`, `job N pass 0 spawned 21 clones (..)`, then within
      0.5-1 s `job N pass 0 resolved in .. steps: -10D@1.5#8 ... +0A +1A +2D@.. | control exact,
      drift 0.000` and the window line `input #k press (seq S) t=..: window 4.17 ms [-2.08,+2.08]
      bounded 240tps res 4.17 ms control=exact -> emitted seq .. (gprl-clone/1)`.
- [ ] at level entry (v0.5.0): `pool warmed - 48 clones in .. ms ...; real player uid 1 ->
      activation slot 1, clone uids from N -> slot 2 mirrored` before the ready line (a real uid
      other than 1 is handled and logged; report it).
- [ ] `5 s summary - inputs .., windows .. (misses ..), coverage P% of N windowable, dropped ..
      (mismatch .. [rings a, speed b, ...], invalid .., pool .., budget .., history .., blocked ..,
      unbound .., other ..), not windowable .. (death unrelated .., restart .., level end ..),
      skipped .. (dead .., dual .., paused .., throttle .., slow frame .., jobs .., other ..),
      deferred spawns .. (expired ..), shadow N steps / 0 mismatches, sim x.x ms/frame (peak y.y,
      z us/step, budget B steps/frame), ..` - `shadow .. / 0 mismatches`, coverage >= 80 % on a
      busy level and sim under 2 ms/frame are the health signs (`peak` = the worst single rendered
      frame: a hitch the average hides, expected under ~8 ms now that clones are created 3 per
      frame; a `GPRL shadow mismatch #1 at step ..: <field> | real .. | shadow ..` line names what
      to chase; `rings`/`speed` mismatch counts should be 0 in v0.5.0 - if not, send the log).
- [ ] a press held through the level start or mashed after a death: `input #k .. skipped: player
      dead` / `level not running` lines (never logged into the replay), and a tap shorter than one
      tick: `job N pass 0 spawned` with the late side blocked (the release is a neighbour at
      distance 0). With CBF, a `job N pass 1 unusable (...), coarse window kept` line means the
      refinement re-run could not be trusted and the tick-resolution window was emitted instead.
- [ ] `DROPPED mismatch (step ..: <field> .. | real (..) | control (..))` lines: the control clone
      diverged from the real player; a few per level around moving objects / triggers are expected,
      many mean a model gap (send the log).
- [ ] with CBF on: `job N pass 1 (0.125 tick) spawned 15 clones: early [..] late [..]` and
      `gprl-clone/1-cbf` with `res 0.52 ms` on the window lines.
- [ ] a death: the fatal input's window line says `MISS (control died with the real player, finished
      in the death pause) .. [ext]`; `attempt done - N inputs, M emitted (K misses), D dropped, ..,
      shadow 0/T mismatches` at the restart.
- [ ] Session tab: `Timing windows: measuring live (gprl-clone/1, sim x.x ms/frame)` and `Windows
      this level: N emitted (M misses, S local samples), D dropped (...)`; HUD `samples N/700
      (local)` growing (local / unsent modes; with a real server the calibration line switches to
      the server's state at the next level entry).
- [ ] with `debug-log`: `GPRL telemetry: batch seq .. sent (.. events: input .., timing_window ..,
      ..)` lines; `local` spool files contain `"kind":"timing_window"`.
- [ ] with Eclipse Physics Bypass at e.g. 2400 TPS: `gate paused: Eclipse Physics Bypass at 2400
      TPS (set 240 or turn it off)`, the Session tab shows the same reason, no windows; setting it
      back to 240 logs `gate=measuring` without a restart.

**v0.7.0 in-game check** (timing solver v2, `docs/TIMING_SOLVER_V2.md` §6; nothing below has
been seen in a real log yet). Procedure: `solver-debug` = `windows`, `solver-trace-max-ticks` =
10 (and `solver-trace-overlay` on for the drawing items), play Deadlocked (main level 20) from 0 %
with at least one completion and a few deaths in the 13-17 % wave, then send the Geode log. Every
item names the line to search for:

- [ ] level entry: `GPRL solver: ready - solver gprl-clone/2, sa gprl-clone-sa/1, status
      gprl-timing-status/1, pool .. warm (..), jobs max .., budget=gprl-clone-budget/3 (..),
      history 2048 steps (x.x MB), cbf=0 subtick=off, N level objects, gate=measuring, sa=on,
      joint-share=off, trace <= 10 ticks, snapshot roundtrip OK, ..`, then `GPRL sa: on -
      gprl-clone-sa/1 (..)` and `GPRL sequence: off (measure-joint-share setting) - ..`.
- [ ] every measured input ends in exactly ONE `GPRL timing: input #<k> <press|release> (seq <s>,
      job <j>) t=<t> tick=<tick> x=<x> pct=<p.ppp>: local <w> ms [<e>,<l>] (<f> f) early <edge>
      late <edge> | sequence .. <pair|chain2|chain3 joined by +, or isolated|local> <decided|undecided
      (early <stop>, late <stop>)> | pair .. |
      hold .. | status <status> (<reasons>) | sims <b> (+<c> controls) | cluster <id> #<i> prev
      <y|n|?> next <y|n|?> -> <emitted seq N | NOT SENT (..)>` line and one `GPRL timing json:
      {..,"kind":"timing_result",..}` line for the same input. `#<k>` is the input's index in the
      attempt (no longer the job id), `pct` has 3 decimals (never seconds, AUDIT §2). A bounded
      edge reads `fail@<shift ticks> self|downstream <k>|extension`. Check (AUDIT §5, §12): the late
      edges of the 13-15 % wave read `downstream`, their `sequence` window is wider than the local
      one and `decided`; a wave timing of 4-10 frames whose sequence side is `undecided` must show
      `status sequence_dependent`, never `ok`. A `[fallback: ..]` suffix means the payload failed
      the C++ validator mirror: send the log.
- [ ] against the live Worker (telemetry revision 3 today) every `GPRL timing:` line ends `NOT
      SENT (this server validates telemetry revision 3, timing_result needs 4)` while
      `timing_window` keeps being sent; the `GPRL timing json:` lines are written either way.
- [ ] sequence-adjusted jobs: `GPRL sa: job N queued: inputs #a..#b (m), narrowest local .. ms, base
      step .. (.. steps old), ..` then `GPRL sa: job N done: controls exact (A + B steps), <negative
      control>, .. trials / .. clone steps over .. real steps (.. ms) | ..`; drops read `GPRL sa: job
      N DROPPED <kind>..`. Most Deadlocked jobs should finish `controls exact`; many `control` /
      `negative` drops mean the delayed replay is wrong there: send the log.
- [ ] the 5 s summary ends with `| results N (sent S, fallback F): statuses ok a, low b,
      unresolved c, replay_failed d, seq_dependent e, no_effect f; local edges self s / downstream
      d / ext x; sa candidates n (+i decided without a replay), measured m, undecided sides u,
      dropped k (..), not started q (..), waiting w, .. trials / .. clone steps; skipped replay r`.
      `fallback` stays 0; `sa measured / candidates` in the dense wave tells whether the SA
      budget keeps up; `sim .. ms/frame` stays under ~4 ms (no `throttled=1` most of the time).
- [ ] at a restart: `GPRL solver: attempt done - .., N timing results (F fallback), input
      placement: a whole-tick, b half-tick, c sub-tick` (without CBF the half-tick count is an open
      design question: report the numbers).
- [ ] after completing the level: no `GPRL shadow mismatch` line and no `DROPPED mismatch` for the
      last inputs (the shadow stops at the finish line).
- [ ] a death in the wave: the fatal input's `GPRL timing:` line reads `status low_confidence
      (miss, ..)`, or `status sequence_dependent (miss_downstream, miss, ..)` when one of its
      bounds was set by a later input (`downstream`) or could still be moved by the next input
      (`timing_status.hpp` statusOf: a miss never gets a sequence window, so it is never `ok`); its
      `GPRL solver: input #k .. window .. res <r> ms` line shows the true bracket as `res` (e.g.
      `20.83 ms`), never one clamped to the width. v0.7.1: exactly ONE such line per death (the
      LATEST input of the died run with a passing shift); the earlier open jobs of that death read
      `GPRL solver: input #k .. MISS downstream - a later input of the died run is the miss; no
      timing_window, timing_result only` and their `GPRL timing:` line `status sequence_dependent
      (miss_downstream, ..)` WITHOUT `miss`; with more than one candidate the engine logs `GPRL
      solver: death at frame F: n MISS candidate(s), the miss is job J (the latest input), n-1
      miss_downstream`. The attempt line's `misses` must be <= the attempt's real deaths.
- [ ] only on a level whose replay cannot be trusted (e.g. "Eon Startpos 240hz"): one warning
      `GPRL solver: replay broken this attempt (N shadow mismatches in M steps, first: <field>) -
      measuring paused until the restart`. It must NOT appear on Deadlocked.
- [ ] traces (AUDIT §11): for windows at most 10 ticks wide (at most 8 per attempt) a `GPRL trace:
      input #k <press|release> seq .. t=.. tick=.. x=.. p.ppp% <mode> spd .. grav .. mini .. |
      local .. | sequence .. | hold ..` header and up to five `GPRL trace: #k <reference|
      earliest-valid|early-invalid|latest-valid|late-invalid> shift .. : frame0 .. pts .. x,y: ..`
      lines (a death ends `| died frame .. on #<id> type .. at (..) rect .. clone (..) box ..
      lastCollision .. laterInputs n (self|downstream)`), plus `sa-...` lines for the
      sequence-adjusted edges. These show where each "4-frame" bound comes from.
- [ ] with `solver-trace-overlay` on: `GPRL trace: overlay shows input #k (n trajectories)` and,
      in the level, the real run white, the valid trajectories green (sequence-adjusted cyan), the
      first invalid ones red (sequence-adjusted magenta) with the hitbox where they died and the
      killer's rect orange, the input point yellow; a middle-left panel lists the input's time,
      tick, percent, type, gamemode, speed, gravity, mini, local / sequence-adjusted windows, hold
      range and for every invalid trajectory its death point and `why:` (self / downstream: n later
      inputs applied first / frozen death pause). Drawing only: the run must feel unchanged.
- [ ] HUD history lines gain ` | local 4.00 f / seq 10.75 f ok` (or the status word) a moment
      after the window line; the Session tab shows a third solver line (`Results: .. | SA ..`) and,
      while tracing, `Trace #k .. | local .. | seq .. | early-invalid <cause>, late-invalid <cause>`.
- [ ] with Click Between Frames (separate session): `cbf=1 subtick=1/8 tick`, edges print sub-tick
      shifts and the window lines name `gprl-clone/2-cbf` (never run in any owner session so far).

**v0.7.1 additions to that check** (the Fable review deltas, `docs/TIMING_SOLVER_V2_FABLE.md` §5
and `docs/TIMING_SOLVER_V2.md` §9.A; nothing run in-game yet):

- [ ] `GPRL timing:` lines of a widened sequence window end `.. decided proof r/r` (wave pairs
      re-join: `r`); `proof s/..` on wave means §1.3 of the Fable review is wrong for that level
      (send the trace block); ship / ufo / swing widenings read `s` and carry `sa_survived_only`.
- [ ] an input open on both sides at +-10 ticks reads `sequence .. local decided` with `open_range`
      in its reasons (status `ok` or `no_effect`), never `sa_not_measured`.
- [ ] an `undecided` sequence side right at the local edge reports the local edge (the sequence
      width is never below the local one on any line).
- [ ] the 5 s summary ends `miss downstream m, speed changes c (flagged f), subtick clock: engine vs
      tracker max |d| x.xxx ms`: `f` stays in the tens on Deadlocked (hundreds = the recorded speed
      toggles between snapshots: send the log); `x` is 0.000 without Click Between Frames (a half
      tick reads 0.5 tick on both clocks: 0.000 with 43 half-tick inputs in the 20.36.22 log) and
      must stay under 0.5 with it.
- [ ] inputs shortly before a speed portal read `low_confidence (speed_change_in_lookahead, ..)`.
- [ ] a result the mod's own gate mirror refuses logs `GPRL timing: input #k (job j) would fail the
      server gate (<reasons>); sent anyway as evidence` - send the log when it appears.
- [ ] the real player's speed changes at the speed portal's x, never a few ticks before it (the
      clones no longer run the game's code for speed portals: `kClonePortals` without 200-203 /
      1334, D7b).
- [ ] local clone steps per input in the wave within ~10 % of the v0.7.0 log's (the island-scan
      pruning is back for sides whose nearest shift passed, D5).
- [ ] no `payload would fail the server gate (window_below_resolution)` warning on a window line
      reading `window 4.17 ms .. res 4.17 ms` (float noise, fixed in the final build); a warning on
      e.g. `window 3.12 ms .. res 4.17 ms` is a real gap and stays.
- [ ] the 5 s summary's SA part reads `dropped n (control c, .., death d)`: `d` counts SA jobs
      whose control died together with the real player; a `DROPPED control` line right after a
      `real player death` at the same frame and object should no longer appear.

Offline: the design's `tools/timing-report` (docs/TIMING_SOLVER_V2.md §6, backend step B6; not
in the tree at the time of writing) is meant to turn the `GPRL timing json:` / `GPRL trace:`
lines of such a log into the AUDIT §12 table and SVG trajectories. Until then the lines above are
read directly.

Not ported (SOLVER_DESIGN §1.3): PhysicsLock, HUD markers, manual tags, macro playback, auto-bot,
NaNDL precision; the FPC `PlayerObject`-ring fallback for a failed snapshot self-test is not
implemented either (the engine simply disables itself for the level and says so).

**v0.5.0 (2026-09-30, solver tuning from the first in-game logs; SOLVER_DESIGN §12).** The
first logs (levels up to 219k objects) showed the solver working - misses finished in the death
pause, 0 shadow mismatches on the first level, 0.1-0.4 ms/frame - and three losses, all fixed
without changing what a window means (`gprl-clone/1` stays; D7 had only dropped, never emitted
these): (1) every `touching rings N vs N-1` mismatch was a release job whose control re-touched an
orb the real player had already used, because GD selects an object's one-shot activation slot by
the player's `m_uniqueID == 1` and a hidden clone lives in the player-2 slot
(`core/solver/activation.hpp`: the clone is shown the real player 1's slot in its own; host test
`activation_tests` reproduces the log with the old and the new rule); (2) `speed` mismatches
(shadow too): speed portals are queued by the layer and pushed to the real players with
`PlayerObject::updateTimeMod` each sub-step, never to clones - `simStep` mirrors the snapshot's
speed with the game's own call; (3) the 17-30 ms `peak`s were `PlayerObject::create` bursts when
the pool grew: 48 clones are made at level setup, then at most 3 per frame, and a pass's remaining
shifts spawn deferred from the ring (24-step deadline). Budgets live in one versioned object
(`core/solver/budget.hpp` `kBudget` = `gprl-clone-budget/2`): 24 open jobs (was 8), 320 clones
(lazy), an adaptive per-frame clone-step budget (2 ms target / measured us per step, oldest job
first), the 4 ms / 2 ms throttle guard kept. The 5 s summary now prints every skip and drop reason,
the mismatch kinds, `coverage` (windows / inputs that could have one: dead-time skips, deaths
unrelated to the input and restart cuts are "not windowable") and the budget it ran with; the
Session tab line and the HUD readout name the same reasons.

**Review pass of the port (2026-09-29, SOLVER_DESIGN §11 lists every divergence from FPC).** Fixed:
(1) inputs of a dead player / before `m_started` / after completion / on platformer levels were
logged into the replay before the skip checks, so a press mashed in the death pause was replayed
into the clones finishing a miss and set a late limit on every open job - they are now skipped
before the log like FPC; (2) a later same-channel input in the SAME step as a job's input (a tap
shorter than one tick without CBF) was not treated as a neighbour, so the shifted presses crossed
the release; it now blocks the late side, the limit is clamped at 0 and can no longer turn the
control clone (shift 0) into `Limit`; (3) a refinement pass whose re-run control could not be
trusted (pool, teleport / dual portal, a mismatch only the re-run saw) dropped the whole job -
the coarse window of pass 0 (whose control matched in lockstep) is kept instead, and a control
that became invalid is counted `invalid`, not `mismatch`; (4) a pass made only of the late limit
point no longer labels the window `gprl-clone/1-cbf`. Added: clones far behind the timeline (a
CBF refinement pass spawns from ~0.5 s old snapshots and needs ~2000 clone steps) catch up at most
8 steps per real step instead of in one burst (`kCatchUpStepsPerStep`), and the summaries print
the peak clone-sim time of a single rendered frame.

## Build and test

```powershell
# host tests (cl.exe only, no Geode, no GD): 32 suites (v0.7.1), exits non-zero on any failure
# (-Only a,b = just those suites; -OutDir <dir> = another build folder, for two runs at once)
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\tests\run_tests.ps1
# optional, needs a real ffmpeg.exe (found like the mod finds it, or -Ffmpeg <path>): runs the
# clipping buffer's exact ffmpeg command lines outside the game (probes, segmenter, mux, hash)
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\tests\clip_ffmpeg_check.ps1
# intended solver change only: (re)write the `golden` blocks of
# tests/fixtures/solver/determinism-local-window.json, then `npx prettier --write` that file
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\tests\run_tests.ps1 -Write

# the mod: portable MSVC + geode CLI (ninja, RelWithDebInfo); installs the .geode into GD
powershell -NoProfile -ExecutionPolicy Bypass -File D:\GPRL\geode\build.ps1
```

Toolchain facts baked into the scripts: portable MSVC via `D:\GeodeMods\_msvc\msvc-env.ps1`,
`GEODE_SDK=C:\Users\gacue\Desktop\GeodeSDK`, `CPM_SOURCE_CACHE=D:\GeodeMods\_cpm`. The build
output is `geode/build/gmo12.gprl.geode`.

**MSVC 14.44 internal compiler error with Argon**: any file that instantiates Argon's / arc's
coroutines (Argon's own sources, `src/Connect.cpp`) dies with `C1001` at
`arc/future/Pollable.hpp(174)` (backend access violation) when compiled with Geode's precompiled
header (`/Yu`); the same files compile cleanly without it (reproduced by hand with `/O2`, `/Od`,
`/Zi`, `/Z7`). `CMakeLists.txt` therefore sets `DISABLE_PRECOMPILE_HEADERS` on the `argon` target
and `SKIP_PRECOMPILE_HEADERS` on `src/Connect.cpp`. Keep every `<argon/argon.hpp>` include inside
`src/Connect.cpp`. `async::TaskHolder` (the Argon README pattern) compiles fine once the PCH is off;
the holder is leaked on purpose so closing the popup does not cancel a Connect.

## Layout

```
mod.json, CMakeLists.txt, build.ps1, about.md
core/                 PURE C++20, no Geode / cocos includes, compiled a second time by the host tests
  vocab.hpp             wire vocabularies (gamemode, speed, button, trust, integrity...) = shared/src/domain
  json.hpp/.cpp         JSON value + JSON.stringify / pretty / canonical writers + parser (no third-party lib)
  telemetry.hpp/.cpp    gprl.telemetry/1 structs, (de)serialisation, validateBatch, checkBatchInvariants
  snapshot.hpp          PlayerStateSnapshot POD (field-for-field = schema.ts PlayerStateSnapshot)
  crypto.hpp/.cpp       SHA-256, HMAC-SHA256, base64, hex (FIPS / RFC 4231 / RFC 4648 tested)
  ringbuffer.hpp        SPSC ring (game thread -> telemetry worker), counts drops instead of blocking
  fingerprint.hpp/.cpp  TimingFingerprint, similarity, familiarity weight (mirror of the TS engine)
  calibration.hpp/.cpp  local calibration progress counter (mirror of calibration.ts) + server-state JSON
  identity.hpp/.cpp     connection state of a saved token vs the logged-in GD account, site origin,
                        "Open my profile" URL check
  ranks.hpp/.cpp        the in-game menu's pure rules: /api/ranks ladder (both shapes), sigma -> band,
                        progress, numerals, colours, formatting, profile / leaderboard JSON -> structs
  classify.hpp/.cpp     the tracker's rules over plain values: flags -> gamemode, m_playerSpeed -> speed,
                        handleButton -> input, sub-tick clock, would-be death, integrity, environment change
  geometry_hash.hpp/.cpp   FNV-1a 64 over quantised (objectId, dx, dy) of the objects around an input
  fingerprint_build.hpp/.cpp  PlayerStateSnapshot + input context -> TimingFingerprint tables
  display.hpp/.cpp      HUD history lines, coverage block, level hints, attempt clock (v0.5.1)
  clip.hpp/.cpp         clipping buffer rules (v0.6.0): ClipParams, settings clamp, encoded size, encoder
                        ladder, the ffmpeg command lines, BufferBook (what is kept, disk cap, selection),
                        AttemptBook, decidePreserve
  clip_flow.hpp/.cpp    clip choice state machine (never an automatic upload), upload contract + PUT plan
                        + retry rules, streaming SHA-256 of a file, clip ids / names, the clip index
  solver/oracle.hpp     IPhysicsOracle, InputSchedule, Outcome (ARCHITECTURE §6)
  solver/boundary_search.hpp/.cpp   coarse scan -> bracket -> adaptive bisection, islands, budgets
  solver/local_window.hpp/.cpp      LocalWindowSolver + HoldRangeSolver, ms with frames@240 display helpers
  solver/pass_planner.hpp/.cpp      BoundarySearch as batched passes for the lockstep engine (coarse, islands,
                                    limits, sub-tick refinement, misses), WindowResult in the same shape
  solver/timeline.hpp/.cpp          step / frame axis math (input placement, half ticks, history walk)
  solver/sequence.hpp/.cpp          sequence windows (v0.6.1): SequenceConfig (`gprl-clone-seq/1`), a local window
                                    as a lattice of atoms, SequencePlanner (coarse grid, refinement along the
                                    pass / fail boundary, the feasible share), the reference SequenceSolver on
                                    the pull oracle, the `sequence_window` payload + server-gate mirror, the
                                    engine's grouping / idle-budget / clone-grant arithmetic
  solver/window_event.hpp/.cpp      WindowResult -> TimingWindowPayload (midpoint edges, hold, solver version)
                                    + a C++ mirror of the server's windows.ts checks
  solver/activation.hpp             one-shot activation flags of orbs / pads / portals in the logical form a clone
                                    must see (GD selects the slot by the player's unique id) + asm mirrors (v0.5.0)
  solver/budget.hpp                 kBudget (gprl-clone-budget/3): jobs, pool, clone creation ration, adaptive
                                    clone-step budget from the measured cost, coverage (v0.5.0); 2048-step history,
                                    SA budget, replay breaker, trace caps (v0.7.0)
  solver/diagnostics.hpp            mismatch kinds and drop buckets for the summaries (v0.5.0)
  solver/timing_units.hpp           canonical units (v0.7.0): ONE window value in ms -> seconds / 240 FPS frames,
                                    actualMs with the sub-tick part, level time, D50 (AUDIT §8, §9, §13)
  solver/timing_status.hpp          `gprl-timing-status/1` (v0.7.0): 6 statuses, 50 grouped reasons (v0.7.1), precedence,
                                    resolveStatus / statusOf from the local, SA and context facts (AUDIT §16)
  solver/miss_attribution.hpp       one miss per death (v0.7.1, Fable D6): attributeMiss, diedWithReal (object -1 by x)
  solver/cluster.hpp                connected clusters (v0.7.0): connectedNext, forced portal breaks, cluster ids,
                                    SA chunking, the position-aware SA queue rank (v0.7.1, Fable D10)
  solver/sequence_adjusted.hpp/.cpp sequence-adjusted windows `gprl-clone-sa/1` (v0.7.0): SAConfig, SAPlanner
                                    (pair / chain2 / chain3, pass / fail / undecided per side), runSAAgainstOracle
  solver/result_ledger.hpp          one timing_result per bound job (v0.7.0)
  solver/trace.hpp                  debug trajectories (v0.7.0): selection, caps, `GPRL trace:` lines, the engine's
                                    last-trace View
  solver/trace_view.hpp             the in-game debug view of one trace (v0.7.0, AUDIT §11): drawable paths by
                                    role, the panel / Session tab lines, why an invalid trajectory failed
  solver/timing_result_event.hpp/.cpp  the `timing_result` payload (v0.7.0) + its checks against the
                                    timing_window and the validator mirror, fallback on failure
  sim/                  the isolated background simulator `gprl-sim/2` (v0.12.0, docs/BACKGROUND_ANALYZER_DESIGN.md)
  analyzer_recorder.hpp v0.12.0: the passive recorder's tick bookkeeping, the per-visit keep policy, FramePressure
  analyzer_extract.hpp  v0.12.0: object classification (id / GameObjectType -> ObjKind), GD's exact rect formula
                        (gdObjectRect, replicated from the 2.2081 asm), GD's slope rules, group hash, unsupported
                        spans, the WorldBuilder (decoration summary, cap while collecting)
  analyzer_status.hpp   v0.12.0: the HUD / Session status texts
src/                  Geode-facing code
  analyzer/             v0.12.0 background level analyzer (READ-ONLY): Analyzer (hook facade, one visit per
                        level entry), Modes (settings -> modes, frame pressure, the clone gate), Extract (time-
                        sliced read of m_objects inside the invariant check), Recorder (passive RecordedTick /
                        inputs), Worker (the below-normal thread: World finish + gameplay hash, caches, sim::Job,
                        upload), Cache (analysis/<gdLevelId>-<gameplayHash>.json), Status (HUD / popup texts, the
                        first-run mode popup)
  main.cpp, Settings.*  mod entry, settings cache, client build string, placeholder-API rule
  Hooks.cpp             PlayLayer / GJBaseGameLayer / PlayerObject / PauseLayer hooks
  Tracker.*             attempts, deaths, progress, gamemode changes, inputs + sub-tick, state samples
  Environment.*         mods, Eclipse state, loaded modules (name + size), hashes, TrustState
  Telemetry.*           ring consumer, background worker, session flow, batching, signing, spooling
  Api.*                 blocking /v1 calls (connect, web-login, sessions, batches, end, me/calibration) and the
                        public /api reads (ranks, leaderboard, players/<name>) - worker only
  Connect.*             Connect (Argon + /v1/client/connect), Disconnect, Open my profile - main thread
  LocalStore.*          bounded rotating JSONL spool in the mod save dir
  Hud.*, Popup.*        bottom-left "jumps | attempts | sigma/s LOCKED" line, the tabbed GPRL menu (v0.3.0);
                        v0.7.0: the trace overlay + panel (setting solver-trace-overlay), the v2 Session lines
  Clipper.*             clipping buffer orchestration (v0.6.0): settings, attempt ranges, preserve, the clip
                        thread (cut + hash), the four choices, the upload thread, clips.json
  ClipHooks.cpp         CCEGLView::swapBuffers, FMODAudioEngine::update, PlayLayer::showEndLayer,
                        PauseLayer::customSetup (frame / sound hand-over, offering a pending clip)
  clip/Capture.*        GL read-back (3 PBOs) -> writer thread -> ffmpeg 1 s H.264 segments; housekeeping
  clip/AudioTap.*       FMOD master-mix DSP (game sound), clip/MicCapture.* WASAPI default microphone (opt-in)
  clip/Process.*        ffmpeg child process (pipes, kill-on-close job), clip/ClipUtil.* clock + paths
  clip/ClipPopup.*      Save to computer / Send to GPRL moderators / Save + send / Do nothing
  solver/PlayerFields.inc, CopyFields.inc   verbatim frame-perfect-counter field lists (never hand-edit)
  solver/CloneState.hpp     PlayerState (fields + hitbox block + rings + collision logs), capture / apply / copy
  solver/CloneEngine.*      the lockstep clone engine: history ring, shadow clone, jobs, simStep in the game's
                            own order, pass planner driving, death pause extension, throttle, summaries
  solver/CloneHooks.cpp     side-effect guards for stepped clones (triggers, sounds, streaks, portals, ...)
  solver/GdOracle.*         facade: gate, seq linkage, event emission, local calibration, status (v0.4.0)
  eclipse/              Eclipse Menu public API headers (vendored, identical to startpos-noclip)
tests/                host tests: boundary_search, local_window, telemetry_roundtrip, crypto, fingerprint,
                      calibration, config, identity (token vs GD account, profile URL allow-list), menu (core/ranks:
                      ladder shapes, sigma -> band, progress, profile / leaderboard parsing), classify (SPEC §45 input classification + gamemode detection),
                      determinism (recorded callback log replayed twice; solver replay; the solver
                      fixture's `golden` blocks: compared when present, written with --write),
                      pass_planner (equivalence with LocalWindowSolver over 200 random interval sets, misses,
                      late limits, gaps, refinement passes, budget cuts, determinism), timeline, window_event
                      (payload shape + server-gate mirror + batch invariants), geometry_hash, fingerprint_build,
                      activation (the game's slot rules as asm mirrors, the logical-world round trip, the
                      2026-09-30 "rings 1 vs 0" release regression with the old and the new rule, pads / portals),
                      tuning (budget config + arithmetic, coverage, mismatch kinds, drop buckets),
                      display (v0.5.1), clip (v0.6.0: settings, sizes, ffmpeg command lines, encoder ladder,
                      BufferBook window / open attempt / pins / disk cap / selection, attempts, the preserve
                      rule), clip_flow (v0.6.0: the four choices, "no upload without a Send choice" over every
                      event sequence, upload request / session / PUT plan / retry rules, file SHA-256 against
                      the FIPS vectors, ids, names, the index), sequence (v0.6.1: synthetic joint oracle with a
                      known feasible region - independent pairs, diagonal bands, an ellipse, triples, order
                      crossing; exact lattice share at full refinement, the sample cap, invalid trials,
                      determinism, the reference solver, the event + gate mirror, the idle budget)
                      sim_modes (v0.12.0: clonesAllowed / simAllowedNow / extraction budget truth tables),
                      analyzer_recorder (tick boundaries under half ticks / CBF, sub-ticks, the caps, the keep
                      policy, frame pressure, status texts), analyzer_extract (classification of every
                      GameObjectType and the trigger table, GD's rect formula / box offset / oriented box /
                      slope rules, the cache check, spans, moved objects, the cap while collecting)
                      clip_args_tool.cpp + clip_ffmpeg_check.ps1: OPTIONAL check with a real ffmpeg.exe
```

## What this build does (IMPLEMENTED)

- **Events** (all with `t` = level time since the attempt start, `tick` = `m_currentProgress / 2`,
  strictly increasing `seq`, `attemptId`):
  `environment` (at session start AND again whenever it changes, see below), `attempt_start` (GD
  attempt number, session count, start percent, practice, StartPos tick, noclip), `input` (player
  1/2, jump/left/right, press/release, `tSubTick`), `state_sample` (every 24 ticks per player),
  `gamemode_change` (from/to + portal id), `death` (percent, x, object id, `wouldBe` under noclip),
  `progress` (per whole percent, `best` vs the saved percent), `attempt_end` (death / complete /
  exit / restart, `legit`, `noclipSeen`).
- **Mid-session environment changes (SPEC §19-§21, TELEMETRY.md §6)**: the tracker re-reads the
  mod menus every 0.5 s from `PlayLayer::postUpdate` (independent of the HUD, which may be hidden)
  and at every attempt start. When trust, noclip, bot, TPS bypass / tps, CBF or integrity differ
  from the last report (`core/classify environmentChanged`) a new `environment` event is emitted
  (`seq` continues; session-scoped, so exempt from the t / tick rule) - at an attempt start it
  precedes that attempt's `attempt_start`. Mods, modules and hashes are copied from the level-entry
  report (they cannot change while the game runs). Every poll also marks the open attempt:
  `attempt_end.noclipSeen` = noclip was on at any poll of the attempt (including a toggle after
  `attempt_start`) or a would-be death happened; `legit` = no poll saw a non-allowed trust state and
  `noclipSeen` is false. A noclip toggled on and off between two polls (< 0.5 s) without a death is
  not seen.
- **Sub-tick timing**: the tracker accumulates the `PlayerObject::update` deltas of the current 240
  TPS tick (hook priority after Click Between Frames, so CBF's split steps are seen); an input's
  `tSubTick` is the fraction already simulated when `handleButton` runs (0 on a tick boundary,
  0.5 for GD's own half tick, arbitrary with CBF). `GJBaseGameLayer::processCommands` is not
  hooked: the player deltas are the ground truth the FPC engine also uses. The rules (sub-tick
  clock, flags -> gamemode, `m_playerSpeed` -> speed with the GD 2.2 constants 0.7 / 0.9 / 1.1 /
  1.3 / 1.6 and midpoint boundaries, button mapping, would-be-death rule) live in `core/classify`
  with every constant in `ClassifyParams`; `Tracker.cpp` only reads the game fields and calls them.
- **Would-be deaths (SPEC §19)**: `PlayLayer::destroyPlayer` is hooked with `Priority::First`;
  after the original returns, `m_isDead == false` (a noclip menu swallowed it) is recorded as
  `death.wouldBe = true` and the attempt continues. GD's anti-cheat spike is ignored
  (GD_PHYSICS_NOTES.md).
- **Session flow (ARCHITECTURE §4)**: Connect (Argon, see "Connecting") -> `POST /v1/client/connect`
  (device token + GD account id saved with `Mod::setSavedValue`) -> per level `POST /v1/client/sessions` (session key base64 -> bytes,
  memory only) -> batches every 2 s / 500 events (max 4000 events, 1 MB) with
  `X-GPRL-Signature = hex HMAC-SHA256(sessionKey, canonical body)`, `seq` from `seq0`, nonce echoed
  -> `POST /v1/client/sessions/:id/end`. Retries (3, backoff) for transient failures; rejected or
  failed batches are spooled as `failed` records (with the HTTP status), never dropped.
  `GET /v1/me/calibration` after each remote session start feeds the HUD (server authoritative).
- **`session_invalid` (a batch answered with that ApiErrorBody code)**: the worker opens a
  replacement session for the same level (integrity from the latest environment report) and
  re-sends the rejected batch from the new `seq0`, preceded by the last environment event the
  server had accepted. The replacement knows none of the old attempts, so events of the attempt
  in flight (started in the old session) are spooled as `unsent` under the old session id and
  never re-labelled; everything from the next `attempt_start` on goes to the new session
  (`core/telemetry splitForReopenedSession`, host-tested against the server's invariants). The old
  session is not ended by the client. After 2 reopens per level the batch is spooled as `failed`
  and the rest of the level as `unsent` (spool + stop). A 401 on a batch disconnects the device (the popup then asks
  for Connect again) and also spools the rest of the level.
- **Game exit**: Geode's `GameEvent Exiting` (sent from `CCDirector::purgeDirector` before static
  destructors and before Geode's async runtime shuts down) ends the open attempt (reason `exit`,
  last known percent, the layer is not touched) and stops the worker, which spools every pending
  batch synchronously as `unsent` (signed, under the remote session id) - **no network on the exit
  path**, no `/end` call: closing the abandoned remote session is the server's job. The exit waits
  at most 2.5 s for a request already in flight; after that the worker is detached and its pending
  events are lost (logged). A static-destructor reaper only detaches a still-joinable worker (a
  crash / exit without the event) so the process never hits `std::terminate`; it never flushes.
- **Modes**: Remote (connected for the logged-in GD account + real API), Unsent (API configured,
  not connected / other GD account / old link-code token: JSONL spool `unsent`),
  Local (local-only setting or placeholder API URL: JSONL spool `local`, nothing networks).
  Spool: `<save dir>/telemetry/batches-<n>.jsonl`, 4 MB files, 8 kept.
- **Environment / trust (SPEC §21-§22)**: loaded Geode mods with versions; Eclipse
  `player.noclip`, `bot.state` (2 = playback -> botting), `global.tpsbypass(.toggle)`,
  `global.speedhack(.toggle)`; CBF loaded + active (`soft-toggle`, `click-on-steps`); known menus
  (`absolllute.megahack`, `prevter.openhack`, ...) marked `gameplayAffecting`; loaded modules via
  `EnumProcessModules` as **file name + size only** hashed `sha256("name|size")` (module contents
  are NOT read in Phase 1 - the server can request content hashes later); `hashes.gd/geode/gprl`
  are name|size(|version) hashes, `hashes.level` = SHA-256 of the level string. TrustState precedence:
  botting > physics changed > noclip > unknown gameplay mod > allowed. Integrity summary: flagged
  (bot / physics), warnings (noclip / unknown mod), clean, unknown (module list unreadable).
- **HUD**: `GPRL: jumps 42 | attempts 7 | sigma/s LOCKED (calibrating 12%)` bottom-left, refreshed
  twice a second ("sigma" spelled out: GD's bitmap fonts have no Greek glyphs; `|` instead of `·`).
  Suffixes: `(local)` until the server state arrives, `trust: ...`, `local-only`, `not connected`.
- **GPRL menu** (pause-menu and main-menu buttons): the tabbed popup described in "The in-game
  menu (v0.3.0)" above - Profile | Ranks | Board | Session | Account. The Account tab keeps the
  v0.2.1 content: calibration line (SPEC §10: `sigma/s: LOCKED`), connection line (`Connected as
  X (GD account verified)` / `Not connected - ...` with the reason / `Connecting... (<step>)`),
  API / session / batch counters, last error, spool line; buttons Connect or Disconnect, Open my
  profile, Website code (connected only), Flush, Settings (the row scales down if the font metrics
  would overflow it); the "Website sign-in code" panel over the lower 70 px of the panel (the
  session / counter lines are left out while it is shown; Hide brings them back).
- **core/** host-tested against the cross-package fixtures: `tests/fixtures/telemetry/batch-basic*`
  (parse -> validate -> invariants -> canonical bytes, SHA-256 and HMAC byte for byte),
  `tests/fixtures/playtest/local-spool/*.jsonl` (the mod's own spool format: 18 batches / 891
  events / 294 timing windows parse, validate, verify their TEST-key signatures, re-serialise
  byte for byte, pass the per-session ordering invariants and match `local-spool.expected.json`
  `summary.input`), `tests/fixtures/familiarity/*.json` (every similarity / weight case,
  identical-series sums), `tests/fixtures/rating/synthetic-player-{a,b,c}.json` (shared golden)
  and `synthetic-player-{90,150,300}.json` (tools/fixture-gen) (all 150 familiarity weights per
  player, effective sample count and the calibration percent at the fitted L), RFC 4231 / FIPS
  vectors. No C++ mirror exists for `rating/nandl-reference.json` (NaNDL is a fixture tool in
  `shared`, never client-side) or for the export summary of the playtest fixture (tools logic).

## STUB / not implemented (honest list)

- Timing windows are measured since v0.4.0 (see "The timing-window solver"). The v0.4.x clone
  path **ran in-game** on 2026-09-30 (the owner's logs of 00:19-01:20, docs/SOLVER_DESIGN.md §12:
  ring self-test OK, the shadow at 0 mismatches on the first level, misses emitted with the
  death-pause extension, 0.1-0.4 ms per frame); the fixes those logs led to (v0.5.0: activation
  slots, speed changes, the adaptive budget) are built and host-tested but **not yet confirmed
  in-game**. Half-tick
  placement without CBF is not synthesised (windows are tick resolution then, SOLVER_DESIGN D5);
  the death-pause extension steps a frozen world (moving objects near the input can make an
  `[ext]` window wrong, risk 2); every `timing_window` is `scope: "local"`.
- Sequence windows (v0.6.1, "v0.6.1" below) are built and host-tested but **not run in-game**: the
  delayed replays they rest on (clones started from history-ring snapshots up to ~4 s old) are
  proven per job by two exact controls and one known death, so a level where that does not hold
  only produces `DROPPED control` / `DROPPED negative` log lines, never a wrong number; how often
  that happens on real levels is unknown until the owner plays. Player 2 / dual mode, platformer
  levels and groups of more than 3 inputs are not measured. No rating reads the result yet.
- The local calibration is now fed by the solver's samples (HUD `samples N/700 (local)`), but it
  never unlocks anything by itself (no fitted L locally); the server's state replaces it when
  fetched (connected + real API).
- SPEC §24 local encryption: the device token is stored as a plain Geode saved value (the mod's
  `saved.json`). An AES-GCM / ChaCha20-Poly1305 wrapper with a per-install key is a later phase.
- LocalStore: `unsent` spool files are not re-uploaded after a later Connect (Phase 2); they stay on
  disk for the playtest tooling.
- The device token and the other saved values are plain Geode saved values (see SPEC §24 above).
  The Argon token is not stored by GPRL (Argon keeps its own cache) and is only sent in the one
  `/v1/client/connect` request.
- Clipping (SPEC §28-§31) is built since v0.6.0 ("v0.6.0" below) but **not run in-game**, and its
  upload has no LIVE server yet: `POST /v1/me/evidence/uploads` + `PUT` exist in the API since
  stream V2 (docs/EVIDENCE.md, migration 0028) but are not deployed and stay switched off until
  the R2 bucket exists (`EVIDENCE_STORAGE`, docs/DEPLOY-RUNBOOK.md §7.5); against today's Worker a
  Send ends with "this GPRL server does not accept
  evidence uploads yet" and the clip stays for a later Retry. The server does not send the
  `preserveEvidence` hint yet either, so only the local completion rule and `Clip last attempt`
  preserve clips. Clips over the upload limit (95 MB until the server states one) can be saved,
  not sent: the PUT reads the whole file into memory, there is no chunked / resumable upload.
- Closing a remote session left open by a game exit: the mod does not do it (no network on exit);
  the API has no session-timeout job yet (reported to the API owner), so such sessions stay open
  server side until one exists.
- `PlayerStateSnapshot.geometryHash` (state samples and window fingerprints) and the fingerprint
  `yVelocity` sign / scale convention still need the shared owner's confirmation before Phase 4
  tunes familiarity scales (SOLVER_DESIGN risk 9).
- Anti-cheat beyond the environment event: no integrity self-check of the GPRL binary, no memory
  scans (SPEC §22 forbids anything outside the GD environment anyway).

## Not hooked yet

- `PlayLayer::storeCheckpoint` / `loadFromCheckpoint` / `checkpointActivated`: the schema has no
  checkpoint event kind. Practice respawns are visible as `attempt_start` with `practice = true`
  and the checkpoint's `fromPercent`.
- Mirror portal state: the snapshot schema has no mirror field (portal object ids 45/46 are still
  recorded in `lastPortalObjectId`).
- Telemetry `input` events are still taken from `GJBaseGameLayer::handleButton` (player slot +
  button); `PlayerObject::pushButton` / `releaseButton` (pre-hooks since v0.4.0) feed the solver
  and the hook-check counter. Bots that call the player object directly bypass `handleButton`:
  their inputs get no event and their solver jobs are dropped as `unbound`; they are 0 sigma/s
  anyway (SPEC §20) and Eclipse playback is classified as botting.
- `GJBaseGameLayer::processCommands` is hooked since v0.4.0 for the solver's timeline only; the
  sub-tick clock still uses the player deltas ("Sub-tick timing").
- `PlayLayer::updateProgressbar`: progress is polled once per rendered frame in `postUpdate`.
- Dual-mode player 2 physics are sampled but its sub-tick uses player 1's accumulator.

## Notes for the other packages

- **Connect contract the mod implements (v0.2.0)** - the API owner must match it exactly:
  - `POST /v1/client/connect`, no Authorization header, JSON body (canonical key order)
    `{ "accountId": <int>, "argonToken": "<string>", "clientBuild": "gprl-geode 0.2.0+win",
    "modList": [{ "id", "version" }], "userId": <int>, "username": "<GD name as the game has it>" }`.
    Success (any 2xx): `{ deviceToken, playerId, username, displayName, identityVerified }`;
    `displayName` falls back to `username` when missing. Error: `ApiErrorBody`; the mod special-cases
    `error.details.reason == "gd_identity_invalid"` (the API sends `error.code` `unauthorized`;
    the mod also accepts the reason as the code) (clears the Argon token, Retry dialog) and shows
    `error.message` for anything else. The request timeout is 30 s (the server calls Argon).
  - `POST /v1/client/web-login`, `Authorization: Bearer <deviceToken>`, body `{}` ->
    2xx `{ url, expiresAt, code }`. A 401 disconnects the mod. `url` must start with `<site-url>/`
    (default `https://gprl.pages.dev/`) or the mod refuses to open it. `code` (v0.2.1) must be
    the 20-character Crockford base32 code (dashes optional) or the mod shows nothing; when it is
    missing the mod reads the url's `#code=` fragment. `expiresAt` is ISO 8601 UTC (`...Z`).

- **Telemetry schema**: `timing_window.holdMinMs / holdMaxMs` are `number | null | omitted`
  (`schema.ts`). The golden fixture omits them, `tools/fixture-gen` writes `null`, and
  `canonical.ts` keeps `null` while dropping `undefined` (rule 5), so the two "not computed" forms
  produce different signed bytes. `core/telemetry.hpp` therefore keeps them as a tri-state
  `Nullable<double>` and reproduces whichever form it parsed (2026-09-29: the earlier
  `std::optional` collapsed explicit nulls into "omitted", so every re-signed spool batch failed
  to verify - caught by the playtest spool host test). Windows the mod computes itself omit the
  keys, like the golden fixture. The `schema.ts` comment saying "the C++ side always writes the
  key with `null`" is outdated (shared owner).
- `environment` events need a non-empty `attemptId` (validate.ts requires it on every event); the
  mod sends the current (or upcoming) attempt's id, like the fixture. A session can now carry
  several `environment` events (one per change): the server must apply each in `seq` order.
- `attempt_end.noclipSeen?: boolean` (schema.ts / validate.ts, optional): the mod always writes it.
  The server should treat `noclipSeen = true` like a noclip attempt (never `completed`, would-be
  deaths count as misses) and should not ignore `legit = false` (API owner, `derive.ts`).
- The mod's `gameplayAffecting` is only a hint from a small client-side list; the server owns the
  classification (SPEC §21).
- `V1CreateSessionResponse.sessionKey` is decoded from base64 before signing; the signature is the
  hex HMAC over `canonicalJson(batch)` exactly as `tests/fixtures/telemetry/batch-basic.expected.json`.
- API base URL: the mod ships with the placeholder `https://gprl-api.example.workers.dev` and treats
  any `*.example.*` host as "not configured" (local-only behaviour). Set the deployed Worker URL in
  the mod settings.
- Both engines must keep passing `tests/fixtures/**`: change the TypeScript, regenerate the
  fixtures (`npm run fixtures:write -w @gprl/shared`), then fix `geode/core` to match.
- (v0.5.1) The mod program owns additions to `shared/src/telemetry/{schema,validate}.ts`
  (docs/FINISH_PLAN.md): `timing_window.evidenceHint`, `attempt_start.gdAttemptCount`,
  `attempt_end.activeMs / practiceMs / startPosMs` are always written by the mod, optional on
  the wire; the API accepts `reportedAttempts` on the session end (validated, not stored until
  stream P's migration). Backend streams only read these.

## v0.5.1 (stream M1: HUD history, coverage, rated-demon policy, bot evidence, capture)

Everything below is diagnosable from the Geode log (`GPRL:` / `GPRL solver:` / `GPRL site:`
lines) and from the popup; nothing here changes what the server may rate.

- **HUD history (middle-right panel, setting `show-last-window`)**: the last 8 presses /
  releases from `GdOracle::recentWindows`, newest at the top, one line each:
  `PRESS 4.17 ms [-2.08 +2.08]`, `REL 2.50 ms [-1.25 +1.25]`, `PRESS miss 4.17 ms [-6.25 -2.08]`
  (the real player died on that input), `PRESS dropped: mismatch` / `REL dropped: cut by
  restart`; an open side keeps its `<` / `>` marker; `bot PRESS ...` while a bot plays. Green
  measured, red miss, grey dropped; older lines fade (255 -> 110 opacity in steps of 18).
  Rules + tests: `core/display.{hpp,cpp}` (`historyText`, `historyLines`, `DisplayParams`
  `gprl-display/1`), `tests/display_tests.cpp`. The panel resizes to the widest line.
- **Level-analysis coverage (Session + Profile tabs)**: `GET /v1/levels/<gd id>/analysis`
  (public, through the telemetry worker like the other site reads, cached 60 s, one retry per
  10 s after an error, `SiteKind::Coverage`) rendered as the MASTER §20 block on one line:
  `Level Analysis Coverage: 83% / Missing: - 61.2-64.7% - 91.0-94.3%`, `... 100% / Complete`,
  `... (not a rated demon: not analyzed)` when the server says the level does not count; the
  server's en dashes become `-` (GD's bitmap fonts). `Loading...` / `unavailable - <error>` /
  `not fetched (local-only mode / API not set)` otherwise. Parser + line: `core/display
  parseLevelCoverage / coverageLine` (tolerant of hostile numbers, older producers without
  `missingDisplay`).
- **Rated-demon policy (MASTER C6)**: `POST /v1/client/sessions` now carries the hints
  `levelStars` (`GJGameLevel::m_stars`), `levelIsDemon` (`m_demon`), `levelDemonDifficulty`
  (`m_demonDifficulty`: 3 easy, 4 medium, 5 insane, 6 extreme, else hard; `core/display
  demonDifficultyName`) and `levelName` (`m_levelName` reduced to printable ASCII, 64 chars,
  `levelNameHint`); the server decides `levelCounts` from the GD servers and the response's
  `levelCounts / levelCountsReason / levelRating` land in `client::Status`. The HUD line appends
  `| Not a rated demon: not counted`, the Session tab shows it next to `Server: session ratable`
  (else `level counts (rated demon: extreme, 10 stars (gd_api))`), the Profile tab at the bottom.
  The session-open log line names the verdict, its source and the hints sent.
- **Bot playback = level-only evidence (MASTER §13 / §20)**: the solver gate no longer pauses
  for Eclipse `bot.state == 2`; it measures with status `measuring (bot playback: level-only
  evidence, not counted for you)` and every window of an input made while the bot played is
  emitted with `timing_window.evidenceHint: "level_only"` (remembered per job, so a bot that
  stops before the window finishes still tags it) and logged `evidence=level_only`. The
  `environment` event carries `bot = true` (unchanged), the local calibration counts them as
  bot samples (weight 0), the Session tab counts them (`N level-only`). The gate still pauses
  for Eclipse Physics Bypass at a TPS other than 240 and for speedhack, now read from the menus
  before the trust state (trust ranks botting above physics).
- **Practice / attempt capture (MASTER §10 / §11)**: `attempt_start.gdAttemptCount` =
  `GJGameLevel::m_attempts` (the GD save's total, untrusted - the server keeps it apart from its
  own observed count); `attempt_end.activeMs / practiceMs / startPosMs` = unpaused wall-clock ms
  of the attempt (real elapsed time per rendered frame while `PlayLayer::m_isPaused` is false;
  the first frame after the pause menu closed carries the whole pause and is discarded by the
  0.5 s guard; practice = `m_isPracticeMode`, StartPos = `m_startPosObject` set and practice
  off; `core/display AttemptClock`, host-tested); `POST /v1/client/sessions/:id/end
  reportedAttempts` = the GD save count when the level was left. Debug log per attempt end
  (`active N ms (practice P, startpos S), F frames counted, D discarded`), level totals in the
  quit line and on the Session tab (`active 12.3 s (level 480 s: practice 120, startpos 60)`,
  `GD save attempts N (untrusted)`).
- **Contract**: `gprl.telemetry/1` gains only optional keys (absent = older client):
  `attempt_start.gdAttemptCount`, `timing_window.evidenceHint`, `attempt_end.activeMs /
  practiceMs / startPosMs`; `V1EndSessionRequest.reportedAttempts`. C++ mirror `core/telemetry`,
  golden `tests/fixtures/telemetry/batch-capture*.json` (new; `batch-basic` untouched),
  `docs/contracts/m1.md` for the website. `about.md` carries "Created by gmo" (MASTER §29).

## v0.6.0 (stream M3: clipping buffer)

Design, cost, privacy and the log lines to look for: **`docs/CLIPPING.md`**. Website / stream V2
contract: `docs/contracts/m3.md`. Summary:

- **Settings** (`mod.json`, section "Clipping"): `clipping` (**off**), `clip-buffer-seconds` (120,
  10..600), `clip-quality` (`480p` / `720p` / `1080p`, 720p), `clip-disk-cap-mb` (1024, 128..8192),
  `clip-game-audio` (on), `clip-mic` (off until v0.9.0, **on** since), `clip-desktop-audio`
  (v0.9.0, on: the default output device as a third track), `clip-folder` (empty = the mod
  save folder; holds `gprl-clip-buffer` and `gprl-clips`), `ffmpeg-path` (empty = this mod's save
  folder, then the In-Game Clipper's ffmpeg, then PATH). The mod's master switch `enabled` off =
  nothing is recorded.
- **Rolling buffer** (`src/clip/Capture`, rules in `core/clip`): while a level is open, the window
  picture at 60 fps -> ffmpeg -> 1-second H.264 segments at a constant bitrate (hardware encoder
  when its self-test passes, else libx264), kept for the last N seconds + the whole open attempt
  (max 600 s), under the disk cap; free-space guard; `gprl-clip-buffer` wiped at every start.
- **Preserve** (`core/clip decidePreserve`): the server's `preserveEvidence` hint in a batch ack
  (contract appended to `contracts.ts`; never sent - the v0.9.0 run review's `verificationRequests`
  replaced it and is sent automatically), else the local rule - a legit
  completion from 0 % of a level that counts - or `Clip last attempt` (GPRL menu > Account). The
  attempt's segments are stream-copied into one MP4 (+ AAC game sound, + the microphone as its own
  second track when opted in), SHA-256 hashed, kept in `gprl-clips/pending`.
- **Popup** (`src/clip/ClipPopup`): Save to computer / Send to GPRL moderators / Save + send / Do
  nothing, on the end screen, at the next pause, or from the Account tab. Closing = decide later
  (at most 3 clips wait; the oldest is discarded). Buttons ignore input for the first second.
- **Save** -> `gprl-clips/GPRL <level> <pct>pct <date time>.mp4` + telemetry `clip_available
  {clipId, attemptId, durationMs, sha256}` (telemetry revision 2; only to a server whose session
  response says `telemetryRevision >= 2`, and only while the attempt's level session is open).
- **Send** -> `POST /v1/me/evidence/uploads`, then `PUT` (3 tries, new upload session per try,
  the file re-hashed first, the device token only to the API origin); progress, the result and
  `Retry upload` in the Account tab. **Never automatic** (`core/clip_flow`).
- **Account tab**: the line `Clipping: recording 1280x720 60 fps h264_nvenc - buffer 118 s, 74.0 MB
  (cap 1024 MB)` (or why nothing is recorded), the newest clip's state (`Clip: <level> 100% 1:58
  78.2 MB - uploading 43%`), and the row `Clip last attempt` / `Clip choice` / `Retry upload` /
  `Clips folder`. `Clip choice` also opens the popup for a clip whose upload failed (Send = retry,
  Save, Save + send, Do nothing), so such a clip can be kept or discarded, not only retried.
- **Verifier fixes (2026-09-30, `docs/CLIPPING.md` §12)**: the failed-upload dead end above;
  `Clip last attempt` only takes attempts of the open (or last) level session (a clip is never
  stamped with another level's id / hash / server session); the clipper only trusts a telemetry
  status of its own session generation (`client::Status::sessionGen`); the microphone is kept
  only while a level is open, forgotten on every opt-out, and not opened without ffmpeg; a
  recording session's folder is emptied before use.
- **Tests**: `clip_tests`, `clip_flow_tests`, `testClipFixture` in `telemetry_roundtrip_tests`
  (golden `batch-clip.json`), `shared/test/telemetry.test.ts`, `api/src/test/clip-available.test.ts`;
  the optional `clip_ffmpeg_check.ps1` ran the exact command lines with the owner's ffmpeg 7.1
  (NVENC, AMF and libx264 work here; Quick Sync does not): 43 checks at each of the 3 qualities.

## v0.6.1 (stream M4: sequence windows)

Design: `docs/SOLVER_DESIGN.md` §13 (MASTER §5 "sequence difficulty", SPEC §8). Contract:
`docs/contracts/m4.md`.

**What it measures.** A local window moves ONE input while the others stay as performed. A
sequence window moves 2 or 3 NEIGHBOURING inputs together and reports which part of the box of
their local windows is feasible: `jointFeasibleShare` = feasible measure / product of the local
widths, in [0, 1]. 1 = independent (every combination of individually valid timings works), less
= the sequence is tighter than its local windows say (a press and its release whose hold length
matters, a wave click pair, an orb chain). Combinations that would swap two inputs count as
infeasible. It describes the LEVEL, never the player, and no rating reads it yet.

**How.** `core/solver/sequence` (pure, host-tested) turns each input's local window into a
lattice of *atoms*: the passing whole ticks without Click Between Frames, an even subdivision of
the window with it (never finer than the window's own resolution, at most 21 atoms per axis for
a pair, 9 for a triple). The planner samples a coarse grid that always contains the first, the
last and the performed timing of each input (5 nodes per axis for a pair, 3 for a triple),
then cuts every grid box whose corners disagree in two (along the axis they disagree on most,
the largest box first) until it is one atom wide or the sample cap is reached (48 / 64 simulated
combinations). Combinations with at most one shifted input are local trials (already known to
pass) and are never simulated. The share is the weighted mean over all atoms: measured atoms
count as measured, the rest follow their box (its outcome when the corners agree, else the side
of a straight boundary between the corners they lie on). `resolutionMs` = the widest edge of a
box whose corners disagree. On 600 random synthetic jobs: exact on the lattice whenever the
refinement finishes (359 of 360), 0.004 mean error for pairs that hit the cap, 0.018 for triples.
A job cut short (its history leaves the ring) reports only what was decided: nothing while the
coarse grid is incomplete (`DROPPED invalid (cut short before the coarse grid was complete ...)`),
afterwards the last fully simulated refinement round with that round's `resolutionMs`.

**Delayed replays, proven per job.** The second input does not exist yet when the first one
happens, so a sequence job cannot run in lockstep: its clones start from the history-ring
snapshot before the first shifted input and replay the logged inputs with the group's inputs
moved (`Clone::moved`, the same `simStep` as every other clone). Every job proves that such a
replay is trustworthy where and when it runs:

1. **control first**: the unshifted replay must equal the ring's recorded real states step by
   step (same tolerances as the lockstep control) up to the look-ahead;
2. **negative control**: one death the local solver saw in lockstep (a member at its first
   failing shift) must happen again; a group without any known death is not measured;
3. samples: pass = re-joins the recorded real run for 16 steps after every moved input was
   applied, or survives the look-ahead; fail = dies;
4. **control last**: the unshifted replay again after the samples.

A failed control or negative control drops the job with the reason (moving objects, triggers
and anything else the clones do not reproduce show up here, never in an emitted number).

**Only with spare solver time.** One job at a time, after the local jobs of each step. Clone
steps per rendered frame: at most 35 % of the step budget and 0.5 ms at the measured cost, and
only while the local jobs' expected steps (the larger of this frame's so far and the previous
frame's) plus the sequence's stay under 60 % of the budget. At most 4 sequence clones at once,
16 steps per clone per real step; 24 idle clones are always left for the local jobs, a new clone
is created at most once per frame. No sequence work while the solver's 1 s load average is
within 0.5 ms of the 4 ms throttle that pauses new window measurements (its own time counts in
that average). Caps: 12 jobs per attempt (4 triples), 2 measurements per
place (first input's x block + group size) per level visit, 8 waiting. A waiting or running job
whose first snapshot leaves the ring (1024 steps) is dropped. Everything is
`core/solver/sequence.hpp SequenceConfig` (`gprl-clone-seq/1`), printed in the ready line.

**Groups.** Neighbours in the attempt's input log, at most 24 ticks apart (a triple spans at
most 36), all with an emitted HIT window measured in the same mode; no death or would-be death
between the first shifted input and the look-ahead; not after the finish line.

**Telemetry.** `sequence_window {inputSeqs[], localWidthsMs[], jointFeasibleShare, samples,
resolutionMs, solverVersion}` (`gprl-clone-seq/1`), `t` / `tick` of the first input, deferred
like `timing_window`. Telemetry revision 3: the event is only pushed when the session's server
said `telemetryRevision >= 3`; otherwise the job still runs and its log line ends with
`NOT SENT: this server validates telemetry revision 2 (sequence_window needs 3)`. Local-only /
unsent sessions spool it.

**Setting.** `measure-sequences` (default on). Off: nothing is queued or run. v0.7.0 renamed it
`measure-joint-share` (default OFF: superseded by the sequence-adjusted windows, runs only while
no sequence-adjusted job waits).

**Session tab.** The solver's counter line ends with `| seq <emitted>/<measured> of <groups>`
(or `| seq off`).

**Also in v0.6.1: speed portals and the control comparison.** The layer hands the real player a
queued speed change at the very start of a step, before `processCommands`, so when a clone that
finished step k is compared with the real player, the real player can already carry the speed
of step k + 1 (position and velocity equal, only `m_playerSpeed` differs). v0.5.0 mirrored the
speed at the clone's next step, which is after that comparison. `mirrorNextStepSpeed` now gives
the clone the new speed before the control / shadow comparison, but only when its own speed is
the one the ring says step k ran with; any other difference is still a mismatch. If the summary
kept showing `mismatch [speed N]` at speed portals, this is the fix.

**Geode log lines** (verbosity 0 unless noted):

```
GPRL sequence: on - gprl-clone-seq/1 (joint windows of neighbouring inputs, level evidence only): inputs <= 24 ticks apart (triples <= 36), 5 / 3 nodes per axis and <= 48 / 64 samples for a pair / triple, <= 12 jobs per attempt (4 triples), 2 per place; only when idle: <= 35% of the step budget and 0.5 ms per frame while the local jobs + sequence stay under 60%, 4 clones at once, 24 idle clones always left for the local jobs; every job proves itself with two exact controls and one known death
GPRL sequence: job 3 queued: pair #12 press + #13 release (seq 55, 57; gap 12.00 ticks), 5x5 atoms, 16 combinations to simulate first, base step 4801 (322 steps old), 0 waiting   (verbose)
GPRL sequence: job 3 started after waiting 0 steps: base step 4801 is 322 steps old (ring 1024), look-ahead to frame 5031.0, placement whole ticks, negative control = input #13 at +3.00 ticks   (verbose)
GPRL sequence: job 3 pair #12 press + #13 release (seq 55, 57; gap 12.00 ticks): share 0.760 of 20.83 x 20.83 ms (329.9 of 434.0 ms^2), 16 samples (pass 10, fail 6; local 9, crossed 0 of 25 points), res 4.17 ms, controls exact (143 + 143 steps against the ring), negative control died on #8 as known (input #13 at +3.00 ticks), 19 trials / 2210 clone steps over 41 real steps (172 ms) -> emitted seq 812 (gprl-clone-seq/1)   (windows)
GPRL sequence: job 3 map  input 2 at  +2.00 |..o##|  (input 1 from -2.00 to +2.00 ticks; # pass, . fail, x order, o local, ! invalid)   (verbose, pairs)
GPRL sequence: job 4 triple ...: DROPPED control (the first control differs from the recorded run at step 5123 (frame 5102.0, 37 steps in): position off by (0.0000,0.4210) | control (...)) | sim 0 ... | 1 trials / 37 clone steps over 3 real steps   (windows)
GPRL sequence: job 5 pair ...: DROPPED negative (input #21 at -2.00 ticks died on #8 in lockstep but survived the look-ahead in the delayed replay: hazards are not reproduced back there) | ...
GPRL sequence: pair #30 press + #31 release (...) not measured: no input of the group has a known death next to its window (...)   (verbose)
GPRL sequence: 5 s summary - groups 14 (pairs 10, triples 4), measured 5 (emitted 5, not sent 0, refused 0), trivial 3, dropped 2 (control 1, negative 1, invalid 0, history 0, restart 0, level end 0), not started 4 (attempt cap 0, place already measured 2, no known death 1, death in look-ahead 0, queue full 1, waited too long 0, cut by restart 0, other 0), waiting 0, running 0, 96 trials / 8421 clone steps, at most 35% of the step budget and 0.5 ms per frame when idle, v=gprl-clone-seq/1
```

`trivial` = nothing to simulate: an input whose local window is a single tick (frame perfect
without Click Between Frames) has no timing to vary, so at whole ticks its sequences are
independent by definition; with CBF the same input has a sub-tick window and is measured.

**In-game checklist** (solver log `windows`, or `verbose` for the queue / map lines):

- [ ] level entry: the `GPRL sequence: on - gprl-clone-seq/1 ...` line after the solver's ready line;
- [ ] a level with inputs close together: `GPRL sequence: job N pair ...: share ...` lines about
      1 s after the inputs, `controls exact (A + B steps against the ring)`;
- [ ] `negative control died on #<object> as known` on those lines;
- [ ] the 5 s summary's `dropped (control N, negative N ...)`: many of either on a level without
      moving objects means the delayed replay is wrong and needs the log;
- [ ] `sim ... ms/frame` in the solver's own 5 s summary stays where it was with sequences off,
      and `coverage` does not drop (the sequence job may not cost a window);
- [ ] with the live Worker still on telemetry revision 2: lines end with `NOT SENT: ...`; after
      the Worker deploy: `emitted seq N`, and `GET /v1/levels/<gd id>/sequences` lists them;
- [ ] a level with speed portals: no `mismatch [speed ...]` in the solver's 5 s summary.

**Tests**: `sequence_tests` (host), `testSequenceFixture` in `telemetry_roundtrip_tests` (golden
`batch-sequence.json`), `shared/test/telemetry.test.ts`, `api/src/test/sequence.test.ts`,
`database/tests/pglite/sequences.mjs`.

## v0.7.0 (timing solver v2)

Design: `docs/TIMING_SOLVER_V2.md` (from the audit `docs/TIMING_SOLVER_AUDIT_SPEC.md`, cited as
AUDIT §N). In-game checklist: "v0.7.0 in-game check" in the solver section above.

**Why.** The Deadlocked audit showed local windows bounded by deaths the input did not cause: a
wave click moved alone runs into the NEXT click's path, so its "window" measured the later input,
not this one (RC1), and misses were narrowed and trusted (RC3). v0.7.0 keeps the local window
(every other input as performed) but says, for every edge, what ended it (`stop`) and what killed
the clone (`cause`: self, downstream = a later fixed input acted first, extension = the frozen
death pause), and adds the SEQUENCE-ADJUSTED window: earlier inputs fixed, the next 1-3 inputs of
the same connected cluster follow the moved input by the same shift. An input whose side cannot
be decided is reported `sequence_dependent`, never given a guessed number.

**What is sent.** Every measured input gets one `timing_result` (telemetry revision 4,
`docs/TELEMETRY.md` §10): status (`ok`, `low_confidence`, `unresolved`, `state_replay_failed`,
`sequence_dependent`, `no_effect`) with reasons, the local window with attributed edges, the
sequence-adjusted window, the pair window, the hold range for releases, the canonical position
(attempt input index, x, percent, sub-tick), the cluster links and the simulation counts
(reference observation = this one result; boundary simulations = the shifted schedules behind it,
AUDIT §3). It is sent only to a server that advertises revision 4 (the live Worker is on 3: the
lines say `NOT SENT`); the `GPRL timing json:` log line carries the same payload either way.
`timing_window` keeps its shape: `solverVersion` `gprl-clone/2` (`-cbf`), `resolutionMs` no
longer clamped to the width, and with Click Between Frames `actualMs` includes the input's
sub-tick part (the tracker now passes the input event's `tSubTick` to the solver). The server
must allowlist `gprl-clone/2*` before these windows count (backend side of the design, §4.2).

**Settings.** `measure-sequence-adjusted` (on), `measure-joint-share` (off: the v0.6.1 joint
windows, previously `measure-sequences`), `solver-trace-max-ticks` (0 = off; 0-40),
`solver-trace-overlay` (off).

**Debug view (AUDIT §11).** With `solver-trace-max-ticks` > 0 the engine records the clones of
every local window at most that wide (8 per attempt) and prints the `GPRL trace:` block; the
facade hands the last one to the HUD (`hud::showTrace`, pure view `core/solver/trace_view.hpp`,
host test `trace_view_tests`). With `solver-trace-overlay` on it is drawn into the level's object
layer (a `CCDrawNode`: reference white, valid green / cyan, invalid red / magenta with the death
hitbox, killer rect orange, input point yellow) next to a middle-left panel with the input's
time, tick, percent, type, gamemode, speed, gravity, mini, local / sequence-adjusted windows,
hold range and, per invalid trajectory, the death point and why it failed. The Session tab shows
the one-line summary. Drawing only, never physics; **not run in-game**.

**Tests** (host, `run_tests.ps1`): `timing_units_tests`, `timing_status_tests`,
`timing_result_event_tests`, `trace_view_tests`, `telemetry_roundtrip_tests`
(`testTimingResultFixture`: golden `batch-timing-result.json` parsed, validated, re-serialised
byte for byte, the builder's payload equal to the fixture, the same malformed cases as the TS
side) plus the solver suites of the design (`sequence_adjusted_tests`, `cluster_tests`,
`result_ledger_tests`, `trace_tests`, `attribution_tests`, ...). TypeScript:
`shared/test/timing-result.test.ts`, `telemetry.test.ts`, `golden-fixtures.test.ts`; fixture
check `tests/fixture-checks/telemetry.mjs` (cross-event rules).

## v0.7.1 (the Fable review deltas)

The independent redesign `docs/TIMING_SOLVER_V2_FABLE.md` (§4 delta list) applied to the engine,
the telemetry schema / validators and the golden fixtures; the full per-delta table with the
tests is `docs/TIMING_SOLVER_V2.md` §9.A. Solver version strings are unchanged (`gprl-clone/2`,
`gprl-clone-sa/1`, `gprl-timing-status/1`: nothing of 0.7.0 was ever accepted by a server), the
telemetry additions are optional keys (revision 4 unchanged, the v0.7.0 golden byte-identical).

- **D1** an SA side that ends `undecided` (or at a limit) right at the local window's last pass
  inherits the LOCAL bracket (`SAPlanner::result`): the sequence window never reports an edge
  inside the local one; the gate mirror checks containment on pass AND reported edges.
- **D2** a family member's look-ahead runs one horizon after its LAST moved input (a chain3
  follower 90 ticks after the member used to get ~40 frames of evidence); the SA job's look-ahead
  is the planner's bound (`SAPlanner::lookAheadBound`), the ring holds it (`saWorstSpanFrames`).
- **D3a** both local sides open to the search limit: the sequence window is the local copy,
  `decided`, reason `open_range`, no SA job (not even the pair walk).
- **D3b** every sequence edge says what proved its widening: `proof` local / rejoined / survived
  and `provenPassMs`; reason `sa_survived_only`; `GPRL timing:` lines print `proof r/s`.
- **D4** (already in 0.7.0) a MISS bracket skips untested points and flags the gap.
- **D5** the island-scan pruning is back for a side whose nearest tested shift PASSED
  (`hitPruneLimit`); a side whose nearest shift died (a miss) is never pruned.
- **D6** one real / would-be death is ONE miss: the latest input of the died run with a passing
  shift (`miss_attribution.hpp`); earlier jobs are `sequence_dependent (miss_downstream)`, `miss`
  false, no `timing_window`. Two object -1 deaths match only at the same x (|dx| <= 2).
- **D7** the real player's speed changes are recorded from the ring; a job whose span contains one
  is `low_confidence (speed_change_in_lookahead)`. **D7b** the speed portals are off the clones'
  trigger list.
- **D9** the full pair walk is off by default (`pairWalkForPresses`); `pair` only when the SA
  walk's own pair trials cover it. **D10** the SA queue ranks positions measured twice this
  level visit behind unmeasured ones. **D11** `engineSubTickMs` (the engine's sub-tick) is sent and
  is the source of `actualMs` for both events. **D16** `fail_near_horizon` (informational).
- Not in the mod: D8a / D8b / D13 (tools) / D14 / D15 (backend); D12 skipped (NICE; needs a
  moving-object scan at level load that nothing here can verify); D17 is a design note.
- Fixes from a read-only look at the owner's live log (`Geode 2026-09-30 19.47.37.log`, an
  intermediate 0.7.1 build; numbers in `docs/TIMING_SOLVER_V2.md` §9.A): a one-tick window whose
  `latestMs - earliestMs` missed its one-tick resolution by float noise was refused by the server
  (`window_below_resolution`, 18 of 21 warnings) and labelled `width_below_resolution`: the payload
  now carries that exact difference as `resolutionMs` when the two differ by noise only (1e-6 ms);
  a miss window with an invalid trial names the trial's reason (was `payload_invalid`); an SA
  control that dies together with the real player drops the job as `death` (`sa_death_in_span`),
  not as a control mismatch.
- MOD verifier fixes (host-tested, `docs/SOLVER_DESIGN.md` §14.5): an SA trial of the input alone
  at a shift the lockstep job left untested proves `rejoined` / `survived`, never `local` (the
  server's gate refused such a widened side as `proof_inconsistent`); the SA planner reads the
  local window within its own late neighbour limit, so a `neighbour` SA side never inherits a
  bracket the local window does not have (the result fell back to `unresolved (payload_invalid)`).
  Neither case occurs in the owner's v0.7.x logs.

**The integrator's final v0.7.1 build (`.geode` SHA-256 `B0225A3A...8CECC953`) ran once, unattended,
in the owner's session of 2026-09-30 20:36-20:41** (`Geode 2026-09-30 20.36.22.log`; level
Congregation 68668045, 25 attempts, trust `noclip_modified`, telemetry revision 1 so every
`timing_result` is `NOT SENT`). Read-only numbers: 331 `timing_result`s, 0 fallbacks, 0 gate-mirror
warnings, shadow 0 mismatches in 33 277 steps, at most 1 miss per attempt (15 misses, 12
`miss_downstream`), 82 SA jobs measured and 0 dropped, 0 undecided SA sides, every sequence proof
`l/l`, sim 0.1 ms/frame (peak 0.8), never throttled, no GPRL error line. Nobody worked through the
checklist, and the session had no wave section (no `proof r/r` line to check). The verifier's rebuild
with the two fixes above was not run in-game. An intermediate build (installed during the
integration, before the three fixes above) ran in the owner's session of 2026-09-30 19:47.
Checklist: "v0.7.1 additions to that check" in the solver section above.

## v0.8.0 (live-state isolation, step 1)

The clone engine no longer writes anything into the live game (docs/SOLVER_DESIGN.md §15,
docs/LIVE_ISOLATION_DESIGN.md). In-game check, on a level with orbs, pads, gravity portals and at
least one gamemode portal, and on a dual level with gravity portals:

- `GPRL solver: ready - ... solver gprl-clone/3, sa gprl-clone-sa/2 ...` at level entry.
- No `GPRL isolation: tripwire ...` line during normal play. A tripwire line names a GD function the
  census missed; send the log.
- Near a gamemode portal, windows are dropped as `control INVALID (gamemode portal (not modelled
  yet))` or `Invalid (gamemode portal ...)`: expected in step 1, fixed in step 2.
- `GPRL isolation: paused - checkpoint timeout pending` may appear once per practice death: expected.
- The symptoms the fix targets: the camera no longer drifts on Y after passing portals, and in dual
  sections both players keep their own gravity through gravity portals (the v0.7.x shadow lines
  `y velocity ... vs ...` with one side `flipped` at the same position must be gone).
- The 5 s summary prints `isolation tripwires N`.

## v0.8.1 (live-state isolation, step 2: gamemode portals on the clone)

The step 1 check above still applies, with one line changed: near a gamemode portal the windows
are measured again (no `Invalid (gamemode portal ...)` outside dual sections). In-game check, on a
level with ship / ball / ufo / wave / robot / spider / swing portals and a cube portal back:

- Timing windows for inputs right before and right after a gamemode portal appear in the log
  (`GPRL timing:` lines) and the `Invalid (gamemode portal (not modelled yet))` drops are gone.
- No shadow mismatch at the portal frame (`GPRL solver: shadow mismatch ... game mode` or
  `... gravity` right where a portal is passed would mean the transcription differs from GD): the
  shadow clone passes every portal the real player passes.
- The 5 s summary prints `portals modelled N` (N grows by one per clone that passes a portal; a
  level with portals and zero modelled portals means the model never ran).
- In a dual section a gamemode portal still drops the clone: `Invalid (gamemode portal in dual (pair
  model pending))` is expected until step 4.
- No portal circle / shine effect appears anywhere except where the real player passes.
- With `solver-verbosity` 2: `GPRL isolation: portal model type T on clone N at x=... step S` for the
  first three per attempt.

## v0.8.2 (live-state isolation, step 3: the invariant check)

The analyzer compares a snapshot of the live game before and after everything it does
(docs/LIVE_ISOLATION_DESIGN.md §3). In-game check, any level with orbs and portals, then a dual level:

- `GPRL isolation: ready - gprl-isolation/2, portal model gprl-portal-model/1, live state
  gprl-live-state/1, invariant check every block ...` at level entry (`every clone step` with the
  Debug log or the `isolation-check` setting).
- The 5 s summary prints `invariant every block (blocks B, checks C at X us, breaches 0, inputs
  skipped after a breach 0)`: C grows with B (C < B only after `GPRL isolation: the invariant check
  costs ... us per block ...: checking every N blocks`, which is expected on a slow PC), X stays
  in the low microseconds.
- **No `LIVE_STATE_MUTATION_DETECTED` block in the whole session.** One means the analysis still
  writes the live game: the block names the field (`camera.position.y`, `player2.gravity`, ...),
  the `GPRL isolation: LIVE_STATE_MUTATION_DETECTED ... | block: ... | action: ...` line names the
  block and the clone (with `every clone step`); send the log. After it the attempt measures
  nothing more (`input ... skipped: isolation breached this attempt`) and the attempt line ends
  `isolation BREACHED`; the next attempt measures again with the check running per clone step.
- Timing results of a breached attempt arrive as `live_mutation_detected` (no window) and the
  server stores them unusable; the website never counts them.
- `GPRL timing: ... NOT SENT (this server validates telemetry revision 4, a timing_result of this
  mod needs 5)` means the Worker was not redeployed with revision 5: deploy first.

## v0.12.2 (Patreon plans: the plan line, Connect Patreon + code, Sync Patreon, analysis speeds)

docs/contracts/patreon.md, shared/src/entitlements (plans.ts, api.ts). A plan is a SERVICE level:
nothing in the mod's measuring, telemetry, sigma/s, verification or upload content reads it.

**Where the plan comes from.** Only `GET /v1/me/entitlements` (device token), parsed by
`core/entitlements.cpp` (host-tested, `tests/entitlements_tests.cpp` + `tests/fixtures/entitlements.json`).
The telemetry worker asks after a successful Connect, when it starts with a saved token, every 10
minutes (a network failure retries after 2), after Sync Patreon, and right after a confirmed
Patreon code (the link only exists from then on). The answer lives in `client::Status`
(memory only - never a setting, a saved value or a file) and is forgotten on Disconnect (Free). A
failed fetch keeps the last answer and never disconnects the device. Parser rules: missing /
unknown fields are Free; an unknown plan is Free; a paid plan whose status is `none` / `expired` is
Free; `trial` only on a paid plan; Priority Evidence only on Pro and never on a trial;
`features.backgroundAnalysis` is clamped to the plan (a Free body claiming `fastest` stays `normal`);
the label shown is the mod's own (`GPRL Pro`), never server text; every other member is ignored.

**Account tab** (only while connected), one line under the connection line:

- `Plan: Free` / `Plan: GPRL Supporter (Patreon)` / `Plan: GPRL Plus (Patreon)` / `Plan: GPRL Pro (Patreon)`,
  with ` - trial`, ` - cancelled, active until 2026-11-02`, ` - payment issue`, or
  `Plan: Free - Patreon membership expired`; when Patreon is connected also
  `   Last synchronized: just now | N min ago | N h ago | N days ago`. Before the first answer:
  `Plan: asking the GPRL server...`; after a failure without an earlier answer:
  `Plan: not known (<why>) - Free until the server answers`.
- At the right end of that line, while Patreon is not linked: **Connect Patreon** and **Enter
  Patreon code**; once linked: **Sync Patreon**.
- **Connect Patreon** -> `POST /v1/me/patreon/connect {"returnTo":"mod"}` on the worker; the
  `authorizeUrl` must start with exactly `https://www.patreon.com/oauth2/authorize?` (security
  review LOW-9; `entitlements::authorizeUrlAllowed`: something after the `?`, no whitespace,
  control / non-ASCII characters, quotes, backslashes or angle brackets, <= 4096 chars) and is then
  opened with `geode::utils::web::openLinkInBrowser` on the main thread ("Opened Patreon in your
  browser: allow GPRL there, then press Enter Patreon code"); the URL is never logged.
- **The code step** (security review: a link started from one GPRL account can never land on
  another; shared/src/entitlements/api.ts V1PatreonConfirmRequest). After "Allow" on Patreon the
  page the callback shows names the Patreon and GPRL accounts and a one-time code of 8 characters
  from `PATREON_CONFIRM_CODE_ALPHABET` = `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (no I, L, O, 0, 1).
  **Enter Patreon code** opens a small popup (src/PatreonCodePopup): "After you allowed GPRL on
  Patreon, the GPRL page in your browser / shows a code of 8 characters. Type or paste it here and
  press Confirm. / It works once, only for the GPRL account connected in this game.", a text box (8
  characters, the alphabet in either case, lowercase shown uppercased), **Paste** (a pasted
  `ABCD-2345` / `abcd 2345` is taken whole; anything else is filtered) and **Confirm**. Confirm sends
  `POST /v1/me/patreon/confirm {"code":"<CODE>"}` with the device token on the worker, only for a code
  that passes `entitlements::normalizePatreonCode` (trimmed, uppercased, spaces and dashes dropped,
  exactly 8 of the alphabet); else "The code has 8 characters: letters and digits 2-9 (no I, L or
  O)" and nothing is sent. The result shows in the popup and as a notification: success (200, a
  V1PatreonStatusResponse with `connected`) `Patreon connected: GPRL Pro` (the mod's own label for
  the answer's plan id), then the plan is fetched at once. Failures (`entitlements::confirmErrorText`
  / `confirmErrorTextFromBody`) are mapped by ApiErrorBody `error.details.reason` first, then
  `error.code` when it names a kind, the HTTP status only when neither does (409 -> link_mismatch,
  410 -> ticket_expired, 429 -> too_many_attempts): `link_mismatch` (409 conflict) "This Patreon link
  was started from another GPRL account, so it was cancelled. Connect again from your own account.",
  `already_linked` (409 conflict) "This Patreon account is already linked to another GPRL account",
  `ticket_expired` (410) "The link expired, try again", `too_many_attempts` (429 rate_limited) "Too
  many wrong codes: connect again", `ticket_invalid` (404), `invalid_body` (400) and anything else
  "Patreon could not be connected, try again". The code is never logged (the log line names the
  HTTP status, error.code and details.reason only) or stored.
- **Sync Patreon** -> `POST /v1/me/patreon/sync`, then the plan is fetched again; the notification
  says "GPRL: Patreon synchronized - Plan: ...", "no Patreon account is connected yet - press
  Connect Patreon, then Enter Patreon code", "Patreon memberships are not open yet" (`available:
  false` / 503 / code `unavailable`) or the server's error message (429: "used a moment ago - try
  again in a minute").
- To keep the clip lines clear of the clip button row, the `spool:` line is left out while both the
  plan line and a `Last error` line are shown.

**Analysis speeds** (`analysis-cpu`, now `low` | `normal` | `fast` | `fastest`; default still `low`).
The setting is what the player wants, the server's plan caps what runs (`core/sim/modes.hpp`
`resolveCpu`, host-tested in `sim_modes_tests` + `entitlements_tests`):

| tier | needs | analyzer worker burst / rest | extraction slice while playing |
|------|-------|------------------------------|--------------------------------|
| low | every plan | 8 / 8 ms (at most half a core) | 0.5 ms |
| normal | every plan | 16 / 1 ms | 1 ms |
| fast | GPRL Plus (backgroundAnalysis `faster`) or Pro | 32 / 1 ms | 1 ms |
| fastest | GPRL Pro (`fastest`, the Pro trial included) | 64 / 1 ms | 1 ms |

Free / Supporter keep exactly the 0.12.0 choices. A setting above the plan runs as the best tier the
plan allows (`normal` for Free / Supporter and while the plan is unknown, `fast` for Plus asking for
`fastest`); the Session tab's analyzer line and the Account tab's analysis line then end with
`Analysis speed: Normal (Fast needs GPRL Plus)` / `Analysis speed: Fast (Fastest needs GPRL Pro)`
(a granted plan speed shows `Analysis speed: Fast (GPRL Plus)`). Every tier: below-normal thread
priority, nothing more on the game thread (fast / fastest extract with normal's 1 ms slice), paused
under frame pressure and, with Record-Safe, during attempts. `analyzer::modes::setSpeedAllowance` is
called by the telemetry worker with every answer (Normal on Disconnect); one log line when the
effective speed changes.

## v0.12.0 (background level analyzer: modes, Record-Safe, read-only extraction, worker)

docs/BACKGROUND_ANALYZER_DESIGN.md (every section; the simulator itself is `core/sim`, the mod
integration is `src/analyzer`). Nothing in `src/analyzer` writes a GD field or calls a GD method
with a side effect; every file there starts with that rule.

**Settings** (title "Level analysis (background simulator)", seven settings):

| setting | default | effect |
|---------|---------|--------|
| `analysis-mode` | `full` | ONLY whether the background simulator runs (owner decision 2026-10-02): `passive` = no simulator - and so no level identity and no family notice either (they come from the same read-only copy of the level); `offline` = `full` = the background simulator. The live clone solver (the player's sigma/s source) runs in EVERY mode and cannot be switched off (`measure-windows` was removed); only `record-safe` stops it. |
| `record-safe` | off | no hidden clones, no bot / replay playback evidence, no noclip use; the simulator - and the worker's world build, gameplay hash and level identity - run only while no attempt is active (pause menu, menus; the death pause counts as part of the attempt). Telemetry stays passive. The text never claims list approval (design §9). |
| `analysis-cpu` | `low` | low: below-normal thread priority, 8 ms work bursts with 8 ms rests (at most half a core), extraction slices of 0.5 ms while playing; normal: 16 / 1 ms bursts, 1 ms slices. Outside an attempt (death pause) a slice is at most min(8 ms, 25 % of the frame target); 8 ms only for the first slice inside `setupHasCompleted`; no slice at all under frame pressure. |
| `analysis-cache` | on | ask `GET /v1/levels/:gdId/sim-cache` first; a `client` / `reviewed` result with solvedPercent >= 99 is used as is ("cached"). Local results are `analysis/<gdLevelId>-<gameplayHash>-<versions>.json` (analyzer / sim / gameplay-hash / extract versions in the name and the record: an older analysis is never reused); `analysis/` + `identity/` share a 200 MB least-recently-used cap. |
| `analysis-upload` | on | `POST /v1/me/level-sim { sessionId?, result }` with the device token when connected (the 2 MB limit is checked on that whole body); one retry after 30 s on a network failure; a 429 waits for the server's `Retry-After` (1-60 min, else 10 min). |
| `analysis-debug` | off | the analyzer's steps in the Geode log (extraction counters, rect checks, spans, worker phases, cache answers). |
| `family-notice` | on | the related-level notice (level families, docs/LEVEL_FAMILY_DESIGN.md); never shown in `passive` (no identity is computed). |

A popup on the main menu asks once (saved value `analysis-mode-chosen`): **Full (recommended)**
/ **Passive** plus the Record-Safe tick box with the §9 text; it writes the two settings. It is shown
only while that MenuLayer is the running scene (retried while a scene transition runs; a menu that
was left lets a later MenuLayer try again).

**What runs where**

- `src/analyzer/Modes` maps the settings onto `core/sim/modes.hpp` (`clonesAllowed`,
  `botPlaybackEvidenceAllowed`, `simAllowedNow`, `extractionSliceUs`, `summary`) and samples the
  frame time in `PlayLayer::postUpdate` (`core/analyzer_recorder.hpp` FramePressure: the last 30
  frames average > 1.5 x the target; target = CCDirector's animation interval (the FPS cap), and
  with VSync on (`GameVar::VerticalSync`, "0030") the slower of that and the display refresh rate,
  clamped 1..50 ms). A real physics step (`processCommands` of the open level, never a clone step)
  clears the pause fact, so a resume path that bypasses `PlayLayer::resume` cannot leave
  Record-Safe "paused". **The live clone solver's one gate**: `oracle::setup` creates no clone at
  all unless `clonesAllowed()` (= enabled and not Record-Safe; the state line reads "live solver
  off (Record-Safe)"); a pause-menu switch to Record-Safe stops the engine for the rest of the
  visit (`CloneEngine::stopForVisit`: every open result ends like at a level exit, every clone is
  freed, but no PlayerObject is removed mid-level - that happens at quit / goEdit as always);
  `refreshGate` keeps the gate closed.
- `src/analyzer/Extract` walks `PlayLayer::m_objects` in time slices from `postUpdate` (first slice
  8 ms inside `setupHasCompleted`), each slice wrapped in a `LiveStateSnapshot` before / after
  (`captureLive`; the level length / end portal are read inside it) plus a fingerprint of every
  64-object chunk (rect cache + dirty flags, activation / disabled bytes, position, rotation,
  visibility). A difference logs the owner's `LIVE_STATE_MUTATION_DETECTED / field / before /
  after` blocks and `GPRL analyzer: LIVE_STATE_MUTATION_DETECTED ... | action: extraction abandoned
  ...`, the HUD says "stopped (isolation)" and `oracle::tripIsolationBreaker("analyzer_extraction")`
  makes the clone engine behave as on its own breach. A failed allocation (or any error) stops the
  walk for the visit ("out of memory"); nothing is thrown out of a GD hook (the Analyzer facade
  guards every entry point the same way).
- **Rects (gprl-extract/2): GD's exact formula, never GD's cache.** `getObjectRect()` /
  `getObjectRect2()` / `getBoxOffset()` / `getOrientedBox()` are never called (they write GD's
  caches). `core/analyzer_extract.hpp` `gdObjectRect` replicates them instruction by instruction from
  the 2.2081 disassembly (getObjectRect 0x1976c0, getObjectRect2 0x197850, getRealPosition 0x197b20,
  getBoxOffset 0x1a17d0, updateIsOriented 0x1a1730, updateOrientedBox 0x1a1570, OBB2D 0x6da80 /
  0x6e270, setRotation 0x197e00): size = (`m_scaleX` x `m_width`) x `m_spriteWidthScale` (fabs
  only with `m_isMirroredByScale`, swapped when `m_isRotationAligned` = rotation exactly +-90 /
  +-270), centre = position + box offset, rect = centre - size / 2; objects GD orients (not type 7,
  types 0 / 21 / 25 only when no-touch, int(rotation) % 90 != 0, no radius) get the oriented box's
  bounding rect. The World uses the level's STATIC fields (`m_startPosition`, `m_startRotationX /
  Y`, `m_startScaleX / Y`, `m_startFlipX / Y`); the same formula on GD's LIVE fields is compared
  with every clean `m_objectRect` (log: `estimate check 0 of N clean rects off by > 0.05` - it must
  read 0). Every offset is pinned by a `static_assert` in Extract.cpp. GD's anti-cheat spike
  (`m_anticheatSpike`, moved onto player 1 every step) is skipped. The World's level start is ONE
  rule (x 0 on the floor line, the level settings' mode), whatever the visit entered with.
- Only STATIC level properties enter the World (the gameplay hash `gprl-gameplay-hash/2` covers
  rects, `hidden` and positions): `hidden` = the editor's no-touch flag (`m_isNoTouch`), runtime
  visibility is only counted; an object a move trigger already moved is kept at `m_startPosition`
  (self-checked: when most objects disagree with the field it is not trusted). The 400 000-object
  cap applies while collecting (the World is cut at the leftmost dropped object, `too_large`).
  Slopes take GD's own rules on the static rotation / flips (`determineSlopeDirection` 0x19c2c0 and
  `isFacingDown` 0x1a1910 replicated; GD's live `m_slopeUphill` is only compared) in the simulator's
  encoding (1 floor "/", 2 floor "\", 3 ceiling "\", 4 ceiling "/"). Passable blocks, hazard / odd-angle / sideways (90 / 270 degree)
  slopes, dual / mirror / teleport portals, gravity-toggle portals, dash / custom / teleport orbs,
  collision blocks, special objects and the gameplay triggers of `kGameplayTriggers` become §4.7
  unsupported spans; objects in a group targeted by a move / rotate / scale / toggle / follow
  trigger get their own span. Colour / pulse / alpha / camera / song triggers are harmless (not
  hashed: a colour edit keeps the cache).
- `src/analyzer/Recorder` records player 1 passively: one `RecordedTick` per 240 TPS tick (the
  end state read in the `processCommands` pre-hook once the update deltas add up to a whole tick;
  half ticks and CBF splits record once), every jump press / release with its tick and sub-tick
  (the simulated fraction of the tick - the tracker's SubTickClock figure), the start state GD
  placed the player in (speed from the level / StartPos settings). Not recorded: practice mode,
  platformer, TPS bypass != 240 / speedhack (the menus the tracker read at the attempt start; the
  recorder's attempt starts after `tracker::onLevelReset`); dropped at the end: noclip seen, bot
  playback in Record-Safe. Caps: 60 000 ticks per attempt, 40 attempts per level visit (completed
  first, then longest; +10 completed after the cap).
- `src/analyzer/Worker` (one thread, `THREAD_PRIORITY_BELOW_NORMAL`) finishes the World
  (sort / cap / spans) and computes `sim::gameplayHash` off the game thread - only while
  `simAllowedNow()` (a level handed over during a Record-Safe attempt waits in a small ordered
  backlog, its attempts and exit attached) - then: the kept job of the same gameplay version (10
  minutes after a visit; a revisit re-arms uploads that waited for a session) -> the local result
  file -> the server cache -> a new `sim::Job`. Recorded attempts reach the job only inside its
  run (they are verified there). Results are written locally first, then sent; an unsent one is
  sent on a later visit. Every message, job run and upload is guarded (an error marks that entry
  Failed, the thread never dies); no request starts once the game is exiting. The session per level
  version comes from the telemetry worker's own (session, level hash, generation) triple. Status for
  the HUD / popup is a mutex copy.
- HUD bottom-left: `| Level analysis: reading level 37%` / `searching 42%, 38 s` / `verified 96%` /
  `done, solved 100%` / `cached` / `Record-Safe: waiting for the attempt to end` /
  `paused (game busy)` / `stopped (isolation)`, plus ` - sent (Physics Verified)` once the server
  answered. Session tab: the mode + status line and the job line (progress, coverage, attempts,
  world counts, upload, recorder state, frame time / target). Account tab: `Analysis: ...`.

**In-game checklist (design §10.5; not run in-game yet)**

1. Settings: the new title and seven settings exist; the main menu shows the mode popup once
   (Full (recommended) / Passive); choosing Full + Record-Safe writes both settings and the Account
   tab reads `Analysis: offline simulation (Record-Safe: live solver off)`.
2. Record-Safe (in ANY mode): the log has `GPRL solver: live solver off (Record-Safe) - no hidden
   clones on this level` and NO `pool warmed` / `snapshot roundtrip` lines; the Session tab's timing
   line says `Timing windows: live solver off (Record-Safe)`. Passive / offline / full WITHOUT
   Record-Safe still measure (`pool warmed` present). Switching Record-Safe on in the pause menu:
   the solver stops for the visit, no clone object disappears mid-level, the next level starts off.
3. Full mode, `isolation-check = every clone step`, a level with orbs + portals: play several
   attempts; the log shows `GPRL analyzer: extraction done (gprl-extract/2) - ... isolation checks N
   (0 differences)` and never `LIVE_STATE_MUTATION_DETECTED`.
4. The extraction line: `max slice` <= ~600 us while playing at `low` (8 ms only in the first
   slice at level load), `anti-cheat spike skipped 1`, `estimate check 0 of N clean rects` - report
   anything but 0 (`GD's rect formula differs from GD's cache: #id type ...` lines name the objects;
   the formula would then be wrong for them), `box offsets 0 of M off`, `oriented flag 0 off`, and
   `slopes N by GD's rules (0 corner derivation / K live m_slopeUphill differ)`.
5. The worker line `level <id> gameplay version <16 hex> (gprl-gameplay-hash/2)`; leave and
   re-enter the level, also once from a StartPos: the SAME hash and `resumed the kept analysis`;
   re-enter after it finished: `cached analysis used (this computer ...)`. Deadlocked (level 20) is
   simulated again once (its /1 result is not reused) and no longer reports physics 0.0 %.
6. Frame time: the Session tab's `frame X / target Y ms` stays near the target while the
   simulator runs; HUD `paused (game busy)` appears only during real stutters.
7. Record-Safe on: during an attempt the HUD reads `Record-Safe: waiting for the attempt to end`;
   in the pause menu the job progresses (a level entered during an attempt is built and hashed only
   then).
8. When a job finishes: `analysis done - ...` then `analysis of level <id> sent - stored yes ...`
   (or `not sent: the server does not know this level version yet` before the session existed).

## v0.14.0 (Ship: lockstep compensation; docs/SHIP_SOLVER.md)

The connected-control inputs (ship) get their sequence-adjusted window from the lockstep
compensation planner (core/solver/compensation.hpp) instead of the delayed replay. In-game check,
on a level with ship sections (play from 0, trust `allowed`, `measure-sequence-adjusted` on):

- The ready line names `solver gprl-clone/5, sa gprl-clone-sa/4`; the session line says the server
  validates `telemetry revision 6` (else every `GPRL timing:` line reads `NOT SENT (... needs 6)` and
  the Worker must be deployed first).
- Ship inputs print `GPRL comp: input #k (job j) decided: late +1 pair[+1] compensated +2 comp1[+1]
  compensated +3 fail@... -> fail | early ... | followers 2 | trials N | ... control S steps
  compared` seconds after the input (the trials settle 2 s after their last moved input). An
  `undecided` line says why per side (`undecided(sa_not_measured_budget)`, `neighbour`).
- The `GPRL timing:` line of such an input shows `sequence <window> comp1 decided proof c/l`
  (c = compensated) and the `GPRL timing json:` payload carries `"adaptationUsed":["comp1"]` and
  `"compensation":{"earlyOffsetsMs":[...],"lateOffsetsMs":[...]}`; a ship input with a downstream
  local edge and no decided sequence window stays `sequence_dependent` (never rated).
- The 5 s summary ends with `compensation (ship): jobs N (open M), trials T (P passes, C compensated,
  D died, I invalid), clone steps S, controls K (mismatch 0), decided X, undecided Y, ...`. `mismatch`
  must stay 0 or near it: a delayed control that differs from the live player drops the job
  (`GPRL comp: ... DROPPED sa_control_mismatch: the delayed control differs from the live player at
  step ...`); many of them on one level = time-dependent geometry the catch-up replay cannot
  reproduce (report the level).
- `sim .. ms/frame` stays under ~4 ms with ship sections (the trials are lockstep clones; the
  job cap is 12 jobs x 6 clones); `throttled=1` should stay rare.
- Must NOT appear: `[fallback:`, `payload_invalid`, `LIVE_STATE_MUTATION_DETECTED`.
- On the website / Session tab after a recalculation: Ship shows `calibrating` until this build has
  measured enough Ship (every older Ship sample is `legacy_untrusted`, docs/SHIP_SOLVER.md SH-D4); the
  per-gamemode debug carries `rawSampleCount / usedSampleCount / effectiveSampleCount / clusterCount`.

## v0.11.0 (the settled look-ahead)

docs/SOLVER_DESIGN.md §16. In-game check:

- The 5 s summary ends with `settled look-ahead ON (8 s max, ground 24 ticks): settled N,
  unsettled M`; N grows with every measured click on a cube / ball / robot / spider section, M stays
  small (long ship sections or copies still airborne at 8 s).
- An orb click right before a gamemode portal: the window line appears only after the copies
  landed past the portal (seconds, not 0.5 s); a shift that would die on the hazard after the
  portal is now a fail edge instead of a "survived" pass.
- `Mod settings > Timing windows > Settled look-ahead` OFF restores the old 0.5 s behaviour (the
  summary says `off`); the solver version in the log / server is `gprl-clone/4` either way.
- The server accepts the windows (Session tab: windows sent / stored go up; no
  `solver_version_invalidated` rows).

## v0.10.0 (runs too long to upload: the warning and the YouTube link)

docs/CLIPPING.md §9b. In-game check (needs the Worker with migration 0038):

- Play a run longer than about 2:25 (720p, three sound tracks) that ends past 50 %: the top-right
  notice "Runs over 2:25 (at 720p) are too long for GPRL to upload ..." appears once per level
  session.
- A run the server asks to verify whose clip is over 95 MB: no upload; the notice says it was
  SAVED and to send a YouTube link; GPRL menu > Account shows a **YouTube link** button first in
  the clip row; the popup lists the clip, the three steps, a text box (Paste works with a copied
  link) and Send link. Clips folder opens the saved file.
- Send a link of a PRIVATE YouTube video: refused with "set it to Unlisted or Public"; an Unlisted
  one: "YouTube link sent to the GPRL moderators", the status line says "YouTube link sent", the
  moderators' case lists it.
- Noclip deaths (Mega Hack or Eclipse noclip on): fly through one spike for a second: the Session
  tab's "would-be deaths (noclip)" goes up by ONE (it went up by dozens before), the same number
  Eclipse's own Noclip Deaths label shows; through three separate spikes: three.

## v0.9.0 (run review: the "!" notification, the automatic verification clip, the desktop sound track)

Owner decision 2026-10-01; server side docs/SECURITY.md §10.6, mod side docs/CLIPPING.md §2-§3.
In-game check (needs the Worker with migration 0037 and evidence storage ON, `docs/DEPLOY-RUNBOOK.md`
§10; the owner's saved settings may still say `clipping` / `clip-mic` off: turn them on):

- Complete a rated demon you have few attempts on (or die past 50 % within the first 10 attempts):
  within a few seconds (the next batch ack or the session end) a dark panel with a red pulsing
  `!` appears TOP RIGHT, "Verify this run (completion on attempt N): GPRL is sending the clip to
  the moderators", also with Show HUD off; the Geode log has `GPRL: batch seq N ack asks to verify
  1 run(s)` / `GPRL clip: the server asks to verify attempt ...` / `... will be sent for
  verification as soon as it is ready` / the upload lines. No four-choice popup for that clip.
- The Account tab shows the clip as Uploading -> Uploaded; the website's queue has a "Run review"
  case with `performances.attempts` = your total attempts on the level (GD's count, not 1).
- A level you have thousands of attempts on: no notification, no case (`GPRL: ... none too good
  (attempts on level 3041 ...)` in the Worker's debug lines).
- The clip has three sound tracks (`ffprobe`: Game, Microphone, Desktop; Discord / music in the
  Desktop one), and the log says `recording the desktop sound '<device>' at 48000 Hz stereo`.
- With Clipping off the notification says to turn it on; with the attempt gone from the buffer it
  says so; nothing is sent either way.

## v0.8.5 (timing window history with the HUD line off)

With `Show HUD` OFF and `Show timing window history` ON: the middle-right panel with the last 8
presses / releases ("PRESS 4.17 ms [-2.08 +2.08]", then "local 1.00 f ... ok" = frame perfect)
must still appear and update; before v0.8.5 it vanished with the line.

## v0.8.4 (every level feeds calibration)

In-game check on a NON-demon level (any rated non-demon, an unrated upload, or a local level):

- The HUD line says `Not a rated demon: not on the levels list (still feeds your calibration)`.
- The Profile tab's calibration moves after the session (and mid-session with Live updates on);
  `GET /v1/me/calibration` carries no "did not count" note.
- The website's levels list does not show the level (rated demons only).

## v0.8.3 (live-state isolation, step 4: the dual pair shadow)

In-game check on a dual level with gravity portals and a gamemode portal inside the dual section
(Acheron x~5289, Sakupen Hell x~15380 / 15488, trollmachine sp x~27285 were the reported spots):

- `GPRL isolation: ready - ... dual = pair shadow (...)` at level entry; the first dual step logs
  `GPRL dual: pair shadow on (dual section at x=..., real P2 uid ...)`.
- `GPRL dual: 5 s - pair shadow N steps / M mismatches ...`: M stays 0 (or near it) through gravity
  portals and gamemode portals inside the dual section. A `GPRL shadow mismatch #k (dual pair) ...`
  line names which player (P1 / P2) differs and how (`gravity`, `game mode`, `position ...`): send it.
- **Both real players keep their own gravity through every gravity portal in dual** (the v0.7.x
  symptom: one player's gravity reverted right after the portal). No `LIVE_STATE_MUTATION_DETECTED`.
- `input #n ... skipped: dual mode (pair shadow only, not measured yet)` for inputs inside dual
  sections: expected (pair jobs are the next step).
- `solver-dual = off` removes every clone step in dual (the `GPRL dual:` lines disappear).

## Status of in-game verification

**Summary (2026-09-30, the state this README describes).** Created by gmo.

| version | what | host tests + build | in-game |
| ------- | ---- | ------------------ | ------- |
| v0.1.x-v0.3.0 | recorder, session flow, Connect, menu | green, installed | exercised as part of the v0.4.x run below (a connected session whose batches reached the live server is what produced those windows); the menu tabs, the Website code panel and the local-only spool have no recorded in-game check |
| v0.4.0-v0.4.2 | clone solver, last-window readout | green, installed | **ran in-game 2026-09-30 00:19-01:20** (three sessions, six levels of 2.5k-219k objects, `solver-debug` windows / verbose): real `timing_window` events reached the live server (first level GD 68848817, where about 20 % of the windows were dropped as control mismatches near orbs: docs/LEVEL_ANALYSIS.md); v0.5.0 addresses the mismatches |
| v0.4.3 | HUD follows the server's display state (the server shows the σ/s from 50 % Rating Confidence, 80 % until the owner's second decision of 2026-09-30; the mod holds no threshold, it reads `displayState`) | green, installed | not run (needs the Worker with migration 0020, not deployed) |
| v0.5.0 | activation slots, speed changes, adaptive budget | green, installed | **not run** |
| v0.5.1 | HUD history, coverage block, "not counted", bot playback as level evidence, capture fields | green, installed | **not run** |
| v0.6.0 | clipping buffer, popup, upload client | green, installed | **not run**; no upload was ever performed |
| v0.6.1 | sequence windows | green, installed | **not run**; the live Worker predates telemetry revision 3, so the events are logged `NOT SENT` until it is deployed |
| v0.7.0 | timing solver v2: attributed local edges, sequence-adjusted windows, statuses, `timing_result` (revision 4), traces + overlay, replay breaker | host tests green; mod build by the solver builder / lead | **not run**; `timing_result` is logged `NOT SENT` until a server advertises telemetry revision 4, and `gprl-clone/2` windows count only once the server allowlists them |
| v0.7.1 | the Fable review deltas: inherited SA edge, look-ahead from the last follower, open_range, proof kinds, one miss per death, speed-change flag, hit-side pruning, pair walk off, position-aware SA queue, engine sub-tick; float-noise width fix, miss-window invalid reason, SA control death with the real player; verifier: SA-trial proofs never `local`, SA reads the local bracket within the late limit | integrator: host tests 32 suites green, clean `build.ps1` (log `build/build-log-v071-modint-final2.txt`), installed byte-identical (`B0225A3A...`). Verifier (scratchpad copies, because the calibration stream's uncommitted header did not compile): 32 suites / 120 578 checks / 0 failures, clean build 46 objects / 0 warnings, **not installed**; the lead rebuilds once `calibration.hpp` compiles (`docs/TIMING_SOLVER_V2.md` §9.A-V) | the integrator's final build ran **unattended** 2026-09-30 20:36-20:41 (Congregation, 25 attempts, read-only numbers above and in §9.A-V); an intermediate build ran 19:47; the verifier's fixes **not run**; the checklist was not worked through |

Last full check of the tree as it stands (2026-09-30, C/T stream): host tests 22 suites /
111 707 checks / 0 failures (`run_tests.ps1`); `build.ps1` after deleting the GPRL objects, PCH
and link outputs (log `build/build-log-ct-clean.txt`): 47 steps, 0 warnings, exit 0, the
installed `.geode` byte-identical to `build/gmo12.gprl.geode` (SHA-256 `40B6B7C6...`, 3 919 177
bytes; the only change against the v0.6.1 package is the description line in `mod.json`).
C/T verifier, same day: host tests re-run 22 suites / 111 707 checks / 0 failures; `build.ps1`
re-run as an incremental build (2 steps: no source changed, the package was zipped again), exit
0, so the installed `.geode` now has SHA-256 `A9355D2B...` (same 3 919 177 bytes, byte-identical
to `build/gmo12.gprl.geode`; its `mod.json` is v0.6.1 and ends "Created by gmo.").
What to look for in the first log of each version is in the version's section above; the dated
paragraphs below are the reports of each build, newest first, kept as written.

2026-09-30, v0.6.1 (sequence windows, stream M4): see the checks in `docs/STATE.md` §9l. **Not
run inside Geometry Dash**: the delayed replays, the per-job controls, the budget and every
`GPRL sequence:` log line are unverified until the owner plays (checklist in "v0.6.1" above).
Host tests 22 suites / 109 370 checks / 0 failures (`sequence_tests` 102 028,
`telemetry_roundtrip_tests` 1451 with the new golden `batch-sequence.json`), clean `build.ps1`
(GPRL objects and PCH deleted first, log `build/build-log-v061-clean.txt`, 47 steps, 0
warnings), installed `.geode` byte-identical. The live Worker still validates telemetry
revision 2, so until it is deployed the mod measures sequences and logs `NOT SENT`.
Verified 2026-09-30 (M4 verifier; `docs/STATE.md` §9l last bullet): three fixes (a job cut short
no longer reports a share from an incomplete grid; no sequence work within 0.5 ms of the load
throttle; server-side int4 clamp), host tests now 22 suites / 111 707 checks / 0 failures
(`sequence_tests` 104 365), rebuild after `ninja clean` 76 steps (dependencies included), exit 0,
installed `.geode` byte-identical (log `build/build-log-m4-verify.txt`).

2026-09-30, v0.6.0 (clipping buffer, stream M3): see the checks in `docs/STATE.md` §9j. **Not
run inside Geometry Dash** and no server implements the upload endpoints: the GL read-back (next
to the In-Game Clipper), frame pacing, the FMOD tap and picture / sound sync, the microphone
path, the popup on the end screen / pause menu, the Account tab row and any upload are
unverified until the owner plays with Clipping on; `docs/CLIPPING.md` §10 lists the log lines.
Verifier pass the same day: host tests 21 suites / 7245 checks / 0 failures (`clip_tests` 507,
`clip_flow_tests` 1980), clean `build.ps1` (GPRL objects and PCH deleted first, log
`build/build-log-v060-verify.txt`, 46 steps, 0 warnings), installed `.geode` byte-identical.

2026-09-30, v0.5.1 (stream M1): see the checks in `docs/STATE.md` §9h. **Not run inside
Geometry Dash**: the history panel layout, the coverage fetch against the live Worker, the
hints in the live session request, the bot-playback measuring path and the attempt clock's
pause handling are unverified until the user plays; the log lines above are what to look for.

2026-09-30, v0.5.0 (solver tuning, stream M2): host tests 18 suites, 4528 checks (the 16 of
v0.4.x plus `activation_tests` 43 and `tuning_tests` 72), 0 failures; `build.ps1` from a clean
state (GPRL objects, PCH and link outputs deleted first, log `build/build-log-v050-clean.txt`,
35 build steps) compiled with zero compiler / linker warnings, packaged and installed
`gmo12.gprl.geode` (byte-identical to `build/gmo12.gprl.geode`). Diagnosed from the user's
2026-09-30 00:19-01:20 logs (SOLVER_DESIGN §12); **the fixes are not yet run inside Geometry
Dash**: the next log must show `rings`/`speed` mismatch counts of 0 in the summaries, `peak`
under ~8 ms, and coverage >= 80 % on a busy level.

2026-09-29, v0.4.0 review pass of the port (see "Review pass of the port" above and
SOLVER_DESIGN §11): host tests 16 suites, 4248 checks (`pass_planner_tests` 159), 0 failures;
`build.ps1` recompiled `pass_planner.cpp`, `GdOracle.cpp`, `CloneEngine.cpp` with zero warnings,
linked, packaged and installed `gmo12.gprl.geode` (log `build/build-log-v040-review.txt`, the
installed file is byte-identical to `build/gmo12.gprl.geode`); `npm run typecheck -w @gprl/api`
exit 0, `npm run test -w @gprl/api` 15 files / 178 + 11 files / 147 passed (allowlist unchanged by
the review). Still **not run inside Geometry Dash**.

2026-09-29, v0.4.0 (timing-window solver): host tests 16 suites (the 11 of v0.3.0 plus
`pass_planner_tests` 157 checks, `timeline_tests` 51, `window_event_tests` 67,
`geometry_hash_tests` 58, `fingerprint_build_tests` 81), 0 failures; `build.ps1` compiled the new
engine sources with zero compiler warnings from a clean state (GPRL objects, PCH and link outputs
deleted first, log `build/build-log-v040-clean.txt`; the final incremental build after the last
fixes is `build/build-log-v040-final.txt`), linked and installed `gmo12.gprl.geode`. API: `npm run typecheck -w @gprl/api` and `npm run test -w
@gprl/api` pass with the allowlist entries (processing.test.ts covers builds 0.4.0 / 0.4.3 /
0.12.0 / 1.0.0, both solver strings, the resolution range, dev / mac / 0.3.0 builds refused, a
miss window). **Not run inside Geometry Dash**: everything in the in-game log checklist above is
unverified until the user plays with `solver-debug` verbose and sends the Geode log.

2026-09-29, v0.3.0 (in-game menu): host tests 11 suites, 3832 checks, 0 failures (`menu_tests`,
414 checks, joined the suites); the review pass of the same day (hostile-JSON hardening:
saturating `json::Value::asInt`, clamped int members, `formatSigma` / `scaled` safe for absurd
magnitudes, no trailing space in `bandName`, pushButton counter on player 1 only, Ranks-row name
clipped left of its tag) rebuilt from a `ninja -t clean` state with zero GPRL warnings and
reinstalled. Earlier: a clean `build.ps1` build (GPRL objects, PCH and link outputs
deleted first, log `build/build-log-v030-clean.txt`) compiled with zero compiler warnings, linked
and installed `gmo12.gprl.geode` (the 12 badge sprites packed with -hd / -uhd variants). **Not run inside Geometry Dash**: the tab row,
the badge sprites (resource loading through `expandSpriteName` at the three texture qualities),
the hexagon fallback, the scroll lists, the main-menu button, the Session counters and the
public /api fetches against the live Worker are unverified in-game; the Profile / Board tabs were
designed against the live API's LOCKED / empty answers of 2026-09-29 (captured in
`tests/menu_tests.cpp`).

2026-09-29, v0.2.1 (Website code): host tests 10 suites, 3418 checks, 0 failures (`identity_tests`
grew from 59 to 164 with the code format / countdown rules); a clean rebuild (GPRL + Argon objects,
PCH and link outputs deleted first) compiled, linked and installed `gmo12.gprl.geode`. **Not run
inside Geometry Dash**: the new button, the panel layout (code fitting left of Copy, the panel
over the counter lines), the clipboard write and the countdown against the live Worker are
unverified in-game.

2026-09-29, v0.2.0 (GD-account Connect): host tests 10 suites, 3313 checks, 0 failures (new `identity_tests`: 59); a clean
rebuild (all GPRL + Argon objects deleted first) compiled with zero compiler warnings, linked and
installed `gmo12.gprl.geode`. **Not run inside Geometry Dash and not run against a server** (the
API's `/v1/client/connect` and `/v1/client/web-login` were being built in parallel): the Argon
round trip, the popup layout with the new buttons, the retry dialogs and opening the browser are
unverified.

Earlier (v0.1.x):

Built and installed (`geode build --install`), host tests green. 2026-09-29 (second pass: core/classify,
environment re-emit, session_invalid reopen, game-exit spooling, Clipper stub): a clean rebuild
(every GPRL object file deleted first) compiled with zero warnings, linked, and `geode build`
packaged + installed `gmo12.gprl.geode` into `D:\Steam Games\steamapps\common\Geometry Dash\geode\mods\`;
`build.ps1` now exits with the build's exit code (it used to exit 0 on a failed compile).
`tests/run_tests.ps1` = 8 suites, 3215 checks (66 + 59 + 1245 + 40 + 156 + 1026 + 448 + 175, with the goldens),
0 failures; `classify_tests` and `determinism_tests` also reproduce every case of
`tests/fixtures/classification/*.json` and the `analytic` blocks of
`tests/fixtures/solver/determinism-local-window.json`. Final verification pass the same day: clean
rebuild again (all GPRL objects, PCH and link outputs deleted), zero compiler / linker warnings,
installed `.geode` byte-identical to `build/gmo12.gprl.geode` (log `build/build-log-verify.txt`).
`determinism_tests` now builds each solver case's `golden` block (the exact output trial by trial,
in the shape `tests/fixture-checks/solver.mjs` checks), compares it when the fixture carries one
and writes it with `--write <path>` / `run_tests.ps1 -Write`. The repo fixture carries all 7
goldens since 2026-09-29 (`run_tests.ps1 -Write`, then `npx prettier --write`): the suite
reproduces 7 of 7 exactly and `run-fixture-tests.mjs --strict` checks them (the solver fixture's
assertions become 1213); a tampered copy failed both checkers.
**Not yet run inside Geometry Dash**: the
hook set, HUD placement, popup layout, the environment poll, the `GameEvent Exiting` path and the
worker's network path (incl. the session_invalid reopen) need a play test / a live API (with the
placeholder API the mod only writes `local` spool files, which is the safe default).
