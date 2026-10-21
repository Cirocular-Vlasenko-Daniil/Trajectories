#include "common.h"
void band(int rows) {
  for (int y = 0; y < rows; y++) {
    printf("  ");
    for (int x = 0; x < 128; x++)
      putchar(g_fb[y][x] ? '#' : (g_tx[y][x] && g_tx[y][x] != '-' ? g_tx[y][x] : ' '));
    printf("|\n");
  }
}
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("=== 1. Внешний CC доезжает до таблицы ===\n");
  reset();
  memset(ccVal, 0, sizeof(ccVal));
  // Байты как с провода: статус 0xB1, номер, значение.
  midiRunningStatus = 0xB1; midiHaveFirstData = true; midiPendingByte = 74;
  { uint8_t data = 100, type = midiRunningStatus & 0xF0;
    midiHaveFirstData = false;
    if (type == 0xB0) ccVal[midiPendingByte & 0x7F] = (uint8_t)(data & 0x7F); }
  ck("CC 74 записан", ccVal[74], 100);

  printf("\n=== 2. Привязка ведёт параметр ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::TOPOLOGY);
  cfg[0].running = true; cfg[0].p[1] = 64;      // плотность дорожки 1
  cfg[0].modCC[MP_MAIN + 1] = 74;               // ведём её контроллером 74
  cfg[0].modDepth[MP_MAIN + 1] = 64;
  int seen[3];
  int vals[3] = {0, 64, 127};
  for (int k = 0; k < 3; k++) {
    ccVal[74] = (uint8_t)vals[k];
    serviceSlots();
    seen[k] = modP(0, 1);
    printf("  CC74=%3d -> плотность %3d (хранится %d)\n", vals[k], seen[k], (int)cfg[0].p[1]);
  }
  ck("центр ничего не меняет", seen[1], 64);
  ck("минимум уводит вниз", seen[0] < 20, 1);
  ck("максимум уводит вверх", seen[2] > 110, 1);
  ck("хранимое значение не испорчено", cfg[0].p[1], 64);
  cfg[0].modDepth[MP_MAIN + 1] = -64;
  ccVal[74] = 127; serviceSlots();
  printf("  при глубине -64 и CC=127: %d\n", modP(0, 1));
  ck("знак глубины разворачивает ход", modP(0, 1) < 20, 1);

  printf("\n=== 3. Свой модуль как источник, без петли через MIDI ===\n");
  reset();
  memset(ccVal, 0, sizeof(ccVal));
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].running = true; cfg[0].rate = 4; cfg[0].midiCh = 0; cfg[0].p[0] = 60;
  loadApp(1, (uint8_t)AppId::MLFO);
  cfg[1].arm = 0; cfg[1].running = true; cfg[1].midiCh = 0;   // наружу молчит
  cfg[1].ccDest = 50; cfg[1].p[3] = 127; cfg[1].sub4 = 2;
  loadApp(2, (uint8_t)AppId::TOPOLOGY);
  cfg[2].running = true; cfg[2].p[1] = 64;
  cfg[2].modCC[MP_MAIN + 1] = 50; cfg[2].modDepth[MP_MAIN + 1] = 64;
  int lo = 999, hi = -999;
  for (int t = 0; t < 3000; t++) {
    onClockTick(); serviceSlots(); serviceScheduler();
    g_ms += 2; g_us += 2000; state.midiTicks++;
    int v = modP(2, 1);
    if (v < lo) lo = v; if (v > hi) hi = v;
  }
  printf("  LFO с каналом NO ведёт плотность: %d..%d\n", lo, hi);
  ck("внутренний источник работает без MIDI наружу", hi - lo > 40, 1);

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
  memset(ccVal, 0, sizeof(ccVal));
  loadApp(0, (uint8_t)AppId::BASIC);
  loadApp(1, (uint8_t)AppId::MLFO); cfg[1].arm = 0;
  loadApp(2, (uint8_t)AppId::TOPOLOGY);
  loadApp(3, (uint8_t)AppId::ARP);  cfg[3].arm = 2;
  cfg[2].modCC[MP_MAIN + 1] = 50; cfg[2].modDepth[MP_MAIN + 1] = 48;
  cfg[2].modCC[MP_MAIN + 3] = 74; cfg[2].modDepth[MP_MAIN + 3] = -20;
  cfg[0].modCC[MP_MAIN + 0] = 12; cfg[0].modDepth[MP_MAIN + 0] = 30;
  state.currentSlot = 2; state.subPage = false; state.selectedCol = 1;
  state.modLayer = false; drawSlotPage();
  printf("TOPOLOGY, обычный вид (рамки у A и C — их ведут):\n"); band(32);
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
    printf("  слот %c: привязок %d, пикселей рамки %d\n",
           'A' + sl, slotModulated(sl) ? 1 : 0, framed(sl));
  ck("A помечен",       framed(0) >= 6, 1);
  ck("B не помечен",    framed(1) <= 2, 1);
  ck("C помечен",       framed(2) >= 6, 1);
  ck("D не помечен",    framed(3) <= 2, 1);
  cfg[2].modDepth[MP_MAIN + 1] = 0; cfg[2].modDepth[MP_MAIN + 3] = 0;
  drawSlotPage();
  ck("нулевая глубина рамку снимает", framed(2) <= 2, 1);

  return done("MODLAYER");
}
