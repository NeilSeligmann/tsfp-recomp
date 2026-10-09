#!/usr/bin/env fish
# T1720 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_render_shots.fish tmp/run_render_shots.fish
# then: fish tmp/run_render_shots.fish [--route FILE | --free]      (DRY_RUN=1 prints the plan and the commands, starts nothing)
# What you have to do is printed from tools/data/owner_scenarios/run_render_shots.json.
#
# Screenshot session in the Map Maker preview. The script replays your saved route (the newest usable
# tmp/owner-profiles/routes/*.txt, the one tmp/record_mapmaker_route.fish wrote, for example a *.trim2.txt or *.nopage.txt) up to the
# preview exactly like tmp/run_button_dumps.fish does (--replay-input ROUTE --replay-handover, the route waits, the event waits), then
# the pad is YOURS. The host hotkey label `shot` (pad Back+Start+LB+A, keyboard Ctrl+Shift+S) writes shot-NNN.png of the presented
# frame plus a line in shots.manifest, `stop` ends the run cleanly. The terminal shows the live shot count. Afterwards
# tools.render_shots checks the pictures and tools.shot_index writes shots.jsonl, contact-sheet.html and contact-sheet.png
# (docs/play.md 'Screenshots while playing'). A new timestamped folder is used every run.
#
# Options:  --route FILE   replay this route instead of the newest usable one (also env ROUTE=FILE)
#           --free         no route: start from boot with tmp/hdds/profile-hdd and drive to the preview yourself
#           -h, --help
# Env:      ROUTE ROUTE_SEARCH_DIR (default tmp/owner-profiles/routes) SHOT_MAX (default 200) SHOT_MAX_BYTES (default 536870912)
#           DRY_RUN=1  PYTHON (the interpreter, default python, else python3)
# With no usable route the script falls back to the free flow and prints a NOTE. A host without the marker T1720b in --help gets
# the old flags (the pictures then go to the hotkey folder with the default limits).

# the repo root: the first folder at or above this file that holds tools/render_shots.py (the file may sit in tmp/, which can be a
# symlink, so the path is walked as written, not resolved)
set -l root (dirname (status filename))
string match -q '/*' -- $root; or set root $PWD/$root
while not test -f $root/tools/render_shots.py; and test $root != /
    set root (dirname $root)
end
cd $root; or exit 1

set -g py python
test -n "$PYTHON"; and set py $PYTHON
if not type -q $py; and type -q python3
    set py python3
end

# --- options -------------------------------------------------------------------------------------------------------------------
set -l route ""
set -l free 0
test -n "$ROUTE"; and set route $ROUTE
set -l optindex 1
while test $optindex -le (count $argv)
    switch $argv[$optindex]
        case --free
            set free 1
        case --route
            set optindex (math $optindex + 1)
            if test $optindex -gt (count $argv)
                echo "--route needs a value"; exit 2
            end
            set route $argv[$optindex]
        case -h --help
            echo "usage: fish tmp/run_render_shots.fish [--route FILE | --free]"
            echo "env: ROUTE ROUTE_SEARCH_DIR SHOT_MAX SHOT_MAX_BYTES DRY_RUN PYTHON    (fish: env DRY_RUN=1 fish tmp/run_render_shots.fish)"
            echo "see the header of this file and docs/play.md, Screenshots while playing"
            exit 0
        case '*'
            echo "unknown option: $argv[$optindex] (--route FILE, --free)"; exit 2
    end
    set optindex (math $optindex + 1)
end
if test $free -eq 1; and test -n "$route"
    echo "--free and --route exclude each other"; exit 2
end

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l session render-$stamp
set -l outdir tmp/owner-profiles/$session
set -l dry (test "$DRY_RUN" = 1; and echo 1; or echo 0)
set -l shotmax 200
set -l shotmaxbytes 536870912
test -n "$SHOT_MAX"; and set shotmax $SHOT_MAX
test -n "$SHOT_MAX_BYTES"; and set shotmaxbytes $SHOT_MAX_BYTES
set -l searchdir tmp/owner-profiles/routes
test -n "$ROUTE_SEARCH_DIR"; and set searchdir $ROUTE_SEARCH_DIR
set -l pristine ./tmp/hdds/route-hdd-pristine

