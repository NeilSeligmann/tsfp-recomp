#!/usr/bin/env fish
# T1736 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_button_dumps.fish tmp/run_button_dumps.fish
# It is the T1629..T1767 wrapper plus --checklist (T1736): fish tmp/run_button_dumps.fish --checklist [FILE]   (DRY_RUN=1 prints the plan, starts nothing)
#   --checklist [FILE] (env CHECKLIST): ALL-BUTTONS-AND-AXES mode. After the route, instead of free play, tools.button_checklist prints the next
#      instruction of FILE (default tools/data/button_checklist.txt: 14 digital buttons, then left/right stick in 8 directions at half and full
#      deflection, then both triggers 25..100 percent) and waits for Enter. Digital steps use the host button dump (--dump-on-button), analog
#      steps take a rest and a held SIGUSR1 dump of the pad arrays 0x6B93C0:0x1F8 (the host button dump counts digital edges only). The report
#      is DIR/checklist_report.md (python -m tools.button_checklist_report DIR --write). Poke and idle controls default OFF in this mode.
# T1629: DUMP ON EVERY BUTTON PRESS. You play normally, the host writes a guest-memory dump just BEFORE and just AFTER each digital
# button press or release of pad port 0 (16 buttons, never the sticks) and a JSONL manifest. Afterwards tools.button_dump_report
# diffs the player record per press (what did switching to the bat, picking up the pistol, firing, reloading change?).
# T1637 POKE (default ON): the Map Maker level has empty weapon spawners, so the six spawner slots of the editor weapon set are poked
# with the guarded --forced-state poke at mark 3 of the route (editor up, BEFORE the preview, exactly like tmp/run_forced_weapons.fish),
# default: spawner 1..6 = pistol 0x02, pistol x2 0x04, bat 0x36, Kruger 0x05, Kruger x2 0x07, LX-18 0x08. Evidence class: the EDITOR SLOT
# STATE is FABRICATED-STATE (a FORCED_STATE_NOTE.txt is written), everything after the preview starts is unmodified game code, so
# pickup, switch, fire and reload transitions are still xemu-level MEASURED observation, of a map with a poked weapon set. --no-poke:
# PASSIVE as before (no --forced-state, no poke, nothing written into the game). The route that carries you to the preview is FABRICATED
# input (your own recorded keys), and the button dumping only arms once the route has handed the pad back to you (AUTO mode).
# Design and file formats: docs/t-button-dumps.md. The host flags are listed by `<host> --help` (marker T1629).
#
# Usage:   fish tmp/run_button_dumps.fish [--route FILE | --manual] [--weapons LIST | --no-poke] [--minutes N] [--after-frames LIST] [--idle-every N] [--max-dumps N]
#   --weapons LIST (env WEAPONS): 1..6 entries (hex 0x.. or decimal, 0..0x45, names in docs/t-weapon-entry-table.md), spawner N = slot N =
#      offset 0x1190C+4*(N-1). Slots NOT listed are left alone (not cleared). Default 0x02,0x04,0x36,0x05,0x07,0x08.
#   --no-poke: no --forced-state, no poke, the passive session of T1629.
#   The poke is verified from DIR/guestpoke.log and the poke dump (python -m tools.button_dump_poke verify): a '[poke] ...' table appears
#      while you play and again at the end. A REFUSED or READBACK-MISMATCH poke is printed loudly and recorded in README_SESSION.txt, the
#      session goes on.
#   AUTO (default): replays the route you recorded ONCE with tmp/record_mapmaker_route.fish (start -> players -> profile -> Map Maker ->
#      load map -> editor -> PREVIEW, 4 marks), waits at each slow screen, then hands your keyboard/gamepad back IN THE PREVIEW. The route
#      is checked against this run's game flags BEFORE the host is built (python -m tools.play --check-route). --route FILE picks a route
#      (default: the newest tmp/owner-profiles/routes/*.txt, its waits are FILE minus .txt plus .waits). The HDD is a fresh copy of
#      tmp/hdds/route-hdd-pristine (the game writes to its HDD).
#   --manual: no route, you drive to the preview yourself, dumping is armed from the start (the idle controls are OFF by default then,
#      they would burn their budget in the menus: set IDLE_EVERY or --idle-every to turn them on). The HDD is tmp/hdds/editor-hdd.
#      With the poke ON the script prints the exact tmp/poke_now.fish line: be in the Map Maker EDITOR on the weapon set page, run it in a
#      SECOND terminal BEFORE entering the preview. Button presses of the menus are recorded too (they use up --max-dumps, --dump-button-
#      start-poll cannot be used by hand).
#   --minutes N (env MINUTES, default 40): a watcher process stops the game with SIGTERM after N minutes.
#   --after-frames LIST (env AFTER_FRAMES, default 2,30): polls (frames) after the press at which the AFTER dumps are taken, 1..4 values.
#   --idle-every N (env IDLE_EVERY, default 600 AUTO / 0 manual): an idle control dump set every N quiet polls, to learn the noise floor.
#   --max-dumps N (env MAX_DUMPS, default 2000): cap on dump FILES. Env MAX_IDLE (default 40) caps the idle control events.
#   Env RANGE_SET: inventory (default, 13 ranges copied INLINE from tmp/run_inventory_session.fish, no runtime dependency on it) or weaponslot
#   (the 14 of tmp/run_weaponslot_session.fish, which lacks the ext block: add EXTRA_RANGES='0x7356D8:4,*0x7356D8+0:0xEA0'). The ranges must contain both
#   *0x7356D8+0:0xEA0 (inventory ext block) and *0x7B0C48+0:0x1584 (player record) or the script refuses to start.
#   Env EXTRA_RANGES: extra --dump-guest-range specs appended to the set (at most 16 ranges in total). With the poke ON the script adds
#   0x7844A8:4 and *0x7844A8+0x11904:0x44 (the editor weapon list the poke is verified from) unless the set already has them.
#   fish syntax for env vars: env AFTER_FRAMES=2,10 IDLE_EVERY=0 fish tmp/run_button_dumps.fish     (NOT bash's VAR=x cmd)
# What you do: see the printed instructions. THE RULE: press ONE button at a time, release it, wait about 1 second. Presses less than 3
# frames apart are merged into one event. Stop with q + Enter in this terminal, Ctrl+C, the time limit, or by closing the game window.
# Each recorded event is up to 3 files (before, after, after) in DIR/buttons/, manifest DIR/buttons.jsonl. A file is up to about 300 KB
# (less with the default inventory range set). The disk check refuses to start below 3 GB free.
# Before playing, switch OFF 'auto switch weapon on pickup' if the game has it (printed block, README_SESSION.txt), and answer the note prompt at the end
# (OWNER_NOTES.txt). Output: tmp/owner-profiles/button-dumps-<stamp>/ (README_SESSION.txt, OWNER_NOTES.txt, buttons/, buttons.jsonl, button_report.md, play.log, play.stdout).
# T1633: a host with the marker T1633 in --help runs the '# wait:' lines of the route instead of the time based .waits specs of the same marks
# (and writes DIR/route-events.log), as tmp/run_forced_weapons.fish does. An older host falls back to the .waits specs.
# Launch: plain tools.play (like tmp/record_mapmaker_route.fish and tmp/run_weaponslot_session.fish), NOT tools.action_profile: this run
# needs no census observers and no phase protocol, and tools.play composes everything the route identity needs (the game flags come from
# python -m tools.play.route_check flags, the same list the recording used; the host ignores the dump flags in the identity). The game and
# the watcher run in their own session (setsid), so Ctrl+C reaches only this script, which then stops the host cleanly by PID.
# Never pkill/killall. Stop = kill -TERM <host pid>, wait up to 30 s, kill -KILL by PID as a last resort. The host stops the button dump on ALL its
# exit paths (SIGTERM, a normal exit and the forced _Exit after the guest-thread safepoint wait), so the manifest gets its pending events and the end
# line after a SIGTERM. A second Ctrl+C while the stop is running is ignored (the stop then still reaches its SIGKILL fallback and the report).
# T1767: WHAT THE OWNER HAS TO DO is printed by `python -m tools.owner_scenario show $OWNER_SCENARIO ...` (default scenario run_button_dumps, data
# file tools/data/owner_scenarios/<name>.json) with the options of THIS run (mode auto|manual, poke on|off, weapons, minutes, idle, max_dumps, ...).
# A wrapper selects its own text through the environment: env OWNER_SCENARIO=run_t1642_session4 fish tmp/run_button_dumps.fish ... . The scenario is
# also appended to README_SESSION.txt. No plan text is hard coded in this script any more.
# fish 4.0.2 was installed in the dev container and this file passes `fish -n`. The pieces that do not need the game (option parsing,
# the watcher, the stop and report path) were exercised against a stub host, see docs/t-button-dumps.md Validation. A real game run has
# NOT been done (the host cannot run in the container, GLIBC 2.43).

