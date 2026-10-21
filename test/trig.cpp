#include "common.h"

// Модель тригов ручных генераторов: шаг звучит ТОЛЬКО там, где нота
// забита тригом, а длинная нота обрывается перезапуском шага.

// Ноты и Note Off в порядке появления: для обрыва важен именно порядок.
struct Msg { bool on; int note; };
static std::vector<Msg> msgs() {
  std::vector<Msg> v;
  for (size_t i = 0; i + 2 < g_midi.size();) {
    uint8_t st = g_midi[i];
    if ((st & 0xF0) == 0x90)      { v.push_back({true,  g_midi[i + 1]}); i += 3; }
    else if ((st & 0xF0) == 0x80) { v.push_back({false, g_midi[i + 1]}); i += 3; }
    else if (st & 0x80) i += 3; else i++;
  }
  return v;
}
static void prm(const char *t) {
  printf("  %-28s", t);
  for (auto &m : msgs()) printf("%s%d ", m.on ? "" : "-", m.note);
  printf("\n");
}

// Один шаг генератора с выдачей наружу.
static void step(uint8_t slot) { fireGenerator(slot); serviceScheduler(); }

static void setupBasic() {
  reset();
  loadApp(0, (uint8_t)AppId::BASIC);
  cfg[0].scale = SCALE_CHROMATIC; cfg[0].root = 0; cfg[0].midiCh = 1;
  cfg[0].p[0] = 60; cfg[0].p[1] = 64; cfg[0].p[2] = 67; cfg[0].p[3] = 1;
  for (int s = 0; s < 16; s++)
    for (int col = 0; col < 3; col++) cfg[0].stepVal[s][col] = cfg[0].p[col];
  rt[0].stepIndex = 0;
}

