// ============================================================
// Trajectories — модульный слотовый гармонический секвенсер/модулятор
// ESP32-C3 / платформа ESPidi (энкодер с кнопкой, PLAY/TAP/PAGE,
// OLED 128x32, DIN MIDI in/out).
//
// 4 слота (A-D), в каждый грузится одно приложение: генератор нот
// или модулятор-процессор. Слоты соединяются в цепочки через ARM.
// Весь материал проходит через общий Scale Engine.
// ============================================================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// Имя, версия и обложка для ESPidi HUB. Под защитой __has_include,
// потому что на хосте (прогон тестов из test/) ESP-IDF и ESPidiHub.h
// отсутствуют, а собираться там прошивка обязана.
#if __has_include(<ESPidiHub.h>)
  #include <ESPidiHub.h>
  ESPIDI_FIRMWARE("trajectories", "Trajectories", "0.1 beta", "Daniil Vlasenko");
  #include "cover.h"   // из cover.pbm: espidi_hub.py cover --invert cover.pbm > cover.h
#endif

// ============================================================
// 1. КОНФИГУРАЦИЯ
// ============================================================
namespace Pins {
  constexpr uint8_t PLAY   = 1;
  constexpr uint8_t TAP    = 2;
  constexpr uint8_t PAGE   = 3;
  constexpr uint8_t ENC_SW = 4;
  constexpr uint8_t ENC_A  = 5;
  constexpr uint8_t ENC_B  = 6;
  constexpr uint8_t MIDI_RX = 7;
  constexpr uint8_t MIDI_TX = 10;
  constexpr uint8_t SDA    = 8;
  constexpr uint8_t SCL    = 9;
  constexpr uint32_t MIDI_BAUD = 31250;
}

namespace Display {
  constexpr uint8_t W = 128;
  constexpr uint8_t H = 32;
  constexpr uint8_t COL_W = 31;
  // Колонки прижаты к низу экрана: освободившаяся строка отдана шапке,
  // чтобы звёзды созвездия могли расходиться по высоте.
  constexpr uint8_t COL_Y = 12;
  constexpr uint8_t COL_H = 20;
}

Adafruit_SSD1306 display(Display::W, Display::H, &Wire, -1);
HardwareSerial midi(1);
Preferences storage;

constexpr uint8_t NUM_SLOTS = 4;
constexpr uint8_t NUM_COLS  = 4;
// Голосов в сигнале. Полифония настоящая и внутри цепочки: BASIC и
// SKELETON отдают столько нот, сколько выставлено на шаге, TOPOLOGY —
// до трёх дорожек. Дублей в аккорде не бывает (см. dedupeChord).
constexpr uint8_t MAX_VOICES = 4;
constexpr uint8_t DEFAULT_VEL = 100;   // когда источник силы не задаёт

// ============================================================
// 2. ПРИЛОЖЕНИЯ И ИХ ПАРАМЕТРЫ
// ============================================================
enum class AppId : uint8_t {
  EMPTY,
  // Генераторы — сами производят ноты, входа не имеют.
  BASIC, SKELETON, EUCLIDIAN, TOPOLOGY,
  // Процессоры — трансформируют захваченный по ARM сигнал.
  ARP, STRUM, NDELAY, ENV, MLFO, DUST, GLIDER, ADD, SHIFT, LOGIC, MATH, KEYTRK,
  MULTX, POLYSPL, RROBIN, META,
  COUNT
};

constexpr uint8_t APP_COUNT = (uint8_t)AppId::COUNT;
constexpr uint8_t PATCH_COUNT = 8;   // ячеек сейвстейтов

// Порядок в браузере — по смыслу, а не по истории добавления.
// Сам enum AppId трогать НЕЛЬЗЯ: его значения записаны во флеш, и
// перестановка перемешала бы приложения во всех сохранённых слотах.
// Поэтому группировка живёт отдельной таблицей отображения.
const uint8_t browseOrder[APP_COUNT] = {
  (uint8_t)AppId::EMPTY,
  // GEN — производят ноты сами
  (uint8_t)AppId::BASIC, (uint8_t)AppId::SKELETON,
  (uint8_t)AppId::EUCLIDIAN, (uint8_t)AppId::TOPOLOGY,
  // NOTE — преобразуют захваченные ноты
  (uint8_t)AppId::ARP, (uint8_t)AppId::STRUM, (uint8_t)AppId::NDELAY,
  (uint8_t)AppId::GLIDER, (uint8_t)AppId::ADD, (uint8_t)AppId::SHIFT,
  (uint8_t)AppId::DUST,
  // CC — производят модуляцию наружу
  (uint8_t)AppId::ENV, (uint8_t)AppId::MLFO, (uint8_t)AppId::KEYTRK,
  // UTIL — считают и управляют самим устройством
  (uint8_t)AppId::LOGIC, (uint8_t)AppId::MATH, (uint8_t)AppId::META,
  // ROUTE — раскладывают сигнал по каналам
  (uint8_t)AppId::MULTX, (uint8_t)AppId::POLYSPL, (uint8_t)AppId::RROBIN
};

const char* const groupNames[] = {"EMPTY", "GEN", "NOTE", "CC", "UTIL", "ROUTE"};
constexpr uint8_t GROUP_COUNT = 6;
// Границы групп в browseOrder: начало каждой плюс конец списка.
const uint8_t groupStart[GROUP_COUNT + 1] = {0, 1, 5, 12, 15, 18, APP_COUNT};

uint8_t groupOfPos(uint8_t pos) {
  for (uint8_t g = 0; g < GROUP_COUNT; g++)
    if (pos < groupStart[g + 1]) return g;
  return (uint8_t)(GROUP_COUNT - 1);
}

// Позиция приложения в браузере — список открывается на том, что уже
// стоит в слоте.
uint8_t browsePosOf(uint8_t app) {
  for (uint8_t i = 0; i < APP_COUNT; i++) if (browseOrder[i] == app) return i;
  return 0;
}
constexpr AppId FIRST_PROCESSOR = AppId::ARP;

inline bool isGenerator(uint8_t app) {
  return app >= (uint8_t)AppId::BASIC && app < (uint8_t)FIRST_PROCESSOR;
}
inline bool isProcessor(uint8_t app) {
  return app >= (uint8_t)FIRST_PROCESSOR && app < APP_COUNT;
}

// Тип отрисовки значения параметра.
// P_CHAN — номер MIDI-канала: 0 показывается как NO и означает «наружу
// не шлём». Отдельный тип, чтобы это правило жило в одном месте, а не
// дублировалось в каждой отрисовке.
enum PType : uint8_t { P_NUM, P_NOTE, P_LIST, P_MS, P_SIGN, P_CHAN };

struct PDesc {
  const char* name;
  int16_t lo, hi;
  uint8_t type;
  const char* const* names;
};

// MODE ведёт по ПАТТЕРНУ, а не по трём нотам фигуры: направление
// чтения шагов. Перебирать три ноты было нечем — с тех пор как шаг сам
// решает, сколько их звучит, порядок внутри фигуры смысла не имеет.
const char* const skelModes[]   = {"LOOP", "PING", "RND", "WALK"};
const char* const topoMode[]    = {"1CH", "3CH"};
const char* const topoTrk[]     = {"XY", "T1", "T2", "T3"};
// «1» — стандартная скорость (шаг в целый такт), дальше по списку
// быстрее, «1/16» — самая быстрая. Порядок именно такой: значение
// растёт вправо вместе со скоростью, а не с длиной шага.
const char* const rateNames[]   = {"1", "1/2", "1/4", "1/8", "1/16"};
// Скорость огибающей ENV: GATE — длина захваченного трига, дальше
// фиксированные доли такта, по возрастанию скорости (как у RATE).
const char* const envSpeedNames[] = {"GATE", "1", "1/2", "1/4", "1/8", "1/16"};
const char* const strumDir[]    = {"UP", "DOWN", "RND"};
const char* const strumPat[]    = {"STRT", "ACCL", "DECL", "BRST"};
const char* const walkNames[]   = {"OCT", "+1", "RND"};
const char* const envShape[]    = {"AD", "AR", "LOOP"};
const char* const envFunc[]     = {"LIN", "EXP", "LOG", "S"};
const char* const lfoShape[]    = {"SINE", "TRI", "SQR"};
const char* const lfoAct[]      = {"FREE", "SYNC", "ONE"};
const char* const glideCurve[]  = {"LIN", "SPRG", "EASE"};
const char* const addIntv[]     = {"3rd", "5th", "6th", "7th", "OCT"};
const char* const addSrc[]      = {"LOW", "HIGH", "LAST"};
// SHIFT: что делать с ячейкой, вышедшей из конца регистра.
// Режим задаёт, ВО ЧТО превращается ячейка под головой записи, когда
// свежий материал в неё не попал. SPRD задаёт величину — в тех
// единицах, которые подразумевает режим.
const char* const shiftMode[]   = {"WALK", "OCT", "MIRROR", "DROP"};
const char* const logicOps[]    = {"AND", "OR", "XOR", "NAND", "NOR", "XNOR"};
const char* const logicCmp[]    = {"TRIG", "T+N", "T+CC"};
const char* const logicRel[]    = {"=", "!=", ">", "<"};
const char* const logicOut[]    = {"A", "B", "LOW", "HIGH"};
const char* const mathSrc[]     = {"NOTE", "CC", "GATE"};
// B-A — обратное вычитание. Без него нечем инвертировать сигнал:
// "-" даёт только A-B, а типовая утилита — это константа минус вход.
const char* const mathOps[]     = {"+", "-", "B-A", "x", "/", "AVG", "MOD", "MIN", "MAX"};
const char* const mathLim[]     = {"CLAMP", "WRAP", "FOLD"};
// KEYTRACK: подписи колонок нужны только для дескриптора — на экране
// вместо них стоит номер CC каждой линии.
const char* const ktLines[]     = {"L1", "L2", "L3", "L4"};
const char* const splitMode[]   = {"BOTH", "LO", "HI"};
const char* const rrMode[]      = {"CYCLE", "RND", "FREE", "CHORD"};
const char* const rrSteal[]     = {"OLD", "NEW", "DROP"};
// Подписи колонок-выходов нужны только дескриптору: на экране
// вместо них стоит номер MIDI-канала этого выхода.
const char* const outLines[]    = {"O1", "O2", "O3", "O4", "O5", "O6", "O7"};
// МетаПерформер: чем модулировать и три цели.
const char* const metaSrc[]     = {"NOTE", "CC", "GATE"};
const char* const metaT[]       = {"T1", "T2", "T3"};
const char* const metaD[]       = {"D1", "D2", "D3"};
// Имена для спрятанного под TAP. Отличаются от подписей колонок
// намеренно: в списке целей глубина L1 и номер CC этой же линии —
// разные параметры, и одинаково звать их нельзя.
const char* const ktCCNames[]   = {"CC1", "CC2", "CC3", "CC4"};
const char* const outChNames[]  = {"CH1", "CH2", "CH3", "CH4", "CH5", "CH6", "CH7"};
// Последнее значение — «источника нет», доступно только второму ARM:
// первый источник у процессора есть всегда.
const char* const armNames[]    = {"A", "B", "C", "D", "EXT", "NO"};
constexpr uint8_t ARM_EXT  = 4;
constexpr uint8_t ARM_NONE = 5;

// Короткая метка источника для колонки ARM, где два источника не
// влезают полными именами: A-D как есть, внешний вход — E.
inline char armShort(uint8_t id) { return id >= ARM_EXT ? 'E' : (char)('A' + id); }
const char* const rootNames[]   = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// Длина шага в тиках MIDI-клока (24 ppqn), в порядке rateNames:
// целая нота .. 1/16.
const uint8_t rateTicks[5] = {96, 48, 24, 12, 6};
// Диатонические интервалы ADD: 3-я/5-я/6-я/7-я = 2/4/5/6 ступеней лада.
const int8_t addDegrees[4] = {2, 4, 5, 6};
// Имена нот дорожек TOPOLOGY как целей модуляции — в списке META
// они обязаны отличаться и от плотностей, и друг от друга.
const char* const topoNoteNames[] = {"NT1", "NT2", "NT3"};

struct AppDesc {
  const char* name;
  PDesc p[NUM_COLS];  // главная страница слота
  PDesc sub4;         // 4-й параметр подстраницы (только у генераторов)
};

