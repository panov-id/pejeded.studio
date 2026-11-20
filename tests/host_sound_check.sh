#!/usr/bin/env bash
# Laptop check, run on the machine (not in the container: it needs a real sound device and screen).
# Spec 19, stage 1: layers slide without stopping the sound; output opens at 48 kHz, block 256.
# Usage: tests/host_sound_check.sh [path/to/PejededStudio]
set -u
app="${1:-./build/PejededStudio}"
home="$(mktemp -d)"
fail=0
x="press Go; wait 1000"
for i in 1 2 3 4 5 6 7 8 9 10; do x="$x; key tilde; wait 200; settings Devices; wait 200; key esc; wait 200"; done
out="$(HOME="$home" timeout 60 "$app" --exec "$x; key tilde; wait 300; key type n 1 ~ x; layers; status" 2>&1)"
if grep -q "no audio output open" <<<"$out"; then echo "SKIP: no sound device open, nothing to check here"; exit 77; fi
# 48 kHz: the laptop and DJ-202 offer it; a device without 48 kHz opens at its own rate and fails only this line
check() { if grep -qE -- "$2" <<<"$out"; then echo "ok: $1"; else echo "MISSING: $1"; fail=1; fi; }
check "sound device open" "rate [0-9]+ Hz, block"
check "48 kHz" "rate 48000 Hz"
check "no audio drops while layers slide (10 cycles)" "audio drops: 0$"
check "typing lands in the console line" "console line 'n 1 ~ x'"
check "console still open after typing" "layers: .*console"
# device names are read from this machine's own list, not written in for one laptop
list="$(HOME="$home" timeout 30 "$app" --exec devices 2>&1 | sed -n 's/^.*[A-Za-z]* out: //p')"
phonesdev="$(sed -n 2p <<<"$list")"
hdmi="$(grep -m1 HDMI <<<"$list")"
skipped=0
[ -n "$list" ] || { echo "MISSING: the devices command listed no outputs"; fail=1; }
# Phones on its own device (spec 6): opens, carries the test tone, goes back to Master without a restart
if [ -z "$phonesdev" ]; then echo "SKIP: only one output here, Phones cannot get its own device"; skipped=1; else
out="$(HOME="$home" timeout 60 "$app" --exec "press Go; set audioOut.phones.device \"$phonesdev\"; wait 1000; wizard 3; press Play test tone; wait 300; status; press Close; set audioOut.phones.device \"\"; wait 800; status" 2>&1)"
check "Phones opens on its own device" "Phones output: .*\(own device\), 48000 Hz"
check "test tone reaches Phones" "phones out: .*level -1[0-9]\.[0-9] dB"
check "Phones back on Master without restart" "Phones output: same device as Master"
check "no warning when Phones is cleared" "audioOut.phones.device = $"
fi
# Master to HDMI and back to the system default without a restart (spec 19, stage 1)
out="$(HOME="$home" timeout 60 "$app" --exec "devices; press Go; set audioOut.master.device \"$hdmi\"; wait 1500; set audioOut.master.device \"\"; wait 2500; status" 2>&1)"
if [ -n "$hdmi" ]; then check "Master opens on HDMI" "Master output: $hdmi, 48000 Hz"
else echo "SKIP: no HDMI output on this machine"; skipped=1; fi
check "Master back on the system default" "Master output: [^!]*48000 Hz, block 256$"
check "sound device open after the switch back" "rate 48000 Hz, block"
rm -rf "$home"
# 77 = something was skipped: not a pass
[ "$fail" = 0 ] && [ "$skipped" = 1 ] && exit 77
exit "$fail"
