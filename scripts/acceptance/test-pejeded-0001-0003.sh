#!/bin/bash
# Test of pejeded-0001-0003.sh. The set of check ids is read from the script itself.
# 1) with archive: every id PASS, rc 0; 2) no unpacked archive: archive-bound ids SKIP, rest PASS, rc 0;
# 3) one forced FAIL: rc 1.
set -u
D="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$D/../.." && pwd)"; S="$D/pejeded-0001-0003.sh"
ids=$(grep -oE '^(r|a|if .* r) 0[0-9]{3}\.[0-9]+|r 0[0-9]{3}\.[0-9]+' "$S" | grep -oE '0[0-9]{3}\.[0-9]+' | sort -u)
arch=$(grep -oE '^a 0[0-9]{3}\.[0-9]+' "$S" | grep -oE '0[0-9]{3}\.[0-9]+' | sort -u)
M=$(echo "$ids" | wc -l); A=$(echo "$arch" | wc -l); ok=0; bad=0
t(){ if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
out=$("$S"); rc=$?
t "with archive: rc 0" "[ $rc -eq 0 ]"
if [ -d "$R/tmp/pejeded-studio-starter" ]; then E=$M; else E=$((M-A)); echo "note: no archive in tmp/, $A archive-bound ids expected SKIP"; fi
t "PASS $E of $M ids" "[ \$(echo \"\$out\" | grep -c '^PASS') -eq $E ]"
t "no FAIL" "! echo \"\$out\" | grep -q '^FAIL'"
for i in $ids; do t "id $i reported" "echo \"\$out\" | grep -q \" $i\"" ; done
out=$(STARTER_DIR=/nonexistent-starter "$S"); rc=$?
t "no unpacked: rc 0" "[ $rc -eq 0 ]"
t "no unpacked: $A archive-bound SKIP" "[ \$(echo \"\$out\" | grep -c '^SKIP') -eq $A ]"
m=$(mktemp); sed 's|^echo "TOTAL|r MUT "false"\necho "TOTAL|' "$S" > "$m"
ACCEPT_ROOT="$R" bash "$m" >/dev/null; rc=$?; rm -f "$m"
t "forced FAIL: rc 1" "[ $rc -eq 1 ]"
# 4) missing file must FAIL its negative checks, not PASS (review-panel 19.11)
w=$(mktemp -d) && [ -d "$w" ] || { echo "mktemp failed"; exit 2; }; trap 'rm -rf "$w"' EXIT
[ -d "$R/.git" ] || { echo "repo .git is not a directory (worktree?): copy would share its index"; exit 2; }
cp -a "$R/." "$w/"
rm "$w/docs/START-HERE.md" "$w/docs/first-message.txt"
out=$(ACCEPT_ROOT="$w" "$S")
t "missing START-HERE: 0002.1 FAIL" "echo \"\$out\" | grep -q '^FAIL 0002.1'"
t "missing first-message: 0003.1 FAIL" "echo \"\$out\" | grep -q '^FAIL 0003.1'"
rm -rf "$w/.git"; out=$(ACCEPT_ROOT="$w" "$S")
t "no .git: 0001.4 FAIL" "echo \"\$out\" | grep -q '^FAIL 0001.4'"
rm -rf "$w"
# 5) panel 19.11 N2-N4: archive in index, empty first-message, tracked *.db must FAIL
rm -rf "$w"; w=$(mktemp -d) && [ -d "$w" ] || { echo "mktemp failed"; exit 2; }; cp -a "$R/." "$w/"
if [ -d "$w/tmp/pejeded-studio-starter" ]; then
: > "$w/docs/first-message.txt"; : > "$w/tmp/pejeded-studio-starter/first-message.txt"
out=$(ACCEPT_ROOT="$w" "$S"); t "both first-message empty: 0003.3 FAIL" "echo \"\$out\" | grep -q '^FAIL 0003.3'"
fi
mkdir -p "$w/tmp"; touch "$w/tmp/pejeded-studio-starter.tar.gz"; git -C "$w" add -f tmp/pejeded-studio-starter.tar.gz; rm -f "$w/tmp/pejeded-studio-starter.tar.gz"
out=$(ACCEPT_ROOT="$w" "$S"); t "archive in index, gone from disk: 0001.4 FAIL" "echo \"\$out\" | grep -q '^FAIL 0001.4'"
touch "$w/docs/x.db"; git -C "$w" add -f docs/x.db
out=$(ACCEPT_ROOT="$w" "$S"); t "tracked *.db: 0001.7 FAIL" "echo \"\$out\" | grep -q '^FAIL 0001.7'"
rm -rf "$w"
echo "TEST ok=$ok bad=$bad; ids covered $M of $M"
[ $bad -eq 0 ]
