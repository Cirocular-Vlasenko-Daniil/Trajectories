#include "common.h"

// Что модуль отдаёт НА ШИНУ как источник модуляции. Правило: сигнал до
// преобразования глубины. CC-выходы несут отмасштабированное, шина —
// исходное.

// Все CC, ушедшие наружу, парами (номер, значение).
struct CC { int num, val; };
static std::vector<CC> ccs() {
  std::vector<CC> v;
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0xB0) { v.push_back({g_midi[i + 1], g_midi[i + 2]}); i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  return v;
}
static void feed(uint8_t slot, uint8_t note, uint32_t gate = 500) {
  SlotRt &r = rt[slot];
  r.capNotes[0] = note; r.capCount = 1; r.capVel[0] = 100;
  r.capGateMs = gate; r.capScale = SCALE_CHROMATIC; r.capRoot = 0;
}

int main() {
  printf("=== ENVELOPE: оба адреса несут одно значение ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::ENV);
  cfg[0].arm = 1; cfg[0].arm2 = ARM_NONE; cfg[0].midiCh = 1;
  cfg[0].p[2] = 70; cfg[0].p[3] = 90;
  feed(0, 60);
  onCapture(0);
  int pairs = 0, mirrored = 0;
  for (int t = 0; t < 60; t++) {
    g_midi.clear();
    serviceProcessor(0);
    auto v = ccs();
    if (v.size() == 2) {
      pairs++;
      if (v[0].val != v[1].val) mirrored++;
      if (pairs <= 3) printf("  cc%d=%-3d cc%d=%-3d  шина=%d\n",
                             v[0].num, v[0].val, v[1].num, v[1].val, busCur[0].ccValue);
      // На шину идёт сама огибающая, то есть то же значение.
      ck("шина несёт огибающую", busCur[0].ccValue, v[0].val);
    }
    g_ms += 20;
  }
  printf("  пар отправлено %d, из них зеркальных %d\n", pairs, mirrored);
  ck("пары отправлялись", pairs > 3, 1);
  ck("зеркала больше нет", mirrored, 0);

  printf("\n=== KEYTRACK: на шину сигнал без глубин ===\n");
  // Две линии с РАЗНОЙ глубиной. Шина не должна зависеть ни от одной.
  int busA = 0, busB = 0;
  for (int pass = 0; pass < 2; pass++) {
    reset();
    loadApp(0, (uint8_t)AppId::KEYTRK);
    cfg[0].arm = 1; cfg[0].arm2 = ARM_NONE; cfg[0].midiCh = 1;
    cfg[0].sub2 = 60; cfg[0].sub3 = 24;
    cfg[0].p[0] = pass ? 16 : 64;      // глубина первой линии разная
    cfg[0].p[1] = pass ? 64 : 16;      // и второй — тоже, наоборот
    cfg[0].p[2] = 0; cfg[0].p[3] = 0;
    feed(0, 72);
    g_midi.clear();
    onCapture(0);
    auto v = ccs();
    printf("  глубины %d/%d -> CC:", cfg[0].p[0], cfg[0].p[1]);
    for (auto &x : v) printf(" cc%d=%d", x.num, x.val);
    printf("   шина=%d\n", busCur[0].ccValue);
    (pass ? busB : busA) = busCur[0].ccValue;
    ck("линии шлют разное", v.size() >= 2 && v[0].val != v[1].val ? 1 : 0, 1);
  }
  ck("шина от глубин не зависит", busA, busB);
  // И сам сигнал осмысленный: нота выше опоры — выше середины.
  printf("  нота 72 при опоре 60:      шина=%d\n", busA);
  ck("выше опоры — выше середины", busA > 64, 1);

  printf("\n=== MORPH LFO: на шину волна до AMT ===\n");
  int lfoBus[2] = {0, 0}, lfoCC[2] = {0, 0};
  for (int pass = 0; pass < 2; pass++) {
    reset();
    loadApp(0, (uint8_t)AppId::MLFO);
    cfg[0].arm = 1; cfg[0].arm2 = ARM_NONE; cfg[0].midiCh = 1;
    cfg[0].p[0] = 0; cfg[0].p[1] = 0; cfg[0].p[2] = 0;
    cfg[0].p[3] = pass ? 127 : 8;      // AMT наружу
    cfg[0].ccDest = 74;
    int maxBus = 64, maxCC = 64;
    // MLFO тикает от транспорта, а не от захвата: гоняем его так же,
    // как это делает прибор.
    for (int t = 0; t < 400; t++) {
      g_midi.clear();
      onClockTick(); serviceSlots(); serviceScheduler();
      for (auto &x : ccs()) if (abs(x.val - 64) > abs(maxCC - 64)) maxCC = x.val;
      if (abs((int)busCur[0].ccValue - 64) > abs(maxBus - 64)) maxBus = busCur[0].ccValue;
      g_ms += 5; g_us += 5000;
    }
    lfoBus[pass] = maxBus; lfoCC[pass] = maxCC;
    printf("  AMT=%-3d размах CC=%-3d  размах шины=%d\n", cfg[0].p[3], maxCC, maxBus);
  }
  ck("AMT меняет выход наружу", lfoCC[1] != lfoCC[0], 1);
  ck("а шину не трогает", lfoBus[0], lfoBus[1]);

  printf("\n=== DUST: отдаёт частицу, а не свой FILL ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::DUST);
  cfg[0].arm = 1; cfg[0].arm2 = ARM_NONE; cfg[0].midiCh = 1;
  cfg[0].p[0] = 127;                   // FILL на максимум
  cfg[0].p[1] = 100;                   // всегда корень — значение предсказуемо
  cfg[0].p[3] = 0;
  cfg[0].ccDest = 75;
  feed(0, 60);
  int sawFill = 0, sawNote = 0;
  for (int t = 0; t < 40; t++) {
    g_midi.clear();
    serviceProcessor(0);
    for (auto &x : ccs()) {
      if (x.val == 127) sawFill++;
      if (x.val >= 55 && x.val <= 65) sawNote++;
    }
    g_ms += 30;
  }
  printf("  CC со значением FILL=127:  %d\n", sawFill);
  printf("  CC похожих на ноту 60:     %d\n", sawNote);
  ck("свой параметр наружу не уходит", sawFill, 0);
  ck("а нота уходит", sawNote > 0, 1);

  return done("SRC");
}