# --- the route: explicit, else the newest usable one, else the free flow ----------------------------------------------------------
# usable = a closed recording with the 4 marks of tmp/record_mapmaker_route.fish (profile, map loaded, editor, PREVIEW), checked by
# the same tool run_button_dumps uses (python -m tools.button_dump_poke route-check).
function pick_route --argument-names dir
    for candidate in (find $dir -maxdepth 1 -name '*.txt' -printf '%T@ %p\n' 2>/dev/null | sort -nr | string replace -r '^[0-9.]+ ' '')
        if $py -m tools.button_dump_poke route-check $candidate >/dev/null 2>&1
            echo $candidate
            return 0
        end
        echo "NOTE: skipping $candidate (no closed recording with 4 marks)" >&2
    end
    return 1
end

set -l mode route
set -l explicit 0
if test $free -eq 1
    set mode free
else if test -n "$route"
    set explicit 1
    if not test -f "$route"
        echo "route not found: $route"; exit 2
    end
    if not $py -m tools.button_dump_poke route-check $route
        echo "$route does not fit: it needs a closed recording with the 4 marks of tmp/record_mapmaker_route.fish. Record again, or use --free"; exit 2
    end
else
    set route (pick_route $searchdir)
    if test -z "$route"
        echo "NOTE: no usable route found in $searchdir (record one ONCE with: fish tmp/record_mapmaker_route.fish). Falling back to the FREE flow: the game starts from boot with tmp/hdds/profile-hdd and you drive to the Map Maker preview yourself."
        set mode free
    end
end
if test $mode = route; and not test -d $pristine
    if test $dry = 1
        echo "NOTE: no $pristine (made by tmp/record_mapmaker_route.fish), a real run would "(test $explicit = 1; and echo "stop here"; or echo "fall back to the FREE flow")
    else if test $explicit = 1
        echo "no $pristine (made by tmp/record_mapmaker_route.fish): the route cannot start from its HDD state. Run it once, or use --free"; exit 2
    else
        echo "NOTE: no $pristine (made by tmp/record_mapmaker_route.fish): the route cannot start from its HDD state. Falling back to the FREE flow."
        set mode free
    end
end

echo "Session: $session"
if test $mode = route
    echo "Mode: ROUTE. $route is replayed up to the Map Maker preview (FABRICATED input, your own recorded keys), then the pad is yours."
else
    echo "Mode: FREE. No route is replayed, you drive to the Map Maker preview yourself (HDD tmp/hdds/profile-hdd)."
end
echo "Phase label (optional, second terminal): echo radar > $outdir/phase"
echo "Pictures: $outdir/shot-NNN.png, index: $outdir/shots.jsonl, contact sheet: $outdir/contact-sheet.png and $outdir/contact-sheet.html"
set -l scenarioopts --opt mode=$mode --opt folder=$outdir
test $mode = route; and set -a scenarioopts --opt route=$route
$py -m tools.owner_scenario show run_render_shots $scenarioopts
or echo "WARNING (T1767): no owner scenario text for run_render_shots, press the shot chord whenever something is worth a picture."

set -l hotkeyflags ($py -m tools.hotkey_defaults flags --labels shot,stop --dir $outdir); or begin; echo "cannot build the hotkey flags (python -m tools.hotkey_defaults flags failed)"; exit 1; end
# the host flags of this feature (T1720b), only used when the host says it knows them
set -l shotflags --shot-dir $outdir --shot-max $shotmax --shot-max-bytes $shotmaxbytes

# --- the game flags -----------------------------------------------------------------------------------------------------------------
set -l playbase
set -l workhdd ./tmp/hdds/profile-hdd
set -l waitspecs
set -l recordwaitmarks
if test $mode = route
    # ONE list shared with the route recording and the other replay scripts (tools.play.route_check.route_game_flags)
    set -l gameflags ($py -m tools.play.route_check flags); or begin; echo "cannot build the route flags (python -m tools.play.route_check flags failed)"; exit 1; end
    set workhdd $outdir/hdd
    set playbase $gameflags --console --gpu-live-blit --present window --pad-source keyboard --pad-source gamepad
    set -a playbase --disc "$disc" --hdd $workhdd --replay-input $route --replay-handover
    set -a playbase $hotkeyflags --log-file $outdir/play.log
    # the route may carry its own event waits ('# wait: markK:...' lines written by tools.route_events), else the .waits file
    set recordwaitmarks (grep -o '^# wait: mark[0-9]*:' $route 2>/dev/null | string replace -r '^# wait: mark([0-9]+):$' '$1')
    set -l waitsfile (string replace -r '\.txt$' '.waits' -- $route)
    if test -f $waitsfile
        for line in (cat $waitsfile)
            if string match -qr '^mark[0-9]+:' -- $line
                set -a waitspecs $line
            end
        end
    else
        echo "NOTE: no $waitsfile, the replay will not wait at the marks (poll timing only)"
    end