// Индексируется AppId. EMPTY-строка держит место, чтобы индекс
// приложения совпадал с позицией в списке загрузки.
const AppDesc apps[APP_COUNT] = {
  {"EMPTY", {{"", 0, 0, P_NUM, nullptr}, {"", 0, 0, P_NUM, nullptr},
             {"", 0, 0, P_NUM, nullptr}, {"", 0, 0, P_NUM, nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  {"BASIC", {{"NOTE1", 0, 127, P_NOTE, nullptr},
             {"NOTE2", 0, 127, P_NOTE, nullptr},
             {"NOTE3", 0, 127, P_NOTE, nullptr},
             // TRIG — длина ноты в ШАГАХ секвенсора, а не доля шага:
             // так нота может тянуться через несколько шагов.
             {"TRIG",  1, 16,  P_NUM,  nullptr}},
            {"STEPS",  1, 16,  P_NUM,  nullptr}},

  {"SKELETON",{{"ROOT",  0, 127, P_NOTE, nullptr},
             {"INTV2", -12, 12, P_SIGN, nullptr},
             {"INTV3", -12, 12, P_SIGN, nullptr},
             {"MODE",  0, 3,   P_LIST, skelModes}},
            {"STEPS",  1, 16,  P_NUM,  nullptr}},

  {"EUCLIDIAN",{{"STEPS", 1, 16,  P_NUM,  nullptr},
             {"PULSE", 0, 16,  P_NUM,  nullptr},
             {"ROTAT", 0, 15,  P_NUM,  nullptr},
             {"NOTE",  0, 127, P_NOTE, nullptr}},
            {"CHANCE", 0, 127, P_NUM,  nullptr}},

  // TOPOLOGY — карта ритмов вместо программирования: X и Y ведут по
  // ней, три дорожки берут свои плотности. Механика восходит к
  // Mutable Instruments Grids; карта здесь своя, потому что таблица
  // Grids под GPL, а это сделало бы GPL весь проект.
  {"TOPOLOGY",{{topoTrk[0], 0, 127, P_NUM, nullptr},
             {topoTrk[1], 0, 127, P_NUM, nullptr},
             {topoTrk[2], 0, 127, P_NUM, nullptr},
             {topoTrk[3], 0, 127, P_NUM, nullptr}},
            {"CHAOS",  0, 127, P_NUM,  nullptr}},

  {"ARPEGGIO",{{"RATE",  0, 4,   P_LIST, rateNames},
             // KTRC — насколько арпеджио следует за сыгранной клавишей.
             // 64 это «полностью», 0 — арпеджио стоит на своей опоре и
             // не двигается вовсе, отрицательное — идёт навстречу.
             {"KTRC",  -64, 64, P_SIGN, nullptr},
             {"OCT",   0, 3,   P_NUM,  nullptr},
             {"STUTT", 0, 127, P_NUM,  nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  {"STRUM", {{"SPEED", 5, 200, P_MS,   nullptr},
             {"VOICE", 1, 3,   P_NUM,  nullptr},
             {"DIR",   0, 2,   P_LIST, strumDir},
             {"PATT",  0, 3,   P_LIST, strumPat}},
            {"", 0, 0, P_NUM, nullptr}},

  // RPEAT считает ноты целиком: 1 — только сама нота без эха, 2 —
  // нота и один повтор. Столбик на экране рисует ровно это.
  {"NOTE DELAY",{{"RPEAT", 1, 8,   P_NUM,  nullptr},
             {"WALK",  0, 2,   P_LIST, walkNames},
             {"PPONG", 0, 63,  P_NUM,  nullptr},
             {"DECAY", 0, 127, P_NUM,  nullptr}},
            // На подстранице эта колонка не рисуется: у процессора там
            // своё. Имя нужно, чтобы META могла назвать цель.
            {"DIV", 0, 4, P_LIST, rateNames}},

  {"ENVELOPE",{{"SHAPE", 0, 2,   P_LIST, envShape},
             // A/D — перекос между атакой и спадом: 0 поровну,
             // минус короче атака, плюс длиннее.
             {"A/D",   -63, 64, P_SIGN, nullptr},
             {"CC1",   0, 127, P_NUM,  nullptr},
             {"CC2",   0, 127, P_NUM,  nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  {"MORPH LFO",{{"SHAPE", 0, 2,   P_LIST, lfoShape},
             {"MORPH", -64, 64, P_SIGN, nullptr},
             {"ACTIV", 0, 2,   P_LIST, lfoAct},
             {"AMT",   0, 127, P_NUM,  nullptr}},
            // Вторая строка колонки ACTIV: доля такта на свободном ходу.
            {"RATE", 0, 4, P_LIST, rateNames}},

  {"DUST",  {{"FILL",  0, 127, P_NUM,  nullptr},
             {"ROOT%", 0, 100, P_NUM,  nullptr},
             {"PLEN",  0, 127, P_NUM,  nullptr},
             {"REACH", 0, 7,   P_NUM,  nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  {"GLIDER",{{"SPEED", 5, 150, P_MS,   nullptr},
             {"STEPS", 1, 16,  P_NUM,  nullptr},
             {"CURVE", 0, 2,   P_LIST, glideCurve},
             {"GATE%", 10, 127, P_NUM, nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  {"ADD",   {{"INTV",  0, 4,   P_LIST, addIntv},
             {"SRC",   0, 2,   P_LIST, addSrc},
             {"INVRT", 0, 127, P_NUM,  nullptr},
             {"VEL",   0, 127, P_NUM,  nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  // LOGIC сравнивает два входа и пропускает ноту, только когда
  // выполняется логическое уравнение. Сетку берёт извне — см.
  // logicGridSlot(), собственного RATE у него нет.
  // SHIFT — регистр сдвига. Единственный модуль, который ПОМНИТ: держит
  // очередь прошедших нот и на каждый триггер выдаёт её по кругу. При
  // нулевом шансе это чистая петля, при полном — сплошная случайность,
  // между ними — то самое «повторяется, но медленно уползает», чего в
  // устройстве больше нет ни у кого.
  // Первая колонка рисует сам регистр, поэтому её подпись на экран не
  // выходит; ROT живёт под TAP на ней же и хранится в sub4.
  {"SHIFT", {{"LEN",   1, 16,  P_NUM,  nullptr},
             {"MUTAT", 0, 127, P_NUM,  nullptr},
             {"SPRD",  0, 7,   P_NUM,  nullptr},
             {"MODE",  0, 3,   P_LIST, shiftMode}},
            {"ROT",   0, 15,  P_NUM,  nullptr}},

  {"LOGIC", {{"OP",    0, 5,   P_LIST, logicOps},
             {"CMP",   0, 2,   P_LIST, logicCmp},
             {"REL",   0, 3,   P_LIST, logicRel},
             {"OUT",   0, 3,   P_LIST, logicOut}},
            // На подстранице не рисуется; имя нужно META как цели.
            {"CCVAL", 0, 127, P_NUM, nullptr}},

  // MATH — арифметика над сигналами. В отличие от LOGIC не решает,
  // пропускать ли, а считает, каким будет значение. Сетка не нужна:
  // в тишине считать нечего.
  {"MATH",  {{"SRC",   0, 2,   P_LIST, mathSrc},
             {"OP",    0, 8,   P_LIST, mathOps},
             {"CONST", 0, 127, P_NUM,  nullptr},
             {"LIM",   0, 2,   P_LIST, mathLim}},
            {"", 0, 0, P_NUM, nullptr}},

  // KEYTRACK — высота захваченной ноты рулит четырьмя линиями CC.
  // Каждая колонка это одна линия: сверху её номер CC (TAP + энкодер),
  // снизу глубина. Глубина 0 гасит линию, отдельного вкл/выкл нет.
  {"KEYTRACK",{{ktLines[0], -64, 64, P_SIGN, nullptr},
             {ktLines[1], -64, 64, P_SIGN, nullptr},
             {ktLines[2], -64, 64, P_SIGN, nullptr},
             {ktLines[3], -64, 64, P_SIGN, nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  // MULTX — размножает вход на семь выходов. Колонка = выход: сверху
  // его MIDI-канал (TAP + энкодер), снизу сдвиг в ступенях лада.
  // Выходы 1-4 на главной странице, 5-7 на подстранице.
  {"MULTI OUT",{{outLines[0], -14, 14, P_SIGN, nullptr},
             {outLines[1], -14, 14, P_SIGN, nullptr},
             {outLines[2], -14, 14, P_SIGN, nullptr},
             {outLines[3], -14, 14, P_SIGN, nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  // POLYSPL — делит клавиатуру по высоте на две зоны со своими
  // каналами и своим транспонированием.
  {"POLY SPLIT",{{"POINT", 0, 127, P_NOTE, nullptr},
             {"XP LO", -14, 14, P_SIGN, nullptr},
             {"XP HI", -14, 14, P_SIGN, nullptr},
             {"ZONES", 0, 2,   P_LIST, splitMode}},
            {"", 0, 0, P_NUM, nullptr}},

  // RROBIN — раскидывает вход по нескольким моно-каналам подряд от
  // базового. RANDOM здесь же: отдельный модуль под него не нужен.
  {"ROUND ROBIN",{{"VOICE", 1, 4,   P_NUM,  nullptr},
             {"MODE",  0, 3,   P_LIST, rrMode},
             {"STEAL", 0, 2,   P_LIST, rrSteal},
             {"SPRD",  -7, 7,  P_SIGN, nullptr}},
            {"", 0, 0, P_NUM, nullptr}},

  // METAP — модулирует параметры ДРУГИХ слотов. Колонка-цель хранит
  // адрес: слот под TAP, номер параметра на энкодере. Смещение
  // накладывается на чтении, хранимое значение цели не трогается.
  // Порядок строки обязан совпадать с местом METAP в enum AppId.
  {"META",{{"SRC",    0, 2,   P_LIST, metaSrc},
             {metaT[0], 0, 71,  P_NUM,  nullptr},
             {metaT[1], 0, 71,  P_NUM,  nullptr},
             {metaT[2], 0, 71,  P_NUM,  nullptr}},
            {"", 0, 0, P_NUM, nullptr}}
};

// Подстраницы: у процессоров она фиксирована, у генераторов 4-й
// параметр берётся из AppDesc::sub4.
const PDesc procSub[NUM_COLS] = {
  {"CH",    0, 16,  P_CHAN, nullptr},
  {"ARM",   0, 4,   P_LIST, armNames},
  {"CCDST", 0, 127, P_NUM,  nullptr},
  {"STEP+", -12, 12, P_SIGN, nullptr}
};

// Часть процессоров занимает 3-ю и 4-ю колонки подстраницы под своё:
// CC DEST и STEP+ им просто не нужны. Значения таких колонок живут в
// SlotCfg::sub2 / sub3, а не в ccDest / stepPlus.
// Номер выхода MULTX, сидящего в колонке; -1 — колонка занята другим.
// ARM оставлен на своём привычном месте (колонка 2), поэтому выходы
// 5-7 разложены по колонкам 1, 3 и 4 подстраницы.
int8_t multxOut(uint8_t app, bool sub, uint8_t col) {
  if ((AppId)app != AppId::MULTX) return -1;
  if (!sub) return (int8_t)col;              // выходы 1-4
  if (col == 1) return -1;                   // ARM
  return (int8_t)(col == 0 ? 4 : 3 + col);   // 5, 6, 7
}

// Пара голосов RROBIN в колонке подстраницы: 0 -> голоса 1 и 2,
// 1 -> голоса 3 и 4. Энкодер правит первый канал пары, TAP — второй.
int8_t rrPair(uint8_t app, bool sub, uint8_t col) {
  if ((AppId)app != AppId::RROBIN || !sub) return -1;
  return (col == 2) ? 0 : (col == 3 ? 1 : -1);
}

PDesc processorSubDesc(uint8_t app, uint8_t col) {
  if (col == 2) {
    // STRUM нот не модулирует по CC — вместо адреса CC у него акцент
    // velocity: насколько сильно выделена первая нота и как быстро
    // спадают следующие.
    if ((AppId)app == AppId::STRUM) return {"ACCNT", -63, 64, P_SIGN, nullptr};
    if ((AppId)app == AppId::ENV)   return {"SPEED", 0, 5, P_LIST, envSpeedNames};
    // У KEYTRACK адрес CC свой у каждой линии, а роль сдвига ноты
    // лучше играет опорная нота — обе стандартные колонки заняты.
    if ((AppId)app == AppId::KEYTRK) return {"CENTR", 0, 127, P_NOTE, nullptr};
    // Верхняя зона сплита — свой канал, CC DEST модулю не нужен.
    if ((AppId)app == AppId::POLYSPL) return {"CH HI", 0, 16, P_CHAN, nullptr};
    if ((AppId)app == AppId::META)   return {metaD[1], -64, 64, P_SIGN, nullptr};
  }
  if (col == 0) {
    // Нижняя зона сплита и базовый канал RROBIN живут там же, где
    // обычный CH: это и есть настройка канала.
    if ((AppId)app == AppId::POLYSPL) return {"CH LO", 0, 16, P_CHAN, nullptr};
    if ((AppId)app == AppId::META)   return {metaD[0], -64, 64, P_SIGN, nullptr};
    // У RROBIN каналы теперь у каждого голоса свои, общий CH не нужен —
    // его место занял STEP+, вытесненный парами каналов.
    if ((AppId)app == AppId::RROBIN)  return {"STEP+", -12, 12, P_SIGN, nullptr};
  }
  {
    int8_t pr = rrPair(app, true, col);
    if (pr >= 0) return {"VOICE", 0, 16, P_CHAN, nullptr};
  }
  {
    int8_t o = multxOut(app, true, col);
    if (o >= 0) return {outLines[o], -14, 14, P_SIGN, nullptr};
  }
  // У ENV STEP+ смысла не имеет (он не переигрывает ноту), поэтому
  // 4-я колонка отдана форме кривой, ушедшей с главной страницы.
  if (col == 3 && (AppId)app == AppId::ENV) return {"FUNC", 0, 3, P_LIST, envFunc};
  if (col == 3 && (AppId)app == AppId::KEYTRK) return {"SPAN", 1, 48, P_NUM, nullptr};
  // Опорная нота, вокруг которой линии расходятся. Колонка звалась
  // CCDST, но ccDest этот модуль не использует вовсе: номера CC у
  // него свои, по одному на линию.
  if (col == 2 && (AppId)app == AppId::KEYTRK) return {"CNTR", 0, 127, P_NOTE, nullptr};
  // SHIFT не шлёт CC, поэтому CCDST у него мертва — колонка отдана
  // приращению velocity за круг: петля затухает или раскачивается.
  if (col == 2 && (AppId)app == AppId::SHIFT) return {"VEL", -64, 64, P_SIGN, nullptr};
  if (col == 3 && (AppId)app == AppId::META)  return {metaD[2], -64, 64, P_SIGN, nullptr};
  return procSub[col];
}
inline bool usesSub2(uint8_t app) {
  return (AppId)app == AppId::SHIFT ||
         (AppId)app == AppId::STRUM || (AppId)app == AppId::ENV ||
         (AppId)app == AppId::KEYTRK || (AppId)app == AppId::POLYSPL ||
         (AppId)app == AppId::META;
}
inline bool usesSub3(uint8_t app) {
  return (AppId)app == AppId::ENV || (AppId)app == AppId::KEYTRK ||
         (AppId)app == AppId::META;
}
const PDesc genSub[3] = {
  // BPM и RATE делят одну колонку: BPM общий на устройство и трогается
  // редко, поэтому он ушёл под TAP, а RATE остался на энкодере.
  {"RATE",  0, 4,   P_LIST, rateNames},
  // 0 = NO: генератор не шлёт MIDI сам, но по шине продолжает кормить
  // процессоры — так его можно использовать как чистый источник.
  {"CH",    0, 16,  P_CHAN, nullptr},
  {"SCALE", 0, 9,   P_LIST, nullptr}   // имена берутся из scales[]
};

// ============================================================
// 3. SCALE ENGINE
// ============================================================
// Лад едет вместе с сигналом по всей цепочке: процессор, армленный
// на генератор, наследует его SCALE/ROOT. "Сдвиг на N ступеней"
// везде означает диатоническую транспозицию, а не полутона.
struct ScaleDef { const char* name; uint8_t count; uint8_t iv[12]; };

// Хроматика — последняя в списке; на неё опирается внешний вход,
// когда генератора на устройстве нет вовсе.
constexpr uint8_t SCALE_CHROMATIC = 9;
const ScaleDef scales[10] = {
  {"MAJ",  7, {0,2,4,5,7,9,11}},
  {"MIN",  7, {0,2,3,5,7,8,10}},
  {"DOR",  7, {0,2,3,5,7,9,10}},
  {"PHR",  7, {0,1,3,5,7,8,10}},
  {"LYD",  7, {0,2,4,6,7,9,11}},
  {"MIX",  7, {0,2,4,5,7,9,10}},
  {"LOC",  7, {0,1,3,5,6,8,10}},
  {"HMIN", 7, {0,2,3,5,7,8,11}},
  {"MMIN", 7, {0,2,3,5,7,9,11}},
  {"CHRM", 12, {0,1,2,3,4,5,6,7,8,9,10,11}}
};

inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Деление с округлением вниз — нужно для корректного переноса октавы
// при отрицательных индексах ступеней (обычное / в C++ рубит к нулю).
inline int floorDiv(int a, int b) {
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

// Притягивает ноту к ближайшей валидной ступени лада. Именно это
// вызывается при смене SCALE/ROOT на лету, поэтому уже забитый
// паттерн не ломается — ноты просто переезжают на соседнюю ступень.
uint8_t quantizeToScale(int note, uint8_t sc, uint8_t root) {
  if (sc >= 10) sc = 0;
  const ScaleDef &s = scales[sc];
  int pc = (note - (int)root) % 12;
  if (pc < 0) pc += 12;
  int octBase = note - pc;
  int best = s.iv[0], bestDist = 127;
  for (uint8_t i = 0; i < s.count; i++) {
    int d = abs((int)s.iv[i] - pc);
    if (d < bestDist) { bestDist = d; best = s.iv[i]; }
  }
  // Верхний сосед следующей октавы может оказаться ближе, чем любая
  // ступень текущей (например pc=11 в мажоре без 11-й ступени).
  if (12 - pc < bestDist) { best = 12; }
  // На краях MIDI-диапазона выходим октавами, а не клампом: клампить
  // до 0/127 нельзя — это выбрасывает ноту из лада, а перенос на
  // октаву сохраняет ступень.
  int v = octBase + best;
  while (v < 0) v += 12;
  while (v > 127) v -= 12;
  return (uint8_t)v;
}

// Диатоническая транспозиция на N ступеней лада.
uint8_t transposeDegrees(int note, int degrees, uint8_t sc, uint8_t root) {
  if (sc >= 10) sc = 0;
  const ScaleDef &s = scales[sc];
  int q = quantizeToScale(note, sc, root);
  int pc = (q - (int)root) % 12;
  if (pc < 0) pc += 12;
  int octBase = q - pc;
  int idx = 0;
  for (uint8_t i = 0; i < s.count; i++) if (s.iv[i] == pc) { idx = i; break; }
  int total = idx + degrees;
  int octShift = floorDiv(total, s.count);
  int newIdx = total - octShift * s.count;
  int v = octBase + octShift * 12 + s.iv[newIdx];
  while (v < 0) v += 12;
  while (v > 127) v -= 12;
  return (uint8_t)v;
}

// Номер ступени высоты относительно тоники, с учётом октав — обратная
// к noteOfDegree(). Нужна арифметике по ладу: складывать и умножать
// осмысленно именно ступени, а не MIDI-номера.
int degreeOf(uint8_t note, uint8_t sc, uint8_t root) {
  if (sc >= 10) sc = 0;
  const ScaleDef &s = scales[sc];
  int q = quantizeToScale(note, sc, root);
  int pc = (q - (int)root) % 12;
  if (pc < 0) pc += 12;
  int octBase = q - pc;                       // = root + 12k
  int oct = floorDiv(octBase - (int)root, 12);
  int idx = 0;
  for (uint8_t i = 0; i < s.count; i++) if (s.iv[i] == pc) { idx = i; break; }
  return oct * s.count + idx;
}

// Без сворачивания в диапазон MIDI: результат может выйти за 0..127,
// и решать, что с этим делать, должен вызывающий — иначе параметр LIM
// у MATH оказался бы мёртвым, всё сворачивалось бы здесь.
int noteOfDegreeRaw(int deg, uint8_t sc, uint8_t root) {
  if (sc >= 10) sc = 0;
  const ScaleDef &s = scales[sc];
  int oct = floorDiv(deg, s.count);
  int idx = deg - oct * s.count;
  return (int)root + oct * 12 + s.iv[idx];
}

uint8_t noteOfDegree(int deg, uint8_t sc, uint8_t root) {
  int v = noteOfDegreeRaw(deg, sc, root);
  while (v < 0) v += 12;
  while (v > 127) v -= 12;
  return (uint8_t)v;
}

void midiNoteToString(uint8_t note, char* out) {
  sprintf(out, "%s%d", rootNames[note % 12], (note / 12) - 1);
}

// ============================================================
// 4. ДАННЫЕ СЛОТОВ
// ============================================================
// Персистентная часть слота — пишется во флеш целиком одним blob'ом.
struct SlotCfg {
  uint8_t app;
  int16_t p[NUM_COLS];   // параметры главной страницы
  uint8_t midiCh;        // процессоры: канал вывода
  uint8_t arm;           // источник 1: 0-3 = слот A-D, 4 = EXT
  uint8_t arm2;          // источник 2: то же + 5 = NO (источника нет)
  uint8_t ccDest;        // целевой CC для модуляции
  int8_t  stepPlus;      // сдвиг захваченной ноты в ступенях лада
  // Колонки 3 и 4 подстраницы, когда приложение занимает их под своё
  // (см. processorSubDesc): STRUM — акцент, ENV — скорость и форма.
  int16_t sub2;
  int16_t sub3;
  // KEYTRACK: номер CC каждой из четырёх линий. Глубины лежат в p[].
  uint8_t ktCC[NUM_COLS];
  // Маршрутизаторы (MULTX/SPLIT): MIDI-канал каждого выхода, 0 — выход
  // выключен; и сдвиг выхода в ступенях лада.
  int8_t  metaDepth[3];   // METAP: глубина по каждой из трёх целей
  uint8_t outCh[8];
  int8_t  outXp[8];
  uint8_t rate;          // генераторы: деление такта
  uint8_t scale;
  uint8_t root;
  int16_t sub4;          // 4-й параметр подстраницы генератора
  // Триги шагов — модель секвенсера RED SEA: у каждой из 4 колонок
  // свой независимый лейн (там это был active[4] внутри шага), и
  // каждый шаг хранит собственное значение по каждой колонке.
  // lane[col] — битовая маска шагов, на которых колонка col
  // затригована; stepVal[шаг][col] — что именно на этом шаге играть.
  // Индексируется absStep % 16, поэтому бит и значение всегда
  // совпадают с той же ячейкой ряда (окно кратно 16).
  uint16_t lane[NUM_COLS];
  // int8 хватает: тригами пользуются только генераторы, а их параметры
  // главной страницы целиком укладываются в -128..127. Это две трети
  // объёма патча — ради сейвстейтов экономия существенная.
  int8_t   stepVal[16][NUM_COLS];
  // Байпас: слот превращается в провод — своей обработки нет, вход
  // уходит на шину как есть, наружу слот ничего не шлёт. Это не мьют:
  // цепочка за слотом продолжает получать сигнал.
  bool    bypass;
  bool    running;       // транспорт (общий, двойной клик TAP)
  // Карта модуляции слота: на какой CC смотрит каждая его колонка и с
  // какой глубиной. Индекс тот же, что у modOffset: 0..3 — главная
  // страница, 4..7 — подстраница. 0 в modCC = привязки нет.
  uint8_t modCC[2 * NUM_COLS];
  int8_t  modDepth[2 * NUM_COLS];
};

SlotCfg cfg[NUM_SLOTS];

// Сигнал на шине: что слот отдаёт наружу для ARM-захвата.
struct Signal {
  uint8_t  notes[MAX_VOICES];
  // Сила у каждой ноты своя, а не одна на сигнал: TOPOLOGY публикует
  // три дорожки одним аккордом, и у них принципиально разные акценты —
  // общее значение сплющило бы то, ради чего velocity и заводится.
  uint8_t  vel[MAX_VOICES];
  uint8_t  count;
  uint32_t gateMs;
  uint8_t  scale, root;
  uint32_t serial;   // растёт на каждом новом триггере — по нему
                     // процессоры детектируют факт захвата
  uint8_t  ccValue;  // последнее CC-значение, отданное слотом наружу:
                     // модулям вроде LOGIC надо с чем сравнивать, а
                     // сами CC уходят прямо в MIDI мимо шины
};

// busCur — то, что слоты пишут прямо сейчас; busSnap — снимок с
// начала цикла, из которого читают все ARM. За счёт этого кольцевые
// цепочки (1->2->1) не виснут, а становятся обратной связью с
// задержкой в один цикл.
Signal busCur[NUM_SLOTS];
Signal busSnap[NUM_SLOTS];
// У каждого слота свой канал внешнего входа: A слушает MIDI-канал 1,
// B — 2, C — 3, D — 4. Так с одной клавиатуры можно вести четыре
// разные цепочки, а не одну общую. Ноты на каналах 5..16 игнорируются:
// слота, который бы их слушал, попросту нет.
Signal extSignal[NUM_SLOTS];

// Последнее значение каждого контроллера — и пришедшего извне, и
// отправленного своим же модулем. Одна таблица на оба случая: для
// привязки неважно, крутит CC ручка на клавиатуре или MORPH LFO в
// соседнем слоте, и внутренняя модуляция не требует петли через MIDI.
uint8_t ccVal[128];

// Рантайм-состояние слота.
struct SlotRt {
  uint32_t lastStepTick;
  uint16_t stepIndex;      // счётчик шагов паттерна
  uint16_t playStep;       // шаг, который прозвучал последним — его и
                           // показывает плейхед (stepIndex к этому
                           // моменту уже смотрит на следующий)
  uint8_t  cursor;         // курсор шага в ряду, 0..15
  int8_t   walkDir;        // SKELETON ping-pong: направление чтения паттерна
  uint32_t lastSerial;     // последний обработанный триггер источника 1
  uint32_t lastSerial2;    // то же для источника 2
  uint8_t  capNotes[MAX_VOICES];
  uint8_t  capVel[MAX_VOICES];
  uint8_t  capCount;
  uint32_t capGateMs;
  uint8_t  capScale, capRoot;
  // ARP
  uint8_t  arpPool[MAX_VOICES * 4];
  uint8_t  arpVel[MAX_VOICES * 4];
  uint8_t  arpPoolLen;
  uint8_t  arpIndex;
  // ENV / MLFO
  uint32_t envStart;
  bool     envActive;
  float    lfoPhase;
  // GLIDER
  uint8_t  glidePrev;
  // DUST
  float    dustDrift;
  uint32_t dustLastMs;
  // ADD — скользящая история, когда настоящего аккорда нет
  uint8_t  hist[MAX_VOICES];
  uint8_t  histLen;
  // KEYTRACK: последнее отправленное значение по каждой линии —
  // шлём только при изменении, иначе четыре линии забьют поток.
  uint8_t  ktLast[NUM_COLS];
  // RROBIN: когда освободится каждый голос и чей ход при CYCLE.
  uint32_t voiceFreeAt[4];
  uint8_t  rrNext;
  // SHIFT: сам регистр. Ноты и их силы едут по кругу.
  uint8_t  shReg[16];
  uint8_t  shVel[16];
  uint16_t shRest;        // биты ячеек, ставших паузой в режиме DROP
  uint8_t  shPos;
  uint8_t  shFilled;      // сколько ячеек уже набралось со входа
  // ARP: опора кейтрекинга — первая увиденная нота. От неё считается,
  // насколько арпеджио двигается вслед за клавишей.
  uint8_t  arpAnchor;
  bool     arpAnchorSet;
  // META: наблюдаемый диапазон источника и сглаженное значение.
  float    metaLo, metaHi, metaVal;
  int16_t  metaRawPrev;
  bool     metaSeen;
  uint32_t metaLastMs;
  // LOGIC
  uint32_t lastGridStep;   // тик последнего шага слота-сетки
  bool     sawA, sawB;     // источник выдал ноту с прошлой проверки
  uint8_t  heldNote;       // последняя выданная высота — ею играют
                           // проходы, на которых никто не тригнул
  // визуальный индикатор активности
  uint32_t lastFireMs;
};

SlotRt rt[NUM_SLOTS];

// ---- Модуляция параметров ----
// Смещения, которые МетаПерформер накладывает на чужие параметры.
// Индекс: [слот][страница * 4 + колонка]. Во флеш НЕ пишутся и
// обнуляются каждый цикл — иначе постоянная модуляция затирала бы
// пользовательские настройки и убивала NVS перезаписью.
// Плоское адресное пространство параметров слота. Номера ЗАКРЕПЛЕНЫ,
// а не пересчитываются от набора: иначе смена приложения в целевом
// слоте перетасовала бы все уже выставленные цели.
//   0..3   колонки главной страницы
//   4..7   колонки подстраницы (4+col); 5 у процессора это ARM и он
//          недоступен — модуляция маршрутизации даёт только хаос
//   8      ROOT генератора      9  общий BPM
//   11..17 то, что на экране спрятано под TAP: номера CC у KEYTRACK,
//          каналы выходов у MULTX и RROBIN
constexpr uint8_t MP_MAIN  = 0;
constexpr uint8_t MP_SUB   = 4;
constexpr uint8_t MP_ROOT  = 8;
constexpr uint8_t MP_BPM   = 9;
// Индекс 10 свободен: на нём жил оператор удалённого SIEVE. Не
// переиспользуем, чтобы не сдвинуть MP_EXTRA и то, что за ним.
constexpr uint8_t MP_EXTRA = 11;
// Вторая строка ПЕРВОЙ колонки главной страницы. У генератора sub4 —
// это четвёртая колонка подстраницы (MP_SUB+3), а у процессора совсем
// другой параметр: ROT у SHIFT, деление у NOTE DELAY, образец CC у
// LOGIC. Один индекс на двоих означал бы, что META, целясь в STEP+,
// заодно двигает ROT.
constexpr uint8_t MP_TOPO_Y = MP_EXTRA + 3;   // Y карты TOPOLOGY
constexpr uint8_t MP_SUB4P  = MP_EXTRA + 4;   // sub4 процессора
constexpr uint8_t MP_COUNT = 18;

int16_t modOffset[NUM_SLOTS][MP_COUNT];

// Значение с наложенным смещением. Единая форма для всех параметров,
// независимо от того, где они хранятся.
inline int16_t modAt(uint8_t slot, uint8_t idx, int16_t base, int16_t lo, int16_t hi) {
  int16_t off = modOffset[slot][idx];
  return off ? (int16_t)clampInt(base + off, lo, hi) : base;
}

void clearModOffsets() { memset(modOffset, 0, sizeof(modOffset)); }

// Значение параметра с учётом модуляции. Хранимое значение не
// трогается: смещение добавляется только в момент чтения.
PDesc columnDesc(uint8_t slot, bool sub, uint8_t col);
int16_t* columnValue(uint8_t slot, bool sub, uint8_t col);

int16_t modParam(uint8_t slot, bool sub, uint8_t col) {
  int16_t base = *columnValue(slot, sub, col);
  PDesc d = columnDesc(slot, sub, col);
  return modAt(slot, (uint8_t)((sub ? MP_SUB : MP_MAIN) + col), base, d.lo, d.hi);
}

// Короткая форма для главной страницы — там живут все музыкально
// осмысленные цели, и её читает движок.
inline int16_t modP(uint8_t slot, uint8_t col) {
  const SlotCfg &c = cfg[slot];
  return modAt(slot, (uint8_t)(MP_MAIN + col), c.p[col],
               apps[c.app].p[col].lo, apps[c.app].p[col].hi);
}

// Спрятанные под TAP значения — с точки зрения модуляции такие же
// параметры, как и всё остальное.
inline uint8_t modRate(uint8_t slot)  { return (uint8_t)modAt(slot, MP_SUB + 1, cfg[slot].rate, 0, 4); }
inline uint8_t modScale(uint8_t slot) { return (uint8_t)modAt(slot, MP_SUB + 2, cfg[slot].scale, 0, 9); }
inline uint8_t modRoot(uint8_t slot)  { return (uint8_t)modAt(slot, MP_ROOT, cfg[slot].root, 0, 11); }
inline uint8_t modKtCC(uint8_t slot, uint8_t i) {
  return (uint8_t)modAt(slot, (uint8_t)(MP_EXTRA + i), cfg[slot].ktCC[i], 0, 127);
}
inline uint8_t modOutCh(uint8_t slot, uint8_t i) {
  return (uint8_t)modAt(slot, (uint8_t)(MP_EXTRA + i), cfg[slot].outCh[i], 0, 16);
}
inline int16_t modSub4(uint8_t slot) {
  PDesc d = apps[cfg[slot].app].sub4;
  uint8_t idx = isGenerator(cfg[slot].app) ? (uint8_t)(MP_SUB + 3) : MP_SUB4P;
  return modAt(slot, idx, cfg[slot].sub4, d.lo, d.hi);
}
// Y карты TOPOLOGY: живёт под TAP на первой колонке и до сих пор читался
// напрямую, то есть модуляции не подчинялся вовсе.
inline int16_t modSub2(uint8_t slot) {
  return modAt(slot, MP_TOPO_Y, cfg[slot].sub2, 0, 127);
}


// ============================================================
// 5. ГЛОБАЛЬНОЕ СОСТОЯНИЕ
// ============================================================
struct State {
  uint8_t  currentSlot = 0;
  bool     subPage = false;
  uint8_t  selectedCol = 0;

  bool     appListOpen = false;
  uint8_t  appListSel = 0;
  bool     patchOpen = false;
  uint8_t  patchSel = 0;
  uint32_t patchHoldStart = 0;   // взведение удержания «сохранить»
  bool     patchHolding = false;
  bool     patchErasing = false;   // удержание с TAP стирает, а не сохраняет
  // Слой модуляции: те же четыре колонки, но показывают не значения, а
  // чем их ведут. Это слой поверх текущей страницы, а не третья
  // страница: subPage проверяется в 46 местах, и третье состояние
  // пришлось бы заводить в каждом.
  bool     modLayer = false;
  uint32_t encSWPressAt = 0;
  bool     encSWRotated = false;
  bool     patchSaved = false;   // сохранение уже сработало на этом удержании
  uint32_t pagePressTime = 0;
  bool     pageWasPressed = false;

  uint16_t bpm = 120;          // общий на всё устройство
  float    extBpm = 0;
  bool     extClock = false;

  uint32_t midiTicks = 0;
  uint32_t lastClockMicros = 0;
  uint32_t clockAccumulator = 0;
  uint8_t  clockCount = 0;
  float    bpmSmooth = 0;

  bool     displayDirty = true;
  bool     needSave = false;
  uint32_t lastEditMs = 0;

  bool     playWasPressed = false;
  uint32_t playPressTime = 0;   // отличить короткое нажатие от удержания
  bool     tapDoubleClicked = false;
  uint32_t lastTapReleaseTime = 0;
  // 0 = "предыдущего нажатия нет": так гасится половинка двойного
  // клика, уже израсходованная на другой жест.
  uint32_t encSWLastPress = 0;
} state;

// ============================================================
// 6. КНОПКИ И ДЕБАУНС
// ============================================================
struct Button {
  uint8_t pin;
  bool raw;
  bool lastStable;
  uint32_t lastChange;
  bool processed;
};

Button buttons[4] = {
  {Pins::PLAY, HIGH, HIGH, 0, false},
  {Pins::TAP, HIGH, HIGH, 0, false},
  {Pins::PAGE, HIGH, HIGH, 0, false},
  {Pins::ENC_SW, HIGH, HIGH, 0, false}
};

constexpr uint32_t DEBOUNCE_MS = 35;
// У ENC_SW ход короче, и двойной клик по нему физически быстрее: при
// общем дебаунсе 35мс два клика рискуют слиться в один.
constexpr uint32_t ENC_SW_DEBOUNCE_MS = 15;
constexpr uint32_t DOUBLE_CLICK_TIME = 300;

// ============================================================
// 7. ЭНКОДЕР
// ============================================================
volatile int encoderTicks = 0;
volatile uint8_t lastEncState = 0;

void IRAM_ATTR encoderISR() {
  uint8_t A = digitalRead(Pins::ENC_A);
  uint8_t B = digitalRead(Pins::ENC_B);
  uint8_t s = (A << 1) | B;
  if (s != lastEncState) {
    int8_t dir = 0;
    if ((lastEncState == 0 && s == 1) || (lastEncState == 1 && s == 3) ||
        (lastEncState == 3 && s == 2) || (lastEncState == 2 && s == 0)) dir = 1;
    else if ((lastEncState == 0 && s == 2) || (lastEncState == 2 && s == 3) ||
             (lastEncState == 3 && s == 1) || (lastEncState == 1 && s == 0)) dir = -1;
    if (dir) encoderTicks += dir;
    lastEncState = s;
  }
}

// ============================================================
// 8. MIDI-ВЫВОД И ПЛАНИРОВЩИК НОТ
// ============================================================
void sendCCRaw(uint8_t ch, uint8_t cc, uint8_t val) {
  midi.write((uint8_t)(0xB0 | ((ch - 1) & 0x0F)));
  midi.write(cc & 0x7F);
  midi.write(val & 0x7F);
}
void sendNoteOnRaw(uint8_t ch, uint8_t note, uint8_t vel) {
  midi.write((uint8_t)(0x90 | ((ch - 1) & 0x0F)));
  midi.write(note & 0x7F);
  midi.write(vel & 0x7F);
}
void sendNoteOffRaw(uint8_t ch, uint8_t note) {
  midi.write((uint8_t)(0x80 | ((ch - 1) & 0x0F)));
  midi.write(note & 0x7F);
  midi.write((uint8_t)0);
}

// Единый пул отложенных нот: STRUM, NOTE DELAY, GLIDER и ARP кладут
// сюда будущие события, а не держат собственные таймеры. Note Off
// снимается тем же пулом, поэтому зависших нот не остаётся.
// Пул общий на все слоты, и делится он неравномерно: один NOTE DELAY
// держит до восьми событий, STRUM до двенадцати. При тридцати двух
// пара дилеев со щипком уже переполняла пул, и самое старое событие
// вытеснялось — на слух это проглоченные ноты.
constexpr uint8_t MAX_SCHED = 64;
struct SchedNote {
  bool used, on;
  uint8_t note, vel, ch;
  uint32_t onAt, offAt;
  // Слот, на шину которого нота попадает В МОМЕНТ СТАРТА; -1 — модуль
  // публикует сам. Нужно тем, у кого выход растянут во времени
  // (STRUM, NOTE DELAY, GLIDER): публиковать их разом наперёд значит
  // отдать следующему звену один аккорд вместо череды нот.
  int8_t pub;
};
SchedNote sched[MAX_SCHED];

void scheduleNote(uint8_t note, uint8_t vel, uint8_t ch, uint32_t delayMs, uint32_t gateMs,
                  int8_t pub = -1) {
  // Канал NO больше не повод выбросить событие: наружу оно не уйдёт,
  // но на шину попасть обязано — иначе цепочка за модулем оборвётся.
  if (ch == 0 && pub < 0) return;
  if (gateMs < 5) gateMs = 5;
  uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_SCHED; i++) {
    if (sched[i].used) continue;
    sched[i] = {true, false, note, vel, ch, now + delayMs, now + delayMs + gateMs, pub};
    return;
  }
  // Пул забит — самое старое запланированное событие вытесняется,
  // иначе плотные цепочки просто молча теряли бы новые ноты.
  uint8_t oldest = 0;
  for (uint8_t i = 1; i < MAX_SCHED; i++) if (sched[i].onAt < sched[oldest].onAt) oldest = i;
  if (sched[oldest].on && sched[oldest].ch) sendNoteOffRaw(sched[oldest].ch, sched[oldest].note);
  sched[oldest] = {true, false, note, vel, ch, now + delayMs, now + delayMs + gateMs, pub};
}

// publish() определена ниже, вместе с шиной — планировщику нужен
// только её вызов.
void publish(uint8_t slot, const uint8_t* notes, uint8_t count,
             uint32_t gateMs, uint8_t sc, uint8_t root,
             const uint8_t* vels = nullptr);

// Две одинаковые высоты в одном аккорде — это не полифония, а потеря.
// На один канал уходит два Note On: второй перетриггерит первый голос,
// а первый Note Off погасит оба. Плюс дубль занимает место на шине,
// которого потом не хватает ниже по цепочке. От схлопнувшихся голосов
// оставляем самый громкий, чтобы не потерять акцент.
uint8_t dedupeChord(uint8_t* notes, uint8_t* vels, uint8_t n) {
  uint8_t m = 0;
  for (uint8_t i = 0; i < n; i++) {
    bool dup = false;
    for (uint8_t j = 0; j < m; j++) {
      if (notes[j] != notes[i]) continue;
      if (vels && vels[i] > vels[j]) vels[j] = vels[i];
      dup = true;
      break;
    }
    if (dup) continue;
    notes[m] = notes[i];
    if (vels) vels[m] = vels[i];
    m++;
  }
  return m;
}

void serviceScheduler() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_SCHED; i++) {
    if (!sched[i].used) continue;
    if (!sched[i].on) {
      if ((int32_t)(now - sched[i].onAt) >= 0) {
        if (sched[i].ch) sendNoteOnRaw(sched[i].ch, sched[i].note, sched[i].vel);
        // Публикуем ровно тогда, когда нота зазвучала, а не заранее.
        if (sched[i].pub >= 0) {
          uint8_t p = (uint8_t)sched[i].pub;
          uint8_t one[1] = {sched[i].note};
          uint8_t ov[1] = {sched[i].vel};
          publish(p, one, 1, sched[i].offAt - sched[i].onAt,
                  rt[p].capScale, rt[p].capRoot, ov);
        }
        sched[i].on = true;
      }
    } else if ((int32_t)(now - sched[i].offAt) >= 0) {
      if (sched[i].ch) sendNoteOffRaw(sched[i].ch, sched[i].note);
      sched[i].used = false;
    }
  }
}

void flushScheduler() {
  for (uint8_t i = 0; i < MAX_SCHED; i++) {
    if (sched[i].used && sched[i].on && sched[i].ch) sendNoteOffRaw(sched[i].ch, sched[i].note);
    sched[i].used = false;
    sched[i].on = false;
  }
}

// Генератор шлёт на свой канал; 0 означает «наружу не шлём».

// ============================================================
// 9. ШИНА И ARM-ЗАХВАТ
// ============================================================
void publish(uint8_t slot, const uint8_t* notes, uint8_t count,
             uint32_t gateMs, uint8_t sc, uint8_t root,
             const uint8_t* vels) {
  Signal &s = busCur[slot];
  uint8_t n = count > MAX_VOICES ? MAX_VOICES : count;
  for (uint8_t i = 0; i < n; i++) {
    s.notes[i] = notes[i];
    // Аргумент необязателен: модули переводятся на velocity по одному,
    // а не все разом. Без него — прежнее поведение.
    s.vel[i] = vels ? vels[i] : DEFAULT_VEL;
  }
  // Шина не несёт дублей — см. dedupeChord. Это единственное место, где
  // на неё что-то попадает, поэтому проверка тут одна на все модули.
  s.count = dedupeChord(s.notes, s.vel, n);
  s.gateMs = gateMs;
  s.scale = sc;
  s.root = root;
  s.serial++;
  rt[slot].lastFireMs = millis();
}

// Слот не может быть армлен сам на себя: это обратная связь без
// внятного смысла и постоянный источник путаницы в цепочке. Значение,
// попавшее на собственный слот, перешагивается в сторону вращения.
uint8_t armSkipSelf(uint8_t slot, int v, int dir, int lo, int hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  if (v != (int)slot) return (uint8_t)v;
  int step = (dir >= 0) ? 1 : -1;
  int t = v + step;
  if (t < lo || t > hi) t = v - step;   // упёрлись в край — шагаем обратно
  return (uint8_t)clampInt(t, lo, hi);
}

// which: 0 — первый источник, 1 — второй. nullptr означает, что
// второй источник выключен (NO).
// Отправка CC от имени слота: помимо MIDI кладёт значение на шину,
// чтобы его могли сравнивать модули ниже по цепочке.
void emitCC(uint8_t slot, uint8_t cc, uint8_t val) {
  // Своё значение кладём в общую таблицу: его увидят привязки, даже
  // если наружу модуль молчит с каналом NO.
  if (cc < 128) ccVal[cc] = val;
  // На шину значение кладём всегда: с каналом NO модуль перестаёт слать
  // MIDI, но остаётся источником для тех, кто на него армлен.
  if (cfg[slot].midiCh) sendCCRaw(cfg[slot].midiCh, cc, val);
  busCur[slot].ccValue = val;
}

const Signal* armSource(uint8_t slot, uint8_t which) {
  uint8_t a = which ? cfg[slot].arm2 : cfg[slot].arm;
  if (a >= ARM_NONE) return nullptr;
  if (a == slot) return nullptr;          // сам на себя — источника нет
  if (a >= NUM_SLOTS) return &extSignal[slot];
  // META в шину ничего не кладёт — он модулирует, а не обрабатывает.
  // Армленный на него слот берёт то, что читает сам META: модуль
  // становится прозрачным, а не немым звеном посреди цепочки. Число
  // прыжков ограничено, поэтому цепочка из META не зацикливается.
  for (uint8_t hop = 0; hop < NUM_SLOTS && (AppId)cfg[a].app == AppId::META; hop++) {
    uint8_t nxt = cfg[a].arm;
    if (nxt >= ARM_NONE || nxt == a) return nullptr;
    if (nxt >= NUM_SLOTS) return &extSignal[slot];
    a = nxt;
  }
  if ((AppId)cfg[a].app == AppId::META) return nullptr;   // кольцо из META
  return &busSnap[a];
}

// Ищет генератор в начале цепочки от одного из источников слота.
// Тот же проход по ARM, что рисует цепочку в шапке, с той же защитой
// от колец. -1 означает "генератора в этой ветке нет".
int8_t chainGenerator(uint8_t slot, uint8_t which) {
  uint8_t src = which ? cfg[slot].arm2 : cfg[slot].arm;
  if (src >= NUM_SLOTS) return -1;             // EXT или NO
  uint8_t visited = (uint8_t)(1u << slot);
  uint8_t cur = src;
  for (uint8_t guard = 0; guard <= NUM_SLOTS; guard++) {
    if (visited & (1u << cur)) return -1;      // кольцо
    visited |= (uint8_t)(1u << cur);
    if (isGenerator(cfg[cur].app)) return (int8_t)cur;
    if (!isProcessor(cfg[cur].app)) return -1; // упёрлись в пустой слот
    uint8_t nxt = cfg[cur].arm;
    if (nxt >= NUM_SLOTS) return -1;
    cur = nxt;
  }
  return -1;
}

// Сетка LOGIC: сперва генератор в начале цепочки от A, затем от B.
// -1 означает, что генератора нет ни там, ни там — тогда включается
// внутренняя сетка 1/16.
int8_t logicGridSlot(uint8_t slot) {
  int8_t g = chainGenerator(slot, 0);
  return (g >= 0) ? g : chainGenerator(slot, 1);
}

// Модули, не знающие о полифонии, при захваченном аккорде берут
// самую нижнюю ноту.
uint8_t lowestOf(const SlotRt &r) {
  if (r.capCount == 0) return 0;
  uint8_t lo = r.capNotes[0];
  for (uint8_t i = 1; i < r.capCount; i++) if (r.capNotes[i] < lo) lo = r.capNotes[i];
  return lo;
}
uint8_t highestOf(const SlotRt &r) {
  if (r.capCount == 0) return 0;
  uint8_t hi = r.capNotes[0];
  for (uint8_t i = 1; i < r.capCount; i++) if (r.capNotes[i] > hi) hi = r.capNotes[i];
  return hi;
}

// ============================================================
// 10. ГЕНЕРАТОРЫ
// ============================================================
// ---- TOPOLOGY: карта ритмов ----
// Механика взята у Mutable Instruments Grids: узлы карты хранят силу
// каждого шага, X и Y ведут между узлами с плавным переходом, а
// плотность работает порогом. Сама таблица здесь СВОЯ: у Grids она под
// GPL v3, и её перенос сделал бы копилефтным весь проект.
//
// Значение 0..255 это одновременно вероятность и акцент — из него же
// берётся velocity, поэтому сильные доли звучат громче без отдельного
// параметра.
//
// Сетка 5x5, как в оригинале: у нечётной есть точный центр, и X=64,
// Y=64 попадают ровно на узел, а не в размытие между четырьмя. Оси
// меняют СИЛЬНЫЕ доли, а не украшения: слабые значения не переходят
// порог при обычной плотности, и карта из них выглядела бы застывшей.
constexpr uint8_t TOPO_NX = 5, TOPO_NY = 5, TOPO_STEPS = 16, TOPO_TRACKS = 3;

const uint8_t topoMap[TOPO_NY][TOPO_NX][TOPO_TRACKS][TOPO_STEPS] = {
 {
   {{255,  0,  0,  0,  0,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0,  0,  0,240,  0,  0,  0},
    {225,  0,  0,  0,175,  0,  0,  0,225,  0,  0,  0,175,  0,  0,  0}},
   {{255,  0,  0,  0,185,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0, 95,  0,240,  0,  0,  0},
    {225,  0,  0,  0,175,  0,  0,  0,225,  0,  0,  0,175,  0,  0,  0}},
   {{255,  0,  0,  0,185,  0,  0,  0,200,  0,  0,  0,170,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,110,  0,  0,  0,110,  0,240,  0,  0,  0},
    {225,  0,120,  0,175,  0,120,  0,225,  0,120,  0,175,  0,120,  0}},
   {{255,  0,120,  0,  0,  0,200,  0,  0,  0,185,  0,  0,  0,170,  0},
    {  0,  0,125,  0,240,  0,125,  0,  0,  0,125,  0,240,  0,  0,  0},
    {225,  0,120,  0,175,  0,120,  0,225,  0,120,  0,175,  0,120,  0}},
   {{255,  0,130,200,  0,  0,185,  0,  0,  0,130,170,  0,  0,130,  0},
    {  0,  0,140,  0,240,  0,140,  0,  0,  0,140,  0,240,  0,140,  0},
    {225, 70,120, 70,175, 70,120, 70,225, 70,120, 70,175, 70,120, 70}}
 },
 {
   {{255,  0,  0,  0,  0,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0,  0,  0,240,  0,  0,  0},
    {  0,  0,  0,  0,175,  0,  0,  0,  0,  0,  0,  0,175,  0,  0,  0}},
   {{255,  0,  0,  0,185,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0, 95,  0,240,  0,  0,  0},
    {  0,  0,  0,  0,175,  0,  0,  0,  0,  0,  0,  0,175,  0,  0,  0}},
   {{255,  0,  0,  0,185,  0,  0,  0,200,  0,  0,  0,170,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,110,  0,  0,  0,110,  0,240,  0,  0,  0},
    {  0,  0,120,  0,175,  0,120,  0,  0,  0,120,  0,175,  0,120,  0}},
   {{255,  0,120,  0,  0,  0,200,  0,  0,  0,185,  0,  0,  0,170,  0},
    {  0,  0,125,  0,240,  0,125,  0,  0,  0,125,  0,240,  0,  0,  0},
    {  0,  0,120,  0,175,  0,120,  0,  0,  0,120,  0,175,  0,120,  0}},
   {{255,  0,130,200,  0,  0,185,  0,  0,  0,130,170,  0,  0,130,  0},
    {  0,  0,140,  0,240,  0,140,  0,  0,  0,140,  0,240,  0,140,  0},
    {  0, 70,120, 70,175, 70,120, 70,  0, 70,120, 70,175, 70,120, 70}}
 },
 {
   {{255,  0,  0,  0,  0,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0,  0,  0,240,  0,  0,  0},
    { 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0}},
   {{255,  0,  0,  0,  0,185,  0,  0,200,  0,  0,  0,  0,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0, 95,  0,240,  0,  0,  0},
    { 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0}},
   {{255,  0,  0,  0,  0,185,  0,  0,200,  0,  0,  0,170,  0,  0,  0},
    {  0,  0,  0,  0,240,  0,110,  0,  0,  0,110,  0,240,  0,  0,  0},
    { 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0}},
   {{255,  0,120,  0,  0,  0,200,  0,  0,  0,120,185,  0,  0,170,  0},
    {  0,  0,125,  0,240,  0,125,  0,  0,  0,125,  0,240,110,  0,110},
    { 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0, 95,  0,215,  0}},
   {{255,  0,130,200,  0,  0,  0,185,  0,  0,130,170,  0,  0,130,  0},
    {  0,  0,140,  0,240,  0,140,  0,  0,  0,140,  0,240,110,140,110},
    { 95, 70,215, 70, 95, 70,215, 70, 95, 70,215, 70, 95, 70,215, 70}}
 },
 {
   {{255,  0,  0,  0,  0,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,150},
    {  0,  0,  0,240,  0,  0,  0,  0,  0,  0,  0,240,  0,  0,  0,  0},
    {  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0}},
   {{255,  0,  0,  0,  0,185,  0,  0,200,  0,  0,  0,  0,  0,  0,150},
    {  0,  0,  0,240,  0,  0,  0,  0,  0,  0, 95,240,  0,  0,  0,  0},
    {  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0}},
   {{255,  0,  0,  0,  0,185,  0,  0,200,  0,  0,  0,170,  0,  0,150},
    {  0,  0,  0,240,  0,  0,110,  0,  0,  0,110,240,  0,  0,  0,  0},
    {  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0}},
   {{255,  0,120,  0,  0,  0,200,  0,  0,  0,120,185,  0,  0,170,150},
    {  0,  0,125,240,  0,  0,125,  0,  0,  0,125,240,  0,110,  0,110},
    {  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0,  0,  0,205,  0}},
   {{255,  0,130,200,  0,  0,  0,185,  0,  0,130,170,  0,  0,130,150},
    {  0,  0,140,240,  0,  0,140,  0,  0,  0,140,240,  0,110,140,110},
    {  0, 70,205, 70,  0, 70,205, 70,  0, 70,205, 70,  0, 70,205, 70}}
 },
 {
   {{255,  0,  0,  0,  0,  0,  0,  0,200,  0,  0,  0,  0,  0,  0,150},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0,  0,  0,  0,  0,240,  0},
    { 80,  0,  0,190, 80,  0,  0,190, 80,  0,  0,190, 80,  0,  0,190}},
   {{255,  0,  0,  0,  0,  0,  0,185,200,  0,  0,  0,  0,  0,  0,150},
    {  0,  0,  0,  0,240,  0,  0,  0,  0,  0, 95,  0,  0,  0,240,  0},
    { 80,  0,  0,190, 80,  0,  0,190, 80,  0,  0,190, 80,  0,  0,190}},
   {{255,  0,  0,  0,  0,  0,  0,185,200,  0,  0,  0,170,  0,  0,150},
    {  0,  0,  0,  0,240,  0,110,  0,  0,  0,110,  0,  0,  0,240,  0},
    { 80,  0, 80,190, 80,  0, 80,190, 80,  0, 80,190, 80,  0, 80,190}},
   {{255,  0,120,  0,  0,  0,200,  0,  0,  0,120,  0,  0,185,170,150},
    {  0,  0,125,  0,240,  0,125,  0,  0,  0,125,  0,  0,110,240,110},
    { 80,  0, 80,190, 80,  0, 80,190, 80,  0, 80,190, 80,  0, 80,190}},
   {{255,  0,130,200,  0,  0,  0,  0,  0,185,130,170,  0,  0,130,150},
    {  0,  0,140,  0,240,  0,140,  0,  0,  0,140,  0,  0,110,240,110},
    { 80, 70, 80,190, 80, 70, 80,190, 80, 70, 80,190, 80, 70, 80,190}}
 }
};

// Узлы оси карты в единицах 0..127 и обратный перевод. Сетка задаёт
// удобные точки, а не разрешение: хранится по-прежнему 0..127, поэтому
// модуляция ходит между узлами плавно и ничего не теряет.
inline uint8_t topoNodeVal(int k, uint8_t n) {
  return (uint8_t)clampInt((k * 127 + (n - 1) / 2) / (n - 1), 0, 127);
}
inline uint8_t topoNodeIdx(int v, uint8_t n) {
  return (uint8_t)clampInt((clampInt(v, 0, 127) * (n - 1) + 63) / 127, 0, n - 1);
}
// Шаг энкодера по оси карты. Между соседними узлами 32 клика — руками
// такую ось не настроить, поэтому энкодер прыгает сразу на узел.
inline uint8_t topoNodeStep(int cur, int steps, uint8_t n) {
  return topoNodeVal(clampInt(topoNodeIdx(cur, n) + steps, 0, n - 1), n);
}

// Сила шага дорожки в текущей точке карты. Между узлами — билинейное
// смешивание, поэтому рисунок перетекает плавно, а не переключается.
uint8_t topoLevel(uint8_t slot, uint8_t track, uint8_t step) {
  int32_t fx = (int32_t)clampInt(modP(slot, 0), 0, 127) * (TOPO_NX - 1) * 256 / 127;
  int32_t fy = (int32_t)clampInt(modSub2(slot), 0, 127) * (TOPO_NY - 1) * 256 / 127;
  uint8_t x0 = (uint8_t)(fx >> 8), y0 = (uint8_t)(fy >> 8);
  if (x0 > TOPO_NX - 1) x0 = TOPO_NX - 1;
  if (y0 > TOPO_NY - 1) y0 = TOPO_NY - 1;
  uint8_t x1 = (x0 + 1 < TOPO_NX) ? (uint8_t)(x0 + 1) : x0;
  uint8_t y1 = (y0 + 1 < TOPO_NY) ? (uint8_t)(y0 + 1) : y0;
  int32_t tx = fx & 255, ty = fy & 255;
  uint8_t s = (uint8_t)(step % TOPO_STEPS);
  int32_t a = topoMap[y0][x0][track][s], b = topoMap[y0][x1][track][s];
  int32_t cc = topoMap[y1][x0][track][s], d = topoMap[y1][x1][track][s];
  int32_t top = a + ((b - a) * tx >> 8);
  int32_t bot = cc + ((d - cc) * tx >> 8);
  return (uint8_t)clampInt((int)(top + ((bot - top) * ty >> 8)), 0, 255);
}

// Звучит ли шаг дорожки и с какой силой. Плотность работает порогом:
// на нуле дорожка молчит, на максимуме проходит почти всё.
bool topoHit(uint8_t slot, uint8_t track, uint8_t step, uint8_t &vel) {
  int dens = clampInt(modP(slot, (uint8_t)(1 + track)), 0, 127);
  if (!dens) return false;
  int v = topoLevel(slot, track, step);
  int chaos = clampInt(modSub4(slot), 0, 127);
  if (chaos) v += (int)random(-chaos, chaos + 1);
  v = clampInt(v, 0, 255);
  if (v <= 255 - dens * 2) return false;
  // Velocity из той же величины: акцент уже заложен в карту.
  vel = (uint8_t)clampInt(40 + v * 87 / 255, 1, 127);
  return true;
}

// Тот же порог, что и в topoHit, но без хаоса: случайность разыгрывается
// в момент события, и предсказать её на экране всё равно нельзя — а
// мигающий от неё рисунок только мешал бы читать паттерн.
bool topoHitShown(uint8_t slot, uint8_t track, uint8_t step) {
  int dens = clampInt(modP(slot, (uint8_t)(1 + track)), 0, 127);
  if (!dens) return false;
  return topoLevel(slot, track, step) > 255 - dens * 2;
}

// Евклидов ритм считается дешёвой модульной аппроксимацией — это
// сознательное упрощение вместо полного алгоритма Бьорклунда.
bool euclidHit(uint16_t i, uint8_t steps, uint8_t pulses, uint8_t rotate) {
  if (steps == 0 || pulses == 0) return false;
  if (pulses >= steps) return true;
  uint16_t idx = (i + rotate) % steps;
  return ((uint32_t)idx * pulses) % steps < pulses;
}


// Длина паттерна в шагах: на ней счётчик шага заворачивается, её же
// показывает ряд шагов.
uint8_t patternLen(uint8_t slot) {
  const SlotCfg &c = cfg[slot];
  switch ((AppId)c.app) {
    case AppId::BASIC:
    case AppId::SKELETON:  return (uint8_t)clampInt(modSub4(slot), 1, 16);
    case AppId::EUCLIDIAN: return (uint8_t)clampInt(modP(slot, 0), 1, 16);
    case AppId::TOPOLOGY:  return TOPO_STEPS;
    default:               return 16;
  }
}

// Значение параметра col на конкретном шаге: если колонка на этом
// шаге затригована — играет запомненное шагом значение, иначе общий
// параметр слота. Это и есть "триг сохраняет выбранный параметр".
int16_t paramAtStep(uint8_t slot, uint8_t col, uint16_t absStep) {
  const SlotCfg &c = cfg[slot];
  uint8_t k = absStep % 16;
  // Модуляция накладывается и на общий параметр, и на запомненное
  // шагом значение: иначе затригованные шаги выпадали бы из-под неё.
  int16_t v = (c.lane[col] & (1u << k)) ? c.stepVal[k][col] : c.p[col];
  int16_t off = modOffset[slot][col];
  if (off) v = (int16_t)clampInt(v + off, apps[c.app].p[col].lo, apps[c.app].p[col].hi);
  return v;
}

inline bool laneOn(const SlotCfg &c, uint8_t col, uint16_t absStep) {
  return c.lane[col] & (1u << (absStep % 16));
}

// Есть ли хоть один триг на шаге — любой лейн заставляет шаг звучать
// поверх алгоритма.
bool anyLaneOn(const SlotCfg &c, uint16_t absStep) {
  for (uint8_t col = 0; col < NUM_COLS; col++) if (laneOn(c, col, absStep)) return true;
  return false;
}

// Единственный источник правды о том, есть ли трига на шаге: по нему
// и играет генератор, и рисуется ряд шагов — картинка не может
// разойтись со звуком. TRG CHANCE у EUCLID сюда не входит: он
// бросается в момент срабатывания и заранее не известен.
bool generatorHit(uint8_t slot, uint16_t absStep) {
  const SlotCfg &c = cfg[slot];
  switch ((AppId)c.app) {
    case AppId::TOPOLOGY: {
      // Ряд показывает дорожку ВЫБРАННОЙ колонки: крутишь X и видишь,
      // как её рисунок перетекает. Первая колонка это точка на карте,
      // ей соответствует первая дорожка.
      uint8_t trk = (state.selectedCol == 0) ? 0 : (uint8_t)(state.selectedCol - 1);
      uint8_t vel;
      return topoHit(slot, trk, (uint8_t)absStep, vel) || anyLaneOn(c, absStep);
    }
    case AppId::EUCLIDIAN: {
      uint8_t steps = (uint8_t)clampInt(paramAtStep(slot, 0, absStep), 1, 16);
      uint8_t pulses = (uint8_t)clampInt(paramAtStep(slot, 1, absStep), 0, steps);
      uint8_t rot = (uint8_t)(clampInt(paramAtStep(slot, 2, absStep), 0, 15) % steps);
      if (euclidHit(absStep, steps, pulses, rot)) return true;
      break;
    }
    case AppId::BASIC:
    case AppId::SKELETON:
      // Фигурные генераторы бьют на каждом шаге.
      return true;
    default:
      return false;
  }
  return anyLaneOn(c, absStep);
}

// Добавляет голос в аккорд шага. Если после квантования он падает на
// уже занятую высоту — уходит на ступень выше: сколько нот выставлено,
// столько обязано и звучать. Иначе лад молча съедал голос, и это было
// не видно ни на экране, ни в параметрах — только на слух.
uint8_t addVoice(uint8_t* out, uint8_t n, int16_t pitch, uint8_t sc, uint8_t root) {
  uint8_t v = quantizeToScale(pitch, sc, root);
  for (uint8_t guard = 0; guard < 12; guard++) {
    bool clash = false;
    for (uint8_t j = 0; j < n; j++) if (out[j] == v) { clash = true; break; }
    if (!clash) break;
    uint8_t up = transposeDegrees(v, 1, sc, root);
    if (up == v) break;              // выше ступеней нет
    v = up;
  }
  out[n] = v;
  return (uint8_t)(n + 1);
}

void fireGenerator(uint8_t slot) {
  SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];
  uint32_t stepMs = 0;
  {
    uint16_t bpm = state.extClock && state.extBpm > 0 ? (uint16_t)state.extBpm : state.bpm;
    if (bpm < 20) bpm = 20;
    // Длина шага = длительность четверти * (ticksPerStep / 24).
    stepMs = (uint32_t)((60000.0f / bpm) * rateTicks[modRate(slot)] / 24.0f);
  }

  // TOPOLOGY — единственный генератор, выдающий несколько нот за шаг,
  // поэтому у него свой путь: три дорожки бьют независимо и уходят
  // на шину аккордом.
  if ((AppId)c.app == AppId::TOPOLOGY) {
    uint8_t out[TOPO_TRACKS], outVel[TOPO_TRACKS];
    uint8_t n = 0;
    uint32_t gate = stepMs / 2;
    bool three = (clampInt(c.sub3, 0, 1) == 1);
    for (uint8_t t = 0; t < TOPO_TRACKS; t++) {
      uint8_t vel;
      if (!topoHit(slot, t, (uint8_t)r.stepIndex, vel)) continue;
      uint8_t note = (uint8_t)clampInt(c.ktCC[t], 0, 127);
      // 1CH — все дорожки на один канал разными нотами; 3CH — подряд
      // от базового, ноты те же.
      uint8_t ch = three ? (uint8_t)clampInt(c.midiCh + t, 0, 16) : c.midiCh;
      scheduleNote(note, vel, ch, 0, gate);
      outVel[n] = vel;
      out[n++] = note;
    }
    r.playStep = r.stepIndex;
    r.stepIndex = (uint16_t)((r.stepIndex + 1) % TOPO_STEPS);
    // Акценты карты уходят и на шину: их видят модули ниже.
    if (n) publish(slot, out, n, gate, c.scale, c.root, outVel);
    return;
  }

  bool hit = generatorHit(slot, r.stepIndex);
  // Путь выдачи несёт до трёх нот: BASIC и SKELETON умеют отдавать
  // свою фигуру аккордом, а не по ноте за шаг.
  uint8_t outN[3];
  uint8_t outCount = 0;
  uint8_t pitch = 0;
  uint32_t gate = stepMs / 2;

  switch ((AppId)c.app) {
    case AppId::BASIC: {
      // Сколько нот выставлено на шаге, столько и уходит в шину.
      // NOTE1 — основной голос, он звучит всегда; NOTE2 и NOTE3
      // добавляются на тех шагах, где на них поставлен триг.
      uint8_t trig = (uint8_t)clampInt(paramAtStep(slot, 3, r.stepIndex), 1, 16);
      gate = stepMs * trig;
      uint8_t k = (uint8_t)(r.stepIndex % 16);
      uint8_t sc = modScale(slot), rt_ = modRoot(slot);
      outCount = addVoice(outN, outCount, paramAtStep(slot, 0, r.stepIndex), sc, rt_);
      for (uint8_t j = 1; j < 3; j++)
        if (c.lane[j] & (uint16_t)(1u << k))
          outCount = addVoice(outN, outCount, paramAtStep(slot, j, r.stepIndex), sc, rt_);
      break;
    }
    case AppId::SKELETON: {
      // То же правило, что у BASIC, но ступенями лада от опорной ноты:
      // ROOT звучит всегда, INTV2 и INTV3 добавляются там, где на них
      // стоит триг. Транспонирование ROOT двигает всю фигуру целиком.
      uint8_t k = (uint8_t)(r.stepIndex % 16);
      uint8_t sc = modScale(slot), rt_ = modRoot(slot);
      int16_t rootDeg = paramAtStep(slot, 0, r.stepIndex);
      outCount = addVoice(outN, outCount, transposeDegrees(rootDeg, 0, sc, rt_), sc, rt_);
      for (uint8_t j = 1; j < 3; j++)
        if (c.lane[j] & (uint16_t)(1u << k))
          outCount = addVoice(outN, outCount,
                              transposeDegrees(rootDeg, paramAtStep(slot, j, r.stepIndex), sc, rt_),
                              sc, rt_);
      break;
    }
    case AppId::EUCLIDIAN: {
      // TRG CHANCE — вероятность, что уже выпавший хит реально сыграет.
      // Затригованный вручную шаг ей не подчиняется: раз его поставили
      // руками, он должен звучать наверняка.
      if (hit && !anyLaneOn(c, r.stepIndex) && modSub4(slot) < 127 && random(0, 128) > modSub4(slot)) hit = false;
      pitch = quantizeToScale(paramAtStep(slot, 3, r.stepIndex), modScale(slot), modRoot(slot));
      break;
    }
    default: return;
  }

  r.playStep = r.stepIndex;
  if ((AppId)c.app == AppId::SKELETON) {
    // MODE ведёт по паттерну: направление чтения шагов. Слышно сразу,
    // потому что на разных шагах стоят разные ноты — перебирать три
    // ноты одной фигуры было нечего, оттого прежние режимы и звучали
    // одинаково.
    uint8_t n = patternLen(slot);
    int cur = (int)(r.stepIndex % n);
    switch ((uint8_t)clampInt(modP(slot, 3), 0, 3)) {
      case 1:                                            // ping-pong
        if (n <= 1) cur = 0;
        else if (r.walkDir >= 0) {
          if (cur + 1 >= n) { r.walkDir = -1; cur = n - 2; } else cur++;
        } else {
          if (cur == 0) { r.walkDir = 1; cur = 1; } else cur--;
        }
        break;
      case 2: cur = (int)random(0, n); break;                          // random
      case 3: cur = (cur + (int)random(-1, 2) + n) % n; break;         // walk
      default: cur = (cur + 1) % n; break;                             // loop
    }
    r.stepIndex = (uint16_t)cur;
    // Счётчик шагов крутим и в байпасе — на нём держатся сетки LOGIC.
    if (c.bypass) return;
  } else {
    r.stepIndex++;
    if (c.bypass) {
      r.stepIndex %= patternLen(slot);
      return;
    }
    // Паттерн заворачивается на своей длине — тогда ряд шагов и есть
    // весь паттерн целиком, без невидимых шагов за краем.
    r.stepIndex %= patternLen(slot);
  }
  if (!hit) return;

  // Публикуем на шину всегда — даже с CH = NO генератор остаётся
  // источником для процессоров ниже по цепочке.
  if (outCount == 0) { outN[0] = pitch; outCount = 1; }
  // До MIDI дубль тоже не должен доходить: иначе два Note On на одном
  // канале, и гейт первого гасит второй.
  outCount = dedupeChord(outN, nullptr, outCount);
  for (uint8_t i = 0; i < outCount; i++)
    if (c.midiCh) scheduleNote(outN[i], DEFAULT_VEL, c.midiCh, 0, gate);
  publish(slot, outN, outCount, gate, modScale(slot), modRoot(slot));
}

// ============================================================
// 11. ПРОЦЕССОРЫ
// ============================================================
// Морфинг формы волны для MLFO: SHAPE задаёт базу, MORPH её
// переформирует (на sine "+" подмешивает гармоники / "-" клиппинг с
// шумом; на triangle скашивает к пиле; на square это ширина импульса).
float computeLFO(float phase, uint8_t type, int morph) {
  float p = phase;
  while (p > TWO_PI) p -= TWO_PI;
  while (p < 0) p += TWO_PI;
  float m = clampInt(morph, -64, 64) / 64.0f;
  switch (type) {
    case 0: {
      float s = sin(p);
      if (m > 0) {
        float h = 0.5f * sin(2 * p) + 0.3f * sin(3 * p) + 0.2f * sin(4 * p);
        return s * (1 - m) + h * m;
      } else if (m < 0) {
        float amt = -m;
        float driven = s * (1 + amt * 6.0f);
        driven = driven > 1 ? 1 : (driven < -1 ? -1 : driven);
        float noise = random(-1000, 1001) / 1000.0f;
        return driven * (1 - amt * 0.6f) + noise * (amt * 0.6f);
      }
      return s;
    }
    case 1: {
      float t = p / TWO_PI;
      float tri = 4.0f * fabs(t - floor(t + 0.5f)) - 1.0f;
      if (m == 0) return tri;
      float saw = (m < 0) ? (2 * t - 1) : (1 - 2 * t);
      float k = fabs(m);
      return tri * (1 - k) + saw * k;
    }
    default: {
      float duty = 0.5f + m * 0.45f;
      float t = p / TWO_PI;
      return (t - floor(t)) < duty ? 1.0f : -1.0f;
    }
  }
}

float envShapeCurve(float x, uint8_t func) {
  if (x < 0) x = 0;
  if (x > 1) x = 1;
  switch (func) {
    case 1:  return x * x;                       // exp
    case 2:  return sqrt(x);                     // log
    case 3:  return x * x * (3.0f - 2.0f * x);   // s-curve
    default: return x;                           // linear
  }
}

// Арифметика MATH. Деление и остаток на нуле пропускают A без
// изменений — глушить шаг тут было бы хуже, чем ничего не сделать.
int mathApply(uint8_t op, int a, int b) {
  switch (op) {
    case 0:  return a + b;
    case 1:  return a - b;
    case 2:  return b - a;
    case 3:  return a * b;
    case 4:  return b ? a / b : a;
    case 5:  return (a + b) / 2;
    case 6:  return b ? a % b : a;
    case 7:  return a < b ? a : b;
    default: return a > b ? a : b;
  }
}

// Что делать с результатом, вышедшим за диапазон.
int applyLim(int v, int lo, int hi, uint8_t lim) {
  if (v >= lo && v <= hi) return v;
  int span = hi - lo + 1;
  if (lim == 1) {                       // WRAP — по кругу
    int t = (v - lo) % span;
    if (t < 0) t += span;
    return lo + t;
  }
  if (lim == 2) {                       // FOLD — отражением от границ
    for (uint8_t guard = 0; guard < 8 && (v < lo || v > hi); guard++) {
      if (v < lo) v = lo + (lo - v);
      if (v > hi) v = hi - (v - hi);
    }
    return clampInt(v, lo, hi);
  }
  return clampInt(v, lo, hi);           // CLAMP
}

// Захват: снимок источников + STEP+ (сдвиг именно захваченной ноты,
// до того как модуль сделает с ней что-то своё).
//
// Источников два, и ноты обоих сливаются в один захваченный аккорд —
// именно так два генератора складываются в общий материал для ARP или
// STRUM. Повторяющиеся высоты отбрасываются, лишние сверх MAX_VOICES
// не влезают. Лад и длину гейта берём у первого источника, где есть
// ноты: смешивать два лада в одном захвате нечем.
void captureFrom(uint8_t slot, const Signal* a, const Signal* b) {
  SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];
  const Signal* srcs[2] = {a, b};
  r.capCount = 0;
  for (uint8_t k = 0; k < 2; k++) {
    const Signal* s = srcs[k];
    if (!s) continue;
    for (uint8_t i = 0; i < s->count && r.capCount < MAX_VOICES; i++) {
      // В байпасе STEP+ не применяем: он такая же обработка, как и всё
      // остальное, а провод обязан отдавать вход нетронутым.
      int8_t xp = c.bypass ? 0 : c.stepPlus;
      uint8_t n = (xp == 0) ? s->notes[i]
                            : transposeDegrees(s->notes[i], xp, s->scale, s->root);
      bool dup = false;
      for (uint8_t j = 0; j < r.capCount; j++) if (r.capNotes[j] == n) dup = true;
      if (!dup) { r.capVel[r.capCount] = s->vel[i]; r.capNotes[r.capCount++] = n; }
    }
  }
  // Момент приёма — по нему гаснет рамка на странице. Отмечаем именно
  // здесь, а не в publish(): ENV и MORPH LFO наружу нот не отдают,
  // но захват у них есть.
  r.lastFireMs = millis();
  const Signal* lead = (a && a->count) ? a : ((b && b->count) ? b : a);
  r.capGateMs = (lead && lead->gateMs) ? lead->gateMs : 120;
  r.capScale = lead ? lead->scale : 0;
  r.capRoot = lead ? lead->root : 0;
  // Скользящая история последних захваченных нот — источник LOWEST/
  // HIGHEST для ADD, когда настоящего аккорда нет.
  if (r.capCount) {
    for (uint8_t i = MAX_VOICES - 1; i > 0; i--) r.hist[i] = r.hist[i - 1];
    r.hist[0] = r.capNotes[0];
    if (r.histLen < MAX_VOICES) r.histLen++;
  }
}

// Собирает пул нот для ARP/STRUM: сначала реальные тона захваченного
// аккорда, затем октавные дубли по остатку слотов.
uint8_t buildPool(const SlotRt &r, uint8_t octaves, uint8_t* out, uint8_t maxLen,
                  uint8_t* outVel = nullptr) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < r.capCount && n < maxLen; i++) {
    if (outVel) outVel[n] = r.capVel[i];
    out[n++] = r.capNotes[i];
  }
  if (n == 0) return 0;
  uint8_t base = n;
  for (uint8_t o = 1; o <= octaves && n < maxLen; o++) {
    for (uint8_t i = 0; i < base && n < maxLen; i++) {
      int v = out[i] + 12 * o;
      // Октавный дубль наследует силу своего оригинала.
      if (v <= 127) { if (outVel) outVel[n] = outVel ? outVel[i] : DEFAULT_VEL; out[n++] = (uint8_t)v; }
    }
  }
  return n;
}

void fireArpStep(uint8_t slot);

void onCapture(uint8_t slot) {
  SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];
  if (r.capCount == 0) return;
  uint8_t ch = c.midiCh;

  switch ((AppId)c.app) {
    case AppId::ARP:
      r.arpPoolLen = buildPool(r, (uint8_t)clampInt(modP(slot, 2), 0, 3),
                               r.arpPool, sizeof(r.arpPool), r.arpVel);
      r.arpIndex = 0;
      if (!r.arpAnchorSet) { r.arpAnchor = lowestOf(r); r.arpAnchorSet = true; }
      // Своей свободной сетки у ARP больше нет: отсчёт начинается от
      // пришедшей ноты, а RATE задаёт скорость самого арпеджио. Первый
      // шаг звучит сразу, поэтому нажатая клавиша слышна без задержки.
      r.lastStepTick = state.midiTicks;
      fireArpStep(slot);
      break;

    case AppId::STRUM: {
      uint8_t pool[MAX_VOICES * 4];
      uint8_t pv[MAX_VOICES * 4];
      uint8_t n = buildPool(r, (uint8_t)clampInt(modP(slot, 1), 1, 3), pool, sizeof(pool), pv);
      if (!n) break;
      uint8_t dir = (uint8_t)clampInt(modP(slot, 2), 0, 2);
      uint8_t pat = (uint8_t)clampInt(modP(slot, 3), 0, 3);
      uint16_t speed = (uint16_t)clampInt(modP(slot, 0), 5, 200);
      // ACCNT — акцент velocity по ходу щипка: плюс выделяет первую
      // ноту и гасит следующие, минус наоборот раскрывает щипок к
      // концу. 0 — все ноты одной силой.
      int accent = clampInt(c.sub2, -63, 64);
      uint32_t acc = 0;
      for (uint8_t i = 0; i < n; i++) {
        uint8_t k = (dir == 1) ? (n - 1 - i) : i;
        if (dir == 2) k = random(0, n);
        // Доля позиции в щипке: 0 у первой ноты, 1 у последней.
        int spread = (n > 1) ? (int)((100 * i) / (n - 1)) : 0;
        // Акцент масштабирует силу источника, а не назначает свою:
        // иначе щипок стирал бы динамику приходящей ноты.
        int vel = pv[k] - accent * spread * pv[k] / 100 / 127;
        vel = clampInt(vel, 1, 127);
        // PATTERN задаёт относительный тайминг между щипками.
        uint32_t gap;
        switch (pat) {
          case 1:  gap = speed * (n - i) / n; break;          // accelerating
          case 2:  gap = speed * (i + 1) / n; break;          // decelerating
          case 3:  gap = (i < n / 2) ? speed / 4 : speed; break; // burst
          default: gap = speed; break;                        // straight
        }
        scheduleNote(pool[k], (uint8_t)vel, ch, acc, r.capGateMs, (int8_t)slot);
        acc += gap;
      }
      break;
    }

    case AppId::NDELAY: {
      // Первая «нота» это сам вход, поэтому эхо на одно меньше.
      uint8_t rep = (uint8_t)(clampInt(modP(slot, 0), 1, 8) - 1);
      uint8_t walk = (uint8_t)clampInt(modP(slot, 1), 0, 2);
      uint8_t pp = (uint8_t)clampInt(modP(slot, 2), 0, 63);
      uint8_t decay = (uint8_t)clampInt(modP(slot, 3), 0, 127);
      // Сухой вход — весь аккорд, а не нижняя нота.
      for (uint8_t v = 0; v < r.capCount; v++)
        scheduleNote(r.capNotes[v], r.capVel[v], ch, 0, r.capGateMs, (int8_t)slot);
      // Интервал между эхо: делим длину гейта источника выбранной
      // долей. С внешнего входа делить нечего — там гейт синтетический
      // и к темпу отношения не имеет, поэтому доля становится
      // абсолютной длительностью от общего BPM.
      uint8_t dv = (uint8_t)clampInt(modSub4(slot), 0, 4);
      uint32_t gap;
      if (c.arm == ARM_EXT || r.capGateMs == 0) {
        uint16_t bpm = state.extClock && state.extBpm > 0 ? (uint16_t)state.extBpm : state.bpm;
        if (bpm < 20) bpm = 20;
        gap = (uint32_t)((60000.0f / bpm) * rateTicks[dv] / 24.0f);
      } else {
        gap = (uint32_t)r.capGateMs * rateTicks[dv] / rateTicks[0];
      }
      if (gap < 10) gap = 10;
      // Эхо повторяет аккорд целиком: каждый голос ведёт свою линию
      // транспонирования и своё затухание. Раньше в эхо уходила только
      // нижняя нота, и аккорд рассыпался на первом же повторе.
      uint32_t gate = r.capGateMs;
      int cur[MAX_VOICES];
      uint8_t cvel[MAX_VOICES];
      uint8_t nv = r.capCount > MAX_VOICES ? MAX_VOICES : r.capCount;
      for (uint8_t v = 0; v < nv; v++) { cur[v] = r.capNotes[v]; cvel[v] = r.capVel[v]; }
      for (uint8_t i = 1; i <= rep; i++) {
        gate = gate * (128 - decay) / 128;
        if (gate < 10) gate = 10;
        // Шаг блуждания берётся один на весь аккорд, иначе голоса
        // разъезжаются в кашу вместо того, чтобы ехать строем.
        int rndStep = (int)random(-2, 3);
        for (uint8_t v = 0; v < nv; v++) {
          switch (walk) {
            case 1:  cur[v] = transposeDegrees(cur[v], 1, r.capScale, r.capRoot); break;
            case 2:  cur[v] = transposeDegrees(cur[v], rndStep, r.capScale, r.capRoot); break;
            default: cur[v] = clampInt(cur[v] + 12, 0, 127); break;
          }
          // DECAY гасит не только длину, но и громкость: эхо, тающее
          // лишь по длительности, звучит неправдоподобно.
          cvel[v] = (uint8_t)clampInt(cvel[v] * (128 - decay) / 128, 1, 127);
          scheduleNote((uint8_t)cur[v], cvel[v], ch, gap * i, gate, (int8_t)slot);
        }
        if (pp) {
          uint8_t v = (i & 1) ? (uint8_t)clampInt(64 + pp, 0, 127)
                              : (uint8_t)clampInt(64 - pp, 0, 127);
          emitCC(slot, c.ccDest, v);
        }
      }
      break;
    }

    case AppId::ENV:
      r.envStart = millis();
      r.envActive = true;
      // ENV модулирует, а не потребляет: захваченное уходит дальше по
      // шине как есть. Наружу нот не шлём — иначе они задвоились бы с
      // тем, что сыграет следующее звено.
      publish(slot, r.capNotes, r.capCount, r.capGateMs, r.capScale, r.capRoot);
      break;

    case AppId::MLFO:
      // synced — фаза привязана к длине гейта; one-shot — один цикл.
      if (modP(slot, 2) != 0) { r.lfoPhase = 0; r.envStart = millis(); r.envActive = true; }
      // Как и ENV, пропускаем захваченное дальше, ничего не отправляя
      // наружу нотами.
      publish(slot, r.capNotes, r.capCount, r.capGateMs, r.capScale, r.capRoot);
      break;

    case AppId::GLIDER: {
      uint8_t target = lowestOf(r);
      uint8_t from = r.glidePrev ? r.glidePrev : target;
      uint16_t speed = (uint16_t)clampInt(modP(slot, 0), 5, 150);
      uint8_t maxSteps = (uint8_t)clampInt(modP(slot, 1), 1, 16);
      uint8_t curve = (uint8_t)clampInt(modP(slot, 2), 0, 2);
      uint32_t gate = r.capGateMs * (uint32_t)clampInt(modP(slot, 3), 10, 127) / 127;
      // Глайд идёт по промежуточным СТУПЕНЯМ ЛАДА, а не по хроматике
      // и не питч-бендом.
      int dir = (target > from) ? 1 : -1;
      uint8_t n = 0;
      int probe = from;
      while (n < maxSteps && probe != target) {
        int next = transposeDegrees(probe, dir, r.capScale, r.capRoot);
        if (next == probe) break;
        probe = next;
        n++;
        if ((dir > 0 && probe >= target) || (dir < 0 && probe <= target)) break;
      }
      int cur = from;
      for (uint8_t i = 0; i < n; i++) {
        float t = (float)(i + 1) / (n ? n : 1);
        uint32_t delay;
        switch (curve) {
          case 1: delay = (uint32_t)(speed * i * (1.0f + 0.35f * sin(t * PI * 2))); break; // spring
          case 2: delay = (uint32_t)(speed * i * (0.6f + 0.8f * envShapeCurve(t, 3))); break; // ease
          default: delay = speed * i; break;
        }
        cur = transposeDegrees(cur, dir, r.capScale, r.capRoot);
        scheduleNote((uint8_t)cur, r.capVel[0], ch, delay, gate, (int8_t)slot);
      }
      scheduleNote(target, r.capVel[0], ch, speed * n, gate, (int8_t)slot);
      r.glidePrev = target;
      break;
    }

    case AppId::MULTX: {
      if (r.capCount == 0) break;
      // Идём по всем захваченным нотам, а не только по нижней: модуль
      // рассчитан на полифонию и переживёт её включение как есть.
      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      for (uint8_t o = 0; o < 7; o++) {
        uint8_t ch = modOutCh(slot, o);
        if (ch == 0) continue;                  // выход выключен
        for (uint8_t n = 0; n < r.capCount; n++) {
          uint8_t note = c.outXp[o]
            ? transposeDegrees(r.capNotes[n], c.outXp[o], r.capScale, r.capRoot)
            : r.capNotes[n];
          scheduleNote(note, r.capVel[n], ch, 0, gate);
        }
      }
      publish(slot, r.capNotes, r.capCount, gate, r.capScale, r.capRoot, r.capVel);
      break;
    }

    case AppId::POLYSPL: {
      if (r.capCount == 0) break;
      int split = clampInt(modP(slot, 0), 0, 127);
      uint8_t mode = (uint8_t)clampInt(modP(slot, 3), 0, 2);
      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      uint8_t out[MAX_VOICES], outV[MAX_VOICES];
      uint8_t nOut = 0;
      for (uint8_t n = 0; n < r.capCount; n++) {
        bool low = (r.capNotes[n] < split);
        if (low && mode == 2) continue;         // звучит только верхняя
        if (!low && mode == 1) continue;        // только нижняя
        uint8_t ch = low ? (uint8_t)clampInt(c.midiCh, 0, 16)
                         : (uint8_t)clampInt(c.sub2, 0, 16);
        if (ch == 0) continue;                  // зона выключена
        int xp = clampInt(low ? modP(slot, 1) : modP(slot, 2), -14, 14);
        uint8_t note = xp ? transposeDegrees(r.capNotes[n], xp, r.capScale, r.capRoot)
                          : r.capNotes[n];
        scheduleNote(note, r.capVel[n], ch, 0, gate);
        if (nOut < MAX_VOICES) { outV[nOut] = r.capVel[n]; out[nOut++] = note; }
      }
      if (nOut) publish(slot, out, nOut, gate, r.capScale, r.capRoot, outV);
      break;
    }

    case AppId::RROBIN: {
      if (r.capCount == 0) break;
      uint8_t voices = (uint8_t)clampInt(modP(slot, 0), 1, 4);
      uint8_t mode = (uint8_t)clampInt(modP(slot, 1), 0, 3);
      uint8_t steal = (uint8_t)clampInt(modP(slot, 2), 0, 2);
      int spread = clampInt(modP(slot, 3), -7, 7);
      uint32_t now = millis();
      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      uint8_t out[MAX_VOICES];
      uint8_t nOut = 0;

      if (mode == 3) {
        // CHORD — весь захваченный аккорд разом, по ноте на голос.
        for (uint8_t i = 0; i < r.capCount && i < voices; i++) {
          uint8_t note = spread
            ? transposeDegrees(r.capNotes[i], spread * i, r.capScale, r.capRoot)
            : r.capNotes[i];
          scheduleNote(note, r.capVel[i], modOutCh(slot, i), 0, gate);
          r.voiceFreeAt[i] = now + gate;
          out[nOut++] = note;
        }
      } else {
        uint8_t v;
        if (mode == 0) {                        // CYCLE
          v = r.rrNext % voices;
          r.rrNext = (uint8_t)((v + 1) % voices);
        } else if (mode == 1) {                 // RANDOM
          v = (uint8_t)random(0, voices);
        } else {                                // FREE — первый свободный
          v = 255;
          for (uint8_t i = 0; i < voices; i++)
            if ((int32_t)(now - r.voiceFreeAt[i]) >= 0) { v = i; break; }
          if (v == 255) {
            if (steal == 2) break;              // DROP — ноту роняем
            v = 0;
            for (uint8_t i = 1; i < voices; i++) {
              bool better = (steal == 0) ? (r.voiceFreeAt[i] < r.voiceFreeAt[v])
                                         : (r.voiceFreeAt[i] > r.voiceFreeAt[v]);
              if (better) v = i;
            }
          }
        }
        uint8_t note = spread
          ? transposeDegrees(r.capNotes[0], spread * v, r.capScale, r.capRoot)
          : r.capNotes[0];
        scheduleNote(note, r.capVel[0], modOutCh(slot, v), 0, gate);
        r.voiceFreeAt[v] = now + gate;
        out[nOut++] = note;
      }
      if (nOut) publish(slot, out, nOut, gate, r.capScale, r.capRoot);
      break;
    }

    case AppId::KEYTRK: {
      if (r.capCount == 0) break;
      // Трекаем нижнюю ноту: при полифонии с внешнего входа так же
      // поступают все модули, не знающие об аккордах, и для фильтра
      // это музыкально правильнее.
      uint8_t note = lowestOf(r);
      // Через модуляцию, как и всё остальное в приборе: раньше глубины,
      // опора и размах читались напрямую, и META, целясь в них, молча
      // не делала ничего.
      int center = clampInt(modParam(slot, true, 2), 0, 127);
      int span = clampInt(modParam(slot, true, 3), 1, 48);
      for (uint8_t i = 0; i < NUM_COLS; i++) {
        int depth = clampInt(modP(slot, i), -64, 64);
        if (depth == 0) continue;                 // линия выключена
        // Биполярно вокруг середины: на опорной ноте все линии в 64,
        // отрицательная глубина переворачивает ход линии.
        int v = clampInt(64 + ((int)note - center) * depth / span, 0, 127);
        if (r.ktLast[i] == (uint8_t)v) continue;
        r.ktLast[i] = (uint8_t)v;
        emitCC(slot, modKtCC(slot, i), (uint8_t)v);
      }
      // Нота идёт насквозь: KEYTRACK — врезка, а не терминал цепочки.
      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      scheduleNote(note, r.capVel[0], c.midiCh, 0, gate);
      uint8_t one[1] = {note};
      uint8_t ov[1] = {r.capVel[0]};
      publish(slot, one, 1, gate, r.capScale, r.capRoot, ov);
      break;
    }

    case AppId::MATH: {
      uint8_t src = (uint8_t)clampInt(modP(slot, 0), 0, 2);
      if (src == 1) break;                 // CC считается непрерывно, не по событию
      const Signal* sa = armSource(slot, 0);
      const Signal* sb = armSource(slot, 1);
      if (!sa || !sa->count) break;
      uint8_t op = (uint8_t)clampInt(modP(slot, 1), 0, 8);
      uint8_t lim = (uint8_t)clampInt(modP(slot, 3), 0, 2);
      bool hasB = sb && sb->count;

      if (src == 0) {
        // Складывать MIDI-номера бессмысленно, поэтому считаем СТУПЕНИ
        // внутри октавы: у A берём его позицию, у B — его ступень как
        // интервал. Октава A при этом сохраняется, а перелив за её
        // край естественно уходит в соседнюю.
        const ScaleDef &sA = scales[clampInt(sa->scale, 0, 9)];
        int degA = degreeOf(sa->notes[0], sa->scale, sa->root);
        int octA = floorDiv(degA, sA.count);
        int posA = degA - octA * sA.count;
        int posB;
        if (hasB) {
          const ScaleDef &sB = scales[clampInt(sb->scale, 0, 9)];
          int degB = degreeOf(sb->notes[0], sb->scale, sb->root);
          posB = degB - floorDiv(degB, sB.count) * sB.count;
        } else {
          posB = clampInt(modP(slot, 2), 0, 24);
        }
        int raw = noteOfDegreeRaw(octA * sA.count + mathApply(op, posA, posB),
                                  sa->scale, sa->root);
        // LIM решает судьбу вылета за MIDI-диапазон, а квантование
        // после него возвращает результат в лад: WRAP и FOLD считают
        // в полутонах и сами по себе из лада выбрасывают.
        uint8_t note = quantizeToScale(applyLim(raw, 0, 127, lim), sa->scale, sa->root);
        if (c.stepPlus) note = transposeDegrees(note, c.stepPlus, sa->scale, sa->root);
        uint32_t gate = r.capGateMs ? r.capGateMs : 120;
        scheduleNote(note, r.capVel[0], c.midiCh, 0, gate);
        uint8_t one[1] = {note};
        uint8_t ov[1] = {r.capVel[0]};
        publish(slot, one, 1, gate, sa->scale, sa->root, ov);
      } else {
        // GATE: нота проходит насквозь, меняется только её длина.
        int a = (int)(sa->gateMs ? sa->gateMs : 120);
        int res;
        if (op == 3 || op == 4) {
          // Перемножать две длительности бессмысленно, поэтому множитель
          // здесь всегда из CONST, в шестнадцатых долях: 16 — это x1.
          int f = clampInt(modP(slot, 2), 1, 127);
          res = (op == 3) ? (a * f / 16) : (a * 16 / f);
        } else {
          int b = hasB ? (int)sb->gateMs : clampInt(modP(slot, 2), 0, 127) * 8;
          res = mathApply(op, a, b);
        }
        res = applyLim(res, 10, 4000, lim);
        uint8_t note = sa->notes[0];
        if (c.stepPlus) note = transposeDegrees(note, c.stepPlus, sa->scale, sa->root);
        scheduleNote(note, r.capVel[0], c.midiCh, 0, (uint32_t)res);
        uint8_t one[1] = {note};
        uint8_t ov[1] = {r.capVel[0]};
        publish(slot, one, 1, (uint32_t)res, sa->scale, sa->root, ov);
      }
      break;
    }

    case AppId::SHIFT: {
      if (r.capCount == 0) break;
      uint8_t len  = (uint8_t)clampInt(modP(slot, 0), 1, 16);
      int     mut  = clampInt(modP(slot, 1), 0, 127);
      int     sprd = clampInt(modP(slot, 2), 0, 7);
      uint8_t mode = (uint8_t)clampInt(modP(slot, 3), 0, 3);
      int     rot  = clampInt(modSub4(slot), 0, 15) % len;
      int     velD = clampInt(cfg[slot].sub2, -32, 32);

      // Пока регистр не набрал свои LEN событий, вход идёт насквозь и
      // попутно заполняет ячейки по порядку. Так слышно, что играешь, а
      // закольцовываться оно начинает ровно тогда, когда есть чему.
      if (r.shFilled < len) {
        uint8_t idx = r.shFilled;
        r.shReg[idx] = r.capNotes[0];
        r.shVel[idx] = r.capVel[0];
        r.shRest &= (uint16_t)~(1u << idx);
        r.shFilled++;
        uint32_t g = r.capGateMs ? r.capGateMs : 120;
        scheduleNote(r.capNotes[0], r.capVel[0], c.midiCh, 0, g);
        uint8_t n1[1] = {r.capNotes[0]};
        uint8_t v1[1] = {r.capVel[0]};
        publish(slot, n1, 1, g, r.capScale, r.capRoot, v1);
        break;
      }

      uint8_t rd = (uint8_t)(r.shPos % len);
      // Голова записи отстоит от головы чтения на ROT. Точка записи
      // всегда на одном и том же расстоянии от того, что слышно, —
      // поэтому мутация копится связно, а не разбрасывается по регистру.
      uint8_t wr = (uint8_t)((rd + rot) % len);

      // Снимаем то, что прозвучит, ДО записи: при ROT=0 головы совпадают,
      // и иначе свежий материал перебивал бы ноту в тот же миг, вместо
      // того чтобы дождаться своего круга.
      bool    rest = (r.shRest >> rd) & 1;
      uint8_t note = r.shReg[rd];
      uint8_t vel  = r.shVel[rd];

      // За событие с ячейкой под головой записи происходит РОВНО ОДНО:
      // либо в неё садится свежий материал, либо она портится сама.
      if (mut && (int)random(0, 128) < mut) {
        r.shReg[wr] = r.capNotes[0];
        r.shVel[wr] = r.capVel[0];
        r.shRest &= (uint16_t)~(1u << wr);
      } else if (sprd) {
        int v = r.shReg[wr];
        switch (mode) {
          case 0:   // WALK — блуждание по ступеням лада
            v = transposeDegrees((uint8_t)v, (int)random(-sprd, sprd + 1),
                                 r.capScale, r.capRoot);
            break;
          case 1: { // OCT — октавный разброс в полосе вокруг среднего
            int sum = 0;
            for (uint8_t i = 0; i < len; i++) sum += r.shReg[i];
            int mid = sum / len, band = 12 * sprd;
            v += random(0, 2) ? 12 : -12;
            while (v > mid + band) v -= 12;
            while (v < mid - band) v += 12;
            v = clampInt(v, 0, 127);
            break;
          }
          case 2: { // MIRROR — доля пути к отражению вокруг среднего
            int sum = 0;
            for (uint8_t i = 0; i < len; i++) sum += r.shReg[i];
            int mid = sum / len;
            int mirrored = 2 * mid - v;
            v = v + (mirrored - v) * sprd / 7;
            v = quantizeToScale(clampInt(v, 0, 127), r.capScale, r.capRoot);
            break;
          }
          default:  // DROP — портится не высота, а ритм
            if ((int)random(0, 14) < sprd) r.shRest |= (uint16_t)(1u << wr);
            else                           r.shRest &= (uint16_t)~(1u << wr);
            break;
        }
        if (mode != 3) r.shReg[wr] = (uint8_t)clampInt(v, 0, 127);
      }

      // Приращение velocity срабатывает на ячейке при чтении, то есть
      // ровно раз за круг: петля затухает как плёнка или раскачивается.
      if (velD) r.shVel[rd] = (uint8_t)clampInt((int)vel + velD, 1, 127);
      r.shPos = (uint8_t)((rd + 1) % len);
      if (rest) break;

      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      scheduleNote(note, vel, c.midiCh, 0, gate);
      uint8_t one[1] = {note};
      uint8_t ov[1] = {vel};
      publish(slot, one, 1, gate, r.capScale, r.capRoot, ov);
      break;
    }

    case AppId::ADD: {
      uint8_t src;
      uint8_t mode = (uint8_t)clampInt(modP(slot, 1), 0, 2);
      if (r.capCount > 1) {
        // Настоящий аккорд — LOWEST/HIGHEST берутся прямо из него.
        src = (mode == 0) ? lowestOf(r) : (mode == 1 ? highestOf(r) : r.capNotes[0]);
      } else if (mode == 2 || r.histLen == 0) {
        src = r.capNotes[0];
      } else {
        uint8_t lo = r.hist[0], hi = r.hist[0];
        for (uint8_t i = 1; i < r.histLen; i++) {
          if (r.hist[i] < lo) lo = r.hist[i];
          if (r.hist[i] > hi) hi = r.hist[i];
        }
        src = (mode == 0) ? lo : hi;
      }
      // Октава — это столько ступеней, сколько их в ладу: в диатонике
      // семь, в хроматике двенадцать. Числом полутонов её тут задавать
      // нельзя, весь модуль считает ступенями.
      uint8_t iv = (uint8_t)clampInt(modP(slot, 0), 0, 4);
      int deg = (iv == 4) ? (int)scales[r.capScale % 10].count : (int)addDegrees[iv];
      if (modP(slot, 2) > 0 && random(0, 128) < modP(slot, 2)) deg = -deg;
      uint8_t added = transposeDegrees(src, deg, r.capScale, r.capRoot);
      // Входной аккорд проходит ЦЕЛИКОМ, добавленная нота кладётся
      // сверху: раньше брался только capNotes[0], и аккорд от
      // секвенсера или с клавиатуры терял все голоса кроме первого.
      uint8_t out[MAX_VOICES], ov[MAX_VOICES];
      uint8_t n = 0;
      for (uint8_t i = 0; i < r.capCount && n < MAX_VOICES; i++) {
        out[n] = r.capNotes[i];
        ov[n++] = r.capVel[i];
      }
      if (n < MAX_VOICES) {
        out[n] = added;
        ov[n++] = (uint8_t)clampInt(modP(slot, 3), 1, 127);
      }
      for (uint8_t i = 0; i < n; i++) scheduleNote(out[i], ov[i], ch, 0, r.capGateMs);
      publish(slot, out, n, r.capGateMs, r.capScale, r.capRoot, ov);
      break;
    }

    default: break;
  }
}

// Непрерывная часть процессоров — то, что живёт между захватами.
void serviceProcessor(uint8_t slot) {
  SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];
  uint32_t now = millis();
  uint8_t ch = c.midiCh;

  switch ((AppId)c.app) {
    case AppId::ENV: {
      if (!r.envActive) break;
      uint8_t shape = (uint8_t)clampInt(modP(slot, 0), 0, 2);
      uint8_t func = (uint8_t)clampInt(c.sub3, 0, 3);   // FUNC — с подстраницы
      // SPEED: в положении GATE огибающая занимает длину захваченного
      // трига, дальше — фиксированные доли такта по общему BPM.
      uint8_t sp = (uint8_t)clampInt(c.sub2, 0, 5);
      uint32_t len;
      if (sp == 0) {
        len = r.capGateMs ? r.capGateMs : 400;
      } else {
        uint16_t bpm = state.extClock && state.extBpm > 0 ? (uint16_t)state.extBpm : state.bpm;
        if (bpm < 20) bpm = 20;
        len = (uint32_t)((60000.0f / bpm) * rateTicks[sp - 1] / 24.0f);
      }
      if (len < 20) len = 20;
      uint32_t el = now - r.envStart;
      float t = (float)el / len;
      if (t >= 1.0f) {
        if (shape == 2) { r.envStart = now; t = 0; }      // LOOP
        else { r.envActive = false; t = 1.0f; }
      }
      // A/D — где внутри цикла лежит вершина: 0 ровно посередине,
      // минус двигает её к началу (резкая атака, долгий спад).
      float peak = 0.5f + clampInt(modP(slot, 1), -63, 64) / 160.0f;
      if (peak < 0.05f) peak = 0.05f;
      if (peak > 0.95f) peak = 0.95f;
      float v;
      if (shape == 0)                                     // AD
        v = (t < peak) ? envShapeCurve(t / peak, func)
                       : envShapeCurve((1 - t) / (1 - peak), func);
      else if (shape == 1) v = envShapeCurve(1.0f - t, func);                  // AR
      else                                                                     // LOOP
        v = (t < peak) ? envShapeCurve(t / peak, func)
                       : envShapeCurve((1 - t) / (1 - peak), func);
      uint8_t v1 = (uint8_t)clampInt((int)(v * 127), 0, 127);
      // В LOOP CC2 опережает CC1 на четверть цикла; в одноразовых
      // формах CC2 — дополнение CC1.
      uint8_t v2;
      if (shape == 2) {
        float t2 = t + 0.25f; if (t2 > 1) t2 -= 1;
        float w = (t2 < peak) ? envShapeCurve(t2 / peak, func)
                              : envShapeCurve((1 - t2) / (1 - peak), func);
        v2 = (uint8_t)clampInt((int)(w * 127), 0, 127);
      } else {
        v2 = 127 - v1;
      }
      static uint32_t lastCCMs[NUM_SLOTS] = {0};
      if (now - lastCCMs[slot] >= 15) {
        lastCCMs[slot] = now;
        emitCC(slot, (uint8_t)clampInt(modP(slot, 2), 0, 127), v1);
        emitCC(slot, (uint8_t)clampInt(modP(slot, 3), 0, 127), v2);
      }
      break;
    }

    case AppId::MLFO: {
      uint8_t act = (uint8_t)clampInt(modP(slot, 2), 0, 2);
      if (act == 2 && !r.envActive) break;
      static uint32_t lastMs[NUM_SLOTS] = {0};
      if (now - lastMs[slot] < 15) break;
      // Первый проход (и возврат после долгой паузы) не должен
      // швырять фазу далеко вперёд — ограничиваем шаг.
      float dt = (now - lastMs[slot]) / 1000.0f;
      if (dt > 0.1f) dt = 0.1f;
      lastMs[slot] = now;
      float periodS;
      if (act == 1 && r.capGateMs) periodS = r.capGateMs / 1000.0f;  // synced
      else {
        // Свободный ход идёт по своей доле такта, а не всегда по
        // четверти: без этого LFO нельзя было ни ускорить, ни замедлить.
        uint16_t bpm = state.bpm ? state.bpm : 120;
        periodS = (60.0f / bpm) * rateTicks[clampInt(modSub4(slot), 0, 4)] / 24.0f;
      }
      if (periodS < 0.02f) periodS = 0.02f;
      r.lfoPhase += TWO_PI * dt / periodS;
      if (r.lfoPhase > TWO_PI) {
        r.lfoPhase -= TWO_PI;
        if (act == 2) r.envActive = false;   // one-shot — ровно один цикл
      }
      float out = computeLFO(r.lfoPhase, (uint8_t)clampInt(modP(slot, 0), 0, 2), modP(slot, 1));
      int amt = clampInt(modP(slot, 3), 0, 127);
      uint8_t v = (uint8_t)clampInt(64 + (int)(out * 63.0f * amt / 127.0f), 0, 127);
      emitCC(slot, c.ccDest, v);
      break;
    }

    case AppId::DUST: {
      if (r.capCount == 0) break;
      if (now - r.dustLastMs < 25) break;
      r.dustLastMs = now;
      int fill = clampInt(modP(slot, 0), 0, 127);
      if (fill == 0) break;
      emitCC(slot, c.ccDest, (uint8_t)fill);
      if (random(0, 128) >= fill) break;
      // Корень медленно дрейфует синусоидой — облако не стоит на месте.
      r.dustDrift += 0.01f;
      int root = lowestOf(r) + (int)(sin(r.dustDrift) * 2.0f);
      int rootPct = clampInt(modP(slot, 1), 0, 100);
      uint8_t note;
      if ((int)random(0, 100) < rootPct) {
        note = quantizeToScale(root, r.capScale, r.capRoot);
      } else {
        int reach = clampInt(modP(slot, 3), 0, 7);
        int deg = reach ? (int)random(-reach, reach + 1) : 0;
        note = transposeDegrees(root, deg, r.capScale, r.capRoot);
      }
      uint32_t len = 10 + (uint32_t)clampInt(modP(slot, 2), 0, 127) * 190 / 127;
      scheduleNote(note, r.capVel[0], ch, 0, len);
      // Частицы — полноценные ноты, и они обязаны попадать на шину,
      // иначе цепочка за DUST обрывается.
      uint8_t one[1] = {note};
      publish(slot, one, 1, len, r.capScale, r.capRoot);
      break;
    }

    case AppId::MATH: {
      // Непрерывным тут является только режим CC: источником может быть
      // ползущий LFO, ждать от него «событие» бессмысленно. NOTE и GATE
      // считаются по приходу ноты, в onCapture().
      if (clampInt(modP(slot, 0), 0, 2) != 1) break;
      static uint32_t lastMathMs[NUM_SLOTS] = {0};
      if (now - lastMathMs[slot] < 15) break;
      lastMathMs[slot] = now;
      const Signal* sa = armSource(slot, 0);
      const Signal* sb = armSource(slot, 1);
      int a = sa ? (int)sa->ccValue : 0;
      int b = sb ? (int)sb->ccValue : clampInt(modP(slot, 2), 0, 127);
      int res = mathApply((uint8_t)clampInt(modP(slot, 1), 0, 8), a, b);
      res = applyLim(res, 0, 127, (uint8_t)clampInt(modP(slot, 3), 0, 2));
      emitCC(slot, c.ccDest, (uint8_t)res);
      r.lastFireMs = now;
      break;
    }

    case AppId::LOGIC: {
      // Сетка приходит извне: шаги генератора в начале цепочки, а если
      // его нет — внутренние 1/16 по транспорту самого слота.
      int8_t grid = logicGridSlot(slot);
      bool step = false;
      if (grid >= 0) {
        // Считаем по ТИКУ шага, а не по его номеру: у паттерна длиной
        // в один шаг номер всегда 0 и сетка не тикала бы вовсе.
        if (rt[grid].lastStepTick != r.lastGridStep) {
          r.lastGridStep = rt[grid].lastStepTick;
          step = true;
        }
      } else if (c.running && state.midiTicks - r.lastStepTick >= rateTicks[4]) {
        r.lastStepTick = state.midiTicks;
        step = true;
      }
      if (!step) break;

      const Signal* sa = armSource(slot, 0);
      const Signal* sb = armSource(slot, 1);
      bool hasA = sa && sa->count, hasB = sb && sb->count;
      uint8_t na = hasA ? sa->notes[0] : 0;
      uint8_t nb = hasB ? sb->notes[0] : 0;

      bool a = r.sawA;
      bool b = r.sawB;
      r.sawA = r.sawB = false;

      // Условие по данным навешивается на второй операнд: A задаёт
      // сетку и первый операнд, сравнивается с ним именно B.
      uint8_t cmp = (uint8_t)clampInt(modP(slot, 1), 0, 2);
      if (b && cmp != 0) {
        // Сравнение по полутонам, а не по ступеням: у источников могут
        // быть разные лад и тоника, общей ступени между ними нет.
        int lhs = (cmp == 1) ? (int)nb : (int)(sb ? sb->ccValue : 0);
        int rhs = (cmp == 1) ? (int)na : (int)clampInt(modSub4(slot), 0, 127);
        switch ((uint8_t)clampInt(modP(slot, 2), 0, 3)) {
          case 0: b = (lhs == rhs); break;
          case 1: b = (lhs != rhs); break;
          case 2: b = (lhs > rhs);  break;
          default: b = (lhs < rhs); break;
        }
      }

      bool pass;
      switch ((uint8_t)clampInt(modP(slot, 0), 0, 5)) {
        case 0: pass = a && b; break;
        case 1: pass = a || b; break;
        case 2: pass = a != b; break;
        case 3: pass = !(a && b); break;
        case 4: pass = !(a || b); break;
        default: pass = (a == b); break;
      }
      if (!pass) break;

      // На проходах, где никто не тригнул (NOR, NAND), свежей высоты
      // нет — держим последнюю выданную.
      uint8_t note;
      switch ((uint8_t)clampInt(modP(slot, 3), 0, 3)) {
        case 0: note = hasA ? na : r.heldNote; break;
        case 1: note = hasB ? nb : r.heldNote; break;
        case 2: note = (hasA && hasB) ? (na < nb ? na : nb) : (hasA ? na : (hasB ? nb : r.heldNote)); break;
        default: note = (hasA && hasB) ? (na > nb ? na : nb) : (hasA ? na : (hasB ? nb : r.heldNote)); break;
      }
      if (note == 0) break;          // ещё ничего не приходило
      if (c.stepPlus) note = transposeDegrees(note, c.stepPlus, r.capScale, r.capRoot);
      r.heldNote = note;

      uint32_t gate = r.capGateMs ? r.capGateMs : 120;
      uint8_t lvel = r.capCount ? r.capVel[0] : DEFAULT_VEL;
      scheduleNote(note, lvel, c.midiCh, 0, gate);
      uint8_t one[1] = {note};
      uint8_t ov[1] = {lvel};
      publish(slot, one, 1, gate, r.capScale, r.capRoot, ov);
      break;
    }

    default: break;
  }
}

// Шаг ARP — у него собственное деление такта, независимое от
// захваченного сигнала, поэтому он тактируется от клока.
void fireArpStep(uint8_t slot) {
  SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];
  if (r.arpPoolLen == 0) return;
  uint8_t idx = (uint8_t)(r.arpIndex % r.arpPoolLen);
  uint8_t note = r.arpPool[idx];
  uint8_t vel = r.arpVel[idx];
  r.arpIndex++;
  // Кейтрекинг: нота отходит от опоры ровно на заданную долю своего
  // настоящего отклонения. 64 — как сыграно, 0 — арпеджио прибито к
  // опоре и на клавишу не реагирует, минус — идёт в другую сторону.
  int ktrc = clampInt(modP(slot, 1), -64, 64);
  if (ktrc != 64 && r.arpAnchorSet) {
    int v = (int)r.arpAnchor + ((int)note - (int)r.arpAnchor) * ktrc / 64;
    note = quantizeToScale((uint8_t)clampInt(v, 0, 127), r.capScale, r.capRoot);
  }
  uint32_t gate = 10 + (uint32_t)clampInt(modP(slot, 3), 0, 127) * 190 / 127;
  scheduleNote(note, vel, c.midiCh, 0, gate);
  uint8_t one[1] = {note};
  uint8_t ov[1] = {vel};
  publish(slot, one, 1, 60, r.capScale, r.capRoot, ov);
}

// Адрес цели METAP упакован в число 0..31: слот, страница, колонка.
// Собственный слот из перебора исключён — модуль не может целиться в
// себя, это была бы мгновенная обратная связь без задержки цикла.
inline uint8_t metaTgtSlot(int16_t a) { return (uint8_t)(a / MP_COUNT); }
inline uint8_t metaTgtIdx (int16_t a) { return (uint8_t)(a % MP_COUNT); }
constexpr int16_t META_ADDR_MAX = NUM_SLOTS * MP_COUNT;

// Описание параметра по плоскому номеру. Пустое имя означает, что у
// этого приложения такого параметра нет и адрес пропускается.
PDesc metaParamDesc(uint8_t slot, uint8_t idx) {
  const SlotCfg &c = cfg[slot];
  const PDesc none = {"", 0, 0, P_NUM, nullptr};
  if (c.app == (uint8_t)AppId::EMPTY) return none;
  if (idx < MP_SUB) return apps[c.app].p[idx];
  if (idx < MP_SUB + NUM_COLS) {
    uint8_t col = (uint8_t)(idx - MP_SUB);
    // ARM модулировать нельзя: перекидывание маршрута на ходу не
    // музыкальный приём, а потеря управления.
    if (isProcessor(c.app) && col == 1) return none;
    // У RROBIN колонки 3 и 4 подстраницы держат по ДВА канала голосов;
    // как цели они адресуются поштучно ниже, среди спрятанного.
    if ((AppId)c.app == AppId::RROBIN && col >= 2) return none;
    return columnDesc(slot, true, col);
  }
  if (!isGenerator(c.app)) {
    // Вторая строка первой колонки — у каждого процессора своя.
    if (idx == MP_SUB4P) {
      PDesc d = apps[c.app].sub4;
      return d.name[0] ? d : none;
    }
    // Спрятанное под TAP у процессоров.
    if ((AppId)c.app == AppId::KEYTRK && idx >= MP_EXTRA && idx < MP_EXTRA + 4)
      return {ktCCNames[idx - MP_EXTRA], 0, 127, P_NUM, nullptr};
    if ((AppId)c.app == AppId::MULTX && idx >= MP_EXTRA && idx < MP_EXTRA + 7)
      return {outChNames[idx - MP_EXTRA], 0, 16, P_CHAN, nullptr};
    if ((AppId)c.app == AppId::RROBIN && idx >= MP_EXTRA && idx < MP_EXTRA + 4)
      return {outChNames[idx - MP_EXTRA], 0, 16, P_CHAN, nullptr};
    return none;
  }
  // У TOPOLOGY под TAP спрятана половина модуля: Y карты и ноты дорожек.
  if ((AppId)c.app == AppId::TOPOLOGY) {
    if (idx == MP_TOPO_Y) return {"Y", 0, 127, P_NUM, nullptr};
    if (idx >= MP_EXTRA && idx < MP_EXTRA + TOPO_TRACKS)
      return {topoNoteNames[idx - MP_EXTRA], 0, 127, P_NOTE, nullptr};
  }
  // Не "ROOT": у SKELETON так зовётся колонка с опорной НОТОЙ, и в
  // списке целей META две разные вещи оказывались одноимёнными.
  if (idx == MP_ROOT) return {"KEY", 0, 11, P_LIST, rootNames};
  if (idx == MP_BPM)  return {"BPM", 20, 300, P_NUM, nullptr};
  return none;
}

// Следующий валидный адрес при вращении: пропускает свой слот и
// колонки без имени (у части приложений их меньше четырёх).
int16_t metaNextAddr(uint8_t slot, int16_t a, int dir) {
  int step = (dir >= 0) ? 1 : -1;
  for (int16_t guard = 0; guard < META_ADDR_MAX; guard++) {
    a = (int16_t)((a + step + META_ADDR_MAX) % META_ADDR_MAX);
    if (metaTgtSlot(a) == slot) continue;        // на себя не целимся
    if (metaParamDesc(metaTgtSlot(a), metaTgtIdx(a)).name[0]) return a;
  }
  return a;
}

// Раскладывает модуляцию по целям. Считается от значения источника,
// нормируется под диапазон каждой цели: иначе одна и та же глубина
// на списке из трёх значений и на шкале 0-127 вела бы себя
// несопоставимо.
void serviceMeta(uint8_t slot) {
  const SlotCfg &c = cfg[slot];
  SlotRt &r = rt[slot];

  // Сырое значение источника берём, только если оно есть. Нет данных —
  // держим прошлое, иначе модуляция дёргалась бы в ноль между нотами.
  bool have = false;
  int raw = 0;
  switch ((uint8_t)clampInt(c.p[0], 0, 2)) {
    case 1: {
      const Signal* sg = armSource(slot, 0);
      if (sg) { raw = (int)sg->ccValue; have = true; }
      break;
    }
    case 2:
      if (r.capGateMs) { raw = (int)r.capGateMs; have = true; }
      break;
    default:
      if (r.capCount) { raw = (int)lowestOf(r); have = true; }
      break;
  }

  uint32_t now = millis();
  uint32_t dt = now - r.metaLastMs;
  r.metaLastMs = now;

  if (have) {
    // Нормировать по абсолютной шкале нельзя: ноты живут в пределах
    // пары октав, длина гейта — в сотнях миллисекунд, значение CC — во
    // всех 0..127. По общей мерке первые два почти не сдвигали бы
    // цель, а постоянный сигнал давал бы постоянный перекос. Поэтому
    // META следит за РЕАЛЬНЫМ размахом источника и растягивает
    // модуляцию на него.
    if (!r.metaSeen) {
      r.metaLo = r.metaHi = (float)raw;
      r.metaSeen = true;
      r.metaRawPrev = (int16_t)raw;
    }
    if (raw != r.metaRawPrev) {
      r.metaRawPrev = (int16_t)raw;
      // Окно раздаётся мгновенно и медленно стягивается — так оно
      // подхватывает новый материал, но не застревает на старом.
      float k = (float)dt * 0.0001f;
      if (k > 0.05f) k = 0.05f;
      if (raw < r.metaLo) r.metaLo = (float)raw; else r.metaLo += ((float)raw - r.metaLo) * k;
      if (raw > r.metaHi) r.metaHi = (float)raw; else r.metaHi += ((float)raw - r.metaHi) * k;
    }
    float span = r.metaHi - r.metaLo;
    // Источник стоит на месте — модулировать нечем, держим середину.
    float n = (span > 0.5f) ? (((float)raw - r.metaLo) / span) * 2.0f - 1.0f : 0.0f;
    // Сглаживание по времени, а не по числу проходов: loop крутится
    // тысячи раз в секунду, и покадровый коэффициент был бы мгновенным.
    float sk = (float)dt / 30.0f;
    if (sk > 1.0f) sk = 1.0f;
    r.metaVal += (n - r.metaVal) * sk;
  }

  for (uint8_t i = 0; i < 3; i++) {
    int16_t a = c.p[1 + i];
    uint8_t ts = metaTgtSlot(a);
    if (ts == slot) continue;            // на себя не целимся
    uint8_t ti = metaTgtIdx(a);
    PDesc d2 = metaParamDesc(ts, ti);
    if (!d2.name[0]) continue;
    int depth = clampInt(c.metaDepth[i], -64, 64);
    if (!depth) continue;
    // Полная глубина — размах во весь диапазон цели, то есть по
    // половине в каждую сторону от хранимого значения. Иначе почти
    // любая модуляция упиралась бы в границу и стояла там.
    modOffset[ts][ti] +=
      (int16_t)(r.metaVal * (float)depth / 64.0f * (float)(d2.hi - d2.lo) / 2.0f);
  }
}

void serviceSlots() {
  // У внешнего входа своего лада нет — берём его у первого генератора
  // на устройстве. Без этого всё, что приходит с клавиатуры, цепочка
  // считала бы до мажором: GLIDER скользил бы по чужому ладу, ADD
  // строил терцию не от того корня, MATH неверно считал ступени.
  {
    int8_t ref = -1;
    for (uint8_t s = 0; s < NUM_SLOTS && ref < 0; s++)
      if (isGenerator(cfg[s].app)) ref = (int8_t)s;
    uint8_t sc = (ref >= 0) ? modScale((uint8_t)ref) : SCALE_CHROMATIC;
    uint8_t rt_ = (ref >= 0) ? modRoot((uint8_t)ref) : 0;
    // Генератора нет — не выдумываем лад, отдаём хроматику: она ничего
    // не портит квантованием.
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
      extSignal[s].scale = sc;
      extSignal[s].root  = rt_;
    }
  }
  // Снимок с начала цикла: все ARM читают именно его, поэтому
  // кольцевые цепочки превращаются в обратную связь с задержкой в
  // один цикл, а не в бесконечную рекурсию.
  memcpy(busSnap, busCur, sizeof(busCur));
  // Смещения пересобираются с нуля каждый цикл: снятый или
  // байпаснутый МетаПерформер сразу перестаёт влиять, а хранимые
  // значения целей никогда не портятся.
  clearModOffsets();
  // Привязки по CC. Центр 64 ничего не меняет, края дают размах на
  // половину диапазона цели в каждую сторону — та же мерка, что у META,
  // но без слежения за размахом: CC нормирован по определению.
  static_assert(MP_MAIN == 0 && MP_SUB == NUM_COLS,
                "индекс привязки совпадает с индексом modOffset");
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    if (cfg[s].bypass) continue;
    for (uint8_t i = 0; i < 2 * NUM_COLS; i++) {
      uint8_t cc = cfg[s].modCC[i];
      int depth = cfg[s].modDepth[i];
      if (!cc || !depth) continue;
      bool sub = (i >= NUM_COLS);
      PDesc d = columnDesc(s, sub, (uint8_t)(i % NUM_COLS));
      if (!d.name[0]) continue;
      modOffset[s][i] +=
        (int16_t)(((int)ccVal[cc] - 64) * depth * (d.hi - d.lo) / (64 * 64 * 2));
    }
  }
  for (uint8_t s = 0; s < NUM_SLOTS; s++)
    if ((AppId)cfg[s].app == AppId::META && !cfg[s].bypass) serviceMeta(s);
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    if (!isProcessor(cfg[s].app)) continue;
    // Захват происходит по триггеру ЛЮБОГО из двух источников, но
    // берётся при этом материал обоих: второй источник не обязан
    // срабатывать одновременно с первым, чтобы попасть в аккорд.
    const Signal* s1 = armSource(s, 0);
    const Signal* s2 = armSource(s, 1);
    bool fired = false;
    // sawA/sawB копятся до ближайшего шага сетки — их читает LOGIC,
    // у которого проверка происходит не в момент прихода ноты.
    if (s1 && s1->serial != rt[s].lastSerial)  { rt[s].lastSerial  = s1->serial; fired = true; rt[s].sawA = true; }
    if (s2 && s2->serial != rt[s].lastSerial2) { rt[s].lastSerial2 = s2->serial; fired = true; rt[s].sawB = true; }
    if (fired) {
      captureFrom(s, s1, s2);
      if (cfg[s].bypass) {
        // Провод: отдаём захваченное дальше и на этом всё.
        if (rt[s].capCount)
          publish(s, rt[s].capNotes, rt[s].capCount,
                  rt[s].capGateMs, rt[s].capScale, rt[s].capRoot);
      } else {
        onCapture(s);
      }
    }
    if (!cfg[s].bypass) serviceProcessor(s);
  }
}

// ============================================================
// 12. MIDI CLOCK
// ============================================================
uint32_t lastInternalTickMicros = 0;

bool anySlotRunning() {
  for (uint8_t s = 0; s < NUM_SLOTS; s++) if (cfg[s].running) return true;
  return false;
}

// Синтетический тик 24 ppqn из общего BPM, когда внешнего клока нет.
bool generateInternalTicks() {
  if (state.extClock || !anySlotRunning()) return false;
  uint32_t now = micros();
  if (lastInternalTickMicros == 0) { lastInternalTickMicros = now; return false; }
  uint16_t bpm = state.bpm < 20 ? 20 : state.bpm;
  uint32_t period = (uint32_t)((60.0f / bpm) * 1000000.0f / 24.0f);
  if (now - lastInternalTickMicros >= period) {
    lastInternalTickMicros += period;
    return true;
  }
  return false;
}

void onClockTick() {
  state.midiTicks++;
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    SlotCfg &c = cfg[s];
    if (!c.running) continue;
    bool gen = isGenerator(c.app);
    bool arp = ((AppId)c.app == AppId::ARP);
    if (!gen && !arp) continue;
    uint8_t rate = arp ? (uint8_t)clampInt(modP(s, 0), 0, 4) : modRate(s);
    uint32_t ticksPerStep = rateTicks[rate];
    if (state.midiTicks - rt[s].lastStepTick < ticksPerStep) continue;
    rt[s].lastStepTick = state.midiTicks;
    if (gen) fireGenerator(s);
    else if (!c.bypass) fireArpStep(s);   // в байпасе ARP не свой шаг, а провод
    state.displayDirty = true;
  }
}

void processExternalClockTick() {
  uint32_t now = micros();
  if (state.lastClockMicros) {
    uint32_t delta = now - state.lastClockMicros;
    if (delta > 500 && delta < 100000) {
      state.clockAccumulator += delta;
      if (++state.clockCount >= 24) {
        float avg = (float)state.clockAccumulator / state.clockCount;
        float inst = 60.0f / (avg / 1000000.0f * 24.0f);
        state.bpmSmooth = (state.bpmSmooth == 0) ? inst : state.bpmSmooth * 0.7f + inst * 0.3f;
        state.extBpm = state.bpmSmooth;
        state.clockAccumulator = 0;
        state.clockCount = 0;
      }
    } else {
      state.clockAccumulator = 0;
      state.clockCount = 0;
    }
  }
  state.lastClockMicros = now;
  state.extClock = true;
  onClockTick();
}

// ----- Входящий MIDI -----
// Полноценный разбор нужен только для EXT-захвата: это единственное
// место, где сигнал по-настоящему полифоничен — держим буфер до 4
// одновременно зажатых нот с корректным учётом Note On/Off.
uint8_t midiRunningStatus = 0;
uint8_t midiPendingByte = 0;
bool midiHaveFirstData = false;

void extNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  Signal &e = extSignal[ch];
  for (uint8_t i = 0; i < e.count; i++) if (e.notes[i] == note) return;
  if (e.count < MAX_VOICES) {
    e.vel[e.count] = vel;
    e.notes[e.count++] = note;
  } else {
    for (uint8_t i = 1; i < MAX_VOICES; i++) {
      e.notes[i - 1] = e.notes[i];
      e.vel[i - 1] = e.vel[i];
    }
    e.notes[MAX_VOICES - 1] = note;
    e.vel[MAX_VOICES - 1] = vel;
  }
  e.gateMs = 200;
  e.serial++;
}

void extNoteOff(uint8_t ch, uint8_t note) {
  Signal &e = extSignal[ch];
  for (uint8_t i = 0; i < e.count; i++) {
    if (e.notes[i] != note) continue;
    for (uint8_t j = i; j + 1 < e.count; j++) {
      e.notes[j] = e.notes[j + 1];
      e.vel[j] = e.vel[j + 1];
    }
    e.count--;
    return;
  }
}

void startAll() {
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    rt[s].lastStepTick = state.midiTicks;
    rt[s].stepIndex = 0;
  }
}

void processMIDI() {
  uint8_t count = 0;
  while (midi.available() && count < 64) {
    uint8_t data = midi.read();
    count++;
    if (data >= 0xF8) {
      switch (data) {
        case 0xF8: processExternalClockTick(); break;
        case 0xFA:
          state.midiTicks = 0;
          state.lastClockMicros = 0;
          state.clockAccumulator = 0;
          state.clockCount = 0;
          state.bpmSmooth = 0;
          for (uint8_t s = 0; s < NUM_SLOTS; s++) cfg[s].running = true;
          startAll();
          break;
        case 0xFB:
          for (uint8_t s = 0; s < NUM_SLOTS; s++) cfg[s].running = true;
          break;
        case 0xFC:
          for (uint8_t s = 0; s < NUM_SLOTS; s++) cfg[s].running = false;
          flushScheduler();
          break;
      }
      continue;
    }
    if (data & 0x80) { midiRunningStatus = data; midiHaveFirstData = false; continue; }
    if (midiRunningStatus == 0) continue;
    uint8_t type = midiRunningStatus & 0xF0;
    if (type == 0xC0 || type == 0xD0) continue;  // 1-байтовые — не нужны
    if (!midiHaveFirstData) { midiPendingByte = data; midiHaveFirstData = true; continue; }
    midiHaveFirstData = false;
    // Контроллеры слушаем ОМНИ, по всем каналам: привязка адресуется
    // номером CC, а не слотом, и требовать «нужный канал» значило бы
    // отдать под модуляцию только четыре из шестнадцати.
    if (type == 0xB0) {
      ccVal[midiPendingByte & 0x7F] = (uint8_t)(data & 0x7F);
      continue;
    }
    // Ноты — наоборот: канал сообщения и есть адрес слота, 1..4 -> A..D.
    uint8_t mch = midiRunningStatus & 0x0F;
    if (mch >= NUM_SLOTS) continue;
    if (type == 0x90) {
      if (data > 0) extNoteOn(mch, midiPendingByte & 0x7F, data & 0x7F);
      else extNoteOff(mch, midiPendingByte & 0x7F);
    } else if (type == 0x80) {
      extNoteOff(mch, midiPendingByte & 0x7F);
    }
  }
}

// ============================================================
// 13. РАБОТА С ПАРАМЕТРАМИ
// ============================================================
int16_t metaNextAddr(uint8_t slot, int16_t a, int dir);

void markEdited() {
  state.needSave = true;
  state.lastEditMs = millis();
  state.displayDirty = true;
}

// Дескриптор, значение и запись параметра адресуются тройкой
// (слот, страница, колонка). Страница передаётся аргументом, а не
// берётся из state: иначе нельзя обратиться к подстранице чужого
// слота, не переключив туда экран — а это ровно то, что нужно
// модуляции параметров.
PDesc columnDesc(uint8_t slot, bool sub, uint8_t col) {
  const SlotCfg &c = cfg[slot];
  if (!sub) return apps[c.app].p[col];
  if (isGenerator(c.app)) {
    // TOPOLOGY бьёт ритм, а не играет мелодию — лад ей ни к чему,
    // и колонка SCALE отдана под режим вывода.
    if (col == 2 && (AppId)c.app == AppId::TOPOLOGY)
      return {"MODE", 0, 1, P_LIST, topoMode};
    if (col < 3) return genSub[col];
    return apps[c.app].sub4;
  }
  return processorSubDesc(c.app, col);
}

int16_t* columnValue(uint8_t slot, bool sub, uint8_t col) {
  SlotCfg &c = cfg[slot];
  static int16_t scratch;
  {
    // У MULTX колонка это выход: энкодер крутит его сдвиг, канал
    // спрятан под TAP.
    int8_t o = multxOut(c.app, sub, col);
    if (o >= 0) { scratch = c.outXp[o]; return &scratch; }
    int8_t pr = rrPair(c.app, sub, col);
    if (pr >= 0) { scratch = c.outCh[pr * 2]; return &scratch; }
    if ((AppId)c.app == AppId::RROBIN && sub && col == 0) {
      scratch = c.stepPlus; return &scratch;
    }
  }
  if (!sub) return &c.p[col];
  if (isGenerator(c.app)) {
    switch (col) {
      case 0: scratch = c.rate; return &scratch;      // BPM — под TAP
      case 1: scratch = c.midiCh; return &scratch;
      case 2:
        scratch = ((AppId)c.app == AppId::TOPOLOGY) ? c.sub3 : (int16_t)c.scale;
        return &scratch;
      default: return &c.sub4;
    }
  }
  switch (col) {
    case 0:
      // У METAP модулю нечего слать наружу, поэтому место CH занято
      // первой глубиной; она знаковая, а midiCh беззнаковый.
      scratch = ((AppId)c.app == AppId::META) ? c.metaDepth[0] : (int16_t)c.midiCh;
      return &scratch;
    case 1: scratch = c.arm; return &scratch;
    case 2:
      if ((AppId)c.app == AppId::META) { scratch = c.metaDepth[1]; return &scratch; }
      return usesSub2(c.app) ? &c.sub2 : (scratch = c.ccDest, &scratch);
    default:
      if ((AppId)c.app == AppId::META) { scratch = c.metaDepth[2]; return &scratch; }
      return usesSub3(c.app) ? &c.sub3 : (scratch = c.stepPlus, &scratch);
  }
}

// Приводит все хранимые в слоте высоты к его текущему ладу — и общие
// параметры, и то, что запомнили отдельные шаги. Вызывается при смене
// SCALE/ROOT: уже записанная последовательность переезжает на
// ближайшие валидные ступени нового лада, а не остаётся вне его.
// Разводит голоса, схлопнувшиеся на одну высоту: следующий свободный
// голос уходит на ступень выше. Без этого смена лада молча съедала
// голос — выставлено три ноты, звучит две.
int16_t spreadVoice(int16_t v, const int16_t* taken, uint8_t n, uint8_t sc, uint8_t root) {
  for (uint8_t guard = 0; guard < 12; guard++) {
    bool clash = false;
    for (uint8_t j = 0; j < n; j++) if (taken[j] == v) { clash = true; break; }
    if (!clash) return v;
    int16_t up = (int16_t)transposeDegrees((uint8_t)v, 1, sc, root);
    if (up == v) break;            // выше ступеней нет — оставляем как есть
    v = up;
  }
  return v;
}

void requantizeSlotNotes(uint8_t slot) {
  SlotCfg &c = cfg[slot];
  if (!isGenerator(c.app)) return;
  uint8_t sc = modScale(slot), rootN = modRoot(slot);
  // Колонки-голоса идут подряд, и каждая следующая обязана остаться
  // отличной от предыдущих — и в общем параметре, и на каждом шаге.
  int16_t takenP[NUM_COLS];
  uint8_t nP = 0;
  int16_t takenS[16][NUM_COLS];
  uint8_t nS[16] = {0};
  for (uint8_t i = 0; i < NUM_COLS; i++) {
    if (apps[c.app].p[i].type != P_NOTE) continue;
    c.p[i] = spreadVoice(quantizeToScale(c.p[i], sc, rootN), takenP, nP, sc, rootN);
    takenP[nP++] = c.p[i];
    for (uint8_t s = 0; s < 16; s++) {
      int16_t v = spreadVoice(quantizeToScale(c.stepVal[s][i], sc, rootN),
                              takenS[s], nS[s], sc, rootN);
      c.stepVal[s][i] = (int8_t)clampInt(v, -128, 127);
      takenS[s][nS[s]++] = v;
    }
  }
  if (apps[c.app].sub4.type == P_NOTE)
    c.sub4 = quantizeToScale(modSub4(slot), modScale(slot), modRoot(slot));
}

// Шаг редактирования параметра. Высоты у генератора ходят по СТУПЕНЯМ
// лада, а не по полутонам: нот вне лада на устройстве не существует,
// и прокручивать их незачем.
int16_t nudgeValue(uint8_t slot, const PDesc &d, int16_t cur, int steps) {
  const SlotCfg &c = cfg[slot];
  if (d.type == P_NOTE && isGenerator(c.app))
    return (int16_t)transposeDegrees(cur, steps, modScale(slot), modRoot(slot));
  return (int16_t)clampInt(cur + steps, d.lo, d.hi);
}

// Записывает отредактированное значение туда, где оно реально живёт
// (часть параметров подстраницы — не поля SlotCfg::p).
void applyColumn(uint8_t slot, bool sub, uint8_t col, int16_t v) {
  SlotCfg &c = cfg[slot];
  {
    int8_t o = multxOut(c.app, sub, col);
    if (o >= 0) { c.outXp[o] = (int8_t)clampInt(v, -14, 14); return; }
    int8_t pr = rrPair(c.app, sub, col);
    if (pr >= 0) { c.outCh[pr * 2] = (uint8_t)clampInt(v, 0, 16); return; }
    if ((AppId)c.app == AppId::RROBIN && sub && col == 0) {
      c.stepPlus = (int8_t)clampInt(v, -12, 12); return;
    }
  }
  if (!sub) {
    c.p[col] = v;
    // Зависимые пределы: RES не может выйти за свой MOD, PULSE — за STEPS.
    if ((AppId)c.app == AppId::EUCLIDIAN) {
      uint8_t steps = (uint8_t)clampInt(c.p[0], 1, 16);
      c.p[1] = clampInt(c.p[1], 0, steps);
      c.p[2] = clampInt(c.p[2], 0, steps - 1);
    }
    return;
  }
  if (isGenerator(c.app)) {
    switch (col) {
      case 0: c.rate = (uint8_t)clampInt(v, 0, 4); break;
      case 1: c.midiCh = (uint8_t)clampInt(v, 0, 16); break;
      // Смена лада тянет за собой уже записанные высоты — иначе на
      // экране остались бы ноты, которых в новом ладу нет.
      case 2:
        if ((AppId)c.app == AppId::TOPOLOGY) { c.sub3 = (int16_t)clampInt(v, 0, 1); break; }
        c.scale = (uint8_t)clampInt(v, 0, 9);
        requantizeSlotNotes(slot);
        break;
      default: c.sub4 = v; break;
    }
    return;
  }
  PDesc d = processorSubDesc(c.app, col);
  switch (col) {
    case 0:
      if ((AppId)c.app == AppId::META) c.metaDepth[0] = (int8_t)clampInt(v, -64, 64);
      else c.midiCh = (uint8_t)clampInt(v, 0, 16);   // 0 = NO
      break;
    case 1: c.arm = armSkipSelf(slot, v, v - (int)c.arm, 0, ARM_EXT); break;
    case 2:
      if ((AppId)c.app == AppId::META) c.metaDepth[1] = (int8_t)clampInt(v, -64, 64);
      else if (usesSub2(c.app)) c.sub2 = (int16_t)clampInt(v, d.lo, d.hi);
      else c.ccDest = (uint8_t)clampInt(v, 0, 127);
      break;
    default:
      if ((AppId)c.app == AppId::META) c.metaDepth[2] = (int8_t)clampInt(v, -64, 64);
      else if (usesSub3(c.app)) c.sub3 = (int16_t)clampInt(v, d.lo, d.hi);
      else c.stepPlus = (int8_t)clampInt(v, -12, 12);
      break;
  }
}

void loadApp(uint8_t slot, uint8_t app) {
  SlotCfg &c = cfg[slot];
  c.app = app;
  // Параметры нового приложения стартуют с нижней границы своего
  // диапазона — иначе значения от прежнего приложения читались бы
  // как чужие (например PULSE = 0 у EUCLIDIAN). Исключение — высоты нот:
  // у них нижняя граница это C-1, которую большинство приборов просто
  // не отыграет, поэтому они стартуют с середины клавиатуры.
  for (uint8_t i = 0; i < NUM_COLS; i++)
    c.p[i] = (apps[app].p[i].type == P_NOTE) ? 60 : apps[app].p[i].lo;
  c.sub4 = (apps[app].sub4.type == P_NOTE) ? 60 : apps[app].sub4.lo;
  c.bypass = false;
  // Канал по умолчанию — свой у каждого слота: A на 1, B на 2 и так
  // далее. Совпадает с каналом, который этот слот слушает на входе.
  c.midiCh = (uint8_t)(slot + 1);
  // Колонки подстраницы, которые приложение занимает под своё,
  // начинают с нижней границы уже НОВОГО смысла.
  c.sub2 = processorSubDesc(app, 2).lo;
  c.sub3 = processorSubDesc(app, 3).lo;

  // Там, где нижняя граница даёт заведомо немой или вырожденный патч
  // (нулевая глубина, нулевая вероятность, паттерн в один шаг),
  // приложение стартует с осмысленного значения — иначе только что
  // загруженный модуль просто молчит.
  switch ((AppId)app) {
    case AppId::BASIC:
      c.sub4 = 16;                          // STEPS — полный паттерн
      break;
    case AppId::SKELETON:
      c.sub4 = 16;
      // Нижняя граница у обоих интервалов это -12, то есть две ноты из
      // трёх выходили одинаковыми и фигура звучала как одна нота.
      // Стартуем трезвучием: терция и квинта по ступеням лада.
      c.p[1] = 2; c.p[2] = 4;
      break;
    case AppId::EUCLIDIAN:
      c.p[0] = 16; c.p[1] = 4;              // STEPS / PULSES
      c.sub4 = 127;                         // TRG CHANCE — иначе ни один хит не сыграет
      break;
    case AppId::TOPOLOGY:
      c.p[0] = 64;                          // середина карты по X
      c.sub2 = 64;                          // и по Y
      c.p[1] = 90; c.p[2] = 80; c.p[3] = 70;  // плотности дорожек
      c.ktCC[0] = 36; c.ktCC[1] = 38; c.ktCC[2] = 42;  // бочка/малый/хэт
      c.sub3 = 0;                           // один канал
      c.sub4 = 0;                           // без хаоса
      break;
    case AppId::MLFO:
      c.p[1] = 0;                           // MORPH по центру, без клиппинга
      c.p[3] = 127;                         // AMT — иначе нулевая глубина
      break;
    case AppId::ENV:
      c.p[1] = 0;                           // A/D поровну
      break;
    case AppId::DUST:
      c.p[0] = 32;                          // FILL — иначе ни одной частицы
      break;
    case AppId::SHIFT:
      c.p[0] = 8;                           // LEN — петля в 8 событий
      c.p[1] = 20;                          // MUTAT — тихо подменяется
      break;
    case AppId::ADD:
      c.p[3] = 100;                         // VEL — иначе добавленная нота беззвучна
      break;
    case AppId::LOGIC:
      // sub4 у процессора свободен (это слот 4-го параметра подстраницы
      // генератора) — LOGIC держит в нём образец для сравнения CC.
      c.sub4 = 64;
      // AND при единственном источнике не выполняется никогда, и
      // свежезагруженный модуль просто обрывал цепочку. OR пропускает
      // A насквозь, пока второй вход не назначен.
      c.p[0] = 1;
      break;
    case AppId::MATH:
      c.p[2] = 4;                           // CONST — сразу слышимый интервал
      break;
    case AppId::META:
      // Целимся в соседние слоты, не в себя; глубины в нуле, чтобы
      // свежезагруженный модуль ничего не крутил без спроса.
      for (uint8_t i = 0; i < 3; i++) {
        c.p[1 + i] = metaNextAddr(slot, (int16_t)(slot * MP_COUNT), 1);
        c.metaDepth[i] = 0;
      }
      break;
    case AppId::NDELAY:
      c.p[0] = 3;                           // сама нота и два эха
      c.sub4 = 0;                           // деления нет: интервал = гейт источника
      break;
    case AppId::MULTX:
      // Первый выход — сухая копия на канал 1, остальные ждут в нуле.
      c.outCh[0] = 1;
      for (uint8_t i = 1; i < 8; i++) c.outCh[i] = 0;
      for (uint8_t i = 0; i < 8; i++) c.outXp[i] = 0;
      break;
    case AppId::POLYSPL:
      c.p[0] = 60;                          // точка разделения — середина
      c.p[1] = c.p[2] = 0;
      c.sub2 = (int16_t)(slot + 2);         // верхняя зона — следующий канал
      break;
    case AppId::RROBIN:
      c.p[0] = 4;                           // четыре голоса
      for (uint8_t i = 0; i < 4; i++) c.outCh[i] = (uint8_t)(i + 1);
      c.p[3] = 0;
      c.stepPlus = 0;
      break;
    case AppId::KEYTRK:
      c.sub2 = 60;                          // опорная нота — середина клавиатуры
      c.sub3 = 24;                          // ход в две октавы
      // Нижняя граница глубины это -64, то есть полный обратный ход:
      // по общему правилу все четыре линии открылись бы разом. Гасим
      // их явно и оставляем работать только первую.
      c.p[0] = 48;
      c.p[1] = c.p[2] = c.p[3] = 0;
      c.ktCC[0] = 74; c.ktCC[1] = 71;       // привычные cutoff / resonance
      c.ktCC[2] = 1;  c.ktCC[3] = 11;
      // STEP+ у KEYTRACK на подстранице не показан — обнуляем, иначе
      // он незаметно смещал бы трекинг.
      c.stepPlus = 0;
      break;
    default:
      break;
  }
  // Стартовая высота 60 не обязана лежать в текущем ладу слота.
  requantizeSlotNotes(slot);
  // Привязки модуляции указывали на колонки ПРЕЖНЕГО приложения — у
  // нового они означают совсем другое, так что карта слота чистится.
  for (uint8_t i = 0; i < 2 * NUM_COLS; i++) { c.modCC[i] = 0; c.modDepth[i] = 0; }
  // Триги и запомненные шагами значения относятся к прежнему
  // приложению — у нового колонки означают совсем другое.
  for (uint8_t col = 0; col < NUM_COLS; col++) {
    c.lane[col] = 0;
    // Значения шагов имеют смысл только у генератора; у процессора
    // поле не читается, и складывать туда его параметры (которые могут
    // не влезть в int8) незачем.
    for (uint8_t s = 0; s < 16; s++)
      c.stepVal[s][col] = isGenerator(app) ? (int8_t)c.p[col] : 0;
  }
  rt[slot] = SlotRt{};
  // 0 — валидное значение CC, поэтому «ещё ничего не слали» помечаем
  // невозможным 255: иначе первая же отправка нуля была бы съедена
  // фильтром «шлём только при изменении».
  for (uint8_t i = 0; i < NUM_COLS; i++) rt[slot].ktLast[i] = 255;
  busCur[slot] = Signal{};
  busCur[slot].scale = c.scale;
  busCur[slot].root = c.root;
  markEdited();
}

// Общий транспорт: если играет хоть один слот — останавливаем все,
// иначе запускаем все с начала их паттернов. Висит на удержании PLAY;
// короткое нажатие PLAY по-прежнему открывает список приложений.
constexpr uint32_t PLAY_HOLD_MS = 700;
// Сохранение затирает ячейку, поэтому требует осознанного удержания
// и предупреждает рамкой начиная с ARM.
constexpr uint32_t PATCH_HOLD_ARM  = 700;
constexpr uint32_t PATCH_HOLD_SAVE = 2000;

void toggleGlobalTransport() {
  bool any = anySlotRunning();
  for (uint8_t s = 0; s < NUM_SLOTS; s++) cfg[s].running = !any;
  if (any) {
    flushScheduler();          // висящие ноты гасим, иначе застрянут
  } else {
    startAll();
    lastInternalTickMicros = 0;
  }
  markEdited();
  state.displayDirty = true;
}

// Патчи объявлены ниже, вместе с хранилищем — кнопкам нужны только
// вызовы.
// Сводка для браузера: читается один раз при открытии, чтобы не
// перечитывать по 500 байт на каждую перерисовку.
struct PatchInfo {
  bool     used;
  uint8_t  app[NUM_SLOTS];
  uint16_t bpm;
};
PatchInfo patchInfo[PATCH_COUNT];

void patchScan();
void patchSave(uint8_t i);
void patchErase(uint8_t i);
bool patchLoad(uint8_t i);

// ============================================================
// 14. ОБРАБОТКА КНОПОК
// ============================================================
void updateButtons() {
  static bool tapWasReleased = false;

  for (uint8_t i = 0; i < 4; i++) {
    Button &b = buttons[i];
    bool raw = digitalRead(b.pin);
    if (raw != b.raw) { b.raw = raw; b.lastChange = millis(); }
    uint32_t debounceMs = (i == 3) ? ENC_SW_DEBOUNCE_MS : DEBOUNCE_MS;
    if (millis() - b.lastChange < debounceMs || b.lastStable == b.raw) continue;

    bool prev = b.lastStable;
    b.lastStable = b.raw;

    if (prev == LOW && b.lastStable == HIGH) {
      if (i == 1) { state.lastTapReleaseTime = millis(); tapWasReleased = true; }
      else if (i == 2) {
        if (state.pageWasPressed) {
          if (millis() - state.pagePressTime >= PLAY_HOLD_MS) {
            state.patchOpen = !state.patchOpen;
            if (state.patchOpen) { state.appListOpen = false; patchScan(); }
          } else if (state.patchOpen) {
            state.patchOpen = false;
          } else if (state.appListOpen) {
            state.appListOpen = false;
          } else if (buttons[1].lastStable == LOW) {
            state.subPage = !state.subPage;
          } else {
            state.currentSlot = (state.currentSlot + 1) % NUM_SLOTS;
            state.subPage = false;
          }
          state.displayDirty = true;
        }
        state.pageWasPressed = false;
      }
      else if (i == 3 && state.patchOpen) {
        // Короткое нажатие грузит; удержание уже сделало своё дело.
        // С зажатым TAP короткое нажатие не грузит ничего: иначе
        // сорвавшееся стирание молча заменило бы рабочий патч.
        if (state.patchHolding && !state.patchSaved && !state.patchErasing)
          patchLoad(state.patchSel);
        state.patchHolding = false;
        state.patchErasing = false;
        state.patchSaved = false;
        state.displayDirty = true;
      }
      else if (i == 0) {
        // Решение принимается на ОТПУСКАНИИ: до него неизвестно,
        // короткое это нажатие или удержание.
        if (state.playWasPressed) {
          if (state.patchOpen) {
            state.patchOpen = false;      // из браузера патчей PLAY просто выходит
            state.displayDirty = true;
          } else if (millis() - state.playPressTime >= PLAY_HOLD_MS) {
            cfg[state.currentSlot].bypass = !cfg[state.currentSlot].bypass;
            markEdited();
            state.displayDirty = true;
          } else {
            state.appListOpen = !state.appListOpen;
            if (state.appListOpen)
              state.appListSel = browsePosOf(cfg[state.currentSlot].app);
            state.displayDirty = true;
          }
        }
        state.playWasPressed = false;
      }
      b.processed = false;
      continue;
    }
    if (b.processed) continue;

    uint32_t now = millis();

    // ---- TAP ----
    if (i == 1) {
      if (tapWasReleased && (now - state.lastTapReleaseTime) < DOUBLE_CLICK_TIME
          && !state.tapDoubleClicked) {
        // Старт/стоп всего устройства разом. Отдельный транспорт у
        // слота смысла не имеет: ARP и внутренняя сетка LOGIC ждут
        // СВОЕГО флага running, поэтому при потактовом запуске
        // генератор шёл, а модулятор за ним стоял.
        toggleGlobalTransport();
        state.tapDoubleClicked = true;
        state.lastTapReleaseTime = 0;
        tapWasReleased = false;
        markEdited();
        b.processed = true;
        continue;
      }
      state.tapDoubleClicked = false;
      state.lastTapReleaseTime = 0;
      tapWasReleased = false;
      b.processed = true;
      continue;
    }

    // ---- PLAY: короткое нажатие — список приложений, удержание —
    // байпас текущего слота. Действие на отпускании: до него неясно,
    // короткое это нажатие или удержание.
    if (i == 0) {
      state.playWasPressed = true;
      state.playPressTime = now;
      b.processed = true;
      continue;
    }

    // ---- PAGE: короткое нажатие — слот или подстраница, удержание —
    // браузер патчей. Как и у PLAY, решение принимается на отпускании.
    if (i == 2) {
      state.pageWasPressed = true;
      state.pagePressTime = now;
      b.processed = true;
      continue;
    }

    // ---- ENC_SW ----
    if (state.patchOpen) {
      state.patchHoldStart = now;
      state.patchHolding = true;
      state.patchSaved = false;
      // TAP — вторая функция того же контрола, как и везде в приборе:
      // удержание без него сохраняет, с ним стирает. Решение снимается
      // в момент нажатия, чтобы отпущенный по дороге TAP не превратил
      // стирание в сохранение.
      state.patchErasing = (buttons[1].lastStable == LOW);
      b.processed = true;
      continue;
    }
    if (state.appListOpen) {
      loadApp(state.currentSlot, browseOrder[state.appListSel]);
      state.appListOpen = false;
      b.processed = true;
      continue;
    }

    if (state.encSWLastPress != 0 && now - state.encSWLastPress < DOUBLE_CLICK_TIME) {
      // Сбрасываем метку, чтобы третий клик подряд не читался как
      // ещё один "двойной".
      state.encSWLastPress = 0;
      SlotCfg &c = cfg[state.currentSlot];
      if (!state.subPage && isGenerator(c.app)) {
        // Триг на шаге под курсором — только для ВЫБРАННОЙ колонки,
        // у каждой колонки свой лейн (как active[4] в RED SEA).
        uint8_t k = rt[state.currentSlot].cursor & 15;
        uint8_t col = state.selectedCol;
        uint16_t bit = (uint16_t)(1u << k);
        if (c.lane[col] & bit) {
          c.lane[col] &= ~bit;
        } else {
          c.lane[col] |= bit;
          // Триг запоминает текущее значение выбранного параметра —
          // иначе шаг стартовал бы с нуля вместо того, что на экране.
          c.stepVal[k][col] = (int8_t)clampInt(c.p[col], -128, 127);
        }
        markEdited();
      }
      b.processed = true;
      continue;
    }
    state.encSWLastPress = now;

    if (buttons[1].lastStable == LOW) {
      state.selectedCol = (state.selectedCol + 1) % NUM_COLS;
      state.displayDirty = true;
    }
    b.processed = true;
  }
}

// ============================================================
// 15. ОБРАБОТКА ЭНКОДЕРА
// ============================================================
void handleEncoder() {
  static int accum = 0;
  noInterrupts();
  int raw = encoderTicks;
  encoderTicks = 0;
  interrupts();
  if (raw == 0) return;

  // Один физический щелчок энкодера = 4 квадратурных перехода;
  // логически это ровно один шаг ±1, а не ±4.
  accum += raw;
  int steps = accum / 4;
  accum -= steps * 4;
  if (steps == 0) return;

  if (state.patchOpen) {
    state.patchSel = (uint8_t)clampInt((int)state.patchSel + steps, 0, PATCH_COUNT - 1);
    state.patchHolding = false;        // сдвинули выбор — взведение снимаем
    state.displayDirty = true;
    return;
  }
  if (state.appListOpen) {
    if (buttons[1].lastStable == LOW) {
      // TAP + энкодер — на начало соседней группы. Тот же приём
      // «TAP = крупный шаг», что у RATE, ARM и остальных.
      uint8_t g = groupOfPos(state.appListSel);
      if (steps > 0) g = (uint8_t)((g + 1) % GROUP_COUNT);
      else g = (uint8_t)((g + GROUP_COUNT - 1) % GROUP_COUNT);
      state.appListSel = groupStart[g];
    } else {
      state.appListSel = (uint8_t)clampInt((int)state.appListSel + steps,
                                           0, APP_COUNT - 1);
    }
    state.displayDirty = true;
    return;
  }

  SlotCfg &c = cfg[state.currentSlot];
  if (c.app == (uint8_t)AppId::EMPTY) return;

  // Зажатый ENC_SW + вращение двигает курсор шага по ряду. Это
  // нажатие гасим как половинку двойного клика: иначе следующее
  // быстрое нажатие спарилось бы с ним и случайно зафиксировало шаг.
  if (buttons[3].lastStable == LOW && isGenerator(c.app) && !state.subPage) {
    SlotRt &r = rt[state.currentSlot];
    // Курсор ходит по длине паттерна, а не всегда по 16: за её краем
    // ячеек уже не нарисовано.
    uint8_t n = patternLen(state.currentSlot);
    r.cursor = (uint8_t)((r.cursor + steps % n + n) % n);
    state.encSWLastPress = 0;
    state.displayDirty = true;
    return;
  }

  uint8_t col = state.selectedCol;
  // В слое энкодер выбирает контроллер, TAP+энкодер — глубину. Тот же
  // двухстрочный приём, что у SCALE, ARM и половины модулей.
  if (state.modLayer && c.app != (uint8_t)AppId::EMPTY) {
    uint8_t mi = (uint8_t)((state.subPage ? NUM_COLS : 0) + col);
    if (columnDesc(state.currentSlot, state.subPage, col).name[0]) {
      if (buttons[1].lastStable == LOW)
        c.modDepth[mi] = (int8_t)clampInt(c.modDepth[mi] + steps, -64, 64);
      else
        c.modCC[mi] = (uint8_t)clampInt(c.modCC[mi] + steps, 0, 127);
      markEdited();
    }
    return;
  }
  // У MORPH LFO под TAP на колонке ACTIV лежит доля такта: активацию
  // меняешь редко, а скорость — постоянно.
  if (!state.subPage && (AppId)c.app == AppId::MLFO && col == 2 &&
      buttons[1].lastStable == LOW) {
    c.sub4 = (int16_t)clampInt(c.sub4 + steps, 0, 4);
    markEdited();
    return;
  }
  // У SHIFT под TAP на первой колонке лежит ROT — расстояние от головы
  // чтения до головы записи. Верхний предел ходит за LEN: писать дальше
  // собственной петли бессмысленно.
  if (!state.subPage && (AppId)c.app == AppId::SHIFT && col == 0 &&
      buttons[1].lastStable == LOW) {
    int len = clampInt(modP(state.currentSlot, 0), 1, 16);
    c.sub4 = (int16_t)clampInt(c.sub4 + steps, 0, len - 1);
    markEdited();
    return;
  }
  // Первая колонка TOPOLOGY — оси карты: энкодер ведёт X, TAP — Y, и
  // обе ходят по узлам сетки, а не по единицам. Затригованный шаг
  // правится так же, иначе он ездил бы по другим правилам, чем слот.
  if (!state.subPage && (AppId)c.app == AppId::TOPOLOGY && col == 0) {
    if (buttons[1].lastStable == LOW) {
      c.sub2 = topoNodeStep(c.sub2, steps, TOPO_NY);
    } else {
      uint8_t k = rt[state.currentSlot].cursor & 15;
      if (c.lane[0] & (uint16_t)(1u << k))
        c.stepVal[k][0] = (int8_t)topoNodeStep(c.stepVal[k][0], steps, TOPO_NX);
      else
        c.p[0] = topoNodeStep(c.p[0], steps, TOPO_NX);
    }
    markEdited();
    return;
  }
  // У остальных колонок TOPOLOGY под TAP лежит нота дорожки.
  if (!state.subPage && (AppId)c.app == AppId::TOPOLOGY && buttons[1].lastStable == LOW) {
    c.ktCC[col - 1] = (uint8_t)clampInt(c.ktCC[col - 1] + steps, 0, 127);
    markEdited();
    return;
  }
  // BPM общий на всё устройство, не за слотом: два генератора всегда
  // играют в одном темпе. Живёт второй строчкой в колонке RATE.
  if (state.subPage && isGenerator(c.app) && col == 0 && buttons[1].lastStable == LOW) {
    state.bpm = (uint16_t)clampInt(state.bpm + steps, 20, 300);
    markEdited();
    return;
  }
  // На подстранице генератора колонка SCALE с зажатым TAP крутит ROOT.
  if (state.subPage && isGenerator(c.app) && col == 2 && buttons[1].lastStable == LOW) {
    c.root = (uint8_t)((c.root + steps % 12 + 12) % 12);
    requantizeSlotNotes(state.currentSlot);
    markEdited();
    return;
  }
  // У MULTX зажатый TAP на колонке-выходе крутит его MIDI-канал:
  // сдвиг меняешь чаще, поэтому он остался на энкодере.
  {
    int8_t o = multxOut(c.app, state.subPage, col);
    if (o >= 0 && buttons[1].lastStable == LOW) {
      c.outCh[o] = (uint8_t)clampInt(c.outCh[o] + steps, 0, 16);
      markEdited();
      return;
    }
  }
  // У RROBIN зажатый TAP правит второй канал пары — нижнюю строчку
  // колонки; энкодер без TAP остаётся на первом канале.
  {
    int8_t pr = rrPair(c.app, state.subPage, col);
    if (pr >= 0 && buttons[1].lastStable == LOW) {
      c.outCh[pr * 2 + 1] = (uint8_t)clampInt(c.outCh[pr * 2 + 1] + steps, 0, 16);
      markEdited();
      return;
    }
  }
  // У METAP энкодер перебирает параметры целевого слота, а TAP
  // перескакивает на следующий слот, сохраняя номер колонки.
  if (!state.subPage && (AppId)c.app == AppId::META && col >= 1) {
    int16_t a = c.p[col];
    if (buttons[1].lastStable == LOW) {
      uint8_t ts = metaTgtSlot(a);
      for (uint8_t g = 0; g < NUM_SLOTS; g++) {
        ts = (uint8_t)((ts + (steps >= 0 ? 1 : NUM_SLOTS - 1)) % NUM_SLOTS);
        if (ts != state.currentSlot) break;
      }
      a = (int16_t)(ts * MP_COUNT + metaTgtIdx(a));
      if (!metaParamDesc(ts, metaTgtIdx(a)).name[0])
        a = metaNextAddr(state.currentSlot, a, 1);
    } else {
      for (int k = 0; k < (steps > 0 ? steps : -steps); k++)
        a = metaNextAddr(state.currentSlot, a, steps);
    }
    c.p[col] = a;
    markEdited();
    return;
  }
  // У NDELAY зажатый TAP на колонке RPEAT крутит деление интервала:
  // число повторов меняешь чаще, поэтому оно осталось на энкодере.
  if (!state.subPage && (AppId)c.app == AppId::NDELAY && col == 0 &&
      buttons[1].lastStable == LOW) {
    c.sub4 = (int16_t)clampInt(c.sub4 + steps, 0, 4);
    markEdited();
    return;
  }
  // У KEYTRACK зажатый TAP на любой колонке крутит номер CC этой
  // линии — глубину меняешь чаще, поэтому она осталась на энкодере.
  if (!state.subPage && (AppId)c.app == AppId::KEYTRK && buttons[1].lastStable == LOW) {
    c.ktCC[col] = (uint8_t)clampInt(c.ktCC[col] + steps, 0, 127);
    markEdited();
    return;
  }
  // У LOGIC в режиме T+CC колонка CMP с зажатым TAP крутит значение,
  // с которым сравнивается CC второго входа.
  if (!state.subPage && (AppId)c.app == AppId::LOGIC && col == 1 &&
      c.p[1] == 2 && buttons[1].lastStable == LOW) {
    c.sub4 = (int16_t)clampInt(c.sub4 + steps, 0, 127);
    markEdited();
    return;
  }
  // На подстранице процессора колонка ARM с зажатым TAP крутит ВТОРОЙ
  // источник. У него, в отличие от первого, есть значение NO.
  if (state.subPage && isProcessor(c.app) && col == 1 && buttons[1].lastStable == LOW) {
    c.arm2 = armSkipSelf(state.currentSlot, c.arm2 + steps, steps, 0, ARM_NONE);
    markEdited();
    return;
  }

  PDesc d = columnDesc(state.currentSlot, state.subPage, col);
  if (d.name[0] == 0) return;

  // Если шаг под курсором затригован по этой колонке — крутим значение
  // САМОГО ШАГА, а не общий параметр слота (так же ведёт себя энкодер
  // на странице секвенсера в RED SEA).
  if (!state.subPage && isGenerator(c.app)) {
    uint8_t k = rt[state.currentSlot].cursor & 15;
    if (c.lane[col] & (uint16_t)(1u << k)) {
      c.stepVal[k][col] = (int8_t)nudgeValue(state.currentSlot, d, c.stepVal[k][col], steps);
      markEdited();
      return;
    }
  }

  int16_t cur = *columnValue(state.currentSlot, state.subPage, col);
  applyColumn(state.currentSlot, state.subPage, col,
              nudgeValue(state.currentSlot, d, cur, steps));
  markEdited();
}

// ============================================================
// 16. ОТРИСОВКА
// ============================================================
// Пиктограммы приложений 8x8, бит 7 = левый столбец. Индексируются
// прямо по AppId, поэтому порядок строк обязан совпадать с enum.
const uint8_t appIcons[APP_COUNT][8] = {
  // EMPTY — пусто
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
  // BASIC — три ноты фигуры разной высоты на общей линии
  {0x00, 0x18, 0x18, 0xD8, 0xDB, 0xDB, 0xDB, 0xFF},
  // SKELETON — три опорные ступени, не связанные между собой
  {0x00, 0x03, 0x00, 0x18, 0x00, 0x00, 0xC0, 0x00},
  // EUCLIDIAN — колесо евклидова ритма со ступицей
  {0x3C, 0x42, 0x81, 0x99, 0x99, 0x81, 0x42, 0x3C},
  // TOPOLOGY — рельеф карты с точкой навигации
  {0x00, 0x18, 0x24, 0x5A, 0xA5, 0x42, 0x81, 0x00},
  // ARP — восходящая лесенка арпеджио
  {0x00, 0x03, 0x03, 0x0C, 0x0C, 0x30, 0x30, 0xC0},
  // STRUM — ноты, разнесённые во времени: штрихи уходят вправо-вниз
  {0xE0, 0x00, 0x38, 0x00, 0x0E, 0x00, 0x03, 0x00},
  // NOTE DELAY — затухающие повторы на общей базовой линии
  {0x00, 0x80, 0x80, 0xA0, 0xA0, 0xA8, 0xAA, 0xFF},
  // ENV — пик атаки/спада над базовой линией
  {0x10, 0x28, 0x24, 0x44, 0x42, 0x82, 0x81, 0xFF},
  // MORPH LFO — период синуса
  {0x00, 0x00, 0x20, 0x50, 0x88, 0x05, 0x02, 0x00},
  // DUST — рассыпанные частицы
  {0x22, 0x04, 0x88, 0x12, 0x42, 0x88, 0x24, 0x04},
  // GLIDER — кривая глайда, круто вверх и выполаживается
  {0x03, 0x0C, 0x10, 0x20, 0x40, 0x40, 0x80, 0x80},
  // ADD — исходная нота снизу, добавленная (полая) сверху
  {0x78, 0x48, 0x78, 0x00, 0x00, 0x78, 0x78, 0x00},
  // SHIFT — цепочка ячеек со стрелкой возврата
  {0x00, 0x7E, 0x42, 0x7E, 0x42, 0x7E, 0x08, 0x1C},
  // LOGIC — силуэт вентиля: два входа слева, выход справа
  {0x00, 0xF8, 0x86, 0x81, 0x81, 0x86, 0xF8, 0x00},
  // MATH — плюс над минусом, знак арифметики
  {0x00, 0x18, 0x18, 0xFF, 0x18, 0x18, 0x00, 0xFF},
  // KEYTRACK — рампа над клавиатурой
  {0x03, 0x0C, 0x30, 0xC0, 0x00, 0xFF, 0xDB, 0xFF},
  // MULTX — один вход расходится веером
  {0x80, 0x43, 0x24, 0x1F, 0x24, 0x43, 0x80, 0x00},
  // SPLIT — клавиатура, разрубленная пополам
  {0xFF, 0xDB, 0xFF, 0x10, 0x10, 0xFF, 0xDB, 0xFF},
  // RROBIN — стрелка по кругу
  {0x3C, 0x42, 0x99, 0x81, 0x81, 0x99, 0x42, 0x3C},
  // METAP — рука над тремя ручками
  {0x00, 0x54, 0xFE, 0x54, 0x10, 0x38, 0x54, 0x92}
};

void drawIcon(uint8_t x, uint8_t y, const uint8_t* bmp, uint16_t color) {
  for (uint8_t r = 0; r < 8; r++)
    for (uint8_t col = 0; col < 8; col++)
      if (bmp[r] & (0x80 >> col)) display.drawPixel(x + col, y + r, color);
}

// ---- Созвездие слотов ----
// Карта глобальная: все четыре слота всегда стоят на своих местах и
// все связи видны разом, независимо от того, какая страница открыта.
// Страница только двигает курсор по звёздам — сам рисунок созвездия
// не перестраивается, поэтому его положение запоминается глазом.
//
// Позиции и высоты закреплены за слотом. Разнобой по высоте намеренный:
// он и делает из ряда точек созвездие.
const uint8_t SLOT_CX[NUM_SLOTS] = {20, 48, 76, 104};
const uint8_t SLOT_CY[NUM_SLOTS] = {7, 3, 8, 4};
constexpr uint8_t STAR_R = 3;     // радиус звезды вместе с лучами

// Звезда — полый ромб с четырьмя лучами по сторонам света. Текущий
// слот тот же ромб, но залитый.
void drawStarGlyph(uint8_t cx, uint8_t cy, bool filled) {
  display.drawPixel(cx, cy - 3, SSD1306_WHITE);
  display.drawPixel(cx, cy + 3, SSD1306_WHITE);
  display.drawPixel(cx - 3, cy, SSD1306_WHITE);
  display.drawPixel(cx + 3, cy, SSD1306_WHITE);
  if (filled) {
    display.drawFastHLine(cx - 2, cy, 5, SSD1306_WHITE);
    display.drawFastHLine(cx - 1, cy - 1, 3, SSD1306_WHITE);
    display.drawFastHLine(cx - 1, cy + 1, 3, SSD1306_WHITE);
    display.drawPixel(cx, cy - 2, SSD1306_WHITE);
    display.drawPixel(cx, cy + 2, SSD1306_WHITE);
  } else {
    display.drawPixel(cx, cy - 2, SSD1306_WHITE);
    display.drawPixel(cx - 1, cy - 1, SSD1306_WHITE);
    display.drawPixel(cx + 1, cy - 1, SSD1306_WHITE);
    display.drawPixel(cx - 2, cy, SSD1306_WHITE);
    display.drawPixel(cx + 2, cy, SSD1306_WHITE);
    display.drawPixel(cx - 1, cy + 1, SSD1306_WHITE);
    display.drawPixel(cx + 1, cy + 1, SSD1306_WHITE);
    display.drawPixel(cx, cy + 2, SSD1306_WHITE);
  }
}

// Курсор текущего слота — скобки по бокам звезды. Обрамления сверху и
// снизу нет намеренно: звёзды расходятся по высоте на всю полосу
// шапки, и рамке там уже не хватило бы места.
// Слот под модуляцией: уголки вплотную к контуру звезды. По вертикали
// не выходят за её лучи (полоса шапки всего 12 px, верхние звёзды стоят
// на y=3), по горизонтали не доходят до коридора, по которому приходят
// связи. Тот же язык, что у уголков вокруг выбранной колонки.
void drawStarMod(uint8_t cx, uint8_t cy) {
  for (int8_t sx = -1; sx <= 1; sx += 2)
    for (int8_t sy = -1; sy <= 1; sy += 2) {
      display.drawPixel((uint8_t)(cx + sx * 2), (uint8_t)(cy + sy * 3), SSD1306_WHITE);
      display.drawPixel((uint8_t)(cx + sx * 3), (uint8_t)(cy + sy * 2), SSD1306_WHITE);
    }
}

// Ведут ли слот прямо сейчас: есть хоть одна привязка с глубиной.
bool slotModulated(uint8_t slot) {
  const SlotCfg &c = cfg[slot];
  if (c.app == (uint8_t)AppId::EMPTY) return false;
  for (uint8_t i = 0; i < 2 * NUM_COLS; i++)
    if (c.modCC[i] && c.modDepth[i]) return true;
  return false;
}

void drawStarCursor(uint8_t cx, uint8_t cy) {
  for (int8_t d = -1; d <= 1; d++) {
    display.drawPixel(cx - 6, cy + d, SSD1306_WHITE);
    display.drawPixel(cx + 6, cy + d, SSD1306_WHITE);
  }
  display.drawPixel(cx - 5, cy - 2, SSD1306_WHITE);
  display.drawPixel(cx - 5, cy + 2, SSD1306_WHITE);
  display.drawPixel(cx + 5, cy - 2, SSD1306_WHITE);
  display.drawPixel(cx + 5, cy + 2, SSD1306_WHITE);
}

// Фоновая россыпь: одиночные точки в промежутках между звёздами
// созвездия. Координаты подобраны так, чтобы не попадать ни на звёзды
// (центры ±3), ни на скобки курсора (центр ±6), ни на иконку слева,
// ни на индикаторы справа. У каждой свой период мерцания — поэтому
// россыпь дышит вразнобой, а не пульсирует целиком.
struct BgStar { uint8_t x, y; uint16_t period; };
const BgStar bgStars[] = {
  { 10,  3,  9000}, { 12,  9, 15000},
  { 29,  1, 11000}, { 33,  8,  7000}, { 38,  4, 16000}, { 40, 10, 12500},
  { 57,  2,  8500}, { 61,  9, 14500}, { 66,  5, 10000}, { 68, 11, 17500},
  { 85,  1,  9500}, { 89,  8, 13500}, { 94,  3,  7800}, { 96, 10, 15500}
};
constexpr uint8_t BG_STAR_COUNT = sizeof(bgStars) / sizeof(bgStars[0]);

void drawBgStars() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < BG_STAR_COUNT; i++) {
    const BgStar &s = bgStars[i];
    // Сдвиг фазы по координате — иначе точки с одинаковым периодом
    // мигали бы синхронно.
    uint32_t t = now + (uint32_t)s.x * 370;
    // Гаснет лишь на пятую часть цикла: это мерцание, а не мигалка.
    if ((t % s.period) > (uint32_t)(s.period / 5))
      display.drawPixel(s.x, s.y, SSD1306_WHITE);
  }
}

// Метка внешнего входа — маленький квадрат слева от звезды со своим
// коротким поводком. Отдельного узла EXT в карте нет: он не слот, и
// резервировать под него постоянное место значило бы поджимать
// остальные четыре.
void drawExtStub(uint8_t cx, uint8_t cy) {
  display.drawRect(cx - 11, cy - 1, 3, 3, SSD1306_WHITE);
  display.drawFastHLine(cx - 8, cy, 4, SSD1306_WHITE);
}

// Упорядоченный дизеринг 8x8 — тот же, что в RED SEA. Нужен, чтобы
// рамка приёма гасла плавно, а не моргала целиком.
const uint8_t bayer[8][8] = {
  { 0, 32,  8, 40,  2, 34, 10, 42},
  {48, 16, 56, 24, 50, 18, 58, 26},
  {12, 44,  4, 36, 14, 46,  6, 38},
  {60, 28, 52, 20, 62, 30, 54, 22},
  { 3, 35, 11, 43,  1, 33,  9, 41},
  {51, 19, 59, 27, 49, 17, 57, 25},
  {15, 47,  7, 39, 13, 45,  5, 37},
  {63, 31, 55, 23, 61, 29, 53, 21}
};

// Рамка приёма: сплошная в момент захвата, дальше редеет по дизерингу
// и сходит на нет. Заливкой не мигаем — на 128x32 это бьёт по глазам.
void drawFadingFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     uint32_t elapsed, uint32_t len, uint16_t color) {
  if (elapsed >= len) return;
  uint8_t threshold = (uint8_t)(64 - (elapsed * 64) / len);
  for (uint8_t dy = 0; dy < h; dy++) {
    for (uint8_t dx = 0; dx < w; dx++) {
      if (dx && dy && dx != w - 1 && dy != h - 1) continue;   // только периметр
      if (bayer[dy & 7][dx & 7] < threshold) display.drawPixel(x + dx, y + dy, color);
    }
  }
}

// Признак того, что колонку кто-то ведёт: чёрточка по нижней кромке,
// растущая от центра в сторону смещения. Центр — модуляции нет, вправо
// — параметр подняли, влево — опустили, длина = насколько. Числа тут
// не хватило бы места, а главное нужно читать мгновенно: «это не я
// выставил, это его ведут».
void drawModBar(uint8_t x, int16_t off, int16_t lo, int16_t hi) {
  if (!off) return;
  constexpr uint8_t REACH = 10;                  // не доходя до уголков курсора
  const uint8_t cx = (uint8_t)(x + Display::COL_W / 2);
  const uint8_t y  = Display::COL_Y + Display::COL_H - 1;
  int span = hi - lo;
  if (span < 1) span = 1;
  int len = (int)((long)abs(off) * REACH * 2 / span);
  if (len < 1) len = 1;
  if (len > REACH) len = REACH;
  if (off > 0) display.drawFastHLine(cx, y, (uint8_t)len, SSD1306_WHITE);
  else         display.drawFastHLine((uint8_t)(cx - len + 1), y, (uint8_t)len, SSD1306_WHITE);
  display.drawPixel(cx, y - 1, SSD1306_WHITE);   // засечка центра
}

void drawCornerCursor(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color) {
  for (uint8_t i = 0; i < 3; i++) {
    display.drawPixel(x + i, y, color);
    display.drawPixel(x, y + i, color);
    display.drawPixel(x + w - 1 - i, y, color);
    display.drawPixel(x + w - 1, y + i, color);
    display.drawPixel(x + i, y + h - 1, color);
    display.drawPixel(x, y + h - 1 - i, color);
    display.drawPixel(x + w - 1 - i, y + h - 1, color);
    display.drawPixel(x + w - 1, y + h - 1 - i, color);
  }
}

// Форма волны MORPH LFO — порт визуала FOG из RED SEA: та же
// сплошная линия, построенная по той же computeLFO, что и звучит,
// так что картинка не разъезжается с выходом.
void drawWaveform(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                  uint8_t type, int morph, float phaseOffset, uint16_t color) {
  constexpr float AMP = 0.7f;
  float prevVal = 0;
  for (uint8_t px = 0; px < w; px++) {
    float t = (float)px / w;
    float val = computeLFO(t * TWO_PI + phaseOffset, type, morph) * AMP;
    int py = y + h / 2 - (int)(val * (h / 2 - 1));
    py = clampInt(py, y, y + h - 1);
    if (px == 0) {
      display.drawPixel(x + px, py, color);
    } else {
      // Соседние отсчёты соединяем вертикальным отрезком — иначе на
      // крутых участках волна рассыпается в отдельные точки.
      int prevPy = clampInt(y + h / 2 - (int)(prevVal * (h / 2 - 1)), y, y + h - 1);
      int a = prevPy < py ? prevPy : py;
      int b = prevPy < py ? py : prevPy;
      for (int ly = a; ly <= b; ly++) display.drawPixel(x + px, ly, color);
    }
    prevVal = val;
  }
}

void printCentered(uint8_t x, uint8_t w, uint8_t y, const char* text, uint16_t color) {
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  display.setTextColor(color);
  display.setCursor(x + (w > tw ? (w - tw) / 2 : 0), y);
  display.print(text);
}

void formatValue(const PDesc &d, int16_t v, char* out) {
  switch (d.type) {
    case P_NOTE: midiNoteToString((uint8_t)clampInt(v, 0, 127), out); break;
    case P_LIST: strcpy(out, d.names ? d.names[clampInt(v, d.lo, d.hi)] : "?"); break;
    case P_CHAN: if (v <= 0) strcpy(out, "NO"); else sprintf(out, "%d", v); break;
    case P_MS:   sprintf(out, "%dm", v); break;
    case P_SIGN: sprintf(out, "%+d", v); break;
    default:     sprintf(out, "%d", v); break;
  }
}

// Ряд из 16 шагов вместо текстовой шапки — визуал страницы секвенсера
// из RED SEA. По краям остаются буква слота и индикатор транспорта,
// поэтому ряд занимает не всю ширину.
//
// Что показывает ячейка:
//   заливка       — шаг прозвучит
//   точка в центре — на шаге стоит триг ВЫБРАННОЙ колонки (лейн
//                    переключается вместе с колонкой, как в RED SEA)
//   внутренняя рамка — плейхед
//   штрих под ячейкой — курсор шага
// Все четыре признака независимы и не перекрывают друг друга.
// Точка TOPOLOGY на её карте. Сетка 5×5 — это настоящие узлы, между
// которыми идёт билинейное смешивание. Энкодер ставит маркер ровно на
// узел, а модуляция водит его между ними непрерывно: под сеткой
// по-прежнему 0..127. Координаты считаются той же формулой, что и в
// topoLevel, иначе картинка разъехалась бы с ритмом.
void drawTopoMap(uint8_t slot, uint8_t x, uint16_t color) {
  constexpr uint8_t PITCH = 4;
  constexpr uint8_t SPAN  = (TOPO_NX - 1) * PITCH;   // 16 px между крайними узлами
  const uint8_t gx = x + 2;
  const uint8_t gy = Display::COL_Y + 2;
  for (uint8_t iy = 0; iy < TOPO_NY; iy++)
    for (uint8_t ix = 0; ix < TOPO_NX; ix++)
      display.drawPixel(gx + ix * PITCH, gy + iy * PITCH, color);

  int16_t vx = (int16_t)clampInt(modP(slot, 0), 0, 127);
  int16_t vy = (int16_t)clampInt(modSub2(slot), 0, 127);
  int32_t fx = (int32_t)vx * (TOPO_NX - 1) * 256 / 127;
  int32_t fy = (int32_t)vy * (TOPO_NY - 1) * 256 / 127;
  int mx = gx + (int)((fx * PITCH) >> 8);
  // Ось Y на экране растёт ВВЕРХ: крутишь энкодер в плюс — маркер идёт
  // вверх, как на любой карте. В самих данных строка 0 остаётся первой,
  // это чисто экранное соглашение.
  int my = gy + SPAN - (int)((fy * PITCH) >> 8);
  // Ромб радиусом 1 — тот же язык, что у звёзд созвездия, и его
  // не спутать с точкой узла, даже когда он стоит ровно на ней.
  display.drawPixel(mx, my, color);
  display.drawPixel(mx - 1, my, color);
  display.drawPixel(mx + 1, my, color);
  display.drawPixel(mx, my - 1, color);
  display.drawPixel(mx, my + 1, color);

  // Справа от сетки — номера узлов, X над Y. Маркер показывает, где ты
  // на карте, числа — на каком именно узле стоишь.
  constexpr uint8_t TX = 2 + SPAN + 2;                 // сразу за сеткой
  constexpr uint8_t TW = Display::COL_W - TX;
  char b[4];
  display.setTextSize(1);
  sprintf(b, "X%d", topoNodeIdx(vx, TOPO_NX) + 1);
  printCentered(x + TX, TW, Display::COL_Y + 2, b, color);
  sprintf(b, "Y%d", topoNodeIdx(vy, TOPO_NY) + 1);
  printCentered(x + TX, TW, Display::COL_Y + 11, b, color);
}

// Сам регистр SHIFT: сетка 4×4, но нарисованы только LEN ячеек, так что
// энкодер буквально наращивает и укорачивает её. Залитая ячейка —
// голова чтения, она и есть индикация хода. Пока держишь TAP (то есть
// правишь ROT), заливка перескакивает на голову ЗАПИСИ: видно, куда
// именно ляжет новый материал, а не только что играет сейчас.
void drawShiftReg(uint8_t slot, uint8_t x, bool showWrite, uint16_t color) {
  constexpr uint8_t PITCH = 4, CELL = 3, COLS = 4;
  constexpr uint8_t SPAN = (COLS - 1) * PITCH + CELL;   // 15 px
  const uint8_t gx = x + (Display::COL_W - SPAN) / 2;
  const uint8_t gy = Display::COL_Y + (Display::COL_H - SPAN) / 2;

  const SlotRt &r = rt[slot];
  uint8_t len = (uint8_t)clampInt(modP(slot, 0), 1, 16);
  int     rot = clampInt(modSub4(slot), 0, 15) % len;
  uint8_t rd  = (uint8_t)(r.shPos % len);
  uint8_t mark = showWrite ? (uint8_t)((rd + rot) % len) : rd;

  for (uint8_t i = 0; i < len; i++) {
    uint8_t cx = gx + (i % COLS) * PITCH;
    uint8_t cy = gy + (i / COLS) * PITCH;
    if (i == mark)                display.fillRect(cx, cy, CELL, CELL, color);
    else if ((r.shRest >> i) & 1) display.drawPixel(cx + 1, cy + 1, color);
    else                          display.drawRect(cx, cy, CELL, CELL, color);
  }
}

// Столбики эха у NOTE DELAY: сколько нот, столько и палочек, а высота
// падает по DECAY — видно и число повторов, и скорость затухания.
void drawEchoBars(uint8_t slot, uint8_t x, uint16_t color) {
  uint8_t n = (uint8_t)clampInt(modP(slot, 0), 1, 8);
  int decay = clampInt(modP(slot, 3), 0, 127);
  constexpr uint8_t PITCH = 3, W = 2, HMAX = 9;
  const uint8_t base = Display::COL_Y + Display::COL_H - 1;   // низ колонки
  const uint8_t x0 = x + (Display::COL_W - (n * PITCH - 1)) / 2;
  int h = HMAX;
  for (uint8_t i = 0; i < n; i++) {
    if (h < 1) h = 1;
    display.fillRect(x0 + i * PITCH, (uint8_t)(base - h + 1), W, (uint8_t)h, color);
    h = h * (128 - decay) / 128;
  }
}

// Пинг-понг у NOTE DELAY — не панорама нотами, а CC, скачущий вокруг
// центра. Зигзаг показывает и сам скачок, и его размах.
void drawPingPong(uint8_t slot, uint8_t x, uint16_t color) {
  int pp = clampInt(modP(slot, 2), 0, 63);
  constexpr uint8_t SEG = 6, N = 4;
  const uint8_t x0 = x + (Display::COL_W - SEG * N) / 2;
  const uint8_t mid = Display::COL_Y + 14;
  int a = pp * 4 / 63;                       // размах в пикселях, 0..4
  for (uint8_t i = 0; i < N; i++) {
    int y = mid + ((i & 1) ? a : -a);
    display.drawFastHLine(x0 + i * SEG, (uint8_t)y, SEG, color);
    if (i) {                                 // вертикальная перемычка между ступенями
      int py = mid + ((i & 1) ? -a : a);
      int lo = py < y ? py : y, hi = py < y ? y : py;
      for (int v = lo; v <= hi; v++) display.drawPixel(x0 + i * SEG, (uint8_t)v, color);
    }
  }
}

// Облако DUST: точек столько, сколько частиц пропустит FILL. Раскладка
// детерминированная — случайная мерцала бы каждый кадр и читалась бы
// как шум, а не как значение параметра.
void drawDustField(uint8_t slot, uint8_t x, uint16_t color) {
  int fill = clampInt(modP(slot, 0), 0, 127);
  int rootPct = clampInt(modP(slot, 1), 0, 100);
  int reach = clampInt(modP(slot, 3), 0, 7);
  constexpr uint8_t W = 25, H = 9, TOTAL = 40;
  const uint8_t x0 = x + (Display::COL_W - W) / 2, y0 = Display::COL_Y + 10;
  const int mid = H / 2;
  uint16_t h = 0x2545;
  for (uint8_t i = 0; i < TOTAL; i++) {
    h = (uint16_t)(h * 25173 + 13849); uint8_t px = (uint8_t)((h >> 8) % W);
    h = (uint16_t)(h * 25173 + 13849); uint8_t sel = (uint8_t)((h >> 8) % 100);
    h = (uint16_t)(h * 25173 + 13849); uint8_t spr = (uint8_t)((h >> 8) % 15);
    // FILL решает, сколько частиц вообще есть.
    if ((int)i * 127 / TOTAL >= fill) continue;
    int py;
    // ROOT% — доля частиц, севших ровно на корень; остальные расходятся
    // от него, но не дальше REACH. На нуле REACH облако схлопывается в
    // линию — ровно так оно и звучит.
    if ((int)sel < rootPct) py = mid;
    else if (!reach)        py = mid;
    else                    py = mid + (((int)spr % (2 * reach + 1)) - reach) * mid / reach;
    display.drawPixel(x0 + px, (uint8_t)(y0 + clampInt(py, 0, H - 1)), color);
  }
  // Засечки корня по краям: относительно них и читается разброс.
  display.drawPixel(x0, (uint8_t)(y0 + mid), color);
  display.drawPixel(x0 + W - 1, (uint8_t)(y0 + mid), color);
}

// Огибающая ENVELOPE: сама кривая. Положение вершины — перекос A/D,
// форма хвоста — SHAPE. Число в этой колонке не говорило ничего.
void drawEnvShape(uint8_t slot, uint8_t x, uint16_t color) {
  uint8_t shape = (uint8_t)clampInt(modP(slot, 0), 0, 2);
  int ad = clampInt(modP(slot, 1), -63, 64);
  constexpr uint8_t W = 25, H = 9;
  const uint8_t x0 = x + (Display::COL_W - W) / 2;
  const uint8_t base = Display::COL_Y + 10 + H - 1;
  // A/D двигает вершину: минус — атака короче, плюс — длиннее.
  int peak = clampInt((W - 1) * (ad + 64) / 128, 1, W - 2);
  for (uint8_t i = 0; i < W; i++) {
    int h;
    if (i <= peak)        h = (H - 1) * i / peak;
    else if (shape == 1)  h = H - 1;             // AR держит полку до отпускания
    else                  h = (H - 1) * (W - 1 - i) / (W - 1 - peak);
    display.drawPixel(x0 + i, (uint8_t)(base - clampInt(h, 0, H - 1)), color);
  }
  // Петля — подчёркивание: кривая пойдёт заново.
  if (shape == 2) display.drawFastHLine(x0, base, W, color);
}

void drawStepRow(uint8_t slot) {
  const SlotCfg &c = cfg[slot];
  const SlotRt &r = rt[slot];
  // У TOPOLOGY ячейка поделена на три полосы — по одной на дорожку, — и
  // ради этого выше на два пикселя: три различимые полосы по 2 px не
  // помещаются в четыре внутренних пикселя обычного квадрата.
  const bool topo = (AppId)c.app == AppId::TOPOLOGY;
  const uint8_t CELL_H = topo ? 8 : 6;
  constexpr uint8_t CELL_W = 6, TOTAL = 7, Y = 1;
  // Рисуем ровно столько ячеек, сколько в паттерне, и центрируем их:
  // ячейки за его длиной всё равно никогда не сыграют.
  const uint8_t COUNT = patternLen(slot);
  const uint8_t X0 = (uint8_t)(8 + (112 - COUNT * TOTAL) / 2);

  char letter[2] = {(char)('A' + slot), 0};
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(letter);
  display.setCursor(121, 0);
  display.print(c.bypass ? "B" : (c.running ? ">" : "|"));

  // Все паттерны заворачиваются на своей длине, поэтому окно ряда
  // всегда начинается с нуля.
  const uint16_t base = 0;
  uint8_t play = (uint8_t)r.playStep;
  bool flash = (millis() - r.lastFireMs) < 60;

  for (uint8_t i = 0; i < COUNT; i++) {
    uint8_t x = X0 + i * TOTAL;
    bool isPlay = (i == play) && c.running;

    if (topo) {
      // Закрашена только та полоса, где у дорожки реально стоит удар:
      // три рисунка видно сразу, а не их слипшуюся сумму, как раньше.
      bool lit = isPlay && flash;
      display.fillRect(x, Y, CELL_W, CELL_H, lit ? SSD1306_WHITE : SSD1306_BLACK);
      display.drawRect(x, Y, CELL_W, CELL_H, SSD1306_WHITE);
      uint16_t bar = lit ? SSD1306_BLACK : SSD1306_WHITE;
      for (uint8_t t = 0; t < TOPO_TRACKS; t++)
        if (topoHitShown(slot, t, base + i))
          display.fillRect(x + 1, Y + 1 + t * 2, CELL_W - 2, 2, bar);
      // Плейхед ушёл штрихом НАД ячейкой: внутри её занимают полосы, и
      // рамке там уже не поместиться. Курсор остался штрихом снизу —
      // они на разных сторонах и не путаются.
      if (isPlay && !flash) display.drawFastHLine(x, Y - 1, CELL_W, SSD1306_WHITE);
    } else {
      bool hit = generatorHit(slot, base + i);
      bool trig = c.lane[state.selectedCol] & (uint16_t)(1u << i);

      if (isPlay && flash) display.fillRect(x, Y, CELL_W, CELL_H, SSD1306_WHITE);
      else display.fillRect(x, Y, CELL_W, CELL_H, hit ? SSD1306_WHITE : SSD1306_BLACK);
      display.drawRect(x, Y, CELL_W, CELL_H, SSD1306_WHITE);

      uint16_t inner = hit ? SSD1306_BLACK : SSD1306_WHITE;
      // Плейхед — внутренняя рамка в цвете фона ячейки, иначе на залитой
      // ячейке она рисовалась бы белым по белому и была бы не видна.
      if (isPlay && !flash) display.drawRect(x + 1, Y + 1, CELL_W - 2, CELL_H - 2, inner);
      // Метка трига — центр 2x2, он свободен от рамки плейхеда.
      if (trig) display.fillRect(x + 2, Y + 2, 2, 2, inner);
    }

    // Курсор выведен под ячейку: внутри неё места под лишний признак
    // нет, а полоса под рядом всё равно пустует.
    if (i == r.cursor) display.drawFastHLine(x, Y + CELL_H + 1, CELL_W, SSD1306_WHITE);
  }
}

void drawChainLink(uint8_t srcSlot, uint8_t dstSlot) {
  int16_t x0 = SLOT_CX[srcSlot], y0 = SLOT_CY[srcSlot];
  int16_t x1 = SLOT_CX[dstSlot], y1 = SLOT_CY[dstSlot];
  int16_t dir = (x1 > x0) ? 1 : -1;
  int16_t sx = x0 + dir * (STAR_R + 2);
  int16_t ex = x1 - dir * (STAR_R + 2);
  int16_t len = (int16_t)abs(ex - sx);
  if (len <= 0) return;
  uint16_t sparkAt = (uint16_t)((millis() / 16) % (uint16_t)(len + 8));
  for (int16_t k = 0; k <= len; k++) {
    int16_t px = sx + dir * k;
    int16_t py = y0 + (int16_t)(((int32_t)(y1 - y0) * (px - x0)) / (x1 - x0));
    if (k == (int16_t)sparkAt || k == (int16_t)sparkAt - 1)
      display.fillRect(px, py - 1, 2, 2, SSD1306_WHITE);
    else if (k % 2 == 0)
      display.drawPixel(px, py, SSD1306_WHITE);
  }
}


// Шапка: пиктограмма приложения текущего слота + созвездие всех
// слотов. Карта одна и та же на любой странице — рисуются все связи
// сразу, а страница лишь переставляет курсор на нужную звезду.
// Связь модуляции отличается от ARM намеренно: редкий пунктир и без
// бегущей искры. По ARM едут ноты, по этой линии — только управление,
// и путать их нельзя.
void drawModLink(uint8_t srcSlot, uint8_t dstSlot) {
  int16_t x0 = SLOT_CX[srcSlot], y0 = SLOT_CY[srcSlot];
  int16_t x1 = SLOT_CX[dstSlot], y1 = SLOT_CY[dstSlot];
  if (x1 == x0) return;
  int16_t dir = (x1 > x0) ? 1 : -1;
  int16_t sx = x0 + dir * (STAR_R + 2);
  int16_t ex = x1 - dir * (STAR_R + 2);
  int16_t len = (int16_t)abs(ex - sx);
  for (int16_t k = 0; k <= len; k += 4) {
    int16_t px = sx + dir * k;
    int16_t py = y0 + (int16_t)(((int32_t)(y1 - y0) * (px - x0)) / (x1 - x0));
    display.drawPixel(px, py, SSD1306_WHITE);
  }
}

void drawChainHeader(uint8_t slot) {
  if (state.modLayer) {
    // В слое вместо пиктограммы — надпись: иначе четыре числа на
    // привычных местах читались бы как значения параметров.
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("MOD");
  } else {
    drawIcon(0, 1, appIcons[cfg[slot].app], SSD1306_WHITE);
  }
  drawBgStars();   // фон — до всего остального

  // Связи рисуются первыми, чтобы звёзды легли поверх них. У каждого
  // процессора их до двух — по одной на источник.
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    if (!isProcessor(cfg[s].app)) continue;   // источник есть только у процессора
    bool extDrawn = false;
    for (uint8_t k = 0; k < 2; k++) {
      uint8_t src = k ? cfg[s].arm2 : cfg[s].arm;
      if (src >= ARM_NONE) continue;                      // второй источник выключен
      if (src == ARM_EXT) {
        // Оба источника с внешнего входа рисуют одну метку — вторая
        // легла бы ровно на первую.
        if (!extDrawn) { drawExtStub(SLOT_CX[s], SLOT_CY[s]); extDrawn = true; }
        continue;
      }
      if (cfg[src].app == (uint8_t)AppId::EMPTY) continue;  // связь в пустой слот не рисуем
      drawChainLink(src, s);
    }
  }

  // Куда уходит модуляция. Рисуем только работающие цели: с нулевой
  // глубиной линия врала бы о том, что связь есть.
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    if ((AppId)cfg[s].app != AppId::META || cfg[s].bypass) continue;
    for (uint8_t i = 0; i < 3; i++) {
      if (!cfg[s].metaDepth[i]) continue;
      uint8_t ts = metaTgtSlot(cfg[s].p[1 + i]);
      if (ts == s || cfg[ts].app == (uint8_t)AppId::EMPTY) continue;
      if (!metaParamDesc(ts, metaTgtIdx(cfg[s].p[1 + i])).name[0]) continue;
      drawModLink(s, ts);
    }
  }

  // Звёзды: пустой слот — место в созвездии без звезды, чтобы карта
  // не меняла разметку от того, что в слот что-то загрузили.
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    if (cfg[s].app == (uint8_t)AppId::EMPTY) {
      if (s == slot) drawStarCursor(SLOT_CX[s], SLOT_CY[s]);
      continue;
    }
    drawStarGlyph(SLOT_CX[s], SLOT_CY[s], s == slot);
    // Байпас виден на всей карте: сквозь звезду проходит линия —
    // сигнал идёт через слот не задерживаясь.
    if (cfg[s].bypass)
      display.drawFastHLine(SLOT_CX[s] - 3, SLOT_CY[s], 7, SSD1306_WHITE);
    if (slotModulated(s)) drawStarMod(SLOT_CX[s], SLOT_CY[s]);
    if (s == slot) drawStarCursor(SLOT_CX[s], SLOT_CY[s]);
  }

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(121, 2);
  // В байпасе вместо транспорта показываем B: транспорт общий и виден
  // с любого другого слота, а вот байпас — состояние именно этого.
  display.print(cfg[slot].bypass ? "B" : (cfg[slot].running ? ">" : "|"));
  if (state.subPage) { display.setCursor(113, 2); display.print("S"); }
}

void drawSlotPage() {
  display.clearDisplay();

  const SlotCfg &c = cfg[state.currentSlot];
  // Единственное место, где шапка не отдана созвездию, — главная
  // страница генератора: там её занимает ряд из 16 шагов. Везде
  // остальном видна карта слотов, и с пустого слота тоже — курсор
  // показывает, куда именно грузится приложение.
  // В слое шапка одна для всех страниц: ряд шагов показывает содержимое
  // паттерна, а оно к привязкам отношения не имеет. Зато карта слотов
  // здесь кстати — по рамкам вокруг звёзд сразу видно, что вообще
  // ведётся на устройстве.
  if (!state.modLayer && isGenerator(c.app) && !state.subPage)
    drawStepRow(state.currentSlot);
  else
    drawChainHeader(state.currentSlot);

  if (c.app == (uint8_t)AppId::EMPTY) {
    printCentered(0, Display::W, 14, "EMPTY", SSD1306_WHITE);
    printCentered(0, Display::W, 24, "PLAY = LOAD APP", SSD1306_WHITE);
    display.display();
    return;
  }

  // На главной странице генератора колонка живёт по правилам
  // секвенсера RED SEA: инверсия означает "на шаге под курсором стоит
  // триг этой колонки", а показанное значение — значение ЭТОГО шага.
  bool seqCols = isGenerator(c.app) && !state.subPage;
  uint8_t k = rt[state.currentSlot].cursor & 15;

  // Приём модулятором отмечаем одной затухающей рамкой вокруг всего
  // блока параметров. Рисуем до колонок, чтобы курсор лёг поверх.
  if (isProcessor(c.app)) {
    drawFadingFrame(0, Display::COL_Y, Display::W, Display::H - Display::COL_Y,
                    millis() - rt[state.currentSlot].lastFireMs, 260, SSD1306_WHITE);
  }

  for (uint8_t i = 0; i < NUM_COLS; i++) {
    PDesc d = columnDesc(state.currentSlot, state.subPage, i);
    uint8_t x = i * 32;
    if (d.name[0] == 0) continue;

    // Слой перехватывает колонку ДО всех остальных веток: у половины
    // модулей своя графика с выходом по continue, и иначе в слое
    // рисовались бы карта TOPOLOGY, регистр SHIFT и прочие картинки
    // вместо привязок.
    if (state.modLayer) {
      char lb[8], vb[8];
      uint8_t mi = (uint8_t)((state.subPage ? NUM_COLS : 0) + i);
      if (!c.modCC[mi]) { strcpy(lb, "--"); vb[0] = 0; }
      else {
        sprintf(lb, "~%d", c.modCC[mi]);
        sprintf(vb, "%+d", c.modDepth[mi]);
      }
      display.setTextSize(1);
      printCentered(x, Display::COL_W, Display::COL_Y + 2, lb, SSD1306_WHITE);
      if (vb[0])
        printCentered(x, Display::COL_W, Display::COL_Y + Display::COL_H - 9, vb, SSD1306_WHITE);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, SSD1306_WHITE);
      continue;
    }

    bool trig = seqCols && (c.lane[i] & (uint16_t)(1u << k));
    bool inv = trig;   // инверсией отмечается только триг шага
    uint16_t fg = inv ? SSD1306_BLACK : SSD1306_WHITE;
    if (inv) display.fillRect(x, Display::COL_Y, Display::COL_W, Display::COL_H, SSD1306_WHITE);

    char label[12];
    strncpy(label, d.name, sizeof(label) - 1);
    label[sizeof(label) - 1] = 0;

    // Показываем значение С МОДУЛЯЦИЕЙ, а не хранимое: при работающей
    // META на экране стояло одно число, а движок использовал другое, и
    // понять это было нельзя никак.
    int16_t shown = trig ? c.stepVal[k][i]
                         : modParam(state.currentSlot, state.subPage, i);
    // Смещение этой колонки прямо сейчас — по нему рисуется индикатор.
    int16_t modOff = modOffset[state.currentSlot][(state.subPage ? MP_SUB : MP_MAIN) + i];

    // У MORPH LFO колонка SHAPE вместо названия волны показывает саму
    // волну — так же, как FOG в RED SEA. Фаза берётся от живого
    // состояния движка, поэтому картинка бежит вместе со звуком.
    if (!state.subPage && (AppId)c.app == AppId::MLFO && i == 0) {
      // Подписи нет намеренно: имя волны говорит меньше, чем сама
      // волна, а освободившаяся строка отдана её высоте.
      drawWaveform(x + 2, Display::COL_Y + 2, Display::COL_W - 4, Display::COL_H - 4,
                   (uint8_t)clampInt(c.p[0], 0, 2), c.p[1],
                   rt[state.currentSlot].lfoPhase, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }

    // NOTE DELAY: вместо числа повторов — сами повторы, столбиками.
    // Сверху остаётся деление интервала, оно живёт под TAP.
    if (!state.subPage && (AppId)c.app == AppId::NDELAY && i == 0) {
      printCentered(x, Display::COL_W, Display::COL_Y + 2,
                    rateNames[clampInt(c.sub4, 0, 4)], fg);
      drawEchoBars(state.currentSlot, x, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }
    // PPONG — скачок CC вокруг центра; зигзаг показывает и его, и размах.
    if (!state.subPage && (AppId)c.app == AppId::NDELAY && i == 2) {
      printCentered(x, Display::COL_W, Display::COL_Y + 2, label, fg);
      drawPingPong(state.currentSlot, x, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }
    // ENVELOPE: вместо числа перекоса — сама кривая.
    if (!state.subPage && (AppId)c.app == AppId::ENV && i == 1) {
      printCentered(x, Display::COL_W, Display::COL_Y + 2, label, fg);
      drawEnvShape(state.currentSlot, x, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }
    // DUST: плотность облака показана самим облаком.
    if (!state.subPage && (AppId)c.app == AppId::DUST && i == 0) {
      printCentered(x, Display::COL_W, Display::COL_Y + 2, label, fg);
      drawDustField(state.currentSlot, x, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }

    // Первая колонка SHIFT — сам регистр: длина видна как размер сетки,
    // ход — как бегущая заливка.
    if (!state.subPage && (AppId)c.app == AppId::SHIFT && i == 0) {
      drawShiftReg(state.currentSlot, x, buttons[1].lastStable == LOW, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }

    // Первая колонка TOPOLOGY — не пара чисел, а сама карта: два числа
    // ничего не говорили о том, где ты на ней находишься.
    if (!state.subPage && (AppId)c.app == AppId::TOPOLOGY && i == 0) {
      drawTopoMap(state.currentSlot, x, fg);
      if (state.selectedCol == i)
        drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
      continue;
    }


    char val[12];
    if (!state.subPage && (AppId)c.app == AppId::TOPOLOGY) {
      // Остальные колонки — дорожки: сверху нота (под TAP), снизу плотность.
      char nn[6];
      midiNoteToString((uint8_t)clampInt(modKtCC(state.currentSlot, (uint8_t)(i - 1)), 0, 127), nn);
      strncpy(label, nn, sizeof(label) - 1); label[sizeof(label) - 1] = 0;
      sprintf(val, "%d", clampInt(modP(state.currentSlot, i), 0, 127));
    } else if (!state.subPage && (AppId)c.app == AppId::MLFO && i == 2) {
      // Сверху доля такта (под TAP), снизу способ активации.
      strcpy(label, rateNames[clampInt(modSub4(state.currentSlot), 0, 4)]);
      strcpy(val, lfoAct[clampInt(modP(state.currentSlot, 2), 0, 2)]);
    } else if (state.subPage && isGenerator(c.app) && i == 0) {
      // Сверху общий BPM (TAP), снизу деление такта этого слота.
      sprintf(label, "%d", state.bpm);
      strcpy(val, rateNames[clampInt(modRate(state.currentSlot), 0, 4)]);
    } else if (state.subPage && isGenerator(c.app) && i == 2) {
      // Колонка SCALE занимает обе строки: сверху вместо подписи
      // тоника (C-B), снизу сам лад. Подпись «SCALE» не нужна —
      // содержимое говорит само за себя.
      strcpy(label, rootNames[modRoot(state.currentSlot) % 12]);
      strcpy(val, scales[clampInt(modScale(state.currentSlot), 0, 9)].name);
    } else if (state.subPage && isProcessor(c.app) && i == 1) {
      // Колонка ARM держит два источника. Пока второго нет, показываем
      // первый полным именем; как только он появился — оба короткими,
      // иначе "EXT+EXT" не влезает в 31px колонки.
      if (c.arm2 >= ARM_NONE) strcpy(val, armNames[clampInt(c.arm, 0, ARM_EXT)]);
      else sprintf(val, "%c+%c", armShort(c.arm), armShort(c.arm2));
    } else if (rrPair(c.app, state.subPage, i) >= 0) {
      // Две строчки — два голоса: сверху тот, что на энкодере, снизу
      // тот, что под TAP.
      int8_t pr = rrPair(c.app, state.subPage, i);
      char a[4], b[4];
      formatValue({"", 0, 16, P_CHAN, nullptr}, c.outCh[pr * 2], a);
      formatValue({"", 0, 16, P_CHAN, nullptr}, c.outCh[pr * 2 + 1], b);
      sprintf(label, "V%d:%s", pr * 2 + 1, a);
      sprintf(val,   "V%d:%s", pr * 2 + 2, b);
    } else if (multxOut(c.app, state.subPage, i) >= 0) {
      // Колонка = выход: сверху канал (TAP + энкодер), снизу сдвиг в
      // ступенях лада. Выключенный выход помечаем прочерком.
      int8_t o = multxOut(c.app, state.subPage, i);
      if (c.outCh[o] == 0) strcpy(label, "NO");
      else sprintf(label, "CH%d", c.outCh[o]);
      sprintf(val, "%+d", clampInt(c.outXp[o], -14, 14));
    } else if (!state.subPage && (AppId)c.app == AppId::META && i >= 1) {
      // Сверху слот-цель (под TAP), снизу имя её параметра.
      int16_t a = c.p[i];
      uint8_t ts = metaTgtSlot(a);
      PDesc td = metaParamDesc(ts, metaTgtIdx(a));
      sprintf(label, "%c", (char)('A' + ts));
      strncpy(val, td.name[0] ? td.name : "--", sizeof(val) - 1);
      val[sizeof(val) - 1] = 0;
    } else if (!state.subPage && (AppId)c.app == AppId::KEYTRK) {
      // Колонка = линия CC: сверху её номер (TAP + энкодер), снизу
      // глубина. Выключенную линию помечаем прочерком вместо номера —
      // иначе непонятно, работает она или нет.
      int depth = clampInt(modP(state.currentSlot, i), -64, 64);
      if (depth == 0) strcpy(label, "--");
      else sprintf(label, "%d", modKtCC(state.currentSlot, i));
      sprintf(val, "%+d", depth);
    } else if (!state.subPage && (AppId)c.app == AppId::LOGIC && i == 1 && c.p[1] == 2) {
      // В режиме T+CC колонка занимает обе строки: сверху сам режим,
      // снизу значение, с которым сравнивается CC (TAP + энкодер).
      strcpy(label, "T+CC");
      sprintf(val, "%d", clampInt(modSub4(state.currentSlot), 0, 127));
    } else {
      formatValue(d, shown, val);
    }

    display.setTextSize(1);
    printCentered(x, Display::COL_W, Display::COL_Y + 2, label, fg);
    printCentered(x, Display::COL_W, Display::COL_Y + Display::COL_H - 9, val, fg);
    drawModBar(x, modOff, d.lo, d.hi);
    if (state.selectedCol == i)
      drawCornerCursor(x, Display::COL_Y, Display::COL_W, Display::COL_H, fg);
  }
  display.display();
}

void drawAppList() {
  display.clearDisplay();
  char buf[24];
  // В шапке слот и группа выделенного — она меняется по мере листания,
  // поэтому всегда видно, в какой части списка находишься.
  sprintf(buf, "%c  %s", 'A' + state.currentSlot,
          groupNames[groupOfPos(state.appListSel)]);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(buf);

  // Окно из трёх позиций вокруг выбранной.
  int first = (int)state.appListSel - 1;
  if (first < 0) first = 0;
  if (first > APP_COUNT - 3) first = APP_COUNT - 3;
  for (uint8_t row = 0; row < 3; row++) {
    uint8_t pos = (uint8_t)(first + row);
    if (pos >= APP_COUNT) break;
    uint8_t idx = browseOrder[pos];
    uint8_t y = 9 + row * 8;
    bool sel = (pos == state.appListSel);
    if (sel) display.fillRect(0, y - 1, Display::W, 9, SSD1306_WHITE);
    uint16_t fg = sel ? SSD1306_BLACK : SSD1306_WHITE;
    // Та же пиктограмма, что потом стоит в шапке слота — по ней
    // приложение и узнаётся, когда оно уже загружено.
    drawIcon(1, y - 1, appIcons[idx], fg);
    display.setTextColor(fg);
    display.setCursor(12, y);
    display.print(apps[idx].name);
  }
  display.display();
}

// Браузер патчей. Имени у патча нет — вместо него четыре иконки
// слотов: клавиатуры на устройстве нет, а по форме патч узнаётся
// мгновенно и без единой буквы.
void drawPatchBrowser() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(state.patchHolding ? (state.patchErasing ? "ERASE" : "SAVE ")
                                   : "PATCH");

  // Взведение: рамка редеет по мере удержания, как и всюду в
  // устройстве у разрушительных действий.
  if (state.patchHolding) {
    uint32_t held = millis() - state.patchHoldStart;
    if (held > PATCH_HOLD_ARM)
      drawFadingFrame(0, 0, Display::W, Display::H,
                      PATCH_HOLD_SAVE - held, PATCH_HOLD_SAVE - PATCH_HOLD_ARM,
                      SSD1306_WHITE);
  }

  int first = (int)state.patchSel - 1;
  if (first < 0) first = 0;
  if (first > PATCH_COUNT - 3) first = PATCH_COUNT - 3;
  for (uint8_t row = 0; row < 3; row++) {
    uint8_t i = (uint8_t)(first + row);
    if (i >= PATCH_COUNT) break;
    uint8_t y = (uint8_t)(9 + row * 8);
    bool sel = (i == state.patchSel);
    if (sel) display.fillRect(0, y - 1, Display::W, 9, SSD1306_WHITE);
    uint16_t fg = sel ? SSD1306_BLACK : SSD1306_WHITE;
    char n[4]; sprintf(n, "%u", i + 1);
    display.setTextColor(fg);
    display.setCursor(2, y);
    display.print(n);
    if (!patchInfo[i].used) {
      display.setCursor(12, y);
      display.print("- - - -");
      continue;
    }
    for (uint8_t sl = 0; sl < NUM_SLOTS; sl++)
      drawIcon((uint8_t)(12 + sl * 10), y - 1, appIcons[patchInfo[i].app[sl]], fg);
    char b[8]; sprintf(b, "%u", patchInfo[i].bpm);
    display.setCursor(56, y);
    display.print(b);
  }
  display.display();
}

void updateDisplay() {
  if (state.patchOpen) drawPatchBrowser();
  else if (state.appListOpen) drawAppList();
  else drawSlotPage();
}

// ============================================================
// 17. ХРАНИЛИЩЕ
// ============================================================
void saveSettings() {
  storage.begin("trajectories", false);
  storage.putBytes("cfg", cfg, sizeof(cfg));
  storage.putUShort("bpm", state.bpm);
  storage.end();
}

void loadSettings() {
  storage.begin("trajectories", true);
  if (storage.getBytesLength("cfg") == sizeof(cfg)) {
    storage.getBytes("cfg", cfg, sizeof(cfg));
  }
  state.bpm = storage.getUShort("bpm", 120);
  storage.end();

  // Защита от мусора во флеше: диапазоны, деление на ноль в модульной
  // арифметике генераторов и выход за границы таблиц.
  if (state.bpm < 20 || state.bpm > 300) state.bpm = 120;
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    SlotCfg &c = cfg[s];
    if (c.app >= APP_COUNT) c.app = (uint8_t)AppId::EMPTY;
    // 0 = NO, допустимо у генератора; у процессора колонка CH такого
    // значения не даёт выставить.
    c.midiCh = (uint8_t)clampInt(c.midiCh, 0, 16);
    c.arm = armSkipSelf(s, c.arm, 1, 0, ARM_EXT);
    c.arm2 = armSkipSelf(s, c.arm2, 1, 0, ARM_NONE);
    c.ccDest = (uint8_t)clampInt(c.ccDest, 0, 127);
    c.stepPlus = (int8_t)clampInt(c.stepPlus, -12, 12);
    for (uint8_t i = 0; i < NUM_COLS; i++) c.ktCC[i] = (uint8_t)clampInt(c.ktCC[i], 0, 127);
    for (uint8_t i = 0; i < 3; i++) c.metaDepth[i] = (int8_t)clampInt(c.metaDepth[i], -64, 64);
    if ((AppId)c.app == AppId::META)
      for (uint8_t i = 1; i < NUM_COLS; i++)
        if (metaTgtSlot(c.p[i]) == s) c.p[i] = metaNextAddr(s, c.p[i], 1);
    for (uint8_t i = 0; i < 8; i++) {
      c.outCh[i] = (uint8_t)clampInt(c.outCh[i], 0, 16);
      c.outXp[i] = (int8_t)clampInt(c.outXp[i], -14, 14);
    }
    {
      PDesc d2 = processorSubDesc(c.app, 2), d3 = processorSubDesc(c.app, 3);
      c.sub2 = (int16_t)clampInt(c.sub2, d2.lo, d2.hi);
      c.sub3 = (int16_t)clampInt(c.sub3, d3.lo, d3.hi);
    }
    for (uint8_t i = 0; i < 2 * NUM_COLS; i++) {
      c.modCC[i] = (uint8_t)clampInt(c.modCC[i], 0, 127);
      c.modDepth[i] = (int8_t)clampInt(c.modDepth[i], -64, 64);
    }
    c.rate = (uint8_t)clampInt(c.rate, 0, 4);
    c.scale = (uint8_t)clampInt(c.scale, 0, 9);
    c.root = (uint8_t)clampInt(c.root, 0, 11);
    for (uint8_t i = 0; i < NUM_COLS; i++) {
      c.p[i] = (int16_t)clampInt(c.p[i], apps[c.app].p[i].lo, apps[c.app].p[i].hi);
      // Значения шагов живут в том же диапазоне, что и общий параметр,
      // но дополнительно ограничены разрядностью int8.
      if (isGenerator(c.app))
        for (uint8_t st = 0; st < 16; st++)
          c.stepVal[st][i] = (int8_t)clampInt(c.stepVal[st][i],
                               clampInt(apps[c.app].p[i].lo, -128, 127),
                               clampInt(apps[c.app].p[i].hi, -128, 127));
      if (c.app == (uint8_t)AppId::EMPTY) c.lane[i] = 0;
    }
    c.sub4 = (int16_t)clampInt(c.sub4, apps[c.app].sub4.lo, apps[c.app].sub4.hi);
    busCur[s].scale = c.scale;
    busCur[s].root = c.root;
    // Флеш мог быть записан ещё в другом ладу — приводим к текущему.
    requantizeSlotNotes(s);
  }
}

// ============================================================
// 18. ПАТЧИ
// ============================================================
// Восемь ячеек, в каждой всё состояние: четыре слота и общий темп.
// От рабочего состояния это отдельный слой: рабочее пишется само
// через 2 секунды после правки, патчи — только вручную. Иначе одна
// неосторожная правка затирала бы сохранённое.
constexpr uint16_t PATCH_MAGIC = 0x4348;   // 'CH'
constexpr uint8_t  PATCH_VER   = 3;   // 2: удалён SIEVE; 3: карта модуляции в SlotCfg

struct Patch {
  uint16_t magic;
  uint8_t  ver;
  uint16_t cfgSize;      // ловит смену SlotCfg: она уже менялась не раз
  uint16_t bpm;
  SlotCfg  slot[NUM_SLOTS];
};


void patchKey(uint8_t i, char* out) { sprintf(out, "p%u", i); }

bool patchRead(uint8_t i, Patch &out) {
  char k[6]; patchKey(i, k);
  storage.begin("trajectories", true);
  bool ok = storage.getBytesLength(k) == sizeof(Patch);
  if (ok) storage.getBytes(k, &out, sizeof(Patch));
  storage.end();
  if (!ok) return false;
  // Патч от другой версии прошивки — не грузим: там уже другая
  // раскладка полей, и загрузка дала бы мусор.
  return out.magic == PATCH_MAGIC && out.ver == PATCH_VER &&
         out.cfgSize == sizeof(SlotCfg);
}

void patchScan() {
  Patch p;
  for (uint8_t i = 0; i < PATCH_COUNT; i++) {
    patchInfo[i].used = patchRead(i, p);
    if (!patchInfo[i].used) continue;
    for (uint8_t s = 0; s < NUM_SLOTS; s++) patchInfo[i].app[s] = p.slot[s].app;
    patchInfo[i].bpm = p.bpm;
  }
}

void patchSave(uint8_t i) {
  Patch p;
  p.magic = PATCH_MAGIC;
  p.ver = PATCH_VER;
  p.cfgSize = sizeof(SlotCfg);
  p.bpm = state.bpm;
  memcpy(p.slot, cfg, sizeof(cfg));
  char k[6]; patchKey(i, k);
  storage.begin("trajectories", false);
  storage.putBytes(k, &p, sizeof(p));
  storage.end();
  patchInfo[i].used = true;
  for (uint8_t s = 0; s < NUM_SLOTS; s++) patchInfo[i].app[s] = cfg[s].app;
  patchInfo[i].bpm = state.bpm;
}

// Стирание ячейки. Ключ убирается из NVS целиком, поэтому patchRead
// не найдёт его и ячейка честно станет пустой, а не «пустой на вид».
void patchErase(uint8_t i) {
  char k[6]; patchKey(i, k);
  storage.begin("trajectories", false);
  storage.remove(k);
  storage.end();
  patchInfo[i].used = false;
}

bool patchLoad(uint8_t i) {
  Patch p;
  if (!patchRead(i, p)) return false;
  flushScheduler();                    // гасим всё, что звучало от прежнего
  memcpy(cfg, p.slot, sizeof(cfg));
  state.bpm = p.bpm;
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    rt[s] = SlotRt{};
    for (uint8_t k2 = 0; k2 < NUM_COLS; k2++) rt[s].ktLast[k2] = 255;
    busCur[s] = Signal{};
    busCur[s].scale = cfg[s].scale;
    busCur[s].root = cfg[s].root;
  }
  clearModOffsets();
  markEdited();                        // рабочее состояние тоже обновим
  return true;
}

// ============================================================
// 19. ТРАНСЛЯЦИЯ ЭКРАНА ПО USB
// ============================================================
// Кадр 128x32 — это 512 байт буфера SSD1306. Отдаём их hex-строкой с
// переводом строки: приёмник синхронизируется по разделителю, и не
// нужно ничего экранировать.
//
// Главное правило — НИКОГДА не блокировать loop(). Запись в USB CDC
// ждёт, если хост не вычитывает порт, а вставший loop() это сразу
// поехавший MIDI-клок. Поэтому кадр уходит порциями: ровно столько,
// сколько прямо сейчас влезает в буфер передачи, и ни байтом больше.
constexpr uint32_t SCR_KEEPALIVE_MS = 2000;
constexpr uint32_t SCR_FRAME_MS = 66;              // ~15 кадров в секунду
constexpr uint16_t SCR_LEN = 1 + 512 * 2 + 1;      // 'F' + hex + '\n'

uint8_t  scrSnap[512];      // снимок кадра: без него полукадр рвался бы
uint16_t scrPos = 0;        // сколько байт вывода уже ушло
bool     scrBusy = false;
uint32_t scrLastCmd = 0;
uint32_t scrLastFrame = 0;

inline char scrHex(uint8_t v) { return v < 10 ? (char)('0' + v) : (char)('a' + v - 10); }

// Байт выходного потока по его позиции — считаем на лету, чтобы не
// держать вторую копию кадра уже в hex.
uint8_t scrByteAt(uint16_t i) {
  if (i == 0) return 'F';
  if (i == SCR_LEN - 1) return '\n';
  uint16_t k = (uint16_t)(i - 1);
  uint8_t b = scrSnap[k >> 1];
  return (uint8_t)scrHex((k & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4));
}

// Досчитывает удержание «сохранить». Вынесено из обработчика кнопок:
// там событие приходит только на нажатие и отпускание, а рамка должна
// расти всё время удержания.
void servicePatchHold() {
  if (!state.patchOpen || !state.patchHolding || state.patchSaved) return;
  if (millis() - state.patchHoldStart < PATCH_HOLD_SAVE) {
    state.displayDirty = true;      // рамка взведения
    return;
  }
  if (state.patchErasing) patchErase(state.patchSel);
  else                    patchSave(state.patchSel);
  state.patchSaved = true;
  state.displayDirty = true;
}

void serviceScreenStream() {
  // Любой байт с хоста продлевает трансляцию; молчание дольше
  // SCR_KEEPALIVE_MS её выключает. Так поток гаснет сам, если скрипт
  // на ПК закрыли, и устройство не пытается писать в никуда.
  while (Serial.available()) { Serial.read(); scrLastCmd = millis(); }
  if (millis() - scrLastCmd >= SCR_KEEPALIVE_MS) { scrBusy = false; return; }

  if (!scrBusy && millis() - scrLastFrame >= SCR_FRAME_MS) {
    memcpy(scrSnap, display.getBuffer(), sizeof(scrSnap));
    scrPos = 0;
    scrBusy = true;
    scrLastFrame = millis();
  }
  if (!scrBusy) return;

  int room = Serial.availableForWrite();
  while (room > 0 && scrPos < SCR_LEN) {
    uint8_t chunk[64];
    uint8_t n = 0;
    while (n < sizeof(chunk) && (int)n < room && scrPos < SCR_LEN)
      chunk[n++] = scrByteAt(scrPos++);
    Serial.write(chunk, n);
    room -= n;
  }
  if (scrPos >= SCR_LEN) scrBusy = false;
}

// ============================================================
// 20. SETUP & LOOP
// ============================================================
void setup() {
  pinMode(Pins::PLAY, INPUT_PULLUP);
  pinMode(Pins::TAP, INPUT_PULLUP);
  pinMode(Pins::PAGE, INPUT_PULLUP);
  pinMode(Pins::ENC_SW, INPUT_PULLUP);
  pinMode(Pins::ENC_A, INPUT_PULLUP);
  pinMode(Pins::ENC_B, INPUT_PULLUP);
  Serial.begin(115200);            // USB CDC — трансляция экрана
  Wire.begin(Pins::SDA, Pins::SCL);
  randomSeed(analogRead(0) + micros());

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (1) delay(1000);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Trajectories");
  display.setCursor(0, 12);
  display.print("BOOTING...");
  display.display();

  lastEncState = (digitalRead(Pins::ENC_A) << 1) | digitalRead(Pins::ENC_B);
  attachInterrupt(digitalPinToInterrupt(Pins::ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(Pins::ENC_B), encoderISR, CHANGE);
  midi.begin(Pins::MIDI_BAUD, SERIAL_8N1, Pins::MIDI_RX, Pins::MIDI_TX);

  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    cfg[s].app = (uint8_t)AppId::EMPTY;
    cfg[s].midiCh = (uint8_t)(s + 1);   // A->1, B->2, C->3, D->4
    cfg[s].arm = (s == 0) ? 1 : 0;   // на себя нельзя, см. armSkipSelf
    cfg[s].arm2 = ARM_NONE;   // второй источник по умолчанию выключен
    cfg[s].rate = 2;      // 1/4 — нейтральный старт
    cfg[s].scale = 0;
    cfg[s].root = 0;
  }
  loadSettings();

  delay(50);
  state.displayDirty = true;
}

void loop() {
  processMIDI();
  updateButtons();
  handleEncoder();

  // Внешний клок считается пропавшим, если тиков не было ~0.5с —
  // после этого снова работает внутренний генератор темпа.
  if (state.extClock && (micros() - state.lastClockMicros) > 500000) {
    state.extClock = false;
    state.extBpm = 0;
    lastInternalTickMicros = 0;
  }

  if (generateInternalTicks()) onClockTick();

  serviceSlots();
  serviceScheduler();
  servicePatchHold();
  serviceScreenStream();

  if (state.needSave && millis() - state.lastEditMs > 2000) {
    state.needSave = false;
    saveSettings();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 40) {
    lastDraw = millis();
    state.displayDirty = true;
  }
  if (state.displayDirty) {
    state.displayDirty = false;
    updateDisplay();
  }
}