set -l self (realpath (status filename))
set -l fishbin (status fish-path)
test -n "$fishbin"; or set fishbin fish
cd (dirname $self)/..; or exit 1

# --- watcher mode (re-executed by this script in its own session, never typed by hand) -------------------------------------------
# args: --watch DIR HOSTPID PARENTPID LIMIT_SECONDS. Every 3 s: live event count, route state, time limit, and a safety net: when this
# script itself has died (Ctrl+C that fish did not handle, a closed terminal) it stops the host with SIGTERM and writes the report.
if test "$argv[1]" = --watch
    set -l dir $argv[2]
    set -l hostpid $argv[3]
    set -l parent $argv[4]
    set -l limit $argv[5]
    set -l pokelabel $argv[6]
    set -l pokeweapons $argv[7]
    set -l pokeseen 0
    set -l started (date +%s)
    set -l termat 0
    set -l orphan 0
    set -l lastcount -1
    set -l lastbeat $started
    set -l routeseen 0
    while kill -0 $hostpid 2>/dev/null
        sleep 3
        set -l now (date +%s)
        set -l count 0
        set -l latest ""
        if test -f $dir/buttons.jsonl
            set count (grep -c -F '"type":"event"' $dir/buttons.jsonl 2>/dev/null)
            test -n "$count"; or set count 0
            set latest (grep -F '"type":"event"' $dir/buttons.jsonl 2>/dev/null | tail -n 1 | string replace -r '.*"label":"([^"]*)".*' '$1')
        end
        if test $routeseen -eq 0; and test -f $dir/play.log
            if grep -q -F 'route FAILED' $dir/play.log 2>/dev/null
                set routeseen 1
                echo
                echo "[buttons] ROUTE FAILED (a wait timed out), see 'route FAILED' in $dir/play.log. Press q + Enter, use --manual or fix the .waits file."
            else if grep -q -F 'the recorded inputs are exhausted' $dir/play.log 2>/dev/null
                set routeseen 1
                echo
                echo "[buttons] route finished: the pad is YOURS, dumping is armed. One button at a time, wait 1 second between presses."
            end
        end
        if test "$pokelabel" != -; and test $pokeseen -eq 0; and test -f $dir/guestpoke.log
            set -l verifyout (timeout 30 python -m tools.button_dump_poke verify $dir --label $pokelabel --weapons $pokeweapons 2>&1)
            set -l verifystatus $status
            if test $verifystatus -ne 3
                set pokeseen 1
                echo
                for line in $verifyout
                    echo "[poke] $line"
                end
                if test $verifystatus -ne 0
                    echo "[poke] !!!! THE POKE DID NOT WORK (verify status $verifystatus): the spawners are NOT as planned. The session goes on, but treat the results as an UNPOKED map. !!!!"
                end
                begin
                    echo
                    echo "POKE VERIFY (during the session, status $verifystatus):"
                    printf '%s\n' $verifyout
                    if test $verifystatus -ne 0
                        echo "!!!! POKE NOT VERIFIED (status $verifystatus: 1 REFUSED, 2 READBACK-MISMATCH): treat this session as an UNPOKED map. !!!!"
                    end
                end >> $dir/README_SESSION.txt
            end
        end
        if test $count != $lastcount; or test (math $now - $lastbeat) -ge 30
            set lastcount $count
            set lastbeat $now
            echo
            set -l elapsed (math --scale 0 "($now - $started) / 60")
            echo "[buttons] events: $count   latest: $latest   elapsed: $elapsed min"
        end
        if test $termat -eq 0
            if test (math $now - $started) -ge $limit
                echo
                echo "[buttons] time limit reached, stopping the game (SIGTERM to pid $hostpid)"
                kill -TERM $hostpid 2>/dev/null
                set termat $now
            else if not kill -0 $parent 2>/dev/null
                echo
                echo "[buttons] the launching script is gone, stopping the game (SIGTERM to pid $hostpid) and writing the report"
                kill -TERM $hostpid 2>/dev/null
                set termat $now
                set orphan 1
            end
        else if test (math $now - $termat) -ge 30
            echo "[buttons] the game ignored SIGTERM for 30 s, last resort SIGKILL pid $hostpid"
            kill -KILL $hostpid 2>/dev/null
            set termat $now
        end
    end
    if test $orphan -eq 1
        timeout 600 python -m tools.button_dump_report $dir --write > $dir/report.log 2>&1
        echo "[buttons] report attempt finished (status $status), see $dir/report.log and $dir/button_report.md"
    else
        echo
        echo "[buttons] the game has ended. Press Enter in this terminal to finish and write the report."
    end
    exit 0
