# GPRL

**Geometry Precision Ranking List** - measures your mechanical timing precision in sigma/s and
ranks players on it (not on which levels they beat).

This mod is the recorder. While you play it keeps reproducible evidence of every attempt:

- inputs (press / release, player 1 / 2, sub-tick timing with Click Between Frames),
- deaths (including would-be deaths under noclip), progress, gamemode changes,
- periodic player-state snapshots,
- the environment: loaded mods, mod-menu state (Eclipse noclip / bot / TPS bypass), FPS, CBF.

It sends that evidence to the GPRL server in signed batches, or keeps it on disk in **local-only
mode**. The server computes every rating; nothing in this mod can raise your sigma/s. Your rating
stays **LOCKED** until calibration is complete (enough varied evidence: several gamemodes, levels,
speeds, presses and releases, timings near your failure boundary).

## Using it

1. Log in to your Geometry Dash account in the game (Settings > Account). You do **not** need a
   website account.
2. Pause any level, press the `GPRL` button and press **Connect**. The mod proves you own that GD
   account with [Argon](https://github.com/GlobedGD/argon) (your password never leaves the game);
   this takes a few seconds. The popup then says `Connected as <your GD name>`.
3. **Open my profile** in the same popup opens your GPRL profile on https://gprl.pages.dev, already
   signed in (the link works once, for 5 minutes). **Website code** shows the same one-time code
   (`XXXXX-XXXXX-XXXXX-XXXXX`) with a Copy button instead, for the website's sign-in box.
4. The bottom-left line shows your calibration progress: `GPRL: calibration 12% | samples 84/700 |
   sigma/s LOCKED`.

If you log in to another GD account, press Connect again for that account; evidence is only sent
for the account that is logged in. **Disconnect** forgets this PC's connection.

Installed mod menus are fine: a normal menu is allowed; noclip keeps mechanical data but nerve and
completions are adjusted; macro playback gives 0 sigma/s; speedhack / TPS changes invalidate the
session. Unknown gameplay mods pause rated sigma/s until verified.

## Privacy

Only the Geometry Dash environment is inspected (GD folder, Geode folder, modules loaded in the GD
process by file name and size, GPRL's own files). Nothing else on your PC is read, and nothing is
uploaded before you press Connect. Local-only mode never talks to any server.

**Clipping is off by default.** When you turn it on, the game picture and the game's own sound are
recorded while a level is open, into a rolling buffer on your disk. A clip is never uploaded on
its own: only when you press **Send to GPRL moderators** or **Save + send** in the popup, and what
you send is private (only the moderators assigned to your run can watch it). Your microphone is
recorded only if you also turn on its own setting.

## Status

Phase 3 (v0.4.0): recorder, session flow, HUD, popup, calibration display, and the local
timing-window solver. Every jump press and release of player 1 is measured by hidden clone players
stepped inside the game's own physics loop (the frame-perfect-counter engine, same author): the
input is moved earlier and later by whole 240 TPS ticks, and with **Click Between Frames** active
the boundaries are refined below one tick (the between-step split re-implements what CBF does;
credit to syzzi's Click Between Frames). Windows are only reported when the unshifted clone
reproduced your real run exactly; the rest is dropped and logged. Measuring pauses under Eclipse's
Physics Bypass at a TPS other than 240, speedhack and bot playback, and on platformer levels.

v0.5.0 (solver tuning): the clones now see orbs, pads and portals the way the real player does
(the game keeps a separate "used" flag per player and picked the wrong one for hidden clones, so
every release after an orb was dropped), follow the level's speed changes, and run on an adaptive
budget (24 open measurements, a warm clone pool, no more first-spawn hitches). The 5 s log summary
names every reason an input got no window.

v0.5.1 (HUD, evidence, capture): the middle-right readout lists your last 8 presses and releases
(green measured, red miss, grey dropped with the reason); the Session and Profile tabs show the
level's **Level Analysis Coverage: 83% / Missing: - 61.2-64.7%** block from the server; only rated
demons count - the HUD and Session tab say **Not a rated demon: not counted** when the server says
so; bot playback keeps measuring windows as level-only evidence (never your sigma/s); every attempt
records its unpaused play time (practice / StartPos split) and the GD save's attempt count as
untrusted context.

v0.6.0 (clipping): an optional rolling recording of your gameplay (mod settings > **Clipping**,
needs ffmpeg.exe; the In-Game Clipper's ffmpeg is found automatically). When you complete a level
that counts - or the GPRL server asks for evidence of a run - the attempt is kept and a popup asks
what to do with it: **Save to computer / Send to GPRL moderators / Save + send / Do nothing**.
`Clip last attempt` in the GPRL menu > Account keeps any attempt by hand. About 36 MB of disk per
minute kept at the default 720p, capped at 1 GB. The capture is adapted from the In-Game Clipper
(same author).

v0.6.1 (sequences): for two or three inputs close together the mod now also measures how their
timings depend on each other (both early, both late, one early and one late), not only each input
on its own. The result is the share of the box of single-input windows that really works: 100 %
means the inputs are independent, less means the sequence is tighter than its single windows
suggest. It describes the level, never you, and it only runs with solver time left over (setting
**Measure sequences**). Also: a speed portal no longer reads as a solver mismatch.

v0.7.0 (timing solver v2): every timing edge now says what ended it and what caused the death
behind it: the input itself, a LATER input that acted first, or the frozen death pause. A wave
click moved on its own runs into the next click's path, so its old window measured the next
input, not this one; the new **sequence-adjusted window** moves the next 1-3 inputs of the same
sequence with it (a press keeps its hold, a wave click keeps its zigzag) and reports an input it
cannot decide as "sequence dependent" instead of a guessed number. Every measured input gets one
result with a status (ok, low confidence, unresolved, replay failed, sequence dependent, no
effect). It only uses solver time left over (setting **Sequence-adjusted windows**); the v0.6.1
joint windows are now an off-by-default setting. Debug settings **trace windows up to** and
**draw the last traced timing** show where a narrow window's limits come from: the real run, the
earliest / latest valid and the first invalid trajectories, the hitbox and the object that killed
them. Not yet tested inside the game.

v0.7.1 (a second, independent review of the timing solver): one death is now one miss - the last
input that could have saved the run - instead of up to five; the other inputs of that run are
marked "miss downstream" and send no window. A sequence-adjusted window is never narrower than
the input's own window, and it says whether its extra room was proven by the run re-joining your
real run or only by surviving (ship and ufo can only survive). Inputs right before a speed portal
are marked "low confidence" (the check moves your click in time, the speed change stays at its
place). Shifts of connected inputs are checked for as long after the LAST moved input as for a
single one. A frame-perfect window is no longer refused by the server over a rounding error in
its last digit. Not yet tested inside the game.

## Credits

Created by gmo.
