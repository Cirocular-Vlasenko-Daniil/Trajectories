#!/bin/sh
# Прогон тестов на хосте. Падение СБОРКИ — это провал: раньше скрипт
# запускал бинарник, оставшийся от прошлого раза, и печатал PASS по
# устаревшему коду.
cd "$(dirname "$0")" || exit 1
out=$(mktemp -d) || exit 1
trap 'rm -rf "$out"' EXIT
# Без аргументов гоняем ВСЕ наборы: забытый набор — это набор, который
# не мешает сломать код.
if [ $# -eq 0 ]; then
  # starmark это не тест, а раскладка вариантов отрисовки для глаза:
  # у неё нет и не должно быть PASS. Гоняется вручную по имени.
  set -- $(ls *.cpp | grep -vE '^(host|starmark)\.cpp$' | sed 's/\.cpp$//')
fi
fail=0
for t in "$@"; do
  if ! g++ -std=gnu++17 -w -Istub "$t.cpp" host.cpp -o "$out/$t" 2>"$out/err"; then
    printf '%-12s СБОРКА УПАЛА\n' "$t"
    head -8 "$out/err"
    fail=1
    continue
  fi
  res=$("$out/$t" 2>&1 | tail -1)
  printf '%-12s %s\n' "$t" "$res"
  case "$res" in *PASS*) ;; *) fail=1 ;; esac
done
[ $fail -eq 0 ] && echo "— всё зелёное" || echo "— ЕСТЬ ПРОВАЛЫ"
exit $fail
