# pejeded.studio#0001 — acceptance

## Criteria
| # | Criterion | Check (command, test, shot) | Result |
|---|---|---|---|
| 1 | CLAUDE.md, README.md, LICENSE equal archive | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.1 | PASS |
| 2 | docs/spec, archive, prototype, environment.md equal archive | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.2 | PASS |
| 3 | .gitignore = archive + /tmp/ line | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.3 | PASS |
| 4 | archive in tmp/, not in git | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.4 | PASS |
| 5 | tmp/ ignored, 0 staged files from tmp/ | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.5 | PASS |
| 6 | START-HERE.md and first-message.txt in docs/ (presence, not read-through) | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.6 | PASS |

## Acceptance (as the customer)
Asked: see ACCEPTANCE_RU.md (owner words verbatim)
Got: 33 files in root, 2 in docs/, archive in tmp/ outside git; 6 of 6 checks PASS
Missing: owner acceptance (commit 1b92048 pushed to origin/main)
Verdict: would return — 6 of 6 checks PASS, but status accepted is owner-only (rule 37); acceptance delegated by owner 19.11.2026