end

# --- the owner instruction block (printed before the start prompt and written to README_SESSION.txt) ----------------------------------
function owner_block
    echo "AUTO SWITCH WEAPON ON PICKUP (T1767): in this game the setting is WEAPON CHANGE on the CONTROLS page of the settings (reachable from the pause menu)."
    echo "  Values: Always, Never, Best, If New, If New and Best (default). Static analysis: the pickup code may select a new weapon by itself, INFERRED, docs/t-button-dump-analysis.md section 6 and 9."
    echo "  The standard plan sets it to NEVER before the first pickup, session 4 (run_t1642_session4) must leave it at the default. The printed scenario says which."
    echo "  This script asks for a one line note at the end (menu name, place and the value you set, or 'not touched' / 'none found') and appends it to OWNER_NOTES.txt."
    echo "  AUTO mode caveat: the HDD of an AUTO run is a fresh copy of tmp/hdds/route-hdd-pristine, so a setting changed in an earlier run is NOT in it. Change it INSIDE the preview, or change the profile once on tmp/hdds/editor-hdd and run with --manual."
end


# --- options ------------------------------------------------------------------------------------------------------------------
set -l manual 0
set -l route ""
set -l minutes 40
set -l after_frames 2,30
set -l idle_every ""
set -l max_dumps 2000
set -l max_idle 40
set -l poke 1
set -l weapons ""
set -l pokelabel buttons_poke
test -n "$WEAPONS"; and set weapons $WEAPONS
test -n "$MINUTES"; and set minutes $MINUTES
test -n "$AFTER_FRAMES"; and set after_frames $AFTER_FRAMES
test -n "$IDLE_EVERY"; and set idle_every $IDLE_EVERY
test -n "$MAX_DUMPS"; and set max_dumps $MAX_DUMPS
test -n "$MAX_IDLE"; and set max_idle $MAX_IDLE

set -l checklist_on 0
set -l checklist_file tools/data/button_checklist.txt
if test -n "$CHECKLIST"
    set checklist_on 1
    set checklist_file $CHECKLIST
end
set -l optindex 1
while test $optindex -le (count $argv)
    switch $argv[$optindex]
        case --manual
            set manual 1
        case --no-poke
            set poke 0
        case --checklist
            # T1736: the FILE is optional (the next word is taken as the file unless it starts with --)
            set checklist_on 1
            set -l nextindex (math $optindex + 1)
            if test $nextindex -le (count $argv); and not string match -q -- '--*' $argv[$nextindex]
                set checklist_file $argv[$nextindex]
                set optindex $nextindex
            end
        case --route --minutes --after-frames --idle-every --max-dumps --weapons
            set -l option $argv[$optindex]
            set optindex (math $optindex + 1)
            if test $optindex -gt (count $argv)
                echo "$option needs a value"; exit 2
            end
            switch $option
                case --route
                    set route $argv[$optindex]
                case --minutes
                    set minutes $argv[$optindex]
                case --after-frames
                    set after_frames $argv[$optindex]
                case --idle-every
                    set idle_every $argv[$optindex]
                case --max-dumps
                    set max_dumps $argv[$optindex]
                case --weapons
                    set weapons $argv[$optindex]
            end
        case -h --help
            echo "usage: fish tmp/run_button_dumps.fish [--route FILE | --manual] [--weapons LIST | --no-poke] [--minutes N] [--after-frames LIST] [--idle-every N] [--max-dumps N] [--checklist [FILE]]"
            echo "  --checklist [FILE] (env CHECKLIST, T1736): every button and axis in a fixed order, default tools/data/button_checklist.txt; poke and idle controls default off"
            echo "env: WEAPONS MINUTES AFTER_FRAMES IDLE_EVERY MAX_DUMPS MAX_IDLE RANGE_SET EXTRA_RANGES    (fish: env AFTER_FRAMES=2,10 fish tmp/run_button_dumps.fish)"
            echo "see the header of tmp/run_button_dumps.fish and docs/t-button-dumps.md"
            exit 0
        case '*'
            echo "unknown option: $argv[$optindex] (--route FILE, --manual, --weapons LIST, --no-poke, --minutes N, --after-frames LIST, --idle-every N, --max-dumps N, --checklist [FILE])"; exit 2
    end
    set optindex (math $optindex + 1)
end
if test $manual -eq 1; and test -n "$route"
    echo "--manual and --route exclude each other"; exit 2
end
# T1736: checklist mode. The poke is off unless --weapons was given (the pad arrays do not depend on the weapons), the idle controls are off.
if test $checklist_on -eq 1
    test -f $checklist_file; or begin; echo "checklist file not found: $checklist_file"; exit 2; end
    timeout 60 python -m tools.button_checklist list $checklist_file > /dev/null; or begin; echo "the checklist $checklist_file is invalid (python -m tools.button_checklist list $checklist_file)"; exit 2; end
    if test -z "$weapons"
        set poke 0
    end
    test -z "$idle_every"; and set idle_every 0
end
if test $poke -eq 0; and test -n "$weapons"
    echo "--weapons and --no-poke exclude each other"; exit 2
end
# T1637: the weapon plan (validated here, before any build; the table is printed at the start). Spawner N = slot N, unlisted slots stay as they are.
set -l planlines
set -l pokeweapons -
if test $poke -eq 1
    if test -z "$weapons"
        set weapons (python -m tools.button_dump_poke default-weapons); or begin; echo "cannot read the default weapons (python -m tools.button_dump_poke default-weapons failed)"; exit 1; end
    end
    set planlines (python -m tools.button_dump_poke plan --weapons $weapons --label $pokelabel 2>&1)
    or begin
        printf '%s\n' $planlines
        echo "bad --weapons (1..6 entries, hex 0x.. or decimal, 0..0x45): $weapons"; exit 2
    end
    set pokeweapons $weapons
