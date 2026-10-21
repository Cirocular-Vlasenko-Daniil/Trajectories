#include "common.h"
void band(int rows) {
  for (int y = 0; y < rows; y++) {
    printf("  ");
    for (int x = 0; x < 128; x++)
      putchar(g_fb[y][x] ? '#' : (g_tx[y][x] && g_tx[y][x] != '-' ? g_tx[y][x] : ' '));
    printf("|\n");
  }
}
// Ставит слоту 2 привязку: его колонку col ведёт слот src с глубиной d.
void bind(uint8_t tgt, bool sub, uint8_t col, uint8_t src, int d) {
  uint8_t mi = (uint8_t)((sub ? NUM_COLS : 0) + col);
  cfg[tgt].modSrc[mi] = src;
  cfg[tgt].modDepth[mi] = (int8_t)d;
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("=== 1. Источником может быть только модуль с непрерывным выходом ===\n");
  reset();
  int src = 0, notSrc = 0;
  for (uint8_t a = 1; a < APP_COUNT; a++) {
    if (isModSource(a)) { printf("  может вести: %s%s\n", apps[a].name,
                                 isModBipolar(a) ? "  (двусторонний)" : "  (от нуля)"); src++; }
    else notSrc++;
  }
  printf("  не может вести: %d модулей\n", notSrc);
  ck("источников ровно пять", src, 5);
  ck("генератор источником не бывает", isModSource((uint8_t)AppId::BASIC), 0);
  ck("роутер тоже", isModSource((uint8_t)AppId::RROBIN), 0);

  printf("\n=== 2. Листание пропускает непригодные слоты ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);      // A — генератор, вести не может
  loadApp(1, (uint8_t)AppId::MLFO);       // B — может
  loadApp(2, (uint8_t)AppId::TOPOLOGY);   // C — цель, вести не может
  loadApp(3, (uint8_t)AppId::ENV);        // D — может
  uint8_t v = MOD_NO;
  printf("  от NO вперёд:");
  for (int i = 0; i < 4; i++) {
    v = modSrcStep(2, v, +1);
    printf(" %s", v == MOD_NO ? "NO" : (v == MOD_EXT ? "EXT" : std::string(1, (char)('A'+v-1)).c_str()));
  }
  printf("\n");
  v = MOD_NO;
  v = modSrcStep(2, v, +1); ck("первый пригодный — B", v, 2);
  v = modSrcStep(2, v, +1); ck("за ним D (A и C пропущены)", v, 4);
  v = modSrcStep(2, v, +1); ck("дальше EXT", v, MOD_EXT);
  v = modSrcStep(2, v, +1); ck("на EXT упирается", v, MOD_EXT);
  v = modSrcStep(2, v, -1); ck("назад к D", v, 4);

  printf("\n=== 3. Глубина мерится в единице самого параметра ===\n");
  reset();
  struct { const char *n; AppId a; bool sub; uint8_t col; } cs[] = {
    {"плотность TOPOLOGY", AppId::TOPOLOGY, false, 1},
    {"MODE 1CH/3CH",       AppId::TOPOLOGY, true,  2},
    {"SHAPE у MORPH LFO",  AppId::MLFO,     false, 0},
    {"RATE",               AppId::BASIC,    true,  0},
    {"STEPS 1..16",        AppId::BASIC,    true,  3},
  };
  for (auto &c : cs) {
    reset(); loadApp(0, (uint8_t)c.a);
    PDesc d = columnDesc(0, c.sub, c.col);
    printf("  %-20s %-6s %3d значений -> предел глубины %s%d\n",
           c.n, d.name, d.hi - d.lo + 1,
           modBySteps(d) ? "" : "", modDepthLimit(d));
  }
  reset(); loadApp(0, (uint8_t)AppId::TOPOLOGY);
  ck("у переключителя предел ±1", modDepthLimit(columnDesc(0, true, 2)), 1);
  ck("у плотности — проценты",    modDepthLimit(columnDesc(0, false, 1)), 100);

  printf("\n=== 3b. Переключатель наконец щёлкает ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::MLFO); cfg[0].arm = 1; cfg[0].midiCh = 0;
  loadApp(1, (uint8_t)AppId::TOPOLOGY); cfg[1].running = true;
  applyColumn(1, true, 2, 0);                 // MODE = 1CH
  bind(1, true, 2, 1, +1);                    // ведёт слот A
  std::set<int> got;
  for (int cc = 0; cc <= 127; cc += 1) {
    busSnap[0].ccValue = (uint8_t)cc;
    clearModOffsets();
    // применяем только привязки
    PDesc d = columnDesc(1, true, 2);
    modOffset[1][NUM_COLS + 2] =
      (int16_t)modShift(d, cfg[1].modDepth[NUM_COLS + 2], cc, isModBipolar(cfg[0].app));
    got.insert(modParam(1, true, 2));
  }
  printf("  значения MODE за свип источника: ");
  for (int x : got) printf("%d ", x);
  printf("\n");
  ck("переключатель принимает оба значения", (int)got.size(), 2);

  printf("\n=== 4. Жест через НАСТОЯЩИЙ обработчик кнопок ===\n");
  // Гоняем updateButtons() с реальными пинами и дебаунсом. Прошлая
  // версия повторяла логику лямбдой в тесте — и не заметила, что ветка
  // жеста в прошивку вообще не попала: слой был, но недостижим.
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  state.currentSlot = 0; state.subPage = false;

  hold(Pins::ENC_SW, 100);
  ck("короткое нажатие слой не включает", state.modLayer, 0);

  hold(Pins::ENC_SW, PLAY_HOLD_MS + 200);
  ck("удержание включает слой", state.modLayer, 1);

  hold(Pins::ENC_SW, PLAY_HOLD_MS + 200);
  ck("повторное удержание выключает", state.modLayer, 0);

  // Удержание С вращением — это курсор шага, слой трогать нельзя.
  uint8_t before = rt[0].cursor;
  g_pin[Pins::ENC_SW] = LOW;
  pump(200);
  encoderTicks = 4; handleEncoder();          // поворот во время удержания
  pump(PLAY_HOLD_MS + 200);
  g_pin[Pins::ENC_SW] = HIGH; pump(60);
  ck("удержание с вращением слой не включает", state.modLayer, 0);
  ck("и двигает курсор шага", rt[0].cursor != before, 1);

  // В браузере патчей то же удержание занято сохранением.
  state.patchOpen = true;
  hold(Pins::ENC_SW, PLAY_HOLD_MS + 200);
  ck("в браузере патчей слой не включается", state.modLayer, 0);
  state.patchOpen = false;

  // И вход, и выход работают, когда слой уже открыт на подстранице.
  state.subPage = true;
  hold(Pins::ENC_SW, PLAY_HOLD_MS + 200);
  ck("на подстранице тоже включается", state.modLayer, 1);
  hold(Pins::ENC_SW, PLAY_HOLD_MS + 200);
  ck("и выключается", state.modLayer, 0);
  state.subPage = false;


  printf("\n=== 5. Как слой выглядит ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::MLFO);
  loadApp(1, (uint8_t)AppId::ENV);
  loadApp(2, (uint8_t)AppId::TOPOLOGY);
  loadApp(3, (uint8_t)AppId::ARP);  cfg[3].arm = 2;
  bind(2, false, 1, 1, +50);          // плотность ведёт слот A (LFO)
  bind(2, false, 3, MOD_EXT, -25);    // третью дорожку — внешняя ручка
  state.currentSlot = 2; state.subPage = false; state.selectedCol = 1;
  state.modLayer = false; drawSlotPage();
  printf("TOPOLOGY, обычный вид:\n"); band(32);
  state.modLayer = true; drawSlotPage();
  printf("\nтот же слот, слой модуляции:\n"); band(32);

  printf("\n=== 6. Рамка только у тех, кого ведут ===\n");
  auto framed = [&](uint8_t sl) {
    int n = 0;
    for (int8_t sx = -1; sx <= 1; sx += 2)
      for (int8_t sy = -1; sy <= 1; sy += 2) {
        if (g_fb[SLOT_CY[sl] + sy * 3][SLOT_CX[sl] + sx * 2]) n++;
        if (g_fb[SLOT_CY[sl] + sy * 2][SLOT_CX[sl] + sx * 3]) n++;
      }
    return n;
  };
  for (uint8_t sl = 0; sl < NUM_SLOTS; sl++)
    printf("  слот %c: ведут=%d, пикселей рамки %d\n",
           'A' + sl, slotModulated(sl) ? 1 : 0, framed(sl));
  ck("C помечен",     framed(2) >= 6, 1);
  ck("A не помечен",  framed(0) <= 2, 1);
  bind(2, false, 1, 1, 0); bind(2, false, 3, MOD_EXT, 0);
  drawSlotPage();
  ck("нулевая глубина рамку снимает", framed(2) <= 2, 1);

  return done("MODLAYER");
}
