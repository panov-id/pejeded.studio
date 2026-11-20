# PEJEDED STUDIO

Desktop app for live shows and for writing music: a real-time 3D picture of where each sound sits, four DJ decks, set recording. Linux first, macOS later. Code is built stage by stage from the development spec in this repository; stage 1 (skeleton) is in progress.

Настольное приложение для живых выступлений и написания музыки. Принимает многоканальный звук с USB-устройств (первое поддержанное — Roland MC-707), в реальном времени показывает, где в пространстве находится каждый звук, играет треки на четырёх деках с контроллера Roland DJ-202 и записывает сет в аудио и видео. Сначала Linux (Debian 13), затем macOS.

**Состояние:** идёт этап 1 (каркас). Разработка идёт по этапам из задания, строго по порядку.

## Сборка

```
git clone --recurse-submodules git@github.com:panov-id/pejeded.studio.git   # или после клонирования: git submodule update --init
docker compose build
docker compose run --rm test        # сборка и все автотесты в контейнере
./build/PejededStudio               # запуск на машине
tests/host_sound_check.sh           # проверка на машине со звуком: 48 кГц, без пропусков, пока выезжают слои
```

## Где что лежит

| Что | Где |
| --- | --- |
| Задание на разработку: оглавление и что читать на каком этапе | `docs/spec/README.md` |
| Эталон картинки (открывается в браузере) | `docs/prototype/sound-space.html` |
| Окружение машины разработки | `docs/environment.md` |
| Прежние документы, только для истории | `docs/archive/` |
| Правила для Claude Code | `CLAUDE.md` |

## Чего здесь нет и не будет

Треков, записей, базы данных, личных настроек, паролей и ключей: репозиторий открытый. Всё это программа хранит в домашней папке пользователя.

## Лицензия

GNU AGPLv3, текст в файле `LICENSE`.
