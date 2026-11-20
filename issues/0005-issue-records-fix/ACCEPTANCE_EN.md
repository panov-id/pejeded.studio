# pejeded.studio#0005 — acceptance

## Criteria
| # | Criterion | Check (command, test, shot) | Result |
|---|---|---|---|
| 1 | no stale phrases | grep stale phrases → 0 |  |
| 2 | Cyrillic in EN only inside «» | `python3 -c "import re,glob;print(sum(len(re.findall(r'[А-Яа-яЁё]+',re.sub(r'«[^»]*»','',open(f).read()))) for f in glob.glob('issues/000[123]-*/*_EN.md')))"` → 0 |  |

## Acceptance (as the customer)
Asked:
Got:
Missing:
Verdict:
