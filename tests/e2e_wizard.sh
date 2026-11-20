#!/usr/bin/env bash
# End-to-end: first run opens Setup wizard; the six steps are walked by console commands that click
# the same buttons a person clicks; Finish opens Start check; Fix opens the step; settings are saved.
# Runs the real app under a virtual screen (xvfb-run) with a throwaway HOME.
set -u
app="$1"
home="$(mktemp -d)"
trap 'rm -rf -- "$home"' EXIT
mkdir -p "$home/tracks"
fail=0
# Linux container: no screen, so xvfb-run; macOS runner has a screen and no xvfb-run
xr=""; command -v xvfb-run >/dev/null && xr="xvfb-run -a"
to=""; command -v timeout >/dev/null && to="timeout 60"   # macOS has no timeout
cfgdir="$home/.config/pejeded-studio"; [ "$(uname)" = Darwin ] && cfgdir="$home/Library/Application Support/PEJEDED STUDIO"
expect() { if grep -qF -- "$2" <<<"$1"; then echo "ok: $2"; else echo "MISSING: $2"; fail=1; fi; }

out="$(HOME="$home" $to $xr "$app" --exec "wait 500; press Next; press Next; press Play test tone; press Next; type $home/tracks; press Next; type $home/rec; press Test write; press Next; press Finish; wait 300; press Fix; press Skip" 2>&1)"

expect "$out" "new file with defaults"
expect "$out" "devices.main = MC-707 (emulator)"
expect "$out" "devices.controller = DJ-202 (emulator)"
expect "$out" "test tone: 1 s, 440 Hz"
expect "$out" "tracks.folder = $home/tracks"
expect "$out" "record.folder = $home/rec"
expect "$out" "test write ok"
expect "$out" "finished at step 6"
expect "$out" "wizard   opened at step"
if grep -q "press: no button" <<<"$out"; then echo "MISSING: every press found its button"; grep "press: no button" <<<"$out"; fail=1; else echo "ok: every press found its button"; fi
cfg="$cfgdir/settings.json"
if grep -qF "\"$home/tracks\"" "$cfg" && grep -qF "\"$home/rec\"" "$cfg"; then echo "ok: folders saved in settings.json"; else echo "MISSING: folders in settings.json"; fail=1; fi
[ -d "$home/rec" ] && echo "ok: recording folder made" || { echo "MISSING: recording folder"; fail=1; }

# second start: settings exist, so the wizard does not open; Start check does
out2="$(HOME="$home" $to $xr "$app" --exec "wait 500; press Finish; press Go; press Settings; press Close; press Setup wizard" 2>&1)"
expect "$out2" "press: no button Finish"
expect "$out2" "settings   opened tab 1"   # top bar button
expect "$out2" "wizard   opened at step 1"  # top bar button
# Settings: a change is saved at once; Reset asks again, then keeps the old file
out3="$(HOME="$home" $to $xr "$app" --exec "wait 500; press Go; settings Audio out; choose Block size = 512; press Maintenance; press Reset settings" 2>&1)"
expect "$out3" "settings   opened tab 2"
expect "$out3" "audioOut.blockSize = 512"
grep -q '"blockSize": 512' "$cfg" && echo "ok: block size saved" || { echo "MISSING: block size 512 in settings.json"; fail=1; }
[ ! -e "$cfgdir/settings.before-reset.json" ] && echo "ok: first Reset press only asks" || { echo "MISSING: Reset waited for Confirm"; fail=1; }
out4="$(HOME="$home" $to $xr "$app" --exec "wait 500; press Go; settings Maintenance; press Reset settings; press Confirm" 2>&1)"
expect "$out4" "settings reset to defaults"
grep -q '"blockSize": 512' "$cfgdir/settings.before-reset.json" && echo "ok: old settings kept" || { echo "MISSING: old settings copy"; fail=1; }
grep -q '"blockSize": 256' "$cfg" && echo "ok: defaults back" || { echo "MISSING: default block size"; fail=1; }
# layers: Esc closes Settings first, the console under it stays; the next Esc closes the console
out5="$(HOME="$home" $to $xr "$app" --exec "wait 500; press Go; key tilde; press Settings; layers; key esc; layers; key esc; layers" 2>&1)"
expect "$out5" "layers: console settings"
grep -q "^console   layers: console$" <<<"$out5" && echo "ok: first Esc closed only Settings" || { echo "MISSING: first Esc closed only Settings"; fail=1; }
grep -q "^console   layers: none$" <<<"$out5" && echo "ok: second Esc closed the console" || { echo "MISSING: second Esc closed the console"; fail=1; }
out6="$(HOME="$home" $to $xr "$app" --exec "wait 1500; bar" 2>&1)"
expect "$out6" "BPM  MC-707 emulator  via USB"   # bottom bar: tempo, source, path
expect "$out6" "| Master "                      # bottom bar: Master meter
# Master path: at 0 dB the default emulator pattern makes the limiter work (CLIP); at -12 dB it does not
out7="$(HOME="$home" $to $xr "$app" --exec "wait 2500; bar; set audioOut.masterGainDb -12; wait 3500; bar; set audioOut.masterGainDb 0" 2>&1)"
if grep -q 'Master output: $' <<<"$out7"; then
  echo "SKIP: no sound device here (container); the Master path check runs on the machine"
