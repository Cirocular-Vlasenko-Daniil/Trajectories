#include "common.h"
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Таблица приложений совпадает с enum ===\n");
  // Порядок enum лежит во флеше: расхождение с apps[] показывало бы
  // параметры чужого модуля. Один раз так и случилось.
  const char *want[] = {
    "EMPTY", "BASIC", "SKELETON", "EUCLIDIAN", "TOPOLOGY",
    "ARPEGGIO", "STRUM", "NOTE DELAY", "ENVELOPE", "MORPH LFO",
    "DUST", "GLIDER", "ADD", "LOGIC",
    "KEYTRACK", "MULTI OUT", "POLY SPLIT", "ROUND ROBIN",
  };
  ck("число приложений", APP_COUNT, (int)(sizeof(want) / sizeof(*want)));
  int bad = 0;
  for (uint8_t i = 0; i < APP_COUNT && i < sizeof(want)/sizeof(*want); i++) {
    if (strcmp(apps[i].name, want[i]) != 0) {
      printf("  РАСХОЖДЕНИЕ %d: таблица \"%s\", ожидалось \"%s\"\n", i, apps[i].name, want[i]);
      bad++;
    }
  }
  ck("имена на своих местах", bad, 0);
  ck("SIEVE удалён из таблицы", strstr(apps[0].name, "SIEVE") == nullptr, 1);

  printf("\n=== Браузер: каждое приложение ровно один раз ===\n");
  int seen[64] = {0};
  for (uint8_t i = 0; i < APP_COUNT; i++) seen[browseOrder[i]]++;
  int dup = 0, miss = 0;
  for (uint8_t i = 0; i < APP_COUNT; i++) { if (seen[i] > 1) dup++; if (seen[i] == 0) miss++; }
  ck("нет дублей в browseOrder", dup, 0);
  ck("нет пропущенных", miss, 0);

  printf("\n=== Группы ===\n");
  for (uint8_t g = 0; g < GROUP_COUNT; g++) {
    printf("  %-6s:", groupNames[g]);
    for (uint8_t i = groupStart[g]; i < groupStart[g + 1]; i++)
      printf(" %s", apps[browseOrder[i]].name);
    printf("\n");
  }
  ck("границы групп возрастают", groupStart[GROUP_COUNT] == APP_COUNT, 1);
  int mono = 1;
  for (uint8_t g = 0; g < GROUP_COUNT; g++) if (groupStart[g] > groupStart[g + 1]) mono = 0;
  ck("границы не идут назад", mono, 1);

  printf("\n=== Иконки и имена целей META ===\n");
  // Пустая иконка = модуль без лица в браузере.
  int blank = 0;
  for (uint8_t i = 1; i < APP_COUNT; i++) {
    int bits = 0;
    for (int k = 0; k < 8; k++) bits += __builtin_popcount(appIcons[i][k]);
    if (bits == 0) { printf("  у %s пустая иконка\n", apps[i].name); blank++; }
  }
  ck("у всех модулей есть иконка", blank, 0);


  printf("\n=== Размеры ===\n");
  printf("  SlotCfg=%zu  патч=%zu  8 ячеек=%zu байт  PATCH_VER=%d\n",
         sizeof(SlotCfg), sizeof(Patch), sizeof(Patch) * PATCH_COUNT, PATCH_VER);
  ck("версия патча поднята после смены SlotCfg", PATCH_VER >= 2, 1);

  printf("\n=== Все модули загружаются и не молчат вхолостую ===\n");
  int silent = 0;
  for (uint8_t a = 1; a < APP_COUNT; a++) {
    reset();
    loadApp(1, a);
    cfg[1].arm = 0; cfg[1].running = true;
    if (isGenerator(cfg[1].app)) {
      g_midi.clear();
      for (int st = 0; st < 32; st++) { fireGenerator(1); serviceScheduler(); g_ms += 20; }
    } else {
      // В слот A ставим живой генератор: LOGIC берёт материал с
      // ШИНЫ, а не из capNotes, и на пустом источнике молчит законно.
      loadApp(0, (uint8_t)AppId::BASIC);
      cfg[0].running = true; cfg[0].rate = 4; cfg[0].sub4 = 4;
      cfg[0].midiCh = 0;        // источник кормит шину, но сам наружу молчит
      cfg[0].p[0] = 60; cfg[0].p[3] = 1;
      // Гоняем именно через onClockTick: LOGIC ловит тик СЕТКИ, который
      // пишется только там, и на прямом fireGenerator он молчит законно.
      cfg[1].running = true;
      g_midi.clear();
      for (int t = 0; t < 600; t++) {
        onClockTick();
        serviceSlots();
        serviceScheduler();
        g_ms += 5; g_us += 5000; state.midiTicks++;
      }
    }
    if (g_midi.empty()) { printf("  %s молчит со значениями по умолчанию\n", apps[a].name); silent++; }
  }
  printf("  немых модулей: %d\n", silent);
  ck("молчащих модулей нет", silent, 0);

  return done("STRUCTURE");
}
