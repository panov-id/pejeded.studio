#!/bin/bash
# Test of pejeded-0001-0003.sh. The set of check ids is read from the script itself.
# 1) with archive: every id PASS, rc 0; 2) no unpacked archive: archive-bound ids SKIP, rest PASS, rc 0;
# 3) one forced FAIL: rc 1.
set -u
D="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$D/../.." && pwd)"; S="$D/pejeded-0001-0003.sh"
ids=$(grep -oE '^(r|a|if .* r) 0[0-9]{3}\.[0-9]+|r 0[0-9]{3}\.[0-9]+' "$S" | grep -oE '0[0-9]{3}\.[0-9]+' | sort -u)
arch=$( { grep -oE '^a 0[0-9]{3}\.[0-9]+' "$S"; grep -oE 'SKIP 0[0-9]{3}\.[0-9]+' "$S"; } | grep -oE '0[0-9]{3}\.[0-9]+' | sort -u)
M=$(echo "$ids" | wc -l); A=$(echo "$arch" | wc -l); ok=0; bad=0
t(){ if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
out=$("$S"); rc=$?
t "with archive: rc 0" "[ $rc -eq 0 ]"
t "with archive: $M of $M ids PASS" "[ \$(echo \"\$out\" | grep -c '^PASS') -eq $M ]"
for i in $ids; do t "id $i reported" "echo \"\$out\" | grep -q \" $i\"" ; done
out=$(STARTER_DIR=/nonexistent-starter "$S"); rc=$?
t "no unpacked: rc 0" "[ $rc -eq 0 ]"
t "no unpacked: $((A-1)) archive-bound SKIP" "[ \$(echo \"\$out\" | grep -c '^SKIP') -eq $((A-1)) ]"
m=$(mktemp); sed 's|^echo "TOTAL|r MUT "false"\necho "TOTAL|' "$S" > "$m"
ACCEPT_ROOT="$R" bash "$m" >/dev/null; rc=$?; rm -f "$m"
t "forced FAIL: rc 1" "[ $rc -eq 1 ]"
echo "TEST ok=$ok bad=$bad; ids covered $M of $M"
[ $bad -eq 0 ]