else
    set playbase --console --gpu-live --gpu-live-inferred --gpu-live-blit --present window --pad-source keyboard --pad-source gamepad --xonline-offline
    set -a playbase --disc "$disc" --hdd $workhdd $hotkeyflags --log-file $outdir/play.log
end

if test $dry = 1
    echo
    echo "DRY_RUN: nothing is started. The plan:"
    if test $mode = route
        echo "  1. check the route (nothing launched):              $py -m tools.play $playbase --check-route"
    else
        echo "  1. (free mode: no route to check)"
    end
    echo "  2. build the private host:                          $py -m tools.private_host build"
    echo "  3. host --help must show T1720 (shot label) and T1720b (shot flags). With T1720b these are added:"
    echo "       $shotflags"
    echo "     without it the old flags run and a NOTE is printed (the pictures go to the hotkey folder, default limits)."
    if test $mode = route
        echo "     the route check is repeated with the shot flags, and they are dropped (NOTE) if the route identity does not ignore them yet."
        echo "     host marker T1633: event waits from the route, else --route-wait for: $waitspecs (marks with event waits: $recordwaitmarks)"
    end
    echo "  4. launch (own session, output to $outdir/play.stdout):"
    echo "       setsid $py -m tools.play $playbase $shotflags --host <private host>"
    echo "  5. live shot count while playing, stop by the stop chord, q + Enter, or closing the window"
    echo "  6. afterwards:  $py -m tools.render_shots report $outdir"
    echo "                  $py -m tools.shot_index $outdir   (writes shots.jsonl, contact-sheet.html, contact-sheet.png)"
    $py -m tools.shot_index --help >/dev/null; or exit 1
    $py -m tools.render_shots --help >/dev/null; or exit 1
    exit 0
end

# --- check the route against the flags of this run BEFORE the multi-minute host build (nothing is launched) ---------------------------
function check_route
    set -l out (timeout 120 $py -m tools.play $argv --check-route 2>&1)
    set -l code $status
    if test $code -ne 0
        printf '%s\n' $out
    else
        printf '%s\n' $out | tail -n 2
    end
    return $code
end
if test $mode = route
    echo "== check the route against the flags of this run (nothing is launched) =="
    if not check_route $playbase
        echo "route was recorded with a different flag set or the check failed, record it again with tmp/record_mapmaker_route.fish (or use --free)"; exit 2
    end
end

echo "== build the private host (re-lifts if needed, a few minutes) =="
timeout 7200 $py -m tools.private_host build; or exit 1
set -l host ($py -c "import json;print(json.load(open('tmp/private-host/current.json'))['host'])")
test -x "$host"; or begin; echo "host not found: $host"; exit 1; end
set -l hosthelp (timeout 60 $host --help 2>&1)
if not printf '%s\n' $hosthelp | grep -q -- T1720
    echo "this host has no shot hotkey (marker T1720 missing in --help, or --help did not answer in 60 s), rebuild: python -m tools.private_host build"; exit 2
end
set -l useshotflags 0
if printf '%s\n' $hosthelp | grep -q -- T1720b
    set useshotflags 1
else
    echo "NOTE: this host predates T1720b (no --shot-dir, --shot-max, --shot-max-bytes): the pictures go to the hotkey folder $outdir with the default limits. Rebuild: python -m tools.private_host build"
