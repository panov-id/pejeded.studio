#!/usr/bin/env bash
# Soak on the machine (spec 19 general rule): the app plays the emulator for N minutes (default 30).
# Pass: the app reaches the end of --exec ("quit: --exec finished"), audio drops 0, memory growth at most 5 %
# (owner's decision 20.11, spec 19:7 and 21).
# Memory growth is counted from minute 2, so a run needs at least 4 minutes (4 samples); each minute gives one sample.
# Usage: tests/host_soak.sh [minutes] [path/to/PejededStudio]; results go to $SOAK_DIR (default ~/pejeded-soak).
# SOAK_NO_INHIBIT=1 runs without systemd-inhibit: a control run that lets the laptop sleep or go idle.
set -u
min="${1:-30}"
app="${2:-./build/PejededStudio}"
dir="${SOAK_DIR:-$HOME/pejeded-soak}"
mkdir -p "$dir"
# a fresh home per run: the quit event read at the end can only come from this run
# memory is read by process name, so a second copy of the app would give wrong numbers
pgrep -x PejededStudio >/dev/null && { echo "SKIP: PejededStudio is already running; close it first"; exit 77; }
run="$dir/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$run"
home="$run/home"; mkdir -p "$home"
# both folders set, or Start check reports PROBLEM; the emulator plays without Go
mkdir -p "$run/tracks" "$run/rec"
# each run keeps its own log, memory samples and result; a later run never overwrites them
out="$run/out.txt"; rss="$run/rss.txt"; res="$run/result.txt"
: > "$rss"
inh=(systemd-inhibit --what=sleep:idle --why="pejeded soak")
[ "${SOAK_NO_INHIBIT:-0}" = 1 ] && inh=()
start=$(date +%T); t0=$(date +%s)
HOME="$home" "${inh[@]}" timeout $((min * 60 + 120)) \
  "$app" --exec "set tracks.folder \"$run/tracks\"; set record.folder \"$run/rec\"; wait $((min * 60000)); check; status" > "$out" 2>&1 &
t=$!
sleep 5; p=$(pgrep -n -x PejededStudio)
while kill -0 "$t" 2>/dev/null; do ps -o rss= -p "$p" >> "$rss" 2>/dev/null; sleep 60; done
wait "$t"; rc=$?
ran=$(( $(date +%s) - t0 ))
grep -q "no audio output open" "$out" && { echo "SKIP: no sound device open, a soak needs one"; exit 77; }
fail=0
{
  echo "start $start end $(date +%T) ran ${ran} s of $((min * 60)) s, inhibit $([ ${#inh[@]} = 0 ] && echo off || echo on), exit $rc"
  [ "$rc" = 0 ] || { echo "FAIL: exit code $rc (124 = killed by timeout)"; fail=1; }
  [ "$ran" -ge $((min * 60)) ] || { echo "FAIL: the app quit after $ran s, before the end"; fail=1; }
  # a refused command or a Start check problem means the run did not play what it claims
  # only the check typed at the end counts: the startup check runs before the folders are set
  fin="$(sed -n '/> check$/,$p' "$out")"
  [ -n "$fin" ] || { echo "FAIL: the final check never ran"; fail=1; }
  # free disk space is a fact of the machine; the soak records nothing (spec 19:7), so it is a warning
  grep "GB free" <<<"$fin" | sed 's/^/warn: /'
  if grep -v "GB free" <<<"$fin" | grep -E "no button|not chosen|PROBLEM|unknown command"; then echo "FAIL: a command was refused or Start check has a problem"; fail=1; fi
  # the line right after each typed start command is its answer; the 30 min log between is not read here
  pre="$(awk 'a{print; a=0} /^console   > (set|press) /{a=1}' "$out")"
  if grep -qE "no button|unknown|error|cannot" <<<"$pre"; then echo "FAIL: a start command was refused"; fail=1; fi
  if grep -q "ok .*levels move" <<<"$fin"; then echo "ok: emulator sound levels move"; else echo "FAIL: no 'levels move' row, sound not proven"; fail=1; fi
  if grep -q "audio drops: 0$" <<<"$fin"; then echo "ok: audio drops 0"; else echo "FAIL: audio drops not 0 or status never printed"; fail=1; fi
  # growth is counted from minute 2: the first minute is startup (buffers, fonts), not a leak
  first=$(sed -n 3p "$rss" | tr -d ' '); last=$(tail -1 "$rss" | tr -d ' '); n=$(wc -l < "$rss")
  [ "$n" -ge "$min" ] && [ "$n" -ge 4 ] || { echo "FAIL: $n memory samples for $min minutes"; fail=1; }
  if [ -n "$first" ] && [ -n "$last" ]; then
    g=$(( (last - first) * 1000 / first ))
    a=${g#-}; s=""; [ "$g" -lt 0 ] && s="-"
    echo "rss minute-2 $first kB last $last kB samples $n growth $s$((a / 10)).$((a % 10)) %"
    if [ "$g" -le 50 ]; then echo "ok: growth at most 5 %"; else echo "FAIL: growth over 5 %"; fail=1; fi
  else echo "FAIL: no memory samples"; fail=1; fi
  q="$(HOME="$home" timeout 60 "$app" --exec "sql SELECT data FROM events WHERE kind='app' AND data LIKE 'quit%' ORDER BY id DESC LIMIT 1" 2>&1 | grep -o 'quit: .*' | tail -1)"
  echo "last quit event: ${q:-none}"
  grep -q "quit: --exec finished" <<<"$q" || { echo "FAIL: the app did not quit by the end of --exec"; fail=1; }
  echo "result: $([ $fail = 0 ] && echo PASS || echo FAIL)"
} > "$res"
cat "$res"
# the per-run home holds only this run's throwaway studio.db; keep it on FAIL for reading
[ "$fail" = 0 ] && find "$home" -delete
exit "$fail"