int main() {
  printf("=== Свежезагруженный ручной генератор звучит ===\n");
  setupBasic();
  g_midi.clear();
  for (int s = 0; s < 4; s++) step(0);
  prm("BASIC после loadApp:");
  ck("свежий BASIC звучит на каждом шаге", (int)noteList().size(), 4);

  printf("\n=== Шаг без тригов это пауза ===\n");
  setupBasic();
  cfg[0].lane[0] = 0;                 // сняли все триги первого голоса
  g_midi.clear();
  for (int s = 0; s < 4; s++) step(0);
  prm("все триги сняты:");
  ck("без тригов генератор молчит", (int)noteList().size(), 0);
  ck("без тригов на шину ничего не идёт", busCur[0].count, 0);

  printf("\n=== Картинка не расходится со звуком ===\n");
  // generatorHit — единственный источник правды и для звука, и для
  // ряда шагов. Если он скажет «хит» там, где нот нет, ряд покажет
  // шаг заполненным, а слот промолчит.
  setupBasic();
  cfg[0].lane[0] = 0x0005;            // шаги 1 и 3
  {
    int shown = 0;
    for (int s = 0; s < 16; s++) if (generatorHit(0, s)) shown++;
    printf("  ряд показывает хитов:      %d\n", shown);
    ck("ряд показывает ровно затригованные шаги", shown, 2);
  }
  cfg[0].lane[0] = 0;
  {
    int shown = 0;
    for (int s = 0; s < 16; s++) if (generatorHit(0, s)) shown++;
    ck("пустой паттерн рисуется пустым", shown, 0);
  }
  // Четвёртая колонка это длина, а не голос: её триг не обязан
  // заставлять шаг звучать.
  cfg[0].lane[3] = 0xFFFF;
  {
    int shown = 0;
    for (int s = 0; s < 16; s++) if (generatorHit(0, s)) shown++;
    ck("триг длины не делает шаг хитом", shown, 0);
    g_midi.clear();
    for (int s = 0; s < 4; s++) step(0);
    ck("и звучать не заставляет", (int)noteList().size(), 0);
  }

  printf("\n=== Через шаг ===\n");
  setupBasic();
  cfg[0].lane[0] = 0x5555;            // чётные шаги
  g_midi.clear();
  for (int s = 0; s < 8; s++) step(0);
  prm("триг через шаг:");
  ck("звучит половина шагов", (int)noteList().size(), 4);

  printf("\n=== Голос без первого ===\n");
  setupBasic();
  cfg[0].lane[0] = 0;                 // NOTE1 выключен совсем
  cfg[0].lane[1] = 0xFFFF;            // звучит только NOTE2
  g_midi.clear();
  step(0);
  prm("только NOTE2:");
  {
    auto n = noteList();
    ck("звучит ровно один голос", (int)n.size(), 1);
    ck("и это NOTE2, а не NOTE1", n.empty() ? -1 : n[0], 64);
  }

  printf("\n=== SKELETON: интервалы без опорного тона ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::SKELETON);
  cfg[0].scale = SCALE_CHROMATIC; cfg[0].root = 0; cfg[0].midiCh = 1;
  cfg[0].p[0] = 60; cfg[0].p[1] = 4; cfg[0].p[2] = 7;
  for (int s = 0; s < 16; s++) {
    cfg[0].stepVal[s][0] = 60; cfg[0].stepVal[s][1] = 4; cfg[0].stepVal[s][2] = 7;
  }
  cfg[0].lane[0] = 0;                 // ROOT молчит
  cfg[0].lane[1] = 0xFFFF;
  cfg[0].lane[2] = 0xFFFF;
  rt[0].stepIndex = 0;
  g_midi.clear();
  step(0);
  prm("ROOT снят, интервалы есть:");
  {
    auto n = noteList();
    ck("звучат два интервала", (int)n.size(), 2);
    for (int v : n) ck("опорный тон не звучит", v == 60 ? 1 : 0, 0);
  }
  reset();
  loadApp(0, (uint8_t)AppId::SKELETON);
  cfg[0].lane[0] = 0; cfg[0].lane[1] = 0; cfg[0].lane[2] = 0;
  cfg[0].midiCh = 1; rt[0].stepIndex = 0;
  g_midi.clear();
  for (int s = 0; s < 4; s++) step(0);
  prm("SKELETON без тригов:");
  ck("SKELETON тоже умеет молчать", (int)noteList().size(), 0);

  printf("\n=== Длинный триг обрывается перезапуском шага ===\n");
  setupBasic();
  // TRIG = 4 шага: без обрыва вторая нота легла бы поверх первой.
  cfg[0].p[3] = 4;
  for (int s = 0; s < 16; s++) cfg[0].stepVal[s][3] = 4;
  cfg[0].sub4 = 1;
  g_midi.clear();
  step(0);
  g_ms += 20; serviceScheduler();
  step(0);
  prm("два шага при TRIG=4:");
  {
    auto m = msgs();
    // Ожидаем: 60, -60, 60 — старая нота снята ДО новой.
    ck("сообщений ровно три", (int)m.size(), 3);
    if (m.size() == 3) {
      ck("первая нота зажглась", m[0].on ? 1 : 0, 1);
      ck("вторым идёт обрыв", m[1].on ? 0 : 1, 1);
      ck("обрывается та же нота", m[1].note, m[0].note);
      ck("третьей зажигается новая", m[2].on ? 1 : 0, 1);
    }
  }

  printf("\n=== Пауза не обрывает тянущуюся ноту ===\n");
  setupBasic();
  cfg[0].p[3] = 4;
  for (int s = 0; s < 16; s++) cfg[0].stepVal[s][3] = 4;
  cfg[0].lane[0] = 0x0001;            // звучит только первый шаг
  g_midi.clear();
  step(0);
  for (int s = 0; s < 3; s++) { g_ms += 20; serviceScheduler(); step(0); }
  prm("шаг 1 звучит, 2-4 пауза:");
  {
    auto m = msgs();
    ck("зажглась одна нота", (int)m.size(), 1);
    ck("и её никто не оборвал", m.empty() ? -1 : (m[0].on ? 1 : 0), 1);
  }

  printf("\n=== Алгоритмические: рисунок только за алгоритмом ===\n");
  // EUCLIDIAN с нулём импульсов обязан молчать. Раньше ручной триг
  // пробивал алгоритм, и снять его после запрета было бы нечем.
  reset();
  loadApp(0, (uint8_t)AppId::EUCLIDIAN);
  cfg[0].midiCh = 1; cfg[0].scale = SCALE_CHROMATIC;
  cfg[0].p[0] = 8; cfg[0].p[1] = 0; cfg[0].p[2] = 0; cfg[0].p[3] = 60;
  cfg[0].sub4 = 127;                  // TRG CHANCE полностью открыт
  rt[0].stepIndex = 0;
  cfg[0].lane[0] = 0xFFFF;            // залежавшийся триг из старой прошивки
  cfg[0].lane[3] = 0xFFFF;
  {
    int shown = 0;
    for (int s = 0; s < 16; s++) if (generatorHit(0, s)) shown++;
    printf("  PULSE=0, хитов:            %d\n", shown);
    ck("ноль импульсов это тишина", shown, 0);
    g_midi.clear();
    for (int s = 0; s < 8; s++) step(0);
    ck("и звука тоже нет", (int)noteList().size(), 0);
  }
  // Теперь без залежавшихся тригов — так слот выглядит после переноса.
  for (int col = 0; col < NUM_COLS; col++) cfg[0].lane[col] = 0;
  cfg[0].p[1] = 4;
  {
    int shown = 0;
    for (int s = 0; s < 8; s++) if (generatorHit(0, s)) shown++;
    printf("  PULSE=4 из 8, хитов:       %d\n", shown);
    ck("алгоритм даёт ровно свои импульсы", shown, 4);
  }

  printf("\n=== Перенос чистит триги у алгоритмических ===\n");
  nvsClear();
  reset();
  loadApp(0, (uint8_t)AppId::EUCLIDIAN);
  loadApp(1, (uint8_t)AppId::BASIC);
  cfg[0].lane[2] = 0x00FF;            // мусор от прошивки до переноса
  cfg[1].lane[0] = 0; cfg[1].lane[1] = 0; cfg[1].lane[2] = 0;
  storage.begin("trajectories", false);
  storage.putBytes("cfg", cfg, sizeof(cfg));
  storage.end();                      // ключа версии нет — как у старой прошивки
  for (uint8_t s = 0; s < NUM_SLOTS; s++) cfg[s] = SlotCfg{};
  loadSettings();
  printf("  EUCLID lane[2] после:      %04x\n", cfg[0].lane[2]);
  printf("  BASIC  lane[0] после:      %04x\n", cfg[1].lane[0]);
  ck("у алгоритмического триги сняты", cfg[0].lane[2], 0);
  ck("ручной генератор не замолчал", cfg[1].lane[0], 0xFFFF);
  // Повторная загрузка ничего не трогает: снять все триги — законное
  // состояние, и возвращать их каждую загрузку нельзя.
  cfg[1].lane[0] = 0;
  storage.begin("trajectories", false);
  storage.putBytes("cfg", cfg, sizeof(cfg));
  storage.end();
  loadSettings();
  printf("  BASIC  lane[0] повторно:   %04x\n", cfg[1].lane[0]);
  ck("перенос не повторяется", cfg[1].lane[0], 0);

  return done("TRIG");
}
