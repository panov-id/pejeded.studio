# pejeded.studio#0008 — steps
Mark one with `issue step pejeded.studio#0008 <N> "<evidence>"`. `issue status pejeded.studio#0008 review` is refused while a step is open,
while «Объём» is empty or while ACCEPTANCE has an empty criterion row (owner 20.11.2026: «Каждая задача должна
собдержать несколько проходов линз, правки и такдалее до полной приемки»).
- [x] 1. scope written in TASK_RU/EN «Объём»/«Scope» — 2026-11-20 12:57 · Scope filled in TASK_RU/EN: stage 1 criteria, 30 min soak, macOS build
- [x] 2. change made (files named) — 2026-11-20 12:57 · files: tests/host_soak.sh, .github/workflows/macos.yml, third_party/sqlite/VERSION.txt, tests/no_std_header_shadow.sh, CMakeLists.txt
- [x] 3. test for the change, run after the last edit — 2026-11-20 12:57 · ctest 52 of 52 after last edit; soak PASS 1807 s x2; no_std_header_shadow fails on VERSION mutant
- [ ] 4. mutant turned the test red (how many failed, restored)
- [ ] 5. lens pass 1 (findings by level)
- [ ] 6. fixes for lens pass 1, with tests
- [ ] 7. lens pass 2 or later: no HIGH/MEDIUM
- [ ] 8. full suite green after the last edit, claim [#N] verified
- [ ] 9. gap-auditor after the last change, its list in comments.md
- [ ] 10. acceptance criteria filled in ACCEPTANCE_RU/EN