end
if test -z "$idle_every"
    if test $manual -eq 1
        set idle_every 0
    else
        set idle_every 600
    end
end
if not string match -qr '^[0-9]+$' -- $minutes; or test $minutes -lt 1; or test $minutes -gt 600
    echo "--minutes must be 1..600 (got $minutes)"; exit 2
end
if not string match -qr '^[0-9]+(,[0-9]+){0,3}$' -- $after_frames
    echo "--after-frames must be 1 to 4 comma separated integers, strictly increasing, 1..3600 (got $after_frames)"; exit 2
end
for number in $idle_every $max_dumps $max_idle
    if not string match -qr '^[0-9]+$' -- $number
        echo "--idle-every, --max-dumps and MAX_IDLE must be non-negative integers (got $number)"; exit 2
    end
end
set -l previous_frame 0
for frame in (string split , -- $after_frames)
    if test $frame -le $previous_frame; or test $frame -gt 3600
        echo "--after-frames must be strictly increasing integers in 1..3600 (got $after_frames)"; exit 2
    end
    set previous_frame $frame
end
# the host refuses --dump-button-max-dumps below 1 + the number of after offsets (one event needs that many files)
set -l needed (math 1 + (count (string split , -- $after_frames)))
if test $max_dumps -lt $needed
    echo "--max-dumps $max_dumps is below $needed (1 before + "(count (string split , -- $after_frames))" after dumps per event): the host would refuse it"; exit 2
end
if test $max_dumps -lt 1; or test $max_dumps -gt 100000
    echo "--max-dumps must be 1..100000 (got $max_dumps)"; exit 2
end

# --- disk space: refuse below 3 GiB (each recorded event is up to 3 files of about 300 KB). KiB compare: df -BG rounds UP, so it is not used ---
set -l freekib (df --output=avail -k . | tail -n 1 | string trim)
if not string match -qr '^[0-9]+$' -- $freekib
    echo "cannot read the free disk space (df said: $freekib)"; exit 2
end
if test $freekib -lt 3145728
    echo "only "(math --scale 2 "$freekib / 1048576")" GiB free on this disk, at least 3 GiB are needed (up to $max_dumps dump files of about 300 KB each). Free some space first."; exit 2
end

# --- ranges (16 allowed). RANGE_SET=inventory (default) is the set of tmp/run_inventory_session.fish (T1628, 13 ranges): the player record
# *0x7B0C48+0:0x1584 (the one tools.button_dump_report diffs by default), the inventory extension block *0x7356D8+0:0xEA0 (owned bytes,
# ammo, current/previous weapon), the profile carry-over record, the live weapon entry table and the game type/mode words. RANGE_SET=weaponslot
# is the 14 ranges of tmp/run_weaponslot_session.fish (Map Maker editor/chunk words plus the player record). EXTRA_RANGES appends more.
set -l rangeset inventory
test -n "$RANGE_SET"; and set rangeset $RANGE_SET
set -l ranges ''
switch $rangeset
    case inventory
        set ranges '0x7B0C48:4,*0x7B0C48+0:0x1584,0x7356D8:4,*0x7356D8+0:0xEA0,0x790950:4,0x7B0C7C:4,0x75F5D0:4,*0x75F5D0+0:0x238,0x4FA128:0x1068,0x4FC888:0x118,0x75A024:4,0x7DE450:0x10,0x79094C:4'
    case weaponslot
        set ranges '0x52D3F4:0x30,0x7844A8:4,*0x7844A8+0:0x800,0x7B0C48:4,*0x7B0C48+0:0x1584,0x784010:0x20,*0x7844A8+0x11860:0xA4,*0x7844A8+0x11904:0x44,*0x784014+0:0x7FE0,0x7DE450:0x10,0x79094C:4,0x761404:4,0x6F1E28:0x30,0x75FC78:0x44'
    case '*'
        echo "RANGE_SET must be inventory or weaponslot (got $rangeset)"; exit 2
end
if test -n "$EXTRA_RANGES"
    set ranges "$ranges,$EXTRA_RANGES"
end
# T1637: the poke is verified from the editor weapon list in the poke dump: add its ranges when the set lacks them
if test $poke -eq 1
    if not string match -qri '(^|,)0x7844A8:4(,|$)' -- $ranges
        set ranges "$ranges,0x7844A8:4"
    end
    if not string match -qri '\*0x7844A8\+0x11904:0x44' -- $ranges
        set ranges "$ranges,*0x7844A8+0x11904:0x44"
    end
end
# both targets must be in the resulting range string: the inventory ext block (owned bytes, ammo, current weapon) and the player record
if not string match -qri '\*0x7356D8\+0:0xEA0' -- $ranges
    echo "the ranges lack the inventory ext block *0x7356D8+0:0xEA0 (RANGE_SET $rangeset). For RANGE_SET=weaponslot add it: EXTRA_RANGES='0x7356D8:4,*0x7356D8+0:0xEA0'"; exit 2
end
if not string match -qri '\*0x7B0C48\+0:0x1584' -- $ranges
    echo "the ranges lack the player record *0x7B0C48+0:0x1584 (RANGE_SET $rangeset), the report diffs it by default"; exit 2
end
# T1736: the pad arrays (remap, physical and effective records, docs/t-guest-input-chain.md) are in every host and SIGUSR1 dump of a checklist run
if test $checklist_on -eq 1
    set -l padrange (python -m tools.button_checklist range); or begin; echo "cannot read the pad range (python -m tools.button_checklist range failed)"; exit 1; end
    if not string match -qri "(^|,)$padrange(,|\$)" -- $ranges
        set ranges "$ranges,$padrange"
    end
end
set -l nranges (count (string split , -- $ranges))
if test $nranges -gt 16
    echo "$nranges ranges, at most 16 are allowed (RANGE_SET $rangeset, plus the poke ranges 0x7844A8:4 and *0x7844A8+0x11904:0x44 unless the set has them, plus the checklist pad range, plus EXTRA_RANGES)"; exit 2
end

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l session button-dumps-$stamp
set -l outdir tmp/owner-profiles/$session

