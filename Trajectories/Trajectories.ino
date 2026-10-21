// Trajectories — точка входа для Arduino IDE.
//
// Здесь только имя для ESPidi HUB и обложка. Весь код прошивки лежит в
// trajectories.cpp рядом, включая setup() и loop() — линковщик найдёт
// их там.
//
// Почему дескриптор ЗДЕСЬ, а не в .cpp: Arduino решает, какие
// библиотеки подключить, сканируя именно .ino. Пока этот файл был
// пустым, ESPidiHub.h не находился, дескриптор и обложка в образ не
// попадали, и HUB прошивку не видел.
//
// Почему код НЕ здесь: Arduino IDE переписывает .ino перед сборкой —
// сама генерирует прототипы всех функций и вставляет их в начало файла,
// ВЫШЕ объявлений struct. Прототипы вроде
//   const Signal* armSource(uint8_t);
// оказываются перед `struct Signal`, и сборка падает с
// "'Signal' does not name a type". Файлы .cpp в папке скетча IDE
// компилирует как есть, без этой подстановки. Функций в этом файле нет,
// поэтому поднимать нечего.

#include <ESPidiHub.h>

ESPIDI_FIRMWARE("trajectories", "Trajectories", "0.2 beta", "Daniil Vlasenko");
#include "cover.h"   // генерируется из cover.png: espidi_hub.py repo . --sync-cover
