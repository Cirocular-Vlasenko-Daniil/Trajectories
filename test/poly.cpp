#include "common.h"

// Отдаёт слоту 1 трезвучие и считает, сколько нот он выпустил наружу
// и сколько положил на шину.
struct Res { int out, bus; };
Res through(AppId app) {
  reset();
  loadApp(1, (uint8_t)app);
  cfg[1].arm = 0; cfg[1].arm2 = ARM_NONE;
  SlotRt &r = rt[1];
  r.capNotes[0] = 60; r.capNotes[1] = 64; r.capNotes[2] = 67;
  r.capCount = 3;
  for (int i = 0; i < 3; i++) r.capVel[i] = 100;
  r.capGateMs = 200; r.capScale = 0; r.capRoot = 0;
  g_midi.clear();
  onCapture(1);
  // Процессоры по сетке и растянутые во времени доигрывают позже.
  for (int t = 0; t < 60; t++) { serviceProcessor(1); serviceScheduler(); g_ms += 20; }
  int bus = busCur[1].count;
  return {(int)evs().size(), bus};
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Что каждый процессор делает с трезвучием 60/64/67 ===\n");
  printf("  %-12s %5s %5s\n", "модуль", "нот", "шина");
  struct { AppId a; const char *n; } list[] = {
    {AppId::ARP,"ARPEGGIO"}, {AppId::STRUM,"STRUM"}, {AppId::NDELAY,"NDELAY"},
    {AppId::GLIDER,"GLIDER"}, {AppId::ADD,"ADD"},    {AppId::DUST,"DUST"}, {AppId::ENV,"ENVELOPE"}, {AppId::MLFO,"MORPH LFO"},
    {AppId::KEYTRK,"KEYTRACK"}, {AppId::LOGIC,"LOGIC"},    {AppId::MULTX,"MULTI OUT"}, {AppId::POLYSPL,"POLY SPLIT"},
    {AppId::RROBIN,"ROUND ROBIN"},
  };
  int mono = 0;
  for (auto &e : list) {
    Res x = through(e.a);
    const char *mark = (x.bus >= 3) ? "" : (x.bus <= 1 ? "  <- схлопнул в моно" : "  <- потерял голос");
    printf("  %-12s %5d %5d%s\n", e.n, x.out, x.bus, mark);
    if (x.bus <= 1) mono++;
  }
  printf("\n  схлопывают полифонию в моно: %d из %d\n", mono, (int)(sizeof(list)/sizeof(*list)));

  printf("\n=== Сколько голосов вообще вмещает шина ===\n");
  printf("  MAX_VOICES = %d\n", MAX_VOICES);
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].scale = 0; cfg[0].root = 0; cfg[0].sub4 = 4;
  cfg[0].p[0] = 60; cfg[0].p[1] = 64; cfg[0].p[2] = 67; cfg[0].p[3] = 1;
  cfg[0].lane[1] |= 1u; cfg[0].stepVal[0][1] = 64;
  cfg[0].lane[2] |= 1u; cfg[0].stepVal[0][2] = 67;
  rt[0].stepIndex = 0;
  g_midi.clear(); fireGenerator(0); serviceScheduler();
  pr("BASIC шаг с тремя:", noteList());
  printf("  шина: %d\n", busCur[0].count);

  printf("\n=== Note Off: гасит ли аккорд сам себя ===\n");
  // Два подряд аккорда на одном канале. Если нота повторяется, Note Off
  // первой убивает вторую — классическая болезнь моно-учёта.
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].scale = 0; cfg[0].root = 0; cfg[0].sub4 = 2; cfg[0].midiCh = 1;
  cfg[0].p[0] = 60; cfg[0].p[1] = 64; cfg[0].p[2] = 67; cfg[0].p[3] = 4;  // гейт 4 шага
  cfg[0].lane[1] = 0xFFFF; cfg[0].lane[2] = 0xFFFF;
  for (int k = 0; k < 16; k++) { cfg[0].stepVal[k][1] = 64; cfg[0].stepVal[k][2] = 67; }
  rt[0].stepIndex = 0;
  g_midi.clear();
  for (int s = 0; s < 3; s++) { fireGenerator(0); for (int t = 0; t < 5; t++) { serviceScheduler(); g_ms += 10; } }
  // Считаем баланс: сколько раз каждая нота включилась и выключилась.
  int on[128] = {0}, off[128] = {0};
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0x90) { on[g_midi[i+1]]++; i += 3; }
    else if ((st & 0xF0) == 0x80) { off[g_midi[i+1]]++; i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  printf("  нота 60: on=%d off=%d | 64: on=%d off=%d | 67: on=%d off=%d\n",
         on[60], off[60], on[64], off[64], on[67], off[67]);
  int stuck = 0;
  for (int n = 0; n < 128; n++) if (on[n] != off[n]) stuck++;
  printf("  нот с перекосом on/off: %d\n", stuck);

  return done("POLY-AUDIT");
}