# --- DRY_RUN=1 (T1736): print the plan and check the helper tools, start and build nothing ---------------------------------------
if test "$DRY_RUN" = 1
    echo "DRY_RUN: session $session, mode "(test $manual -eq 1; and echo MANUAL; or echo AUTO)", poke "(test $poke -eq 1; and echo ON; or echo OFF)", checklist "(test $checklist_on -eq 1; and echo $checklist_file; or echo off)
    echo "ranges ($nranges): $ranges"
    echo "would: check the route, build the host (python -m tools.private_host build), start it with --dump-on-button --dump-after-frames $after_frames --dump-button-idle-every $idle_every --dump-button-max-dumps $max_dumps,"
    if test $checklist_on -eq 1
        echo "then run: python -m tools.button_checklist run $outdir --file $checklist_file    and at the end: python -m tools.button_checklist_report $outdir --write"
        timeout 60 python -m tools.button_checklist list $checklist_file | tail -n 1; or exit 1
        timeout 60 python -m tools.button_checklist_report --help > /dev/null; or exit 1
        timeout 60 python -m tools.owner_scenario show run_button_checklist --opt checklist=$checklist_file | head -n 12; or exit 1
    else
        echo "then the free play loop and python -m tools.button_dump_report $outdir --write"
    end
    exit 0
end

# --- AUTO: pick and check the route BEFORE the multi-minute host build -------------------------------------------------------------
set -l waitspecs
set -l recordwaitmarks
set -l workhdd ./tmp/hdds/editor-hdd
set -l pristine ./tmp/hdds/route-hdd-pristine
if test $manual -eq 0
    if test -z "$route"
        set route (find tmp/owner-profiles/routes -maxdepth 1 -name '*.txt' -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | string replace -r '^[0-9.]+ ' '')
    end
    if test -z "$route"; or not test -f "$route"
        echo "no recorded route found. Record it ONCE:  fish tmp/record_mapmaker_route.fish     (or run with --manual)"; exit 2
    end
    # T1637: the route must carry 4 marks inside a closed recording: mark 3 = editor up on the weapon set page (the poke point), mark 4 = IN the preview,
    # the record ends there and the pad is handed over. The helper checks the order of the marks and that mark 3 comes before the handover.
    if not python -m tools.button_dump_poke route-check $route
        echo "$route does not fit: record again with tmp/record_mapmaker_route.fish, or use --manual"; exit 2
    end
    # T1639: the poke fires at mark 3 = editor up and idle. A route that visits the weapon settings page (advisory check) and pokes on or
    # after it wedges the guest at mode 0x66 in the headless host. Prefer a route without the page visit (the owner's *.nopage.txt).
    if test $poke -eq 1
        set -l pagecheck (python -m tools.route_splice $route --check-page 2>&1)
        if string match -q 'LIKELY*' -- $pagecheck
            echo "NOTE (T1639): $pagecheck"
            echo "      The poke does not need the weapon settings page. Cut the excursion out of this route (python -m tools.route_splice $route --list, then --cut A:B),"
            echo "      or pick a route without it with --route (for example a *.nopage.txt). Running with the route as it is."
        end
    end
    # T1633: the route may carry its own event waits ('# wait: markK:...' lines written by tools.route_events)
    set recordwaitmarks (grep -o '^# wait: mark[0-9]*:' $route 2>/dev/null | string replace -r '^# wait: mark([0-9]+):$' '$1')
    set -l waitsfile (string replace -r '\.txt$' '.waits' -- $route)
    if test -f $waitsfile
        for line in (cat $waitsfile)
            if string match -qr '^mark[0-9]+:' -- $line
                set -a waitspecs $line
            end
        end
    else
        echo "no $waitsfile: the replay will not wait at the marks (poll timing only)"
    end
    test -d $pristine; or begin; echo "no $pristine (made by tmp/record_mapmaker_route.fish)"; exit 2; end
    set workhdd $outdir/hdd
end

# game affecting flags: ONE list shared with the route recording and the other replay scripts (tools.play.route_check.route_game_flags)
set -l gameflags (python -m tools.play.route_check flags); or begin; echo "cannot build the game flags (python -m tools.play.route_check flags failed)"; exit 1; end

# the host flags of this feature (T1629). The dump flags are ignored by the route identity, so a recorded route still replays with them.
set -l hostflags --dump-guest-range $ranges --dump-guest-dir $outdir
set -a hostflags --dump-on-button --dump-after-frames $after_frames --dump-button-idle-every $idle_every
set -a hostflags --dump-button-max-idle $max_idle --dump-button-max-dumps $max_dumps
if test $manual -eq 0
    set -a hostflags --dump-button-after-replay
end
if test $poke -eq 1
    set -a hostflags --forced-state
end

set -l playargs $gameflags --console --gpu-live-blit --present window --pad-source keyboard --pad-source gamepad
set -a playargs --disc "$disc" --hdd $workhdd
if test $manual -eq 0
    set -a playargs --replay-input $route --replay-handover
    # T1637: the poke at mark 3 (editor up), served by the host itself from DIR/guestpoke.LABEL, before the preview is entered. The route-wait specs
    # are added after the host check below (T1633).
    if test $poke -eq 1
        set -a playargs --poke-at-poll mark3:$pokelabel
    end
end
set -a playargs $hostflags --log-file $outdir/play.log

if test $manual -eq 0
    echo "== check the route against the flags of this run (nothing is launched) =="
    set -l checkout (timeout 120 python -m tools.play $playargs --check-route 2>&1)
    set -l checkstatus $status
    if test $checkstatus -ne 0
        printf '%s\n' $checkout
        echo
        set -l onlyline (printf '%s\n' $checkout | grep 'only in this run')
        if test -n "$onlyline"; and string match -q '*--dump-*' -- $onlyline
            echo "this checkout's route identity does not ignore the T1629 dump flags yet (tools/play/route_check.py, src/input/xinput_record.c): update the repo, the route itself may be fine"
        else if printf '%s\n' $checkout | grep -q 'different flag set'
            echo "route was recorded with a different flag set, record it again with tmp/record_mapmaker_route.fish"
        else
            echo "the route check failed (status $checkstatus, see above), nothing was launched"
        end
        exit 2
    end
    printf '%s\n' $checkout | tail -n 2
end

echo "== build the private host (re-lifts if needed, a few minutes) =="
timeout 7200 python -m tools.private_host build; or exit 1
set -l host (python -c "import json;print(json.load(open('tmp/private-host/current.json'))['host'])")
test -x "$host"; or begin; echo "host not found: $host"; exit 1; end
if not timeout 60 $host --help 2>&1 | grep -q -- T1629
    echo "host has no --dump-on-button (T1629), or --help did not answer in 60 s. Rebuild: python -m tools.private_host build"; exit 2
