# Trajectories

Модульный слотовый секвенсер и MIDI-процессор для **ESP32-C3 / ESPidi**.
Экран 128×32, один энкодер с кнопкой, три кнопки, DIN MIDI in/out.

Четыре слота **A–D**. В каждый грузится одно из 20 приложений — генератор
нот или процессор. Слоты соединяются в цепочки параметром **ARM**: слот
читает вывод другого слота и обрабатывает его. Весь материал проходит
через общий Scale Engine: у каждого генератора свой лад и тоника, и они
едут вместе с сигналом по всей цепочке.

Полная инструкция — [MANUAL.md](MANUAL.md) (на русском).

## Поставить

Открыть сборщик и вставить адрес этого репозитория на вкладке **GitHub**:

<https://espidi-hub.github.io/?play=Cirocular-Vlasenko-Daniil/Trajectories>

Или из командной строки инструментами ESPidi HUB:

```bash
python3 tools/espidi_hub.py install --slot 1 Cirocular-Vlasenko-Daniil/Trajectories
```

## Что внутри

**Генераторы** — BASIC, SKELETON, EUCLIDIAN, TOPOLOGY.
**Над нотами** — ARPEGGIO, STRUM, NOTE DELAY, GLIDER, ADD, SHIFT, DUST.
**Над CC** — ENVELOPE, MORPH LFO, KEYTRACK.
**Утилиты** — LOGIC, MATH, META.
**Роутеры** — MULTI OUT, POLY SPLIT, ROUND ROBIN.

Полифония настоящая и внутри цепочки: сколько нот выставлено на шаге,
столько и уходит на шину. Модуляция привязывается к номеру CC — ведёт
её либо ручка на внешнем контроллере, либо любой свой модуль, который
шлёт CC.

## Собрать самому

Папка скетча — `Trajectories/`. Код лежит в `trajectories.cpp`, а `.ino`
намеренно пустой: Arduino IDE вставляет автоматические прототипы выше
объявлений `struct`, и сборка падала бы на `'Signal' does not name a
type`. Файлы `.cpp` в папке скетча IDE компилирует как есть.

Нужны `Adafruit SSD1306`, `Adafruit GFX Library` и
[ESPidiHub](https://github.com/Cirocular-Vlasenko-Daniil/ESPidi-HUB) из
SDK. Плата — ESP32C3, **Partition Scheme → Huge APP**.

На каждый push сборку делает workflow и, когда меняется версия в
`ESPIDI_FIRMWARE()`, коммитит образ по пути из `espidi.json` — именно
его загружает сборщик.

## Тесты

Прошивка прогоняется на хосте: `trajectories.cpp` включается в тест
целиком, Arduino подменяется заглушками. Проверяются и MIDI на выходе, и
картинка на экране, и сохранение патчей.

```bash
./test/run.sh poly poly2 poly3 structure meta modlayer
```

Подробнее — [test/README.md](test/README.md).

## Веб-отображение экрана

`tools/screen.py` показывает экран устройства в браузере по USB
(нужен только `pyserial`):

```bash
python3 tools/screen.py --port /dev/cu.usbmodem1101
```

## Лицензия

MIT — см. [LICENSE](LICENSE).
