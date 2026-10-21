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
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("=== 1. MORPH LFO как источник для META ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);                 // A: генератор, задаёт такт
  cfg[0].running = true; cfg[0].rate = 4; cfg[0].midiCh = 0; cfg[0].p[0] = 60;
  loadApp(1, (uint8_t)AppId::MLFO);                  // B: LFO, армлен на A
  cfg[1].arm = 0; cfg[1].running = true; cfg[1].ccDest = 74;
  cfg[1].p[0] = 0; cfg[1].p[2] = 0; cfg[1].p[3] = 127; cfg[1].sub4 = 2;
  loadApp(2, (uint8_t)AppId::TOPOLOGY);              // C: цель
  cfg[2].running = true; cfg[2].p[1] = 64;
  loadApp(3, (uint8_t)AppId::META);                  // D: ведёт C
  cfg[3].arm = 1;                                    // источник — слот B
  cfg[3].p[0] = 1;                                   // SRC = CC
  cfg[3].p[1] = (int16_t)(2 * MP_COUNT + MP_MAIN + 1);   // цель: C, плотность 1
  cfg[3].metaDepth[0] = 64;
  printf("  цель D->T1: слот %c, параметр %s\n",
         'A' + metaTgtSlot(cfg[3].p[1]), metaParamDesc(2, metaTgtIdx(cfg[3].p[1])).name);
  int lo = 999, hi = -999, moved = 0;
  for (int t = 0; t < 4000; t++) {
    onClockTick(); serviceSlots(); serviceScheduler();
    g_ms += 2; g_us += 2000; state.midiTicks++;
    int v = modP(2, 1);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    if (modOffset[2][MP_MAIN + 1]) moved++;
  }
  printf("  плотность C за прогон: от %d до %d (хранится %d)\n", lo, hi, (int)cfg[2].p[1]);
  ck("LFO реально ведёт параметр", hi - lo > 20, 1);
  ck("хранимое значение не испорчено", cfg[2].p[1], 64);
  ck("смещение живёт, а не мигает один раз", moved > 100, 1);

  printf("\n=== 2. Экран показывает модулированное значение ===\n");
  state.currentSlot = 2; state.subPage = false; state.selectedCol = 1;
  clearModOffsets();
  serviceMeta(3);
  int off = modOffset[2][MP_MAIN + 1];
  printf("  смещение сейчас: %+d, значит играет %d при хранимом 64\n", off, modP(2, 1));
  drawSlotPage();
  printf("колонка DENS1 (хранится 64):\n");
  dumpCol(1);
  // Число на экране обязано совпасть с тем, что читает движок.
  char want[8]; sprintf(want, "%d", modP(2, 1));
  std::string onScreen;
  for (int y = 0; y < 32; y++)
    for (int x = 32; x < 32 + Display::COL_W; x++) {
      char ch = g_tx[y][x];
      if (ch && ch != '-' && ch != '_') onScreen += ch;
    }
  printf("  текст в колонке: \"%s\", движок читает \"%s\"\n", onScreen.c_str(), want);
  ck("на экране то же, что в движке", onScreen.find(want) != std::string::npos, 1);

  printf("\n=== 3. Индикатор модуляции ===\n");
  int barL = 0, barR = 0;
  const int cx = 32 + Display::COL_W / 2, by = Display::COL_Y + Display::COL_H - 1;
  for (int x = 32; x < 32 + Display::COL_W; x++)
    if (g_fb[by][x]) { if (x < cx) barL++; else barR++; }
  printf("  чёрточка по нижней кромке: слева %d px, справа %d px\n", barL, barR);
  ck("индикатор нарисован", barL + barR > 0, 1);
  ck("сторона соответствует знаку", (off < 0) ? (barL > 0) : (barR > 0), 1);
  // Без модуляции чёрточки быть не должно.
  clearModOffsets();
  drawSlotPage();
  int none = 0;
  for (int x = 32 + 4; x < 32 + Display::COL_W - 4; x++) if (g_fb[by][x]) none++;
  ck("без модуляции чёрточки нет", none, 0);

  printf("\n=== 4. Связь модуляции в созвездии ===\n");
  state.currentSlot = 3;
  serviceMeta(3);
  drawSlotPage();
  // Между звёздами D и C должна лежать пунктирная линия.
  int dots = 0;
  int xa = SLOT_CX[2], xb = SLOT_CX[3];
  for (int x = (xa < xb ? xa : xb) + STAR_R + 2; x < (xa < xb ? xb : xa) - STAR_R - 2; x++)
    for (int y = 0; y < Display::COL_Y - 2; y++) if (g_fb[y][x]) dots++;
  printf("  точек между звёздами D и C: %d\n", dots);
  ck("связь модуляции нарисована", dots > 2, 1);
  cfg[3].metaDepth[0] = 0;
  drawSlotPage();
  int dots0 = 0;
  for (int x = (xa < xb ? xa : xb) + STAR_R + 2; x < (xa < xb ? xb : xa) - STAR_R - 2; x++)
    for (int y = 0; y < Display::COL_Y - 2; y++) if (g_fb[y][x]) dots0++;
  printf("  при нулевой глубине: %d\n", dots0);
  ck("нулевая глубина связи не рисует", dots0 < dots, 1);

  return done("META");
}
