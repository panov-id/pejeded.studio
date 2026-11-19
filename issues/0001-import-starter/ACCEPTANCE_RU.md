# pejeded.studio#0001 — приёмка

## Критерии
| # | Критерий | Проверка (команда, тест, снимок) | Итог |
|---|---|---|---|
| 1 | CLAUDE.md, README.md, LICENSE равны архиву | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.1 | PASS |
| 2 | docs/spec, archive, prototype, environment.md равны архиву | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.2 | PASS |
| 3 | .gitignore = архив + строка /tmp/ | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.3 | PASS |
| 4 | архив в tmp/, не в git | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.4 | PASS |
| 5 | tmp/ игнорируется, в индексе 0 файлов из tmp/ | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.5 | PASS |
| 6 | START-HERE.md и first-message.txt в docs/ (наличие, не прочтение) | `scripts/acceptance/pejeded-0001-0003.sh` → 0001.6 | PASS |

## Приёмка (я — заказчик)
Просил: «возьмем всё из архива и разложим как надо начиная с корня ... архив перемести в тмп ... То что распоковали и разложили едет в репо»
Получил: 33 файла в корне, 2 в docs/, архив в tmp/ вне git; проверок 6 из 6 PASS
Не хватает: коммит и push (владелец: «Пока не отправлять»)
Вердикт: вернул бы — проверок 6 из 6 PASS, но статус accepted ставит только владелец (правило 37); приёмку поручил владелец 19.11.2026: «займись ты приёмкой»