end
if not timeout 60 $host --help 2>&1 | grep -q -- T1607
    echo "host has no pointer-indirect --dump-guest-range (T1607), rebuild: python -m tools.private_host build"; exit 2
end
if test $poke -eq 1
    if not timeout 60 $host --help 2>&1 | grep -q -- T1613
        echo "host has no --forced-state (T1613), so it cannot poke. Rebuild: python -m tools.private_host build   (or run with --no-poke)"; exit 2
    end
    if not timeout 60 $host --help 2>&1 | grep -q -- 'poke trigger SIGUSR2'
        echo "host has no SIGUSR2 poke trigger (T1614). Rebuild: python -m tools.private_host build   (or run with --no-poke)"; exit 2
    end
end
if test $manual -eq 0
    if not grep -aq -- '--route-wait' $host
        echo "host has no route replay (T1616), rebuild: python -m tools.private_host build   (or use --manual)"; exit 2
    end
    if not timeout 60 $host --help 2>&1 | grep -q -- T1618
        echo "host predates T1618 (route flag identity), rebuild: python -m tools.private_host build"; exit 2
    end
    # T1633: event driven route. A host with the marker T1633 runs the '# wait:' lines of the route (file opens and quiet, memory sentinels,
    # calls) instead of the recorded delays: the time based .waits specs are only used for the marks without an event wait, or for ALL marks
    # (with a warning) on a host without the marker. --route-event-log writes what the host saw to DIR/route-events.log.
    set -l eventhost 0
    if timeout 60 $host --help 2>&1 | grep -q -- T1633
        set eventhost 1
    end
    for spec in $waitspecs
        set -l waitmark (string replace -r '^mark([0-9]+):.*$' '$1' -- $spec)
        if test $eventhost -eq 1; and contains -- $waitmark $recordwaitmarks
            echo "mark $waitmark: waits for the events of the route ('# wait:' line), not for the time based $spec"
            continue
        end
        set -a playargs --route-wait $spec
    end
    if test $eventhost -eq 1
        set -a playargs --route-event-log $outdir/route-events.log --route-log-mem 0x79094C --route-log-mem 0x52D400
        echo "event driven route (T1633): the host logs file I/O and sentinels to $outdir/route-events.log"
        if test (count $recordwaitmarks) -eq 0
            echo "NOTE: this route has no '# wait:' lines yet, so it waits by the recorded delays. Add them: python -m tools.route_events $route --apply   (or record again with tmp/record_mapmaker_route.fish)"
        end
    else if test (count $recordwaitmarks) -gt 0
        echo "WARNING: the route has event waits ('# wait:' lines) but this host predates T1633, so it falls back to the TIME BASED waits of the .waits file (slower and less safe). Rebuild: python -m tools.private_host build"
    end
end

mkdir -p $outdir
if test $manual -eq 0
    cp -a $pristine $workhdd; or exit 1
end
# T1637: the poke request the host serves BY ITSELF at mark 3 (the editor is up on the weapon set page), written BEFORE the launch like
# tmp/run_forced_weapons.fish does. In MANUAL mode the owner runs tmp/poke_now.fish instead (the line is printed below).
if test $poke -eq 1; and test $manual -eq 0
    python -m tools.button_dump_poke plan --weapons $weapons --label $pokelabel --out $outdir/guestpoke.$pokelabel > /dev/null; or begin; echo "cannot write the poke request $outdir/guestpoke.$pokelabel"; exit 1; end
end
if test $poke -eq 1
    begin
        echo "FORCED_STATE session $session (T1637)"
        echo "FABRICATED-STATE: the six Map Maker spawner slots of the editor weapon set were poked by a guest memory poke (T1613/T1614/T1616), label $pokelabel."
        echo "Only that editor slot state is fabricated. Everything after the preview starts is unmodified game code: the pickup, weapon switch, fire and reload"
        echo "transitions in the button dumps are xemu-level MEASURED observations, of a map whose spawners hold the poked weapons. Naming from this folder: INFERRED at most."
        echo "Spawner N spawns the weapon of slot N (offset 0x1190C + 4*(N-1) of the pointer at 0x7844A8). Slots not listed were not touched."
        printf '%s\n' $planlines
        echo "The host writes guestpoke.log and FORCED_STATE in this folder. Entry names: docs/t-weapon-entry-table.md."
    end > $outdir/FORCED_STATE_NOTE.txt
end
begin
    echo "T1629 button dump session $session"
    echo "What: a guest-memory dump just BEFORE and just AFTER each digital button press/release of pad port 0 (16 buttons, not the sticks)."
    if test $poke -eq 1
        echo "Evidence class: the EDITOR SLOT STATE is FABRICATED-STATE (spawner pokes, see FORCED_STATE_NOTE.txt), everything after the preview starts is unmodified game code."
        echo "Button press transitions are xemu-level MEASURED observations of a map with a poked weapon set."
        echo "Spawner pokes (label $pokelabel, spawner N = slot N, unlisted slots untouched):"
        printf '  %s\n' $planlines
    else
        echo "Evidence class: xemu-level MEASURED passive observation of the unmodified game. No pokes, no --forced-state, not FABRICATED-STATE (--no-poke)."
    end
    if test $manual -eq 0
        echo "Mode: AUTO. Route $route replayed up to the Map Maker preview (FABRICATED input, your own recorded keys), dumping armed after the hand over."
    else
        echo "Mode: MANUAL. No route, the owner drove to the preview, dumping armed from the first poll."
    end
    echo "Range set: $rangeset (+ the poke ranges and EXTRA_RANGES if given)   After frames: $after_frames   idle every: $idle_every polls (max $max_idle)   max dump files: $max_dumps   time limit: $minutes min   ranges: $nranges"
    echo "Ranges: $ranges"
    echo "Files: buttons.jsonl (manifest), buttons/guestdump.<label> (dumps), button_report.md (python -m tools.button_dump_report DIR --write),"
    echo "play.log (host log), play.stdout (launcher output). Design: docs/t-button-dumps.md."
    echo "Primary targets: the inventory ext block *0x7356D8+0:0xEA0 (owned bytes +0x54C, reserve ammo +0x61E, current weapon +0x94, previous +0x98, hand structs) and the player record *0x7B0C48+0:0x1584."
    echo
    owner_block
