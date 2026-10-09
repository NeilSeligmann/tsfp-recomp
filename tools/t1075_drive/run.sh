#!/bin/sh
# usage: run.sh NAME TIMEOUT_S [extra host args...]   (repo root; relative paths)
N=$1; T=$2; shift 2
R=tmp/t1075/$N; rm -rf $R; mkdir -p $R/hdd $R/overlay $R/dump
export SDL_AUDIODRIVER=dummy VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json XDG_RUNTIME_DIR=$(pwd)/tmp/xdg; mkdir -p -m 700 tmp/xdg
DISPLAY=${XD:-:97} timeout --kill-after=15 $T ${HOST:-tmp/private-host/build-b7de1f2885bb336e/tsfp_host} build/default.xbe --hdd $R/hdd --xonline-offline --skip-intro --ac97-ready --headless-streams --headless-buffers --headless-listener --headless-second-vblank --native-shader-assembler --native-xmv --headless-movie-audio --couple-vblank-effects --check-vblank-quiescence --overlay-consume --vblank-owner-waits 1000 --vblank-worker-blanks 100 --thread-timeout 2147483647 --gpu-live --gpu-live-inferred --dump-overlay $R/overlay --interactive --present window --present-hold-ms 0 --audio-sink sdl --audio-mute --synthetic-pad --route-event-log $R/route.log --route-log-mem 0x79094C --replay-input tmp/recorded-input-story-mode --gpu-replay tmp/play/modules --gpu-replay-lenient --gpu-live-translate --census-icalls --census-phases $R/census.txt --disc "tmp/TimeSplitters - Future Perfect (USA) (XBOX).iso" "$@" > $R/out.txt 2> $R/err.txt
echo exit $? >> $R/out.txt
