// tools/test_strings.cpp — host-тест UX-13 (склонения) и UX-15 (человеческие
// ошибки). Заголовочный модуль, линковки не требует.
#include "ui/strings.hpp"

#include <cstdio>
#include <string>

using namespace tnx::app;

static int checks = 0, fails = 0;
static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("   FAIL: %s\n", what);
    }
}

int main() {
    // UX-13: склонения 1 / 2..4 / 5.., 11..14 — «много».
    check(plural(1, "видео", "видео", "видео") == "1 видео", "plural 1");
    check(plural(2, "минута", "минуты", "минут") == "2 минуты", "plural 2");
    check(plural(5, "минута", "минуты", "минут") == "5 минут", "plural 5");
    check(plural(11, "минута", "минуты", "минут") == "11 минут", "plural 11");
    check(plural(21, "минута", "минуты", "минут") == "21 минута", "plural 21");
    check(plural(22, "минута", "минуты", "минут") == "22 минуты", "plural 22");
    check(plural(0, "видео", "видео", "видео") == "0 видео", "plural 0");

    // UX-15: технические коды → человеческий текст.
    check(humanError("curl 28 operation timed out").find("не отвечает") != std::string::npos,
          "curl 28 → не отвечает");
    check(humanError("curl 6 Could not resolve host").find("Нет связи") != std::string::npos,
          "curl 6 → нет связи");
    check(humanError("HTTP 400").find("400") != std::string::npos, "HTTP 400 → упомянут 400");
    check(humanError("HTTP 503").find("временно") != std::string::npos, "HTTP 5xx → временно");
    check(humanError("no upstream headers").find("Ретранслятор") != std::string::npos,
          "upstream → ретранслятор");
    check(humanError("403 Forbidden").find("запрещён") != std::string::npos, "403 → запрещён");
    check(humanError("410 Gone").find("устарела") != std::string::npos, "410 → устарела");
    // URL с токенами не показываем.
    const std::string url = "GET https://rr3---sn-x.googlevideo.com/videoplayback?token=SECRET";
    check(humanError(url).find("SECRET") == std::string::npos, "URL с токеном не утекает в UI");
    // Уже понятные сообщения не портим.
    check(humanError("Сеть недоступна. Проверьте обход DPI.") == "Сеть недоступна. Проверьте обход DPI.",
          "человеческое сообщение сохраняется");
    check(humanError("Видео недоступно для воспроизведения") == "Видео недоступно для воспроизведения",
          "сообщение плеера сохраняется");
    check(humanError("") == "", "пустое остаётся пустым");

    printf("\n== %s: %d проверок, %d провалов ==\n", fails ? "ЕСТЬ ОШИБКИ" : "ЗЕЛЕНО", checks, fails);
    return fails ? 1 : 0;
}
