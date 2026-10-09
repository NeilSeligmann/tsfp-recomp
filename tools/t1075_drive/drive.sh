#!/bin/sh
# T1240: also "<t> sig LABEL" = SIGUSR1 to the host (guest dump + census phase, needs DUMP_RANGES via run.sh args)
# usage: drive.sh NAME TOTAL_S SCHEDULE_FILE [host args]   schedule lines: <t_s> shot LABEL | <t_s> down KEY | <t_s> up KEY
N=$1; T=$2; S=$3; shift 3
export DISPLAY=:97
Xvfb :97 -screen 0 1280x720x24 >/dev/null 2>&1 &
XP=$!
sleep 2
XD=:97 tools/t1075_drive/run.sh $N $T --pad-source keyboard --replay-handover "$@" >/dev/null 2>&1 &
sleep 1
START=$(date +%s)
while read -r t cmd arg; do
  case "$t" in \#*|"") continue;; esac
  now=$(( $(date +%s) - START )); [ "$t" -gt "$now" ] && sleep $(( t - now ))
  case "$cmd" in
    shot) import -window root tmp/t1075/$N/shot-$t-$arg.png;;
    down) W=$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1); xdotool windowfocus $W 2>/dev/null; xdotool keydown $arg;;
    up) xdotool keyup $arg;;
    sig) HP=$(pgrep -f "tsfp_host build/default.xbe --hdd tmp/t1075/$N/" | head -1); mkdir -p tmp/t1075/$N/dump
         echo $arg > tmp/t1075/$N/dump/guestdump.phase; kill -USR1 $HP;;
  esac
done < $S
wait %2 2>/dev/null; sleep $(( T + 20 - ($(date +%s) - START) )) 2>/dev/null
kill $XP
