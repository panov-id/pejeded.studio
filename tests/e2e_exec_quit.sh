#!/usr/bin/env bash
# End-to-end: --exec "wait MS" really waits longer than the old 10 s cap (the soak needs 30 min),
# and the app writes why it quit ("quit: --exec finished") into the events table.
set -u
app="$1"
home="$(mktemp -d)"
trap 'rm -rf -- "$home"' EXIT
fail=0
xr=""; command -v xvfb-run >/dev/null && xr="xvfb-run -a"
# the app must never open on the owner's desktop: on Linux these checks run only under xvfb (the test container)
if [ -z "$xr" ] && [ "$(uname)" = Linux ]; then echo "FAIL: no xvfb-run here; run in the container (docker compose run --rm test), not on the desktop"; exit 1; fi
to=""; command -v timeout >/dev/null && to="timeout 90"

t0=$(date +%s)
# startup and the final log add about 3 s, so the old 10 s cap ends near 13 s; 20 s proves it is gone
HOME="$home" $to $xr "$app" --exec "wait 20000; status" >/dev/null 2>&1
t1=$(date +%s)
if [ $((t1 - t0)) -ge 20 ]; then echo "ok: wait 20000 waited $((t1 - t0)) s"; else echo "MISSING: wait 20000 waited only $((t1 - t0)) s"; fail=1; fi

out="$(HOME="$home" $to $xr "$app" --exec "sql SELECT data FROM events WHERE kind='app' ORDER BY id" 2>&1)"
if grep -qF "quit: --exec finished" <<<"$out"; then echo "ok: quit reason --exec finished is in events"; else echo "MISSING: quit reason in events"; grep -i quit <<<"$out"; fail=1; fi
exit "$fail"
