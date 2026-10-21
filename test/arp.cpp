#include "common.h"

// ARPEGGIO: опора кейтрекинга, остановка по отпусканию аккорда и
// STUTT как пауза.

static void feed(uint8_t slot, uint8_t a, uint8_t b, uint8_t c, uint32_t gate) {
  SlotRt &r = rt[slot];
  r.capNotes[0] = a; r.capNotes[1] = b; r.capNotes[2] = c;
  r.capCount = 3;
  for (int i = 0; i < 3; i++) r.capVel[i] = 100;
  r.capGateMs = gate; r.capScale = SCALE_CHROMATIC; r.capRoot = 0;
}
static void arpSlot() {
  reset();
  loadApp(0, (uint8_t)AppId::ARP);
  cfg[0].arm = 0; cfg[0].arm2 = ARM_NONE;
  cfg[0].midiCh = 1; cfg[0].scale = SCALE_CHROMATIC;
  cfg[0].running = true;
  cfg[0].p[0] = 4;        // самое мелкое деление — шаги идут часто
  cfg[0].p[1] = 64;       // KTRC: следует за клавишей полностью
  cfg[0].p[2] = 0;        // OCT
  cfg[0].p[3] = 0;        // STUTT: легато
}
// Прокрутка клока с настоящим временем.
static void ticks(int n) {
  for (int i = 0; i < n; i++) { onClockTick(); serviceScheduler(); g_ms += 5; g_us += 5000; }
}

int main() {
  printf("=== Арпеджио играет, пока аккорд держат ===\n");
  arpSlot();
  feed(0, 60, 64, 67, 2000);
  g_midi.clear();
  onCapture(0);
  ticks(120);
  int longHold = (int)noteList().size();
  printf("  гейт 2000 мс, нот:         %d\n", longHold);
  ck("при долгом аккорде арпеджио идёт", longHold > 3, 1);

  printf("\n=== И останавливается, когда отпущен ===\n");
  arpSlot();
  feed(0, 60, 64, 67, 40);
  g_midi.clear();
  onCapture(0);
  ticks(120);
  int shortHold = (int)noteList().size();
  printf("  гейт 40 мс, нот:           %d\n", shortHold);
  ck("короткий аккорд не разгоняет арпеджио", shortHold < longHold, 1);
  ck("но первая нота звучит сразу", shortHold >= 1, 1);
  ck("пул погашен", rt[0].arpPoolLen, 0);

  printf("\n=== Опора это низ ПРЕДЫДУЩЕГО аккорда ===\n");
  arpSlot();
  feed(0, 60, 64, 67, 2000);
  onCapture(0);
  printf("  первый аккорд, опора:      %d\n", rt[0].arpAnchor);
  ck("первая опора это свой низ", rt[0].arpAnchor, 60);
  feed(0, 72, 76, 79, 2000);
  onCapture(0);
  printf("  второй аккорд, опора:      %d\n", rt[0].arpAnchor);
  ck("опора сдвинулась на низ первого", rt[0].arpAnchor, 60);
  feed(0, 48, 52, 55, 2000);
  onCapture(0);
  printf("  третий аккорд, опора:      %d\n", rt[0].arpAnchor);
  ck("опора обновляется от входа", rt[0].arpAnchor, 72);

  printf("\n=== KTRC=0 держит арпеджио на опоре ===\n");
  arpSlot();
  cfg[0].p[1] = 0;
  feed(0, 60, 60, 60, 2000);
  onCapture(0);                 // опора становится 60
  // Нота первого аккорда лежит в планировщике и уйдёт позже — её надо
  // выпустить ДО очистки, иначе она всплывёт первой и подменит ответ.
  serviceScheduler();
  feed(0, 84, 84, 84, 2000);    // клавиша ушла на две октавы выше
  g_midi.clear();
  onCapture(0);
  ticks(20);
  {
    auto n = noteList();
    pr("KTRC=0, что звучит:", n);
    ck("арпеджио не поехало за клавишей", n.empty() ? -1 : n[0], 60);
  }
  arpSlot();
  cfg[0].p[1] = 64;
  feed(0, 60, 60, 60, 2000);
  onCapture(0);
  serviceScheduler();
  feed(0, 84, 84, 84, 2000);
  g_midi.clear();
  onCapture(0);
  ticks(20);
  {
    auto n = noteList();
    pr("KTRC=64, что звучит:", n);
    ck("а при полном кейтрекинге поехало", n.empty() ? -1 : n[0], 84);
  }

  printf("\n=== STUTT делает паузы, а не удлиняет ноту ===\n");
  // Нота короче — значит между нотами слышна пауза. Меряем по тому,
  // сколько Note Off успевает уйти за одно и то же время.
  int offs[2] = {0, 0};
  for (int pass = 0; pass < 2; pass++) {
    arpSlot();
    cfg[0].p[3] = pass ? 127 : 0;
    feed(0, 60, 64, 67, 4000);
    g_midi.clear();
    onCapture(0);
    ticks(40);
    for (size_t i = 0; i + 2 < g_midi.size(); i += 3)
      if ((g_midi[i] & 0xF0) == 0x80) offs[pass]++;
  }
  printf("  STUTT=0 снятий:            %d\n", offs[0]);
  printf("  STUTT=127 снятий:          %d\n", offs[1]);
  ck("больше STUTT — больше пауз", offs[1] > offs[0], 1);

  printf("\n=== Мёртвой колонки CCDST у ARP больше нет ===\n");
  arpSlot();
  {
    PDesc d = columnDesc(0, true, 2);
    printf("  подстраница, колонка 2:    \"%s\"\n", d.name);
    ck("колонка пуста", d.name[0] ? 1 : 0, 0);
  }

  return done("ARP");
}
