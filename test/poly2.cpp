#include "common.h"

// Трезвучие из далеко разнесённых нот: по выходу сразу видно, какие из
// них модуль вообще увидел.
void feed3(uint8_t slot) {
  SlotRt &r = rt[slot];
  r.capNotes[0] = 36; r.capNotes[1] = 55; r.capNotes[2] = 78;
  r.capCount = 3;
  for (int i = 0; i < 3; i++) r.capVel[i] = 100;
  r.capGateMs = 200; r.capScale = SCALE_CHROMATIC; r.capRoot = 0;
}
std::vector<int> run(AppId app, int mode = -1) {
  reset();
  loadApp(1, (uint8_t)app);
  cfg[1].arm = 0; cfg[1].arm2 = ARM_NONE; cfg[1].scale = SCALE_CHROMATIC;
  if (mode >= 0) cfg[1].p[1] = mode;
  feed3(1);
  g_midi.clear();
  onCapture(1);
  for (int t = 0; t < 80; t++) { serviceProcessor(1); serviceScheduler(); g_ms += 20; }
  return noteList();
}
// Сколько из трёх входных нот представлено на выходе. Сравниваем по
// классу высоты, потому что модули достраивают октавные дубли, — и
// поэтому сам аккорд обязан быть из РАЗНЫХ классов: 36/60/84 это всё
// «до», и такая проверка проходила бы всегда.
int seen(const std::vector<int> &out) {
  int n = 0;
  for (int in : {36, 55, 78}) {
    for (int v : out) if (((v - in) % 12 + 12) % 12 == 0) { n++; break; }
  }
  return n;
}
void row(const char *name, AppId app, int mode = -1) {
  auto o = run(app, mode);
  printf("  %-12s видит %d/3  нот на выходе %2d  ", name, seen(o), (int)o.size());
  for (size_t i = 0; i < o.size() && i < 8; i++) printf("%d ", o[i]);
  if (o.size() > 8) printf("...");
  printf("\n");
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Кто видит весь аккорд 36/55/78, а кто одну ноту ===\n");
  row("ARPEGGIO",   AppId::ARP);
  row("STRUM",      AppId::STRUM);
  row("NOTE DELAY", AppId::NDELAY);
  row("ADD",        AppId::ADD);
  row("RROBIN cyc", AppId::RROBIN, 0);
  row("RROBIN chrd",AppId::RROBIN, 3);
  row("MULTI OUT",  AppId::MULTX);
  row("POLY SPLIT", AppId::POLYSPL);
  row("GLIDER",     AppId::GLIDER);
  row("DUST",       AppId::DUST);
  row("KEYTRACK",   AppId::KEYTRK);

  printf("\n=== Баланс Note On / Note Off у аккорда ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].scale = SCALE_CHROMATIC; cfg[0].root = 0; cfg[0].sub4 = 2;
  cfg[0].midiCh = 1; cfg[0].rate = 4;                 // 1/16, короткий шаг
  cfg[0].p[0] = 60; cfg[0].p[1] = 64; cfg[0].p[2] = 67; cfg[0].p[3] = 1;
  cfg[0].lane[1] = 0xFFFF; cfg[0].lane[2] = 0xFFFF;
  for (int k = 0; k < 16; k++) { cfg[0].stepVal[k][1] = 64; cfg[0].stepVal[k][2] = 67; }
  rt[0].stepIndex = 0;
  g_midi.clear();
  for (int s = 0; s < 4; s++) {
    fireGenerator(0);
    for (int t = 0; t < 30; t++) { serviceScheduler(); g_ms += 10; }
  }
  for (int t = 0; t < 400; t++) { serviceScheduler(); g_ms += 10; }  // дать всему отзвучать
  int on[128] = {0}, off[128] = {0};
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0x90) { on[g_midi[i+1]]++; i += 3; }
    else if ((st & 0xF0) == 0x80) { off[g_midi[i+1]]++; i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  for (int n : {60, 64, 67}) printf("  нота %d: on=%d off=%d\n", n, on[n], off[n]);
  int stuck = 0;
  for (int n = 0; n < 128; n++) if (on[n] != off[n]) stuck++;
  ck("все ноты погашены", stuck, 0);

  return done("POLY2");
}
