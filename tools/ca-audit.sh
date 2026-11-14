#!/usr/bin/env bash
# ca-audit.sh — аудит CA-бандлов TubeNX (NET-15).
#
# Зачем: в romfs лежат два бандла — resources/cacert.pem (148 корней Mozilla,
# 222 КБ, fallback) и resources/cacert-trim.pem (13 корней, используется по
# умолчанию). Нужно видеть, какие корни реально нужны Google/YouTube, когда
# истекают и проходят ли живые хосты проверку именно урезанным бандлом.
#
# Использование:
#   tools/ca-audit.sh                 # список корней + сроки (+ предупреждения)
#   tools/ca-audit.sh --check         # ещё и curl --cacert по 5 хостам
#   tools/ca-audit.sh --check host... # проверить свои хосты
#   tools/ca-audit.sh --update        # обновить бандлы из Mozilla (curl.se/ca)
#
# Коды выхода: 0 — всё в порядке; 1 — есть истёкший/скоро истекающий корень
# или хост не прошёл TLS; 2 — нет openssl/curl/файла бандла.

set -u
cd "$(dirname "$0")/.." || exit 2

TRIM="resources/cacert-trim.pem"
FULL="resources/cacert.pem"
CA_URL="https://curl.se/ca/cacert.pem"
WARN_DAYS=90

# Хосты, которые TubeNX реально использует. googlevideo сюда не входит: ссылка
# на поток подписана и живёт минуты, статического URL для проверки нет —
# его покрывает тот же GTS/Amazon корень, что и остальные хосты Google.
HOSTS=(
  "https://www.youtube.com/"
  "https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"
  "https://suggestqueries-clients6.youtube.com/complete/search?client=firefox&q=test"
  "https://www.gstatic.com/generate_204"
  "https://accounts.google.com/"
)

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "нет $1 в PATH" >&2; exit 2; }
}

# Печатает "CN|notAfter|sha256|файл" для каждого сертификата в бандле.
certs_of() {
  local file="$1"
  [ -f "$file" ] || return 1
  awk -v f="$file" '
    /BEGIN CERTIFICATE/ {n++; buf=""; in_cert=1}
    in_cert {buf = buf $0 "\n"}
    /END CERTIFICATE/ {
      if (in_cert) {print buf > ("/tmp/tnx-ca-audit-" n ".pem"); print n " " f}
      in_cert=0
    }' "$file" | while read -r n f; do
      local tmp="/tmp/tnx-ca-audit-$n.pem"
      local subj end fp
      subj=$(openssl x509 -in "$tmp" -noout -subject 2>/dev/null | sed 's/^subject=//')
      end=$(openssl x509 -in "$tmp" -noout -enddate 2>/dev/null | sed 's/^notAfter=//')
      fp=$(openssl x509 -in "$tmp" -noout -fingerprint -sha256 2>/dev/null |
           sed 's/^.*Fingerprint=//' | cut -c1-16)
      rm -f "$tmp"
      printf '%s|%s|%s|%s\n' "$subj" "$end" "$fp" "$f"
    done
}

# Секунды до notAfter (пусто, если дата не разобралась).
seconds_left() {
  local end="$1"
  local ts
  ts=$(date -d "$end" +%s 2>/dev/null) || { echo ""; return; }
  echo $((ts - $(date +%s)))
}

audit_bundle() {
  local file="$1"
  local label="$2"
  # Третий аргумент: полный бандл Mozilla исторически хранит истёкшие корни
  # (OpenSSL их не принимает), поэтому для него истечение — не ошибка.
  local history_ok="${3:-0}"
  local total expired soon=0 worst=""
  total=$(grep -c "BEGIN CERTIFICATE" "$file" 2>/dev/null || echo 0)
  echo "== $label: $file — $total корн(ей) =="
  if [ "$total" -eq 0 ]; then
    echo "  ФАЙЛ ОТСУТСТВУЕТ ИЛИ ПУСТ"
    return 1
  fi
  local bad=0
  while IFS='|' read -r subj end fp _f; do
    local left days
    left=$(seconds_left "$end")
    if [ -n "$left" ]; then
      days=$((left / 86400))
      if [ "$days" -lt 0 ]; then
        if [ "$history_ok" = "1" ]; then
          printf '  [истёк, история] %5d дн.  %-64s  %s\n' "$days" "$subj" "$fp"
        else
          printf '  [!] ИСТЁК  %5d дн.  %-64s  %s\n' "$days" "$subj" "$fp"
          bad=$((bad + 1))
        fi
      elif [ "$days" -lt "$WARN_DAYS" ]; then
        printf '  [~] скоро  %5d дн.  %-64s  %s\n' "$days" "$subj" "$fp"
        soon=$((soon + 1))
      else
        printf '  [ok]      %5d дн.  %-64s  %s\n' "$days" "$subj" "$fp"
      fi
      if [ -z "$worst" ] || { [ -n "$left" ] && [ "$left" -lt "$worst" ]; }; then
        worst="$left"
      fi
    else
      printf '  [??] дата не разобрана  %-64s  %s\n' "$subj" "$fp"
    fi
  done < <(certs_of "$file")
  echo
  if [ "$bad" -gt 0 ]; then
    echo "  ИТОГ: $bad истёкших, $soon истекают < $WARN_DAYS дн. — обновите бандл (--update)"
    return 1
  fi
  return 0
}

