#include "common.h"
void dumpCol(int col) {
  int x0 = col * 32;
  for (int y = Display::COL_Y - 1; y < 32; y++) {
    printf("  ");
    for (int x = x0; x < x0 + Display::COL_W; x++)
      putchar(g_fb[y][x] ? '#' : (g_tx[y][x] && g_tx[y][x] != '-' ? g_tx[y][x] : ' '));
    printf("|\n");
  }
}
void bind(uint8_t tgt, bool sub, uint8_t col, uint8_t src, int d) {
  uint8_t mi = (uint8_t)((sub ? NUM_COLS : 0) + col);
  cfg[tgt].modSrc[mi] = src;
  cfg[tgt].modDepth[mi] = (int8_t)d;
}
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("=== 1. LFO ведёт параметр через цепочку, без MIDI наружу ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);                 // A: задаёт такт
  cfg[0].running = true; cfg[0].rate = 4; cfg[0].midiCh = 0; cfg[0].p[0] = 60;
  loadApp(1, (uint8_t)AppId::MLFO);                  // B: источник
  cfg[1].arm = 0; cfg[1].running = true; cfg[1].midiCh = 0;   // наружу молчит
  cfg[1].ccDest = 50; cfg[1].p[3] = 127; cfg[1].sub4 = 2;
  loadApp(2, (uint8_t)AppId::TOPOLOGY);              // C: цель
  cfg[2].running = true; cfg[2].sub4 = 64;
  // Ведём CHAOS — один из двух параметров TOPOLOGY, которые по карте
  // modAllow разрешено модулировать. Плотности дорожек запрещены.
  bind(2, true, 3, 2, +100);
  int lo = 999, hi = -999;
  for (int t = 0; t < 3000; t++) {
    onClockTick(); serviceSlots(); serviceScheduler();
    g_ms += 2; g_us += 2000; state.midiTicks++;
    int v = modSub4(2);
    if (v < lo) lo = v; if (v > hi) hi = v;
  }
  printf("  CHAOS C: %d..%d (хранится %d)\n", lo, hi, (int)cfg[2].sub4);
  ck("источник реально ведёт", hi - lo > 40, 1);
  ck("хранимое не испорчено", cfg[2].sub4, 64);

  printf("\n=== 2. Байпас источника снимает модуляцию ===\n");
  cfg[1].bypass = true;
  serviceSlots();
  ck("смещения нет", modOffset[2][MP_MAIN + 1], 0);
  cfg[1].bypass = false;

  printf("\n=== 3. Отсчёт зависит от источника ===\n");
  // Двусторонний (LFO): середина не трогает. Односторонний (ENVELOPE):
  // ноль не трогает, и уводит только в одну сторону.
  auto sweep = [&](AppId srcApp, int depth, int stored) {
    reset();
    loadApp(0, (uint8_t)srcApp);
    loadApp(1, (uint8_t)AppId::TOPOLOGY);
    applyColumn(1, false, 1, (int16_t)stored);
    bind(1, false, 1, 1, depth);
    int at0, at64, at127;
    PDesc d = columnDesc(1, false, 1);
    bool bip = isModBipolar(cfg[0].app);
    // Клампим так же, как движок: смещение накладывается на чтении и
    // не выводит параметр за его диапазон.
    at0   = clampInt(stored + modShift(d, depth, 0,   bip), d.lo, d.hi);
    at64  = clampInt(stored + modShift(d, depth, 64,  bip), d.lo, d.hi);
    at127 = clampInt(stored + modShift(d, depth, 127, bip), d.lo, d.hi);
    printf("  %-10s %-13s хранится %3d | CC 0 -> %3d, 64 -> %3d, 127 -> %3d\n",
           apps[(uint8_t)srcApp].name, bip ? "двусторонний" : "от нуля",
           stored, at0, at64, at127);
    return std::array<int,3>{at0, at64, at127};
  };
  // Для односторонней огибающей хранимое значение ставим у низа
  // диапазона — иначе полная глубина упирается в потолок, и половина
  // хода ручки тратится впустую. Это свойство одностороннего отсчёта,
  // а не дефект: его стоит знать при настройке.
  auto lfo = sweep(AppId::MLFO, +100, 64);
  auto env = sweep(AppId::ENV,  +100, 0);
  auto envMid = sweep(AppId::ENV, +100, 64);
  ck("у LFO центр не трогает", lfo[1], 64);
  // Точной симметрии быть не может: у MIDI CC нет центра — от 64 вниз
  // 64 шага, вверх 63. Расхождение в одну единицу — это и есть правда.
  ck("у LFO края симметричны с точностью до единицы",
     abs((64 - lfo[0]) - (lfo[2] - 64)) <= 1, 1);
  ck("у огибающей нуль не трогает", env[0], 0);
  ck("и уводит только вверх", env[2] > env[1] && env[1] > env[0], 1);
  ck("от середины она насыщается", envMid[1] == envMid[2], 1);

  printf("\n=== 4. Экран показывает то, что играет, и индикатор ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::MLFO);
  loadApp(1, (uint8_t)AppId::TOPOLOGY);
  applyColumn(1, false, 1, 64);
  bind(1, false, 1, 1, +100);
  busSnap[0].ccValue = 110;
  clearModOffsets();
  PDesc d = columnDesc(1, false, 1);
  modOffset[1][MP_MAIN + 1] = (int16_t)modShift(d, 100, 110, true);
  state.currentSlot = 1; state.subPage = false; state.selectedCol = 1;
  state.modLayer = false; drawSlotPage();
  printf("  смещение %+d, играет %d при хранимом 64\n",
         (int)modOffset[1][MP_MAIN + 1], modP(1, 1));
  dumpCol(1);
  std::string onScreen;
  for (int y = 0; y < 32; y++)
    for (int x = 32; x < 32 + Display::COL_W; x++) {
      char ch = g_tx[y][x];
      if (ch && ch != '-' && ch != '_') onScreen += ch;
    }
  char want[8]; sprintf(want, "%d", modP(1, 1));
  printf("  текст: \"%s\", движок читает \"%s\"\n", onScreen.c_str(), want);
  ck("экран не врёт", onScreen.find(want) != std::string::npos, 1);
  int barR = 0, cx = 32 + Display::COL_W / 2, by = Display::COL_Y + Display::COL_H - 1;
  for (int x = cx; x < 32 + Display::COL_W; x++) if (g_fb[by][x]) barR++;
  ck("индикатор смотрит вправо при плюсе", barR > 0, 1);
  clearModOffsets(); drawSlotPage();
  int none = 0;
  for (int x = 32 + 4; x < 32 + Display::COL_W - 4; x++) if (g_fb[by][x]) none++;
  ck("без модуляции индикатора нет", none, 0);

  printf("\n=== 5. Связь модуляции на карте ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::MLFO);
  loadApp(1, (uint8_t)AppId::ARP);   cfg[1].arm = 0;
  loadApp(2, (uint8_t)AppId::TOPOLOGY);
  bind(2, false, 1, 1, +50);          // C ведёт слот A
  state.currentSlot = 1; state.subPage = false;
  auto dots = [&]() {
    int n = 0, xa = SLOT_CX[0], xb = SLOT_CX[2];
    for (int x = xa + STAR_R + 2; x < xb - STAR_R - 2; x++)
      for (int y = 0; y < Display::COL_Y - 2; y++) if (g_fb[y][x]) n++;
    return n;
  };
  drawSlotPage(); int with = dots();
  bind(2, false, 1, 1, 0);
  drawSlotPage(); int without = dots();
  printf("  пикселей между A и C: с привязкой %d, без неё %d\n", with, without);
  ck("связь появляется", with > without, 1);

  return done("MOD");
}
