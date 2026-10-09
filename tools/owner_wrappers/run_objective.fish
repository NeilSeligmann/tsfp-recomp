#!/usr/bin/env fish
# T1759 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_objective.fish tmp/run_objective.fish
# then: fish tmp/run_objective.fish          (DRY_RUN=1 prints the plan and the commands, starts nothing)
# What you have to do is printed from tools/data/owner_scenarios/run_objective.json.
# Census session: ingame-idle (control), then ingame-objective (the first Story objective). The host dumps
#   *0x6F32F0+0:0x160  the cached mode record M (rule/setup counts at M+0x124/0x12C/0x134)
#   *0x765E44+0:0x1960 the script context C (group, condition, action, point arrays, T1757)
#   **0x6F32F0+0x138+0:0x2200 the first 16 rule records (0x220 each, base M+0x138, two pointer levels, T1759 host form)
#   **0x6F32F0+0x130+0:0x1680 the first 32 setup entries (0xB4 each, base M+0x130)
# at each phase end (guestdump.ingame-idle = start, guestdump.ingame-objective = end), at exit, and around every button press
# (--dump-on-button, DIR/buttons). At the end tools.objective_script_report diffs start and end (docs/t-data-objective-scenario.md).
cd (dirname (status filename))/..; or exit 1

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l session objective-$stamp
set -l outdir tmp/owner-profiles/$session
set -l ranges "*0x6F32F0+0:0x160,*0x765E44+0:0x1960,**0x6F32F0+0x138+0:0x2200,**0x6F32F0+0x130+0:0x1680"
set -l dry (test "$DRY_RUN" = 1; and echo 1; or echo 0)

echo "Session: $session"
echo "Phases: ingame-idle, then ingame-objective"
python -m tools.owner_scenario show run_objective
or echo "WARNING (T1767): no owner scenario text for run_objective, the prompts fall back to the generic example."

if test $dry = 1
    echo "DRY_RUN: would build the host, run tools.action_profile session --only ingame-idle,ingame-objective into $outdir"
    echo "         with --dump-guest-range $ranges --dump-on-button --dump-after-frames 2,30,"
    echo "         then action_census_diff and objective_script_report --session on it."
    python -m tools.action_profile session --help >/dev/null; or exit 1
    python -m tools.objective_script_report --help >/dev/null; or exit 1
    exit 0
end

echo "== build the private host (re-lifts if needed, a few minutes) =="
python -m tools.private_host build; or exit 1
set -l host (python -c "import json;print(json.load(open('tmp/private-host/current.json'))['host'])")
test -x "$host"; or begin; echo "host not found: $host"; exit 1; end

read -l -P "Enter = start the game, q = quit: " answer
if test "$answer" = q
    exit 0
end

mkdir -p $outdir
python -m tools.action_profile session --group ingame --name $session --source both \
    --only ingame-idle,ingame-objective \
    --owner-script run_objective \
    --disc "$disc" --host "$host" \
    --play-arg=--window --play-arg=--interactive --play-arg=--console \
    --play-arg=--gpu-live --play-arg=--gpu-live-inferred --play-arg=--gpu-live-blit \
    --play-arg=--present --play-arg=window \
    --play-arg=--pad-source --play-arg=keyboard \
    --play-arg=--pad-source --play-arg=gamepad \
    --play-arg=--xonline-offline \
    --play-arg=--hdd --play-arg=./tmp/hdds/profile-hdd \
    --play-arg=--dump-guest-range --play-arg=$ranges --play-arg=--dump-guest-dir --play-arg=$outdir \
    --play-arg=--dump-on-button --play-arg=--dump-after-frames --play-arg=2,30 \
    --play-arg=--dump-button-max-dumps --play-arg=600 \
    --play-arg=--log-file --play-arg=$outdir/play.log
or echo "session ended early, see $outdir/play.log (copy the 'unimplemented instruction' / 'STOP' line)"

echo "== analyse =="
python -m tools.action_census_diff $outdir
python -m tools.objective_script_report --session $outdir
or echo "no start/end script dump pair (host refused --dump-guest-range or --dump-on-button?): check $outdir/play.log, send the folder anyway."
grep -n -E "unimplemented instruction|^STOP" $outdir/play.log | tail -3
echo "done: tell Claude the session finished ($outdir) and what the first objective was"