check_hosts() {
  local bundle="$1" label="$2"
  shift 2
  echo "== curl --cacert $bundle ($label) =="
  # 000 означает, что TLS/соединение не состоялось; любой ответ сервера
  # (2xx/3xx/404 и т.п.) доказывает, что цепочка сертификата принята.
  local ok=0 fail=0
  for h in "$@"; do
    local code
    code=$(curl -sS -o /dev/null --max-time 12 --cacert "$bundle" \
                -w '%{http_code}' "$h" 2>/dev/null)
    if [ "$code" = "000" ]; then
      printf '  [FAIL] %-72s TLS/сеть не прошли\n' "$h"
      fail=$((fail + 1))
    else
      printf '  [ok]   %-72s HTTP %s\n' "$h" "$code"
      ok=$((ok + 1))
    fi
  done
  echo "  ИТОГ: $ok ок, $fail ошибок"
  [ "$fail" -eq 0 ]
}

update_bundles() {
  need curl
  need openssl
  echo "Скачиваю $CA_URL ..."
  curl -sSfL --max-time 60 -o "$FULL.tmp" "$CA_URL" || {
    echo "не удалось скачать бандл" >&2
    rm -f "$FULL.tmp"
    return 1
  }
  mv "$FULL.tmp" "$FULL"
  echo "resources/cacert.pem обновлён: $(grep -c 'BEGIN CERTIFICATE' "$FULL") корней"

  # Урезанный бандл пересобираем по CN, которые уже отобраны: так обновление
  # не притащит обратно «лишние» корни, но и не потеряет нужные Google.
  local cnlist
  cnlist=$(certs_of "$TRIM" | sed -n 's/.*CN=\([^,|]*\).*/\1/p' | sort -u)
  if [ -z "$cnlist" ]; then
    echo "не удалось прочитать CN из $TRIM — trim не обновлён" >&2
    return 1
  fi
  local out="$TRIM.tmp" found=0 total=0
  : > "$out"
  local n=0
  awk '/BEGIN CERTIFICATE/{n++} {print > ("/tmp/tnx-ca-full-" n ".pem")}' "$FULL"
  for f in /tmp/tnx-ca-full-*.pem; do
    [ -f "$f" ] || continue
    local cn
    cn=$(openssl x509 -in "$f" -noout -subject 2>/dev/null | sed -n 's/.*CN=\([^,]*\).*/\1/p')
    total=$((total + 1))
    if printf '%s\n' "$cnlist" | grep -qxF "$cn"; then
      cat "$f" >> "$out"
      echo >> "$out"
      found=$((found + 1))
    fi
    rm -f "$f"
  done
  if [ "$found" -eq 0 ]; then
    echo "ни один CN из trim не найден в новом бандле — проверьте вручную!" >&2
    rm -f "$out"
    return 1
  fi
  mv "$out" "$TRIM"
  echo "resources/cacert-trim.pem пересобран: $found из $(printf '%s\n' "$cnlist" | wc -l) CN (просмотрено $total)"
  echo "Проверьте вывод --check: если хост перестал проходить, добавьте его корень."
}

main() {
  need openssl
  local do_check=0
  local hosts=("${HOSTS[@]}")
  case "${1:-}" in
    --update) update_bundles; exit $? ;;
    --check) do_check=1; shift ;;
    --list) ;;
    "") ;;
    *) echo "неизвестный аргумент: $1" >&2; exit 2 ;;
  esac
  [ $# -gt 0 ] && hosts=("$@")

  local status=0
  audit_bundle "$TRIM" "урезанный (по умолчанию)" || status=1
  audit_bundle "$FULL" "полный (fallback)" 1 || status=1

  if [ "$do_check" -eq 1 ]; then
    need curl
    check_hosts "$TRIM" "урезанный" "${hosts[@]}" || status=1
  fi

  echo
  if [ "$status" -eq 0 ]; then
    echo "ИТОГ: бандлы в порядке."
  else
    echo "ИТОГ: есть замечания (см. выше)."
  fi
  return "$status"
}

main "$@"