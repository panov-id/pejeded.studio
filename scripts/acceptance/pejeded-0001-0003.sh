#!/bin/bash
# Acceptance checks for pejeded.studio#0001-0003. Run from anywhere.
# Checks against the starter archive need it in tmp/ and unpacked:
#   tar -xzf tmp/pejeded-studio-starter.tar.gz -C tmp/
# Without it those checks print SKIP, not FAIL.
cd "${ACCEPT_ROOT:-$(dirname "$0")/../..}" || { echo "cannot cd to repo root"; exit 2; }
S="${STARTER_DIR:-tmp/pejeded-studio-starter}"
case "$S" in *[!A-Za-z0-9/._-]*) echo "STARTER_DIR: only letters, digits, / . _ -"; exit 2;; esac
P=0; F=0; K=0
r(){ if bash -c "$2" >/dev/null 2>&1; then echo "PASS $1"; P=$((P+1)); else echo "FAIL $1"; F=$((F+1)); fi; }
a(){ if [ -d "$S" ]; then r "$@"; else echo "SKIP $1 (no $S)"; K=$((K+1)); fi; }
a 0001.1 "for f in CLAUDE.md README.md LICENSE; do cmp $S/pejeded-studio/\$f \$f || exit 1; done"
a 0001.2 "diff -r $S/pejeded-studio/docs/spec docs/spec && diff -r $S/pejeded-studio/docs/archive docs/archive && diff -r $S/pejeded-studio/docs/prototype docs/prototype && cmp $S/pejeded-studio/docs/environment.md docs/environment.md"
a 0001.3 "diff <(cat $S/pejeded-studio/.gitignore) <(head -n \$(wc -l < $S/pejeded-studio/.gitignore) .gitignore)"
r 0001.4 "git rev-parse --git-dir && ! git ls-files --error-unmatch tmp/pejeded-studio-starter.tar.gz"
r 0001.5 "git check-ignore -q tmp/x && test -z \"\$(git ls-files tmp/)\""
r 0001.7 "git rev-parse --git-dir && test -z \"\$(git ls-files '*.db' '*.sqlite*')\""
r 0001.6 "test -f docs/START-HERE.md && test -f docs/first-message.txt"
r 0002.1 "test -f docs/START-HERE.md && ! grep -q -e 'Положи папку' -e 'будущий репозиторий' docs/START-HERE.md"
r 0002.2 "grep -q 'docs/first-message.txt' docs/START-HERE.md && grep -q 'git clone' docs/START-HERE.md"
r 0003.1 "test -f docs/first-message.txt && ! grep -q 'Репозитория на GitHub ещё нет' docs/first-message.txt"
r 0003.2 "grep -q 'panov-id/pejeded.studio' docs/first-message.txt"
a 0003.3 "test -s docs/first-message.txt && diff <(sed '12d' $S/first-message.txt) <(sed '12d' docs/first-message.txt)"
echo "TOTAL pass=$P fail=$F skip=$K (skip = not run)"
[ "$F" -eq 0 ]