else
  [ "$(grep -c 'bar: .* CLIP' <<<"$out7")" = 1 ] && echo "ok: limiter on the output, Master gain applied" || { echo "MISSING: CLIP at 0 dB and none at -12 dB"; fail=1; }
fi
# device loss (spec 6, stage 1): the Master output is lost, warned about, and back without a restart
out8="$(HOME="$home" $to $xr "$app" --exec "wait 2000; lose master 2000; wait 1500; bar; wait 3500; bar" 2>&1)"
if grep -q 'Master output: $' <<<"$out8"; then
  echo "SKIP: no sound device here (container); the device loss check runs on the machine"
else
  expect "$out8" "Master output: running -> lost"
  expect "$out8" "| Master output: lost, trying again every second"
  expect "$out8" "Master output: opening -> running"
fi
# input loss: works in the container too (the emulator needs no sound device)
out9="$(HOME="$home" $to $xr "$app" --exec "wait 2000; lose input 2000; wait 1500; bar; wait 3000; bar" 2>&1)"
expect "$out9" "Sound in: running -> lost"
expect "$out9" "Sound in: lost, trying again every second"   # in the bar, with any other lost device
expect "$out9" "Sound in: opening -> running"
out10="$(HOME="$home" $to $xr "$app" --exec "midi" 2>&1)"
grep -qE "midi: [0-9]+ in, [0-9]+ out" <<<"$out10" && echo "ok: midi ports listed" || { echo "MISSING: midi ports listed"; fail=1; }
# the spec 15 status command is still there (devices, rate, block), not shadowed by bar
out11="$(HOME="$home" $to $xr "$app" --exec "status" 2>&1)"
expect "$out11" "main input: MC-707 (emulator)"   # spec 15 status, not the bar text (no sound device here, so no rate line)
# stage 1 console commands (spec 19, stage 1): each answers
out12="$(HOME="$home" $to $xr "$app" --exec "get audioOut.blockSize; devices; log 3; db; db backup; sql select count(*) from events; !echo sys-ok; wait 1500; reconnect" 2>&1)"
expect "$out12" "audioOut.blockSize = 256"
expect "$out12" "MC-707 emulator (built in)"
expect "$out12" "console > log 3"
expect "$out12" "tracks: 0 rows"
expect "$out12" "backups/studio-"
expect "$out12" "count(*)"
expect "$out12" "system   sys-ok"
expect "$out12" "> reconnect"
out13="$(HOME="$home" $to $xr "$app" --exec "get audioOut.blockSize; clear; help" 2>&1)"
for c in help status devices reconnect get set log clear db sql; do expect "$out13" "console   $c  "; done
# typing in the console goes into the line, and the key under Esc typed there does not close it (spec 15)
out14="$(HOME="$home" $to $xr "$app" --exec "press Go; key tilde; wait 400; key type n 1 ~ x; wait 300; layers" 2>&1)"
expect "$out14" "key type: focus text field, console line 'n 1 ~ x'"
grep -q "layers: .*console" <<<"$out14" && echo "ok: console still open after typing" || { echo "MISSING: console still open after typing"; fail=1; }
[ "$fail" = 0 ] || { echo "--- app output"; echo "$out"; }
exit "$fail"
