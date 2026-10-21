// Общая обвязка тестов: подключает прошивку целиком и даёт мелкие
// помощники. main() прошивки переименован, чтобы не мешать тестовому.
#pragma once
#include <cstdio>
#include <vector>
#include <string>
#include <set>
#include <array>
#include <cstdint>
#define main traj_main_unused
#include "../Trajectories/trajectories.cpp"
#undef main

extern std::vector<uint8_t> g_midi;
extern unsigned long g_ms, g_us;
extern char g_fb[32][128], g_tx[32][128];
void nvsClear();
void fbclear();

static int g_fails = 0;
inline void ck(const char *w, int got, int want) {
  if (got != want) { printf("FAIL %s: got %d want %d\n", w, got, want); g_fails++; }
}
inline int done(const char *name) {
  printf("\n");
  if (g_fails) printf("%d FAILURES\n", g_fails);
  else         printf("ALL %s TESTS PASS\n", name);
  return g_fails ? 1 : 0;
}

// Нота и канал каждого Note On, ушедшего наружу.
struct Ev { int ch, note, vel; };
inline std::vector<Ev> evs() {
  std::vector<Ev> v;
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0x90) { v.push_back({(st & 0x0F) + 1, g_midi[i + 1], g_midi[i + 2]}); i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  return v;
}
inline std::vector<int> noteList() {
  std::vector<int> n; for (auto &e : evs()) n.push_back(e.note); return n;
}
inline void pr(const char *t, const std::vector<int> &v) {
  printf("  %-26s", t); for (int x : v) printf("%d ", x); printf("\n");
}
extern int g_pin[64];

// Кнопки через НАСТОЯЩИЙ updateButtons(): меняем пин и прокручиваем
// время, чтобы отработал дебаунс. Так проверяется тот код, который
// реально исполняется на плате.
inline void pump(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) { g_ms += 5; g_us += 5000; updateButtons(); }
}
inline void hold(uint8_t pin, uint32_t ms) {
  g_pin[pin] = LOW;  pump(ms);
  g_pin[pin] = HIGH; pump(60);
}

// Чистый старт: пустые слоты, пустой планировщик, известное время.
inline void reset() {
  flushScheduler();
  clearModOffsets();
  for (uint8_t s = 0; s < NUM_SLOTS; s++) {
    cfg[s] = SlotCfg{};
    cfg[s].app = (uint8_t)AppId::EMPTY;
    cfg[s].midiCh = (uint8_t)(s + 1);
    cfg[s].arm2 = ARM_NONE;
    rt[s] = SlotRt{};
    busCur[s] = Signal{};
    busSnap[s] = Signal{};
  }
  g_midi.clear();
  g_ms = 100000;
  for (int i = 0; i < 64; i++) g_pin[i] = HIGH;
  for (uint8_t i = 0; i < 4; i++) { buttons[i].raw = HIGH; buttons[i].lastStable = HIGH;
                                    buttons[i].lastChange = 0; buttons[i].processed = false; }
  state.modLayer = false; state.patchOpen = false; state.appListOpen = false;
  state.encSWPressAt = 0; state.encSWRotated = false; state.encSWLastPress = 0;
}
