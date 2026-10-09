#!/usr/bin/env fish
# T1763 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_save_reload.fish tmp/run_save_reload.fish
# then: fish tmp/run_save_reload.fish          (DRY_RUN=1 prints the plan and the commands, starts nothing)
# What you have to do is printed from tools/data/owner_scenarios/run_save_reload.json.
# Two launches of tools.private_host play on ONE persistent --hdd (tmp/hdds/save-reload-hdd, a copy of profile-hdd made once).
# The host logs NtCreateFile/NtWriteFile/NtFlushBuffersFile/NtReadFile on the hard disk files to --log-file; the receipt
# (tools.save_reload_receipt, docs/t-data-save-reload.md) lists path, size, sha256 before and after launch 2 and the verdict.
cd (dirname (status filename))/..; or exit 1

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l outdir tmp/owner-profiles/save-reload-$stamp
set -l hdd ./tmp/hdds/save-reload-hdd
set -l dry (test "$DRY_RUN" = 1; and echo 1; or echo 0)
set -l playargs --disc "$disc" --window --interactive --console --gpu-live --gpu-live-inferred --gpu-live-blit \
    --present window --pad-source keyboard --pad-source gamepad --xonline-offline --hdd $hdd

echo "Session: save-reload-$stamp"
python -m tools.owner_scenario show run_save_reload
or echo "WARNING (T1767): no owner scenario text for run_save_reload."

if test $dry = 1
    echo "DRY_RUN: would build the host, copy tmp/hdds/profile-hdd to $hdd if absent, then run twice:"
    echo "  python -m tools.private_host play -- $playargs --log-file $outdir/play1.log   (launch 1)"
    echo "  python -m tools.private_host --no-build play -- $playargs --log-file $outdir/play2.log   (launch 2)"
    echo "  with python -m tools.save_reload_receipt snapshot after each, then receipt into $outdir/receipt.txt"
    python -m tools.save_reload_receipt --help >/dev/null; or exit 1
    python -m tools.private_host play --help >/dev/null; or exit 1
    exit 0
end

echo "== build the private host (re-lifts if needed, a few minutes) =="
python -m tools.private_host build; or exit 1
mkdir -p $outdir
if not test -d $hdd
    cp -a ./tmp/hdds/profile-hdd $hdd; or exit 1
    echo "copied tmp/hdds/profile-hdd to $hdd (your original is untouched)"
end
python -m tools.save_reload_receipt snapshot --hdd $hdd --out $outdir/before1.json >/dev/null

read -l -P "Enter = start launch 1, q = quit: " answer
test "$answer" = q; and exit 0
python -m tools.private_host --no-build play -- $playargs --log-file $outdir/play1.log
or echo "launch 1 ended with an error, see $outdir/play1.log (copy the 'unimplemented instruction' / 'STOP' line)"
echo "== disk after launch 1 =="
python -m tools.save_reload_receipt snapshot --hdd $hdd --out $outdir/after1.json

read -l -P "Enter = start launch 2 (load the save), q = quit: " answer
test "$answer" = q; and exit 0
python -m tools.save_reload_receipt snapshot --hdd $hdd --out $outdir/before2.json >/dev/null
python -m tools.private_host --no-build play -- $playargs --log-file $outdir/play2.log
or echo "launch 2 ended with an error, see $outdir/play2.log"
python -m tools.save_reload_receipt snapshot --hdd $hdd --out $outdir/after2.json >/dev/null

echo "== receipt =="
python -m tools.save_reload_receipt receipt --after1 $outdir/after1.json --before2 $outdir/before2.json \
    --after2 $outdir/after2.json --log1 $outdir/play1.log --log2 $outdir/play2.log --json $outdir/receipt.json | tee $outdir/receipt.txt
echo "done: tell Claude the session finished ($outdir) and send receipt.txt, play1.log, play2.log"
