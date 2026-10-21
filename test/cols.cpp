#include "common.h"

// Колонка не имеет права показывать не то, что редактирует энкодер.
// Проверка идёт по НАРИСОВАННОМУ тексту: описатель может быть верным,
// а ветка отрисовки — перехватывать колонку и рисовать поверх чужое.

// Весь текст, попавший в полосу колонки col.
static std::string colText(uint8_t col) {
  std::string out;
  int x0 = col * 32;
  for (int y = 0; y < 32; y++)
    for (int x = x0; x < x0 + Display::COL_W; x++) {
      char ch = g_tx[y][x];
      if (ch && ch != '-' && ch != '_') out += ch;
    }
  return out;
}
static bool has(const std::string &s, const char *w) {
  return s.find(w) != std::string::npos;
}

int main() {
  printf("=== Подстраница TOPOLOGY, колонка 2 ===\n");
  reset();
  loadApp(0, (uint8_t)AppId::TOPOLOGY);
  state.currentSlot = 0; state.subPage = true; state.selectedCol = 2;
  state.modLayer = false;
  PDesc d = columnDesc(0, true, 2);
  printf("  описатель:                 \"%s\" %d..%d\n", d.name, d.lo, d.hi);
  ck("описатель это MODE", strcmp(d.name, "MODE") == 0 ? 1 : 0, 1);

  for (int m = 0; m <= 1; m++) {
    cfg[0].sub3 = m;
    fbclear(); drawSlotPage();
    std::string t = colText(2);
    printf("  sub3=%d на экране:          \"%s\"\n", m, t.c_str());
    ck("подпись колонки видна", has(t, "MODE") ? 1 : 0, 1);
    ck("значение режима видно", has(t, m ? "3CH" : "1CH") ? 1 : 0, 1);
    // Лад в этой колонке рисовался поверх MODE и не менялся никогда.
    ck("лада в колонке нет", has(t, "MAJ") ? 1 : 0, 0);
  }

  printf("\n=== У остальных генераторов там по-прежнему лад ===\n");
  for (auto app : {AppId::BASIC, AppId::SKELETON, AppId::EUCLIDIAN}) {
    reset();
    loadApp(0, (uint8_t)app);
    cfg[0].scale = 0; cfg[0].root = 0;
    state.currentSlot = 0; state.subPage = true; state.selectedCol = 2;
    fbclear(); drawSlotPage();
    std::string t = colText(2);
    printf("  %-10s колонка 2:      \"%s\"\n", apps[(uint8_t)app].name, t.c_str());
    ck("лад на месте", has(t, "MAJ") ? 1 : 0, 1);
  }

  return done("COLS");
}