end > $outdir/README_SESSION.txt
begin
    echo "Owner notes for $session (the script appends your one line note at the end)."
    echo "Setting 'auto switch weapon on pickup' = Weapon Change (menu path and the value you set, or 'not touched' / 'none found'):"
end > $outdir/OWNER_NOTES.txt

echo
echo "Session: $session"
if test $manual -eq 0
    echo "AUTO. Route: $route   (HDD: a fresh copy of $pristine)"
    echo "Waits: $waitspecs"
else
    echo "MANUAL. HDD $workhdd. Dumping is armed from the start."
end
if test $poke -eq 1
    echo
    echo "WEAPON SPAWNERS (FABRICATED-STATE poke before the preview; spawner N = slot N, unlisted slots untouched):"
    printf '  %s\n' $planlines
    if test $manual -eq 0
        echo "The host pokes these six slots by itself when the route reaches the editor (request $outdir/guestpoke.$pokelabel) and the check table '[poke] ...' appears in this terminal."
    else
        echo
        printf '%s%s%s\n' (set_color --bold) "MANUAL POKE: be in the Map Maker EDITOR on the weapon set page, then run this in a SECOND terminal from the repo root BEFORE you enter the preview:" (set_color normal)
        echo
        echo "  "(python -m tools.button_dump_poke manual-lines --label $pokelabel --weapons $weapons)
        echo
        echo "Poke only when the editor is up and idle (T1639): the game state mem 0x79094C == 0x66 and the editor weapon-set page value *0x7844A8+0x11908:1 == 6. Do NOT poke on or after a weapon settings page visit."
        echo "It must print 'request served' and OK lines (a REFUSED line wrote nothing). Check it with:"
        echo "  python -m tools.button_dump_poke verify $outdir --label $pokelabel --weapons $weapons"
    end
else
    echo "NO POKE (--no-poke): the spawners hold whatever the map has."
end
# T1767: the plan for THIS run, from tools/data/owner_scenarios/$OWNER_SCENARIO.json (default run_button_dumps), also kept in README_SESSION.txt
set -l scenario_name run_button_dumps
test $checklist_on -eq 1; and set scenario_name run_button_checklist
test -n "$OWNER_SCENARIO"; and set scenario_name $OWNER_SCENARIO
set -l scenario_mode auto
test $manual -eq 1; and set scenario_mode manual
set -l scenario_poke on
test $poke -eq 0; and set scenario_poke off
set -l scenario_opts --opt mode=$scenario_mode --opt minutes=$minutes --opt session=$session
test $checklist_on -eq 0; and set -a scenario_opts --opt poke=$scenario_poke --opt idle=$idle_every --opt max_dumps=$max_dumps --opt max_idle=$max_idle
test $poke -eq 1; and test $checklist_on -eq 0; and set -a scenario_opts --opt weapons=$weapons
test $manual -eq 0; and set -a scenario_opts --opt route=$route
test $checklist_on -eq 1; and set -a scenario_opts --opt checklist=$checklist_file
set -l scenario_text (timeout 60 python -m tools.owner_scenario show $scenario_name $scenario_opts 2>&1)
if test $status -eq 0
    printf '%s\n' $scenario_text
    begin
        echo
        printf '%s\n' $scenario_text
    end >> $outdir/README_SESSION.txt
else
    printf '%s\n' $scenario_text
    echo "WARNING (T1767): no owner scenario text for $scenario_name (python -m tools.owner_scenario show $scenario_name failed, see above). The run still works, but there is NO written plan: ask for one before you play."
end
read -l -P "Enter = start the game, q = quit: " answer
if test "$answer" = q
    echo "nothing launched, the folder $outdir only holds README_SESSION.txt and the HDD copy"
    exit 0
end

# --- launch (own session: Ctrl+C in this terminal must not reach the host) ----------------------------------------------------------
echo "launching the game (plain tools.play), the window opens in a moment ..."
setsid python -m tools.play $playargs --host "$host" > $outdir/play.stdout 2>&1 < /dev/null &
set -l game_job $last_pid

echo "Waiting for the host to come up (up to 5 minutes)..."
set -l waited 0
while not test -f $outdir/guestdump.pid
    if not kill -0 $game_job 2>/dev/null
        echo "the launcher ended before the host came up, last lines of $outdir/play.stdout:"
        tail -n 12 $outdir/play.stdout; exit 1
    end
    sleep 1
    set waited (math $waited + 1)
    if test $waited -ge 300
        echo "host did not start, see $outdir/play.stdout"
        kill -TERM $game_job 2>/dev/null
        exit 1
    end
end
set -g g_outdir $outdir
set -g g_hostpid (cat $outdir/guestdump.pid)
set -g g_game_job $game_job
set -g g_finished 0
set -g g_checklist $checklist_on
if test $poke -eq 1
    set -g g_pokelabel $pokelabel
    set -g g_weapons $weapons
else
    set -g g_pokelabel ""
    set -g g_weapons ""
end
echo "Host is up (pid $g_hostpid)."

set -l limit_seconds (math $minutes \* 60)
set -l pokelabel_arg -
if test $poke -eq 1
    set pokelabel_arg $pokelabel
end
setsid $fishbin $self --watch $outdir $g_hostpid $fish_pid $limit_seconds $pokelabel_arg $pokeweapons &
set -g g_watcher $last_pid

# --- stop and report (used by q, Ctrl+C, the signal handler and the normal end) ----------------------------------------------------
function stop_host --argument-names pid
    if test -z "$pid"; or not kill -0 $pid 2>/dev/null
        return 0
    end
    echo "stopping the game: SIGTERM to pid $pid (the host flushes the manifest) ..."
    kill -TERM $pid
    for i in (seq 30)
        if not kill -0 $pid 2>/dev/null
            echo "the game stopped cleanly after $i s"
            return 0
        end
        sleep 1
    end
    echo "still running after 30 s, last resort: SIGKILL pid $pid (the manifest may lack its end line)"
    kill -KILL $pid 2>/dev/null
end

