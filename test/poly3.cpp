#include "common.h"
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("=== ARPEGGIO по своей сетке (прошлый тест его не тикал) ===\n");
  reset();
  loadApp(1, (uint8_t)AppId::ARP);
  cfg[1].arm = 0; cfg[1].running = true; cfg[1].p[0] = 4; cfg[1].p[1] = 64; cfg[1].p[2] = 0;
  SlotRt &r = rt[1];
  r.capNotes[0] = 36; r.capNotes[1] = 55; r.capNotes[2] = 78; r.capCount = 3;
  for (int i = 0; i < 3; i++) r.capVel[i] = 100;
  r.capGateMs = 200; r.capScale = SCALE_CHROMATIC; r.capRoot = 0;
  g_midi.clear(); onCapture(1);
  for (int t = 0; t < 400; t++) { onClockTick(); serviceScheduler(); g_ms += 5; g_us += 5000; }
  auto a = noteList();
  pr("ARP за 400 тиков:", a);
  int sa = 0; for (int in : {36,55,78}) for (int v : a) if (v == in) { sa++; break; }
  ck("ARP обходит все три ноты", sa, 3);

  printf("\n=== Дубликаты в аккорде после квантования в лад ===\n");
  // В пентатонике соседние полутона схлопываются в одну ступень. Если
  // два голоса шага дадут одну и ту же высоту, на один канал уйдут две
  // одинаковые ноты — вторая перетриггерит первую, а первый Note Off
  // погасит обе.
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].root = 0; cfg[0].sub4 = 1; cfg[0].midiCh = 1; cfg[0].rate = 4;
  cfg[0].p[0] = 60; cfg[0].p[1] = 61; cfg[0].p[2] = 62;   // три соседних полутона
  cfg[0].lane[1] = 0xFFFF; cfg[0].lane[2] = 0xFFFF;
  for (int k = 0; k < 16; k++) { cfg[0].stepVal[k][1] = 61; cfg[0].stepVal[k][2] = 62; }
  for (uint8_t sc = 0; sc < 10; sc++) {
    cfg[0].scale = sc; rt[0].stepIndex = 0;
    g_midi.clear(); fireGenerator(0); serviceScheduler();
    auto o = noteList();
    std::set<int> uniq(o.begin(), o.end());
    printf("  лад %-5s -> ", scales[sc].name);
    for (int v : o) printf("%d ", v);
    if (uniq.size() != o.size()) printf("  <- ДУБЛИ (%d нот, %d разных)", (int)o.size(), (int)uniq.size());
    printf("\n");
    for (int t = 0; t < 200; t++) { serviceScheduler(); g_ms += 10; }
  }

  printf("\n=== Что происходит с дублями на шине ===\n");
  cfg[0].scale = 0; rt[0].stepIndex = 0;   // мажор: 60,61,62 -> 60,60,62
  g_midi.clear(); fireGenerator(0); serviceScheduler();
  printf("  шина: count=%d [", busCur[0].count);
  for (int i = 0; i < busCur[0].count; i++) printf("%d ", busCur[0].notes[i]);
  printf("]\n");
  auto o = noteList();
  pr("наружу:", o);
  std::set<int> uniq(o.begin(), o.end());
  ck("в MIDI не уходит одна и та же нота дважды", (int)o.size(), (int)uniq.size());

  printf("\n=== Висящие ноты от дубля ===\n");
  for (int t = 0; t < 400; t++) { serviceScheduler(); g_ms += 10; }
  int on[128] = {0}, off[128] = {0};
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0x90) { on[g_midi[i+1]]++; i += 3; }
    else if ((st & 0xF0) == 0x80) { off[g_midi[i+1]]++; i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  for (int n = 0; n < 128; n++)
    if (on[n] || off[n]) printf("  нота %d: on=%d off=%d\n", n, on[n], off[n]);

  return done("POLY3");
}
