#include "common.h"
// Чистое сравнение: только звёзды в настоящих координатах и пометка.
// Фон и связи убраны намеренно — сравниваем сам знак, а не шум вокруг.
void vOrbit(uint8_t cx, uint8_t cy, int) {
  for (int8_t dx = -1; dx <= 1; dx += 2)
    for (int8_t dy = -1; dy <= 1; dy += 2)
      display.drawPixel(cx + dx * 3, cy + dy * 3, SSD1306_WHITE);
}
void vBreath(uint8_t cx, uint8_t cy, int amt) {
  int r = 3 + abs(amt) * 2 / 64;                       // 3..5
  for (int8_t dx = -1; dx <= 1; dx += 2)
    for (int8_t dy = -1; dy <= 1; dy += 2)
      display.drawPixel(cx + dx * r, cy + dy * r, SSD1306_WHITE);
}
void vRing(uint8_t cx, uint8_t cy, int) {
  const int R = 5;
  for (int dy = -R; dy <= R; dy++) {
    int w = R - abs(dy);
    display.drawPixel(cx - w, cy + dy, SSD1306_WHITE);
    display.drawPixel(cx + w, cy + dy, SSD1306_WHITE);
  }
}
void vHalfRing(uint8_t cx, uint8_t cy, int) {          // только нижняя половина
  const int R = 5;
  for (int dy = 0; dy <= R; dy++) {
    int w = R - dy;
    display.drawPixel(cx - w, cy + dy, SSD1306_WHITE);
    display.drawPixel(cx + w, cy + dy, SSD1306_WHITE);
  }
}
void vCorners(uint8_t cx, uint8_t cy, int) {
  for (int8_t sx = -1; sx <= 1; sx += 2)
    for (int8_t sy = -1; sy <= 1; sy += 2) {
      display.drawPixel(cx + sx * 2, cy + sy * 3, SSD1306_WHITE);
      display.drawPixel(cx + sx * 3, cy + sy * 2, SSD1306_WHITE);
    }
}
void vRays(uint8_t cx, uint8_t cy, int) {
  display.drawPixel(cx, cy - 5, SSD1306_WHITE);
  display.drawPixel(cx, cy + 5, SSD1306_WHITE);
  display.drawPixel(cx - 5, cy, SSD1306_WHITE);
  display.drawPixel(cx + 5, cy, SSD1306_WHITE);
}
void vArc(uint8_t cx, uint8_t cy, int) {
  display.drawPixel(cx - 2, cy - 4, SSD1306_WHITE);
  display.drawPixel(cx - 1, cy - 5, SSD1306_WHITE);
  display.drawPixel(cx,     cy - 5, SSD1306_WHITE);
  display.drawPixel(cx + 1, cy - 5, SSD1306_WHITE);
  display.drawPixel(cx + 2, cy - 4, SSD1306_WHITE);
}
void vUnder(uint8_t cx, uint8_t cy, int amt) {         // чёрточка под звездой
  int len = 1 + abs(amt) * 3 / 64;                     // 1..4
  display.drawFastHLine((uint8_t)(cx - len), (uint8_t)(cy + 5), (uint8_t)(len * 2 + 1), SSD1306_WHITE);
}

void band() {
  for (int y = 0; y < 14; y++) {
    printf("  ");
    for (int x = 8; x < 120; x++) putchar(g_fb[y][x] ? '#' : '.');
    printf("|\n");
  }
}
void scene(const char *name, void (*mark)(uint8_t, uint8_t, int), int amt) {
  fbclear();
  // A — обычная, B — текущий слот (залита + курсор), C и D помечены.
  for (uint8_t s = 0; s < NUM_SLOTS; s++) drawStarGlyph(SLOT_CX[s], SLOT_CY[s], s == 1);
  drawStarCursor(SLOT_CX[1], SLOT_CY[1]);
  if (mark) { mark(SLOT_CX[2], SLOT_CY[2], amt); mark(SLOT_CX[3], SLOT_CY[3], amt); }
  printf("\n%s\n", name);
  band();
}
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  reset();
  printf("A(20,7)  B(48,3)=текущий  C(76,8)  D(104,4);  радиус звезды %d\n", STAR_R);
  printf("Помечены C и D. A и B — для сравнения.\n");
  scene("0. Без пометки", nullptr, 0);
  scene("A. Орбита: 4 точки по диагоналям", vOrbit, 0);
  scene("B. Дышащая орбита, глубина 64", vBreath, 64);
  scene("B. Дышащая орбита, глубина 8", vBreath, 8);
  scene("C. Дуга сверху", vArc, 0);
  scene("D. Второй контур ромбом (R=5)", vRing, 0);
  scene("E. Нижняя половина контура", vHalfRing, 0);
  scene("F. Удлинённые лучи", vRays, 0);
  scene("G. Уголки вплотную к контуру", vCorners, 0);
  scene("H. Чёрточка под звездой, длина = глубина", vUnder, 64);
  return 0;
}
