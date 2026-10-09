#!/usr/bin/env fish
# T1741 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_enemy_encounter.fish tmp/run_enemy_encounter.fish
# then: fish tmp/run_enemy_encounter.fish          (DRY_RUN=1 prints the plan and the commands, starts nothing)
# What you have to do is printed from tools/data/owner_scenarios/run_enemy_encounter.json.
# Census session: ingame-idle (control), then ingame-enemy-encounter (30 s fighting the first HOSTILE enemies in Story).
# At the end tools.action_census_diff ranks the encounter against the idle control and tools.ai_attribution labels the
# ai_* / class tick functions (docs/t-ai-enemy-encounter.md). A new timestamped session folder is used on every run.
cd (dirname (status filename))/..; or exit 1

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l session enemy-encounter-$stamp
set -l outdir tmp/owner-profiles/$session
set -l dry (test "$DRY_RUN" = 1; and echo 1; or echo 0)

echo "Session: $session"
echo "Phases: ingame-idle, then ingame-enemy-encounter"
python -m tools.owner_scenario show run_enemy_encounter
or echo "WARNING (T1767): no owner scenario text for run_enemy_encounter, the prompts fall back to the generic example."

if test $dry = 1
    echo "DRY_RUN: would build the host, run tools.action_profile session --only ingame-idle,ingame-enemy-encounter into $outdir,"
    echo "         then action_census_diff, and ai_attribution --require-ai on it."
    python -m tools.action_profile session --help >/dev/null; or exit 1
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
    --only ingame-idle,ingame-enemy-encounter \
    --owner-script run_enemy_encounter \
    --disc "$disc" --host "$host" \
    --play-arg=--window --play-arg=--interactive --play-arg=--console \
    --play-arg=--gpu-live --play-arg=--gpu-live-inferred --play-arg=--gpu-live-blit \
    --play-arg=--present --play-arg=window \
    --play-arg=--pad-source --play-arg=keyboard \
    --play-arg=--pad-source --play-arg=gamepad \
    --play-arg=--xonline-offline \
    --play-arg=--hdd --play-arg=./tmp/hdds/profile-hdd \
    --play-arg=--log-file --play-arg=$outdir/play.log
or echo "session ended early, see $outdir/play.log (copy the 'unimplemented instruction' / 'STOP' line)"

echo "== analyse =="
python -m tools.action_census_diff $outdir
python -m tools.ai_attribution $outdir --require-ai
or echo "NO ai_* function above the idle control: did the enemies see you? Send the folder anyway."
grep -n -E "unimplemented instruction|^STOP" $outdir/play.log | tail -3
echo "done: tell Claude the session finished ($outdir) and where you met the first hostile enemies"
