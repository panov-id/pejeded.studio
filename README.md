# PEJEDED STUDIO

Desktop app for live shows and for writing music: a real-time 3D picture of where each sound sits, four DJ decks, set recording. Linux first, macOS later. There is no code yet: this repository holds the development spec; the code comes stage by stage.

Настольное приложение для живых выступлений и написания музыки. Принимает многоканальный звук с USB-устройств (первое поддержанное — Roland MC-707), в реальном времени показывает, где в пространстве находится каждый звук, играет треки на четырёх деках с контроллера Roland DJ-202 и записывает сет в аудио и видео. Сначала Linux (Debian 13), затем macOS.

**Состояние:** кода ещё нет. Здесь лежат задание на разработку и эталон картинки. Разработка идёт по этапам из задания, строго по порядку.

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
