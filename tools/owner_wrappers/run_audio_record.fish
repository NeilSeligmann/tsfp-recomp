#!/usr/bin/env fish
# T1731 tracked TEMPLATE of the owner wrapper (tmp/ is gitignored). Install once from the repo root:
#   cp tools/owner_wrappers/run_audio_record.fish tmp/run_audio_record.fish
# then: fish tmp/run_audio_record.fish          (DRY_RUN=1 prints the plan and the commands, starts nothing)
#       LISTEN=1 fish tmp/run_audio_record.fish  (real audio device, you hear it, nothing is captured)
# Default: the SDL "disk" audio driver. It writes everything the game sends to its audio device to audio.raw
# (48 kHz stereo S16 LE) at the device clock rate, so you hear NOTHING in this mode. After the game window is closed,
# python -m tools.audio_dropout_report analyses audio.raw and run.log. docs/t-engine-audio-record.md.
cd (dirname (status filename))/..; or exit 1

set -l disc "./tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso"
set -l stamp (date +%Y%m%d-%H%M%S)
set -l outdir tmp/owner-profiles/audio-record-$stamp
set -l dry (test "$DRY_RUN" = 1; and echo 1; or echo 0)
set -l listen (test "$LISTEN" = 1; and echo 1; or echo 0)
set -l transcribe (test "$TRANSCRIBE" = 1; and echo 1; or echo 0)
set -l skip (test -n "$SKIP_S"; and echo $SKIP_S; or echo 5)
set -l playargs --disc "$disc" --window --interactive --console --gpu-live --gpu-live-inferred --gpu-live-blit \
    --present window --pad-source keyboard --pad-source gamepad --xonline-offline --audio-sink sdl --log-file $outdir/run.log

echo "Session: audio-record-$stamp (listen mode: $listen)"
python -m tools.owner_scenario show run_audio_record
or echo "WARNING (T1767): no owner scenario text for run_audio_record."

if test $dry = 1
    echo "DRY_RUN: would build the host, then run:"
    if test $listen = 0
        echo "  SDL_AUDIODRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=$outdir/audio.raw SDL_AUDIO_DISK_TIMESCALE=1.0 \\"
    end
    echo "  python -m tools.private_host --no-build play -- $playargs"
    echo "  python -m tools.audio_dropout_report $outdir/audio.raw --skip-s $skip --log $outdir/run.log --json $outdir/dropout_report.json | tee $outdir/dropout_report.txt"
    if test $transcribe = 1
        echo "  python -m tools.audio_transcribe $outdir/audio.raw --out-dir $outdir/transcription --model-dir $TRANSCRIBE_MODEL_DIR"
    end
    python -m tools.audio_dropout_report --control-tone >/dev/null; or exit 1
    python -m tools.private_host play --help >/dev/null; or exit 1
    exit 0
end

echo "== build the private host (re-lifts if needed, a few minutes) =="
python -m tools.private_host build; or exit 1
mkdir -p $outdir

read -l -P "Enter = start the game, q = quit: " answer
test "$answer" = q; and exit 0
if test $listen = 1
    python -m tools.private_host --no-build play -- $playargs
else
    env SDL_AUDIODRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=$outdir/audio.raw SDL_AUDIO_DISK_TIMESCALE=1.0 \
        python -m tools.private_host --no-build play -- $playargs
end
or echo "the game ended with an error, see $outdir/run.log (copy the 'unimplemented instruction' / 'STOP' line)"

if test $listen = 1
    echo "== LISTEN mode: nothing captured. The host's audio log lines =="
    grep -E "audio (clock|stall|cutouts|latency)" $outdir/run.log | tee $outdir/audio_lines.txt
else
    if test -f $outdir/audio.raw
        echo "== dropout report (first $skip s skipped) =="
        python -m tools.audio_dropout_report $outdir/audio.raw --skip-s $skip --log $outdir/run.log \
            --json $outdir/dropout_report.json | tee $outdir/dropout_report.txt
        ls -l $outdir/audio.raw
        if test $transcribe = 1
            python -m tools.audio_transcribe $outdir/audio.raw --out-dir $outdir/transcription \
                --model-dir "$TRANSCRIBE_MODEL_DIR"
            or exit 1
        end
    else
        echo "no audio.raw was written (the game did not start or ended before audio opened): see $outdir/run.log"
        exit 1
    end
end
echo "audio.raw : $outdir/audio.raw"
echo "run.log   : $outdir/run.log"
echo "report    : $outdir/dropout_report.txt (json: $outdir/dropout_report.json)"
echo "done: send audio.raw (or its size and sha256 if large), run.log, the report and your notes (docs/t-engine-audio-record.md)"