end
if test $mode = route
    if not grep -aq -- '--route-wait' $host
        echo "host has no route replay (T1616), rebuild: python -m tools.private_host build   (or use --free)"; exit 2
    end
    if not printf '%s\n' $hosthelp | grep -q -- T1618
        echo "host predates T1618 (route flag identity), rebuild: python -m tools.private_host build"; exit 2
    end
    # T1633: a host with the marker runs the '# wait:' lines of the route instead of the time based .waits specs
    set -l eventhost 0
    if printf '%s\n' $hosthelp | grep -q -- T1633
        set eventhost 1
    end
    for spec in $waitspecs
        set -l waitmark (string replace -r '^mark([0-9]+):.*$' '$1' -- $spec)
        if test $eventhost -eq 1; and contains -- $waitmark $recordwaitmarks
            echo "mark $waitmark: waits for the events of the route ('# wait:' line), not for the time based $spec"
            continue
        end
        set -a playbase --route-wait $spec
    end
    if test $eventhost -eq 1
        set -a playbase --route-event-log $outdir/route-events.log --route-log-mem 0x79094C --route-log-mem 0x52D400
        echo "event driven route (T1633): the host logs file I/O and sentinels to $outdir/route-events.log"
    else if test (count $recordwaitmarks) -gt 0
        echo "WARNING: the route has event waits ('# wait:' lines) but this host predates T1633, so it falls back to the TIME BASED waits of the .waits file. Rebuild: python -m tools.private_host build"
    end
end
set -l playargs $playbase
if test $useshotflags -eq 1
    if test $mode = route; and not check_route $playbase $shotflags >/dev/null 2>&1
        echo "NOTE: the route identity does not ignore the T1720b shot flags yet (tools/play/route_check.py, src/input/xinput_record.c): they are left out, the pictures go to the hotkey folder $outdir with the default limits."
    else
        set -a playargs $shotflags
    end
end

mkdir -p $outdir
if test $mode = route
    rm -rf $workhdd
    cp -a $pristine $workhdd; or exit 1
end

read -l -P "Enter = start the game, q = quit: " answer
if test "$answer" = q
    echo "nothing launched, the folder $outdir holds only the HDD copy"
    exit 0
end

# --- launch (own session: Ctrl+C in this terminal must not reach the host) ----------------------------------------------------------
echo "launching the game (plain tools.play), the window opens in a moment ..."
setsid $py -m tools.play $playargs --host "$host" > $outdir/play.stdout 2>&1 < /dev/null &
set -g g_launcher $last_pid
set -g g_outdir $outdir
set -g g_finished 0
set -g g_started (date +%s)

# the host is the child of the launcher; wait for it (up to 5 minutes)
function host_pids
    pgrep -P $g_launcher 2>/dev/null
end
set -l waited 0
while test (count (host_pids)) -eq 0
    if not kill -0 $g_launcher 2>/dev/null
        echo "the launcher ended before the host came up, last lines of $outdir/play.stdout:"
        tail -n 12 $outdir/play.stdout; exit 1
    end
    sleep 1
    set waited (math $waited + 1)
    if test $waited -ge 300
        echo "host did not start, see $outdir/play.stdout"
        kill -TERM $g_launcher 2>/dev/null
        exit 1
    end
end
echo "Host is up (pid "(host_pids | string join ',')")."

function shot_count
    find $g_outdir -maxdepth 1 -name 'shot-*.png' 2>/dev/null | wc -l | string trim
end

# --- stop and report (used by q, the stop chord, closing the window, Ctrl+C and the normal end) -------------------------------------
function stop_host
    set -l pids (host_pids)
    if test -z "$pids"; or not kill -0 $g_launcher 2>/dev/null
        return 0
    end
    echo "stopping the game: SIGTERM to pid $pids (the host flushes the manifest) ..."
    kill -TERM $pids 2>/dev/null
    for i in (seq 30)
        if not kill -0 $g_launcher 2>/dev/null
            echo "the game stopped cleanly after $i s"
            return 0
        end
        sleep 1
    end
    echo "still running after 30 s, last resort: SIGKILL pid $pids (the manifest may lack its end line)"
    kill -KILL $pids 2>/dev/null
end

