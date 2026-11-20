# pejeded.studio#0008 — comments

One entry per date: decisions, owner words verbatim, findings.

## 2026-11-19 18:09 · claude
done: Docker image (trixie), compose dev/test, JUCE 8.0.15 + Catch2 v3.16.1 submodules, window 'PEJEDED STUDIO' opens on host (xwininfo), ctest 1 of 1, 0 warnings; macos.yml written, runs only after push

## 2026-11-20 10:27 · claude
Control soak SOAK_NO_INHIBIT=1 PASS: ran 1807 s, drops 0, growth 0.0 %, 30 samples, quit by --exec finished (~/pejeded-soak/20261120-095710). Narrowed refusal check ran live: 0 refusals. Window close after ~10 min did not recur in 2 runs of 30 min; cause not found.

## 2026-11-20 12:57 · claude
step 1 closed: Scope filled in TASK_RU/EN: stage 1 criteria, 30 min soak, macOS build

## 2026-11-20 12:57 · claude
step 2 closed: files: tests/host_soak.sh, .github/workflows/macos.yml, third_party/sqlite/VERSION.txt, tests/no_std_header_shadow.sh, CMakeLists.txt

## 2026-11-20 12:57 · claude
step 3 closed: ctest 52 of 52 after last edit; soak PASS 1807 s x2; no_std_header_shadow fails on VERSION mutant
