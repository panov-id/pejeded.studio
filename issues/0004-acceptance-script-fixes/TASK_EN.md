# pejeded.studio#0004 — Acceptance script and test: fresh clone, index check, empty files, no *.db (N1-N4, C1-C3, S4, S5)

Status: see `status.log`. Links: `links.yml`. Log: `comments.md`.

## Owner's request (verbatim)
сначало проверяем что нашли линзы несколько раз, потом составляем план на работы, потом раскидываем на задачи

## Goal
Acceptance script and test: fresh clone, index check, empty files, no *.db (N1-N4, C1-C3, S4, S5)

## Scope
- N1: test passes in a fresh clone without the archive (expects SKIP, not PASS)
- N2: 0001.4 looks at the git index, not the disk
- N3: 0003.3 fails on empty files
- N4: check that no *.db is in the index
- C1-C3, S4, S5: fixes already staged since 17:39

## Out of scope
- 