function finish_session
    if test $g_finished -eq 1
        return 0
    end
    set -g g_finished 1
    stop_host
    echo
    echo "== report =="
    set -l count (shot_count)
    $py -m tools.render_shots report $g_outdir
    or echo "no shots.manifest: was the shot chord pressed while the game was running?"
    if test -f $g_outdir/shots.manifest
        $py -m tools.shot_index $g_outdir
        set -l indexstatus $status
        switch $indexstatus
            case 0
                echo "all pictures are fine"
            case 1
                echo "NOTE: some pictures are blank or unreadable (listed above), the index and the contact sheet were still written"
            case '*'
                echo "tools.shot_index failed (status $indexstatus), run it again by hand: python -m tools.shot_index $g_outdir"
        end
    end
    grep -n -E "unimplemented instruction|^STOP|route FAILED" $g_outdir/play.log 2>/dev/null | tail -3
    echo
    echo "shots taken: $count"
    echo "folder:        $g_outdir"
    echo "index:         $g_outdir/shots.jsonl"
    echo "contact sheet: $g_outdir/contact-sheet.png  (and $g_outdir/contact-sheet.html)"
    echo "done: tell Claude the session finished ($g_outdir) and send one note line per picture number (labels.txt works too, then run: python -m tools.shot_index $g_outdir)"
end

function on_stop_signal --on-signal SIGINT --on-signal SIGTERM --on-signal SIGHUP
    if test $g_finished -eq 1
        echo
        echo "already stopping the game, please wait ..."
        return 0
    end
    echo
    echo "signal received, cleaning up ..."
    finish_session
    exit 130
end

# the last line of defence: when this script ends any other way, the host is not left running
function on_script_exit --on-event fish_exit
    if test "$g_finished" = 0
        stop_host
    end
end

# --- foreground: live shot count, q + Enter stops, the host ending (stop chord, closed window) ends it too ------------------------------
echo
echo "Play now, following the plan above. The shot count is printed here. Type q + Enter to stop (Enter alone prints the count)."
set -l routeseen 0
set -l lastcount -1
set -l lastbeat (date +%s)
set -l pollable 0
type -q bash; and set pollable 1
if test $pollable -eq 0
    echo "NOTE: bash was not found, so the live shot count is only printed when you press Enter (and the game ending is noticed after Enter)."
end
set -l noinput 0
while kill -0 $g_launcher 2>/dev/null
    set -l answer ""
    set -l readstatus 142
    if test $noinput -eq 1
        sleep 2
    else if test $pollable -eq 1
        # bash read -t gives a 2 s poll, fish read has no timeout. 142 = nothing typed yet, 1 = no terminal input (EOF)
        set answer (bash -c 'IFS= read -r -t 2 reply; code=$?; if [ $code -eq 0 ]; then printf "%s" "${reply:-ENTER}"; fi; exit $code')
        set readstatus $status
        if test $readstatus -eq 1
            set noinput 1
            echo "NOTE: no terminal input, q + Enter cannot be used: stop with the stop chord or by closing the game window."
            continue
        end
    else
        if not read -l -P "" answer
            set noinput 1
            echo "NOTE: no terminal input, q + Enter cannot be used: stop with the stop chord or by closing the game window."
            continue
        end
        set readstatus 0
        test -z "$answer"; and set answer ENTER
    end
    if test "$answer" = q
        break
    end
    set -l now (date +%s)
    set -l count (shot_count)
    if test "$count" != "$lastcount"; or test "$answer" = ENTER; or test (math $now - $lastbeat) -ge 30
        set -l lastshot (find $g_outdir -maxdepth 1 -name 'shot-*.png' 2>/dev/null | sort | tail -n 1 | string replace -r '^.*/' '')
        echo "[shots] $count taken"(test -n "$lastshot"; and echo ", last $lastshot")
        set lastcount $count
        set lastbeat $now
    end
    if test $routeseen -eq 0; and test -f $g_outdir/play.log
        if grep -q -F 'route FAILED' $g_outdir/play.log 2>/dev/null
            set routeseen 1
            echo "[shots] ROUTE FAILED (a wait timed out), see 'route FAILED' in $g_outdir/play.log. Press q + Enter, then run the script again with --free."
        else if grep -q -F 'the recorded inputs are exhausted' $g_outdir/play.log 2>/dev/null
            set routeseen 1
            echo "[shots] route finished: the pad is YOURS. Press the SHOT chord for each picture on the plan."
        end
    end
end
if not kill -0 $g_launcher 2>/dev/null
    echo "the game has ended"
end

finish_session