function finish_session
    if test $g_finished -eq 1
        return 0
    end
    set -g g_finished 1
    set -l dir $g_outdir
    kill -TERM $g_watcher 2>/dev/null
    stop_host $g_hostpid
    for i in (seq 15)
        if not kill -0 $g_game_job 2>/dev/null
            break
        end
        sleep 1
    end
    echo
    echo "== report =="
    if python -c "import importlib.util,sys;sys.exit(0 if importlib.util.find_spec('tools.button_dump_report') else 1)" 2>/dev/null
        timeout 600 python -m tools.button_dump_report $dir --write
        or echo "the report tool failed (status $status), run it again by hand: python -m tools.button_dump_report $dir --write"
    else
        echo "tools.button_dump_report is missing in this checkout: no report. Update the repo, then: python -m tools.button_dump_report $dir --write"
    end
    set -l events 0
    set -l idles 0
    set -l dropped 0
    if test -f $dir/buttons.jsonl
        set events (grep -c -F '"type":"event"' $dir/buttons.jsonl)
        set idles (grep -c -F '"edge":"idle"' $dir/buttons.jsonl)
        set dropped (grep -c -F '"type":"dropped"' $dir/buttons.jsonl)
        if not grep -q -F '"type":"end"' $dir/buttons.jsonl
            echo "NOTE: the manifest has no end line (the host was killed or crashed), the reader tolerates that"
        end
    else
        echo "NOTE: no buttons.jsonl was written (no press was recorded, or the host does not know --dump-on-button)"
    end
    set -l files (find $dir/buttons -type f -name 'guestdump.*' 2>/dev/null | wc -l | string trim)
    echo "events: $events (idle controls: $idles)   dropped: $dropped   dump files: $files"
    du -sh $dir/buttons 2>/dev/null
    if test -f $dir/button_report.md
        echo "report: $dir/button_report.md"
    end
    echo "session folder: $dir"
    if test -n "$g_pokelabel"
        echo
        echo "== poke verification (FABRICATED-STATE editor slots) =="
        set -l verifyout (timeout 30 python -m tools.button_dump_poke verify $dir --label $g_pokelabel --weapons $g_weapons 2>&1)
        set -l verifystatus $status
        for line in $verifyout
            echo "[poke] $line"
        end
        switch $verifystatus
            case 0
                echo "[poke] the spawner pokes were applied and read back."
            case 3
                echo "[poke] !!!! NO POKE WAS SERVED (no complete guestpoke.log entry and poke dump for label $g_pokelabel). The spawners were probably EMPTY. In AUTO mode look for 'route FAILED' or 'route poke' in $dir/play.log, in manual mode the poke_now.fish line was not run. !!!!"
            case '*'
                echo "[poke] !!!! THE POKE DID NOT WORK (verify status $verifystatus, 1 = REFUSED, 2 = READBACK-MISMATCH): the spawners are NOT as planned. Treat this session as an UNPOKED map. !!!!"
        end
        begin
            echo
            echo "POKE VERIFY (at the end of the session, status $verifystatus):"
            printf '%s\n' $verifyout
            if test $verifystatus -ne 0
                echo "!!!! POKE NOT VERIFIED (status $verifystatus: 1 REFUSED, 2 READBACK-MISMATCH, 3 never served): treat this session as an UNPOKED map. !!!!"
            end
        end >> $dir/README_SESSION.txt
    end
    if test $g_checklist -eq 1
        echo
        echo "== checklist report =="
        timeout 600 python -m tools.button_checklist_report $dir --write
        or echo "the checklist report failed (status $status), run it again by hand: python -m tools.button_checklist_report $dir --write"
        echo "report: $dir/checklist_report.md"
        echo "done: tell Claude the session finished ($dir) and send checklist_report.md, checklist.jsonl and any step where the terminal said '!!'"
        return 0
    end
    echo
    echo "Weapon Change ('auto switch weapon on pickup'): which menu path and which value did you set? (type 'not touched' if you never opened the menu, 'none found' if the game has no such row, Enter alone skips)"
    set -l note ""
    read -l -P "note> " note
    if test -z "$note"
        set note "(no note given)"
    end
    printf '%s\n' "$note" >> $dir/OWNER_NOTES.txt
    echo "saved to $dir/OWNER_NOTES.txt"
    echo "done: tell Claude the session finished ($dir), what you did in what order, and which weapons you used"
end

function on_stop_signal --on-signal SIGINT --on-signal SIGTERM --on-signal SIGHUP
    if test $g_finished -eq 1
        echo
        echo "already stopping the game, please wait (further Ctrl+C is ignored until the report is written) ..."
        return 0
    end
    echo
    echo "signal received, cleaning up ..."
    finish_session
    exit 130
end

# --- foreground: q + Enter stops --------------------------------------------------------------------------------------------------
if test $checklist_on -eq 1
    # T1736: the checklist drives the foreground. AUTO mode waits for the route handover first (the watcher prints the same line).
    if test $manual -eq 0
        echo "waiting for the route to hand the pad over (do not touch anything) ..."
        set -l routewait 0
        while not grep -q -F 'the recorded inputs are exhausted' $g_outdir/play.log 2>/dev/null
            if not kill -0 $g_hostpid 2>/dev/null
                echo "the game ended before the route finished, see $g_outdir/play.log"
                break
            end
            if grep -q -F 'route FAILED' $g_outdir/play.log 2>/dev/null
                echo "ROUTE FAILED, see $g_outdir/play.log. Run again with --manual."
                break
            end
            sleep 2
            set routewait (math $routewait + 2)
            if test $routewait -ge 1800
                echo "the route did not finish in 30 minutes"
                break
            end
        end
    end
    if test $manual -eq 1
        read -l -P "Get the game to a calm place, then Enter = start the checklist (q = stop): " answer
        test "$answer" = q; and begin; finish_session; exit 0; end
    end
    if kill -0 $g_hostpid 2>/dev/null
        echo
        echo "CHECKLIST: $checklist_file. Click the TERMINAL only to press Enter, hold your pad or stick with the other hand. Type s = skip a step, q = stop."
        timeout 7200 python -m tools.button_checklist run $g_outdir --file $checklist_file
        or echo "the checklist driver ended with status $status"
    end
    finish_session
    exit 0
end
echo
echo "Play now, following the plan above. Type q + Enter here to stop (Enter alone prints the count)."
while true
    read -l -P "" answer; or break
    if test "$answer" = q
        break
    end
    if not kill -0 $g_hostpid 2>/dev/null
        echo "the game is no longer running"
        break
    end
    set -l count 0
    if test -f $g_outdir/buttons.jsonl
        set count (grep -c -F '"type":"event"' $g_outdir/buttons.jsonl)
    end
    echo "[buttons] events so far: $count"
end

finish_session
