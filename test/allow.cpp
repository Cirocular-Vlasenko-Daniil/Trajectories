#include "common.h"

// Карта modAllow обязана совпадать с MODULATION.md — таблицей, которую
// проставил пользователь. Строки ниже сгенерированы из того же файла:
// если карта в прошивке разойдётся с таблицей, тест это покажет.
// Сверка идёт ПО ИМЕНИ колонки, а не по её номеру: у части модулей
// номера сдвинулись (у ARP ушла мёртвая колонка CCDST).
struct Row { AppId app; bool sub; bool allowed; const char* name; };
const Row table[] = {
#include "allow_table.inc"
};

int main() {
  int checked = 0, missing = 0;
  for (auto &r : table) {
    reset();
    loadApp(0, (uint8_t)r.app);
    // Ищем колонку с этим именем на той же странице.
    int col = -1;
    for (uint8_t i = 0; i < NUM_COLS; i++) {
      PDesc d = columnDesc(0, r.sub, i);
      if (d.name[0] && strcmp(d.name, r.name) == 0) { col = i; break; }
    }
    if (col < 0) {
      printf("  нет такой колонки: %-11s %s %s\n",
             apps[(uint8_t)r.app].name, r.sub ? "подстр." : "главная", r.name);
      missing++;
      continue;
    }
    checked++;
    bool got = modAllowed((uint8_t)r.app, r.sub, (uint8_t)col);
    if (got != r.allowed)
      printf("FAIL %-11s %s %-7s: прошивка %s, таблица %s\n",
             apps[(uint8_t)r.app].name, r.sub ? "подстр." : "главная", r.name,
             got ? "можно" : "нельзя", r.allowed ? "можно" : "нельзя");
    ck("карта совпадает с таблицей", got ? 1 : 0, r.allowed ? 1 : 0);
  }
  printf("  сверено колонок:           %d\n", checked);
  printf("  не найдено в прошивке:     %d\n", missing);
  ck("все строки таблицы найдены", missing, 0);

  printf("\n=== Правила, вытекающие из таблицы ===\n");
  // Эти имена запрещены всюду, где встречаются. Проверяем по всем
  // приложениям, а не по строкам таблицы: новый модуль не должен
  // случайно открыть их заново.
  const char* never[] = {"CH", "ARM", "CCDST", "STEP+", "SCALE", "MODE"};
  int opened = 0;
  for (uint8_t app = 1; app < APP_COUNT; app++) {
    reset();
    loadApp(0, app);
    for (uint8_t sub = 0; sub < 2; sub++)
      for (uint8_t i = 0; i < NUM_COLS; i++) {
        PDesc d = columnDesc(0, sub != 0, i);
        if (!d.name[0]) continue;
        for (auto n : never)
          if (strcmp(d.name, n) == 0 && modAllowed(app, sub != 0, i)) {
            printf("FAIL %s: %s открыт\n", apps[app].name, n);
            opened++;
          }
      }
  }
  ck("перекидывающие цепочку закрыты всюду", opened, 0);

  // Колонки без имени разрешать нечего: в карте это означало бы, что
  // бит выставлен не туда, куда задумано.
  int ghosts = 0;
  for (uint8_t app = 1; app < APP_COUNT; app++) {
    reset();
    loadApp(0, app);
    for (uint8_t sub = 0; sub < 2; sub++)
      for (uint8_t i = 0; i < NUM_COLS; i++) {
        PDesc d = columnDesc(0, sub != 0, i);
        if (!d.name[0] && modAllowed(app, sub != 0, i)) {
          printf("FAIL %s: разрешена пустая колонка %s %d\n",
                 apps[app].name, sub ? "подстр." : "главная", i);
          ghosts++;
        }
      }
  }
  ck("пустых колонок в карте нет", ghosts, 0);

  // LOGIC и MULTI OUT закрыты целиком.
  for (auto app : {AppId::LOGIC, AppId::MULTX}) {
    int open = 0;
    for (uint8_t sub = 0; sub < 2; sub++)
      for (uint8_t i = 0; i < NUM_COLS; i++)
        if (modAllowed((uint8_t)app, sub != 0, i)) open++;
    printf("  %-11s открытых колонок: %d\n", apps[(uint8_t)app].name, open);
    ck("модуль закрыт целиком", open, 0);
  }

  return done("ALLOW");
}
