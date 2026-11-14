// tools/test_config.cpp — host-тест CFG-01/02/04/06/07 (без Switch).
//
// Проверяет файловые хелперы ui/config_io.hpp:
//   * атомарную запись и восстановление из .bak;
//   * экранирование полей истории (таб/перевод строки/слэш/UTF-8);
//   * строку ini без переводов строк, редакцию пароля прокси;
//   * проверку каталога на запись.
//
// Сборка/запуск:
//   g++ -std=c++17 -O2 -Isource -Wall tools/test_config.cpp -o /tmp/test_config
//   /tmp/test_config

#include "ui/config_io.hpp"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace tnx::app::configio;

static int checks = 0, fails = 0;

static void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        fails++;
        printf("   FAIL: %s\n", what.c_str());
    }
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    const std::string dir = "/tmp/tubenx-cfg-test";
    const std::string base = dir + "/config.ini";
    remove(base.c_str());
    remove((base + ".bak").c_str());
    remove((base + ".tmp").c_str());
    mkdir(dir.c_str(), 0777);

    // --- CFG-01: атомарная запись ---
    check(writeAtomic(base.c_str(), "version=2\nquality=1\n"), "CFG-01: первая запись");
    std::string text;
    check(readWholeFile(base.c_str(), text) && contains(text, "quality=1"),
          "CFG-01: файл прочитан после записи");
    check(!readWholeFile((base + ".tmp").c_str(), text), "CFG-01: .tmp удалён после rename");

    check(writeAtomic(base.c_str(), "version=2\nquality=2\n"), "CFG-01: перезапись");
    std::string bak;
    check(readWholeFile((base + ".bak").c_str(), bak) && contains(bak, "quality=1"),
          "CFG-01: .bak хранит предыдущую версию");
    check(readWholeFile(base.c_str(), text) && contains(text, "quality=2"),
          "CFG-01: основной файл — новая версия");

    // Имитация обрыва: основной файл испорчен, .bak цел.
    FILE* f = fopen(base.c_str(), "wb");
    if (f) {
        fputs("garbage-without-equals", f);
        fclose(f);
    }
    std::string out;
    const bool fromBak = readWithBak(base.c_str(), out, '=');
    check(fromBak && contains(out, "quality=1"), "CFG-01: битый основной → восстановление из .bak");

    // Оставшийся .tmp не должен подменять данные.
    f = fopen((base + ".tmp").c_str(), "wb");
    if (f) {
        fputs("partial-write", f);
        fclose(f);
    }
    out.clear();
    readWithBak(base.c_str(), out, '=');
    check(!contains(out, "partial-write"), "CFG-01: .tmp от прерванной записи игнорируется");
    remove((base + ".tmp").c_str());

    // Нет .bak и битый основной — пустой результат, без падения.
    remove((base + ".bak").c_str());
    out.clear();
    check(!readWithBak(base.c_str(), out, '=') && out.empty(),
          "CFG-01: без .bak битый файл даёт пустые настройки");

    // --- CFG-04: экранирование истории ---
    const std::string ugly = "Таб\tздесь\nи слэш \\ и CR\r";
    const std::string esc = escapeField(ugly);
    check(unescapeField(esc) == ugly, "CFG-04: tab/nl/backslash/CR переживают roundtrip");
    check(esc.find('\t') == std::string::npos && esc.find('\n') == std::string::npos &&
              esc.find('\r') == std::string::npos,
          "CFG-04: в экранированной строке нет сырых разделителей");
    const std::string emoji = "Видео 🎬 100% — обзор";
    check(unescapeField(escapeField(emoji)) == emoji, "CFG-04: UTF-8 и эмодзи сохраняются");

    // Строка истории v2 не рвётся на 7 полей.
    std::string line = escapeField("id123") + "\t" + escapeField(ugly) + "\t" + escapeField(emoji) +
                       "\t12.5\t300.0\t1700000000\t1\n";
    size_t tabs = 0;
    for (char c : line)
        if (c == '\t') tabs++;
    check(tabs == 6, "CFG-04: запись истории содержит ровно 6 разделителей");

    // --- CFG-02: ini без переводов строк ---
    check(oneLine("a\nb\rc") == "a b c", "CFG-02: значение ini не содержит переводов строк");

    // --- CFG-07: редакция прокси ---
    check(redactUserInfo("http://user:pass@host:8080") == "http://***@host:8080",
          "CFG-07: пароль прокси скрыт в диагностике");
    check(redactUserInfo("socks5h://host:1080") == "socks5h://host:1080",
          "CFG-07: прокси без userinfo не меняется");

    // --- CFG-06: проверка каталога на запись ---
    check(probeWritable(dir.c_str()), "CFG-06: каталог доступен на запись");
    check(!probeWritable("/proc/tubenx-no-such-dir"), "CFG-06: недоступный каталог определяется");

    remove(base.c_str());
    remove((base + ".bak").c_str());
    rmdir(dir.c_str());

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks,
           fails);
    return fails ? 1 : 0;
}