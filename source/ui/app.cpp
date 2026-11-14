// ui/app.cpp — состояние, навигация, фоновые запросы, конфиг.
#include "core/threads.hpp"
#include "ui/app.hpp"

#include "core/canvas.hpp"
#include "dpi/dpi.hpp"
#include "net/http.hpp"
#include "platform.hpp"
#include "ui/config_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tnx::app {

static const char* kConfigPath = "sdmc:/config/TubeNX/config.ini";
static const char* kHistoryPath = "sdmc:/config/TubeNX/history.txt";
static const char* kConfigDir = "sdmc:/config/TubeNX";

namespace {

// CFG-01/04: атомарная запись, восстановление из .bak и экранирование —
// в ui/config_io.hpp, чтобы их можно было покрыть host-тестом.
using namespace configio;

} // namespace

float nowSeconds() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - t0).count() / 1e6f;
}

// Только схема и хост: в query у googlevideo лежат подписанные токены,
// их нельзя светить в логе и они бесполезны для диагностики.
static std::string urlHost(const std::string& u) {
    size_t p = u.find("://");
    size_t s = (p == std::string::npos) ? 0 : p + 3;
    size_t e = u.find_first_of("/?#", s);
    return u.substr(0, e == std::string::npos ? u.size() : e);
}


// ---------------------------------------------------------------- конфиг
void App::loadConfig() {
    // CFG-01: если основной файл пуст/не открылся, пробуем .bak — питание могли
    // выключить в момент записи.
    std::string text;
    const bool fromBak = readWithBak(kConfigPath, text, '=');
    const bool hadFile = !text.empty();
    bool sawFirstRun = false;   // UX-14: был ли ключ в существующем конфиге
    int version = 1;            // CFG-02: старые конфиги без ключа — v1
    extraConfig.clear();

    // CFG-03: мягкая валидация. Битое значение не ломает UI: чиним, пишем в лог.
    auto clampInt = [](const char* key, int& v, int lo, int hi) {
        if (v < lo || v > hi) {
            const int old = v;
            v = std::clamp(v, lo, hi);
            platform::bootLog("config: %s=%d вне [%d..%d] — исправлено на %d", key, old, lo, hi, v);
        }
    };

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "version") {
            version = atoi(v.c_str());
        } else if (k == "dark") {
            cfg.dark = atoi(v.c_str()) != 0;
        } else if (k == "seed") {
            char* end = nullptr;
            errno = 0;
            const unsigned long x = strtoul(v.c_str(), &end, 16);
            if (errno != 0 || end == v.c_str() || *end != 0 || v.size() > 8) {
                platform::bootLog("config: seed='%s' не hex — оставлен %08X", v.c_str(), cfg.seed);
            } else {
                cfg.seed = (uint32_t)x | 0xFF000000u;   // альфа всегда FF
            }
        } else if (k == "quality") {
            cfg.quality = atoi(v.c_str());
        } else if (k == "speed") {
            cfg.speed = atoi(v.c_str());
        } else if (k == "adelay") {
            cfg.audioDelayMs = atoi(v.c_str());
        } else if (k == "dpi") {
            cfg.dpiEnabled = atoi(v.c_str()) != 0;
        } else if (k == "dpi_mode") {
            cfg.dpiMode = atoi(v.c_str());
        } else if (k == "visitor") {
            cfg.visitor = v;
        } else if (k == "visitor_saved") {
            cfg.visitorSavedAt = atoll(v.c_str());
        } else if (k == "autoplay") {
            cfg.autoplay = atoi(v.c_str()) != 0;
        } else if (k == "reco") {
            cfg.localReco = atoi(v.c_str()) != 0;
        } else if (k == "first_run") {
            cfg.firstRun = atoi(v.c_str()) != 0;
            sawFirstRun = true;
        } else if (k == "chip") {
            cfg.homeChip = atoi(v.c_str());
        } else if (k == "proxy") {
            cfg.proxy = oneLine(v);
        } else {
            // CFG-02: чужие/новые ключи не выбрасываем — сохраним обратно как есть.
            extraConfig.emplace_back(k, oneLine(v));
        }
    }

    // CFG-03: допустимые диапазоны.
    clampInt("quality", cfg.quality, 0, 3);
    clampInt("speed", cfg.speed, 50, 200);
    clampInt("adelay", cfg.audioDelayMs, -2000, 2000);
    // Режимы DPI: 0 Off, 1 Split, 2 MultiSplit, 3 SplitDisorder, 4 TlsRecordSplit.
    clampInt("dpi_mode", cfg.dpiMode, 0, 4);
    clampInt("chip", cfg.homeChip, 0, 99);
    if (cfg.visitorSavedAt < 0) cfg.visitorSavedAt = 0;

    // Прокси из файла тоже проверяем (мог остаться от старой версии/правки руками).
    if (!cfg.proxy.empty()) {
        std::string norm, perr;
        if (!validateProxy(cfg.proxy, norm, perr)) {
            platform::bootLog("config: прокси '%s' отброшен (%s)", cfg.proxy.c_str(), perr.c_str());
            cfg.proxy.clear();
        } else {
            cfg.proxy = norm;
        }
    }

    // CFG-02: миграция v1 → v2. Отдельных переименований нет: все ключи v1
    // читаются как есть, при следующем сохранении появится version=2.
    if (version < 2)
        platform::bootLog("config: миграция v%d → v2 (ключей сохранено: %d)", version,
                          (int)extraConfig.size());
    else if (fromBak)
        platform::bootLog("config: восстановлен из .bak");

    // UX-14: старый конфиг без ключа first_run — не первый запуск. Онбординг
    // показываем только на чистой установке (файла ещё нет).
    if (hadFile && !sawFirstRun) cfg.firstRun = false;

    // ---- история (CFG-04: v2 с экранированием, терпимость к v1) ----
    std::string htext;
    const bool historyFromBak = readWithBak(kHistoryPath, htext, 0);
    if (historyFromBak) platform::bootLog("history: восстановлена из .bak");

    bool v2 = false;
    size_t hpos = 0;
    bool firstLine = true;
    while (hpos < htext.size()) {
        size_t eol = htext.find('\n', hpos);
        std::string line = htext.substr(hpos, eol == std::string::npos ? std::string::npos : eol - hpos);
        hpos = (eol == std::string::npos) ? htext.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (firstLine) {
            firstLine = false;
            if (line.rfind("#tubenx-history", 0) == 0) {
                v2 = true;
                continue;
            }
        }
        HistoryEntry h;
        if (v2) {
            // id \t title \t channel \t time \t duration \t watchedAt \t liked
            std::vector<std::string> f;
            size_t s = 0;
            for (;;) {
                const size_t t = line.find('\t', s);
                if (t == std::string::npos) {
                    f.push_back(line.substr(s));
                    break;
                }
                f.push_back(line.substr(s, t - s));
                s = t + 1;
            }
            if (f.size() < 3) continue;
            h.id = unescapeField(f[0]);
            h.title = unescapeField(f[1]);
            h.channel = unescapeField(f[2]);
            if (f.size() > 3) h.time = strtod(f[3].c_str(), nullptr);
            if (f.size() > 4) h.duration = strtod(f[4].c_str(), nullptr);
            if (f.size() > 5) h.watchedAt = strtod(f[5].c_str(), nullptr);
            if (f.size() > 6) h.liked = atoi(f[6].c_str()) != 0;
        } else {
            // v1: сырой TSV без экранирования.
            char id[64] = {0}, title[512] = {0}, ch[256] = {0};
            double t = 0, d = 0, at = 0;
            int liked = 0;
            int n = sscanf(line.c_str(), "%63s\t%511[^\t]\t%255[^\t]\t%lf\t%lf\t%lf\t%d", id, title,
                           ch, &t, &d, &at, &liked);
            if (n < 3) continue;
            h.id = id; h.title = title; h.channel = ch; h.time = t; h.duration = d;
            h.watchedAt = at; h.liked = liked != 0;
        }
        if (h.id.empty()) continue;
        // CFG-04: дедуп по id — остаётся более свежая запись.
        bool merged = false;
        for (auto& old : history) {
            if (old.id != h.id) continue;
            if (h.watchedAt >= old.watchedAt) old = h;
            merged = true;
            break;
        }
        if (!merged) history.push_back(h);
    }

    // CFG-04: сортировка по времени просмотра и потолок 100 записей.
    std::stable_sort(history.begin(), history.end(),
                     [](const HistoryEntry& a, const HistoryEntry& b) {
                         return a.watchedAt > b.watchedAt;
                     });
    if (history.size() > 100) history.resize(100);

    // UX-14: есть история — пользователь уже пользовался приложением,
    // онбординг не показываем.
    if (!history.empty()) cfg.firstRun = false;
}

void App::saveConfig() {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);

    // CFG-02: схема v2. Неизвестные ключи (from extraConfig) пишем обратно,
    // чтобы обновление приложения не стирало чужие настройки.
    std::string out = "version=2\n";
    char buf[256];
    auto addInt = [&](const char* k, int v) {
        snprintf(buf, sizeof(buf), "%s=%d\n", k, v);
        out += buf;
    };
    addInt("dark", cfg.dark ? 1 : 0);
    snprintf(buf, sizeof(buf), "seed=%08X\n", cfg.seed);
    out += buf;
    addInt("quality", cfg.quality);
    addInt("speed", cfg.speed);
    addInt("adelay", cfg.audioDelayMs);
    addInt("dpi", cfg.dpiEnabled ? 1 : 0);
    addInt("dpi_mode", cfg.dpiMode);
    // YT-02: visitorData — единственная строка с произвольным содержимым;
    // убираем переводы строк, чтобы не сломать формат ini.
    if (!cfg.visitor.empty()) {
        out += "visitor=" + oneLine(cfg.visitor) + "\n";
        snprintf(buf, sizeof(buf), "visitor_saved=%lld\n", cfg.visitorSavedAt);
        out += buf;
    }
    addInt("autoplay", cfg.autoplay ? 1 : 0);
    addInt("reco", cfg.localReco ? 1 : 0);
    addInt("first_run", cfg.firstRun ? 1 : 0);
    addInt("chip", cfg.homeChip);
    if (!cfg.proxy.empty()) out += "proxy=" + oneLine(cfg.proxy) + "\n";
    for (const auto& kv : extraConfig) out += kv.first + "=" + oneLine(kv.second) + "\n";

    // CFG-01: атомарная запись. CFG-06: если SD недоступна на запись,
    // запоминаем это и показываем предупреждение в настройках.
    if (!writeAtomic(kConfigPath, out)) {
        if (!storageReadOnly) {
            storageReadOnly = true;
            platform::bootLog("config: SD доступна только для чтения — настройки не сохранены");
        }
        return;
    }
    if (storageReadOnly) {
        storageReadOnly = false;
        platform::bootLog("config: запись на SD восстановлена");
    }
    saveHistory();
}

// UX-10: только история (без ini). Пишется раз в 5 с во время просмотра и на
// выходе — ini при этом не трогаем.
void App::saveHistory() {
    mkdir("sdmc:/config", 0777);
    mkdir(kConfigDir, 0777);

    // CFG-04: сортировка по свежести и потолок 100 записей.
    std::stable_sort(history.begin(), history.end(),
                     [](const HistoryEntry& a, const HistoryEntry& b) {
                         return a.watchedAt > b.watchedAt;
                     });
    if (history.size() > 100) history.resize(100);

    std::string out = "#tubenx-history v2\n";
    char nums[96];
    for (const auto& h : history) {
        snprintf(nums, sizeof(nums), "\t%.1f\t%.1f\t%.0f\t%d\n", h.time, h.duration, h.watchedAt,
                 h.liked ? 1 : 0);
        out += escapeField(h.id) + "\t" + escapeField(h.title) + "\t" + escapeField(h.channel) + nums;
    }
    if (!writeAtomic(kHistoryPath, out)) {
        if (!storageReadOnly) {
            storageReadOnly = true;
            platform::bootLog("history: SD доступна только для чтения — история не сохранена");
        }
    }
}

// UX-10: удалить одну запись истории (с сохранением на диск и переранжировкой
// ленты — иначе удалённый ролик останется в «Продолжить»/рекомендациях).
void App::removeHistory(const std::string& id) {
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].id != id) continue;
        history.erase(history.begin() + (long)i);
        saveConfig();
        rankFeed();
        toast("Убрано из истории");
        platform::bootLog("UX-10: запись истории удалена id=%s", id.c_str());
        return;
    }
}

void App::applyTheme() { theme = md3::Theme::make(cfg.seed, cfg.dark); splashBgReady = false; }

// CFG-07: сброс настроек к значениям по умолчанию. История просмотров и
// локальные рекомендации не трогаются — их очищают отдельной кнопкой.
void App::resetSettings() {
    cfg = Config{};
    cfg.firstRun = false;   // пройденный онбординг не показываем снова
    extraConfig.clear();
    applyTheme();

    // Транспорт возвращаем к дефолту: обход DPI включён, режим Split.
    dpi::setMode((dpi::Mode)cfg.dpiMode);
    dpi::config().enabled = cfg.dpiEnabled;
    if (cfg.dpiEnabled && dpi::socksPort() <= 0) dpi::startSocks(1080);
    if (dpi::relayPort() <= 0) dpi::startRelay(8899);
    if (cfg.dpiEnabled && dpi::socksPort() > 0)
        net::setProxy("socks5h://127.0.0.1:" + std::to_string(dpi::socksPort()));
    else
        net::setProxy("");

    saveConfig();
    rankFeed();
    toast("Настройки сброшены, история сохранена");
    platform::bootLog("CFG-07: настройки сброшены, история сохранена (%d записей)",
                      (int)history.size());
}

// ---------------------------------------------------------------- инициализация
void App::init() {
    using platform::bootLog;
    mkdir("sdmc:/config", 0777);
    storageReadOnly = !probeWritable(kConfigDir);
    bootLog("config: SD %s", storageReadOnly ? "только для чтения" : "запись доступна");
    loadConfig();
    bootLog("config: dark=%d seed=%08X reco=%d history=%d", cfg.dark ? 1 : 0, cfg.seed,
            cfg.localReco ? 1 : 0, (int)history.size());
    applyTheme();
    bool fontsOk = fonts.loadDefaults();
    bootLog("fonts: %s", fontsOk ? "ok" : "НЕ ЗАГРУЖЕНЫ (нет romfs:/fonts?)");

    net::init();
    net::setCaBundlePath("romfs:/cacert-trim.pem");
    net::setUserAgent("Mozilla/5.0 (Nintendo Switch; TubeNX/1.0) AppleWebKit/537.36 (KHTML, like Gecko)");
    bootLog("net: ok");
    // Метка сборки: по ней в логе видно, что на консоли именно этот фикс,
    // а не старая копия NRO (build: показывает время main.cpp и не меняется).
    bootLog("tube: v3 fix — SSL-бюджет 2 UI + 2 плееру, accept-loop живой, retry+backoff");

    dpi::init();
    // CFG-03/PL: setMode меняет enabled по режиму, поэтому флаг из конфига
    // восстанавливаем после него — иначе выключенный обход включался на старте.
    dpi::setMode((dpi::Mode)cfg.dpiMode);
    dpi::config().enabled = cfg.dpiEnabled;
    // NET-09: два слота из четырёх держим под плеер (видео+звук), UI не занимает
    // их. Перед открытием видео освобождаем простаивающие UI-соединения.
    net::SessionBudget::shared().reserveForPlayer(2);
    {
        auto bs = net::SessionBudget::shared().stats();
        bootLog("ssl budget: %d/%d (player reserve %d)", bs.used, bs.total,
                net::SessionBudget::shared().playerReserve());
    }
    if (cfg.dpiEnabled) {
        bool ok = dpi::startSocks(1080);
        bootLog("dpi socks: %s", ok ? "listen 1080" : "НЕ ЗАПУСТИЛСЯ");
        if (ok) net::setProxy("socks5h://127.0.0.1:1080");
        bool rel = dpi::startRelay(8899);
        bootLog("dpi relay: %s (порт %d)", rel ? "запущен" : "ОШИБКА", dpi::relayPort());
    }

    // Внешний прокси важнее обхода по SNI: он нужен там, где провайдер режет
    // сам CDN видео по IP (тогда никакие манипуляции с ClientHello не помогают).
    if (!cfg.proxy.empty()) {
        cfg.proxy = normalizeProxy(cfg.proxy);
        net::setProxy(cfg.proxy);
        // SEC-03: пароль внешнего прокси в лог не попадает.
        bootLog("proxy: внешний %s", net::redactProxy(cfg.proxy).c_str());
    }

    // Ретранслятор нужен всегда, когда видео идёт через сеть: ffmpeg в этом
    // порте собран без TLS, поэтому HTTPS-поток качает libcurl.
    if (dpi::relayPort() <= 0) {
        bool rel = dpi::startRelay(8899);
        bootLog("relay (без DPI): %s (порт %d)", rel ? "запущен" : "ОШИБКА", dpi::relayPort());
    }

    images.start(2);
    bootLog("images: workers started");
    // Плеер (libmpv) инициализируем лениво — при первом видео: если mpv не
    // поднимется, UI всё равно работает, а не падает на старте.

    // UX-01: чипы 0/1 — настоящие ленты; 2..5 — честно поиск (Internet не даёт
    // рубрики без авторизации), поэтому в подписи явное «Поиск: …».
    chips = {"Главная", "В тренде", "Поиск: Музыка", "Поиск: Игры", "Поиск: Новости",
             "Поиск: Подкасты"};
    homeChip = std::clamp(cfg.homeChip, 0, (int)chips.size() - 1);

    // Первичная инициализация InnerTube и проба DPI — в фоне, чтобы сплэш не замирал.
    threads::spawn([this]() {
        dpi::ProbeResult pr = dpi::probe("www.youtube.com");
        {
            std::lock_guard<std::mutex> lk(async.m);
            async.probeMs = pr.ok ? pr.rttMs : -1;
            async.probeOk = pr.ok;
        }

        // ---- подбор режима проверки TLS ----
        // Для обхода DPI имя хоста уходит в сервис консоли в верхнем регистре.
        // Сервис может не принять такое имя при проверке сертификата — тогда
        // оставляем проверку цепочки (защита от подмены сохраняется) и
        // отключаем только сверку имени. Решаем один раз, по факту.
        // Проверяем на том хосте, который реально нужен приложению
        // (bootstrap-страница и InnerTube живут на www.youtube.com).
        const char* probeUrl = "https://www.youtube.com/generate_204";
        const std::string viaProxy = net::proxy();
        // Проба не меняет глобальные настройки: пока она ходит по разным
        // режимам, воркеры обложек уже работают и не должны случайно увидеть
        // «verify=None, proxy=''» посреди запроса.
        int chosenVerify = (int)net::Verify::Full;
        auto attempt = [&](const char* tag, int v, const std::string& proxy) {
            const char* px = proxy.empty() ? "" : proxy.c_str();
            net::Overrides ov{px, v};
            net::Response r = net::get(probeUrl, {}, net::timeouts().probe, 64u << 10, 1, &ov);
            platform::bootLog("tls: %s verify=%d status=%ld err='%s'", tag, v, r.status,
                              r.error.c_str());
            if (r.status > 0) chosenVerify = v;
            return r.status > 0;
        };
        // Порядок: сначала напрямую (на этой сети имя в верхнем регистре
        // проходит само по себе и без прокси), потом через локальный SOCKS.
        bool ok = attempt("direct", 0, "");
        if (!ok) ok = attempt("direct", 1, "");

        // Проверяем, что напрямую идут не только мелкие ответы: двух коротких
        // запросов достаточно. Раньше здесь качался весь HTML главной (217 КБ)
        // — только чтобы убедиться, что канал живой; на консоли это 12 секунд
        // и лишний таймаут на старте.
        bool directUseful = false;
        if (ok) {
            net::Overrides direct{"", chosenVerify};
            net::Response page =
                net::get("https://www.youtube.com/generate_204", {}, net::timeouts().probe, 64u << 10, 1,
                         &direct);
            net::Response img = net::get("https://i.ytimg.com/vi/JJW4L_TcIYg/mqdefault.jpg",
                                         {}, net::timeouts().images, 1u << 20, 1, &direct);
            directUseful = (page.status == 204 || page.status == 200) && img.status == 200 &&
                           img.body.size() > 2000;
            platform::bootLog("tls: напрямую проба=%ld (%zu байт), обложка=%ld (%zu байт) -> %s",
                              page.status, page.body.size(), img.status, img.body.size(),
                              directUseful ? "без прокси" : "через SOCKS");
        }

        if (!ok && !viaProxy.empty()) {
            ok = attempt("proxy", 0, viaProxy);
            if (!ok) ok = attempt("proxy", 1, viaProxy);
            // NET-14: проверку сертификата автоматически не отключаем.
            // Уровень None (verify=2) не перебирается в пробе — только
            // осознанно, из явной настройки пользователя.
        }

        // Короткий CA-бандл покрывает Google/GlobalSign/DigiCert/ISRG, но если
        // в сети подсунули другой корень — молча не ломаем приложение, а
        // возвращаемся к полному набору (222 КБ) и пробуем ещё раз.
        if (!ok) {
            net::setCaBundlePath("romfs:/cacert.pem");
            platform::bootLog("tls: короткий CA-бандл не подошёл, пробуем полный");
            ok = attempt("direct-full-ca", 0, "");
            if (!ok && !viaProxy.empty()) ok = attempt("proxy-full-ca", 0, viaProxy);
        }

        // Если напрямую большой ответ не прошёл — замеряем канал на нейтральном
        // хосте. Это отличает «плохой Wi-Fi» (медленно всё) от «провайдер душит
        // именно YouTube» (нейтральный хост летает, youtube нет).
        if (!directUseful) {
            const auto t0 = std::chrono::steady_clock::now();
            net::Response sp =
                net::get("https://speed.cloudflare.com/__down?bytes=1500000", {}, net::timeouts().api,
                         4u << 20, 1);
            const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
            const double kbs = ms > 0 ? (sp.body.size() / 1024.0) / (ms / 1000.0) : 0.0;
            platform::bootLog("speed: нейтральный хост %ld, %zu байт за %lld мс = %.0f КБ/с (%s)",
                              sp.status, sp.body.size(), ms, kbs,
                              sp.error.empty() ? "ok" : sp.error.c_str());
        }
        // Прокси и режим проверки сертификата — разные решения. Раньше удачный
        // прямой запрос навсегда сбрасывал прокси, и ретранслятор видео потом
        // ходил напрямую, без обхода DPI. Возвращаем прокси всегда... кроме
        // случая, когда напрямую проверены и большой ответ, и обложка.
        // Внешний прокси, заданный пользователем, не отменяем никогда.
        const bool userProxy = !cfg.proxy.empty();
        const bool useProxy = userProxy || !(ok && directUseful);
        net::setVerify((net::Verify)chosenVerify);
        net::setProxy(useProxy ? viaProxy : "");
        dpi::config().relayViaLocalSocks = useProxy;
        platform::bootLog("tls: режим проверки=%d прокси='%s'", (int)net::verify(),
                          net::redactProxy(net::proxy()).c_str());

        // YT-02: подставляем сохранённый visitor ДО bootstrap, чтобы YouTube
        // видел ту же сессию, что и в прошлый запуск.
        if (!cfg.visitor.empty()) yt.setVisitorData(cfg.visitor, cfg.visitorSavedAt);
        yt.init("RU", "ru");
        // YT-02: фиксируем актуальный visitor и время — UI-поток сохранит в конфиг.
        yt.setVisitorData(yt.visitorData(), (long long)::time(nullptr));
        captureVisitor();
        {
            std::lock_guard<std::mutex> lk(async.m);
            async.probeGen++;      // сигнал «загрузка завершена» для сплэша
        }
    });
    probeSpin = 0.f;
}

void App::shutdown() {
    // NET-11: останавливаем незавершённые сетевые запросы до остановки сети.
    if (searchTok) searchTok->cancel();
    if (feedTok) feedTok->cancel();
    saveConfig();
    images.stop();
    player::instance().close();
    dpi::stopRelay();
    dpi::stopSocks();
}

// ---------------------------------------------------------------- навигация
void App::switchTo(Screen s) {
    dirty_ = true;   // PL-02: смена экрана — всегда новый кадр
    // NET-11: уход с экрана отменяет его загрузку — поздний ответ поиска
    // больше не применяем (в транспорте его отменит CancelToken).
    if (s != Screen::Search && screen == Screen::Search) {
        if (searchTok) searchTok->cancel();   // NET-11: отмена на транспорте
        searchReqId++;
        searchLoading = false;
        // YT-10: уход с поиска отменяет и текущий запрос подсказок.
        std::lock_guard<std::mutex> lk(async.m);
        if (async.suggestCancel) async.suggestCancel->cancel();
        async.suggestCancel.reset();
    }
    // Уходя со сплэша, освобождаем 3.7 МБ кэша фона: он больше не нужен.
    if (s != Screen::Splash && screen == Screen::Splash && !splashBg.px.empty()) {
        splashBg.px.clear();
        splashBg.px.shrink_to_fit();
        splashBgReady = false;
    }
    if (screen != s) prevScreen = screen;
    screen = s;
}

void App::go(Screen s) {
    // UX-11: запоминаем, откуда ушли, чтобы `B` вернул обратно.
    if (s != screen) {
        navStack.push_back(screen);
        if (navStack.size() > 16) navStack.erase(navStack.begin());
    }
    switchTo(s);
}

void App::back() {
    // UX-11: `B` в плеере всегда закрывает плеер, а не приложение.
    if (screen == Screen::Player) {
        closePlayer();
        return;
    }
    // Возврат строго по истории переходов.
    while (!navStack.empty()) {
        Screen p = navStack.back();
        navStack.pop_back();
        if (p == Screen::Splash) continue;   // в сплэш не возвращаемся
        if (p == Screen::Player) {
            // Плеер на пути — закрываем его и продолжаем подниматься.
            player::instance().close();
            pl.visible = false;
            plFrame = Image{};
            continue;
        }
        switchTo(p);
        return;
    }
    if (screen == Screen::Home) {
        platform::requestExit();
        return;
    }
    switchTo(Screen::Home);
}

void App::toast(const std::string& s, float seconds) {
    if (s.empty()) return;
    // UX-12: одна очередь. Подряд одинаковые не копим; новый тост встаёт в
    // очередь и показывается после текущего, а не перебивает его.
    if (toastText == s) return;
    for (const auto& t : toastQueue)
        if (t.text == s) return;
    while (toastQueue.size() >= 4) toastQueue.erase(toastQueue.begin());
    toastQueue.push_back({s, seconds});
    dirty_ = true;   // PL-02: тост — новый кадр
}

bool App::takeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

// ---------------------------------------------------------------- данные
void App::captureVisitor() {
    yt::VisitorState vs = yt.visitorState();
    if (vs.data.empty()) return;
    std::lock_guard<std::mutex> lk(async.m);
    if (async.visitor != vs.data) {
        async.visitor = vs.data;
        async.visitorSavedAt = vs.savedAtEpoch;
        async.visitorGen++;
    }
}

void App::loadHome(bool trending) {
    homeIsTrending = trending;
    feedLoading = true;
    feedOk = false;
    feedError.clear();
    platform::bootLog("loadHome: %s", trending ? "trending" : "home");
    // NET-11: новый запрос ленты отменяет предыдущий на транспорте.
    if (feedTok) feedTok->cancel();
    auto tok = std::make_shared<net::CancelToken>();
    feedTok = tok;
    const int myId = ++feedReqId;
    threads::spawn([this, trending, myId, tok]() {
        yt.setCancelToken(tok.get());
        yt::Feed f = trending ? yt.trending() : yt.home();
        if (yt.cancelToken() == tok.get()) yt.setCancelToken(nullptr);
        if (feedReqId.load() != myId) {
            platform::bootLog("feed: поздний ответ отменён (generation %d)", myId);
            return;
        }
        captureVisitor();
        std::lock_guard<std::mutex> lk(async.m);
        async.feed = f;
        async.okFeed = f.ok;
        async.errFeed = f.error;
        async.feedGen++;
    });
}

void App::loadMore() {
    if (feedCont.empty() || feedLoading) return;
    feedLoading = true;
    if (feedTok) feedTok->cancel();
    auto tok = std::make_shared<net::CancelToken>();
    feedTok = tok;
    std::string cont = feedCont;
    threads::spawn([this, cont, tok]() {
        yt.setCancelToken(tok.get());
        yt::Feed more = yt.continueFeed(cont);
        if (yt.cancelToken() == tok.get()) yt.setCancelToken(nullptr);
        captureVisitor();
        std::lock_guard<std::mutex> lk(async.m);
        yt::Feed base = async.feed;
        if (!base.sections.empty() && !more.sections.empty()) {
            // YT-06: дедуп по videoId на стороне UI. InnerTube чистит дубли
            // внутри continuation, но видео с первой страницы тоже могло
            // повториться — на консоли это заметно как «двойная» плитка.
            std::unordered_map<std::string, int> seen;
            for (auto& s : base.sections)
                for (auto& v : s.items) seen[v.id]++;
            for (auto& s : more.sections) {
                if (s.items.empty()) continue;
                std::vector<yt::VideoItem> uniq;
                uniq.reserve(s.items.size());
                for (auto& v : s.items)
                    if (!v.id.empty() && seen[v.id]++ == 0) uniq.push_back(v);
                base.sections.front().items.insert(base.sections.front().items.end(),
                                                  uniq.begin(), uniq.end());
            }
        }
        // Явный конец ленты (YT-06): пустой continuation прячет кнопку «Ещё»,
        // даже когда новых секций не пришло.
        base.continuation = more.continuation;
        async.feed = base;
        async.feedGen++;
    });
}

void App::startSearch(const std::string& q, bool openKeyboard) {
    std::string text = q;
    if (openKeyboard) {
        if (!platform::keyboard(text, "Введите запрос", "Поиск на YouTube")) return;
    }
    if (text.empty()) return;
    query = text;
    showSuggestions = false;
    searchLoading = true;
    searchError.clear();
    lastQuery = text;
    // NET-11: новый поиск делает предыдущий устаревшим — отменяем на транспорте.
    if (searchTok) searchTok->cancel();
    auto tok = std::make_shared<net::CancelToken>();
    searchTok = tok;
    const int myId = ++searchReqId;
    threads::spawn([this, text, myId, tok]() {
        yt.setCancelToken(tok.get());
        yt::Feed f = yt.search(text);
        if (yt.cancelToken() == tok.get()) yt.setCancelToken(nullptr);
        if (searchReqId.load() != myId) {
            platform::bootLog("search: поздний ответ отменён (generation %d)", myId);
            return;
        }
        captureVisitor();
        std::lock_guard<std::mutex> lk(async.m);
        async.search = f;
        async.okSearch = f.ok;
        async.errSearch = f.error;
        async.searchGen++;
    });
}

[[maybe_unused]] static int targetHeightFor(int quality) {
    switch (quality) {
    case 1: return 720;
    case 2: return 480;
    case 3: return 360;
    default: return 720;   // «Авто»: 720p — предел software-декодера
    }
}

// PLR-14: подбор потоков под целевую высоту и открытие в плеере. Вынесено из
// обработчика async, чтобы этим же путём шла смена качества на лету: open()
// с startAt перезапускает поток и после file-loaded возвращается на позицию.
bool App::openPlayerStreams(int targetHeight, double startAt) {
    yt::StreamFormat muxed, v, a;
    if (!yt::InnerTube::pickStreams(pl.details, &muxed, &v, &a, targetHeight)) {
        platform::bootLog("streams: pickStreams не нашёл потоков (h=%d)", targetHeight);
        pl.error = "Видео недоступно для воспроизведения";
        return false;
    }
    platform::bootLog(
        "streams: h=%d muxed itag=%d q='%s' url=%d | video itag=%d q='%s' url=%d | "
        "audio itag=%d url=%d",
        targetHeight, muxed.itag, muxed.qualityLabel.c_str(), muxed.url.empty() ? 0 : 1, v.itag,
        v.qualityLabel.c_str(), v.url.empty() ? 0 : 1, a.itag, a.url.empty() ? 0 : 1);
    if (muxed.url.empty() && v.url.empty()) {
        pl.error = "Нет доступных потоков (нужен вход/PO-token)";
        return false;
    }

    auto& p = player::instance();
    if (!pl.ready) {
        pl.ready = p.init();
        platform::bootLog("player init (late): %s", pl.ready ? "ok" : "ОШИБКА");
    }
    // Кадр рендерим сразу в разрешении экрана: тогда UI копирует его как есть
    // (1:1), без bilinear-апскейла на каждый кадр. Масштабирование делает mpv.
    p.setTargetSize(1280, 720);
    p.setAudioDelay(cfg.audioDelayMs / 1000.0);
    // Плееру нужны 2 SSL-сессии (видео + звук), а всего их 4. Закрываем
    // простаивающие UI-соединения, иначе mpv ждёт рукопожатие.
    net::closeIdleConnections();
    // NET-09: плееру зарезервировано 2 слота; логируем занятость бюджета —
    // по логу видно, что UI отпустил свои соединения перед открытием.
    {
        auto bs = net::SessionBudget::shared().stats();
        platform::bootLog("ssl budget: %d/%d (player reserve %d)", bs.used, bs.total,
                          net::SessionBudget::shared().playerReserve());
    }
    // Всегда отдаём mpv локальный ретранслятор: он ходит в сеть через libcurl,
    // где обход DPI уже проверен.
    const bool relay = dpi::relayPort() > 0;
    // Адаптивные дорожки (видео 720p + отдельный AAC) лучше muxed itag 18
    // (360p), поэтому пробуем их первыми.
    const bool adaptiveReady = !v.url.empty() && !a.url.empty();
    const bool videoOnly = !v.url.empty() && a.url.empty() && muxed.url.empty();
    bool opened = false;
    if (adaptiveReady || muxed.url.empty()) {
        std::string vurl = relay ? dpi::relayUrl(v.url) : v.url;
        platform::bootLog("player: open %s itag=%d q='%s' relay=%d src=%s",
                          urlHost(vurl).c_str(), v.itag, v.qualityLabel.c_str(), relay ? 1 : 0,
                          urlHost(v.url).c_str());
        opened = p.open(vurl, pl.details.title, startAt);
        if (opened && !a.url.empty()) {
            std::string aurl = relay ? dpi::relayUrl(a.url) : a.url;
            platform::bootLog("player: audio %s itag=%d", urlHost(aurl).c_str(), a.itag);
            p.addAudio(aurl);
        }
        pl.qualityLabel = v.qualityLabel.empty() ? "H.264" : v.qualityLabel;
        if (opened && videoOnly) pl.qualityLabel += " (без звука)";
    } else {
        std::string url = relay ? dpi::relayUrl(muxed.url) : muxed.url;
        platform::bootLog("player: open %s itag=%d q='%s' relay=%d src=%s",
                          urlHost(url).c_str(), muxed.itag, muxed.qualityLabel.c_str(),
                          relay ? 1 : 0, urlHost(muxed.url).c_str());
        opened = p.open(url, pl.details.title, startAt);
        pl.qualityLabel = muxed.qualityLabel.empty() ? "H.264" : muxed.qualityLabel;
    }
    if (!opened) {
        // Раньше ошибка open() затиралась, и экран оставался чёрным.
        pl.error = "Не удалось открыть видео-поток";
        pl.qualityLabel.clear();
    }
    return opened;
}

void App::openVideo(const yt::VideoItem& item) {
    pl.item = item;
    pl.visible = true;
    pl.loading = true;
    pl.error.clear();
    pl.controls = true;
    pl.idle = 0.f;
    pl.seeking = false;
    pl.seekTarget = -1.0;   // UX-06: сбрасываем удержание от прошлого видео
    pl.seekHoldT = 0.f;
    pl.menusOpen = false;
    pl.qualityMenuOpen = false;
    pl.moreMenuOpen = false;
    pl.closeDialogOpen = false;
    pl.details = yt::VideoDetails{};
    pl.qualityLabel.clear();
    lastPlayerError.clear();
    plLoggedPlaying = false;
    streamRefreshTried = false;
    refreshPos = 0.0;
    go(Screen::Player);

    // история (локальная, никуда не отправляется)
    bool found = false;
    for (auto& h : history) {
        if (h.id != item.id) continue;
        h.watchedAt = (double)time(nullptr);
        if (!item.title.empty()) h.title = item.title;
        if (!item.channel.empty()) h.channel = item.channel;
        found = true;
        break;
    }
    if (!found) {
        HistoryEntry h;
        h.id = item.id;
        h.title = item.title;
        h.channel = item.channel;
        h.watchedAt = (double)time(nullptr);
        history.insert(history.begin(), h);
    }
    if (history.size() > 100) history.resize(100);

    // UX-10: при открытии ранее недосмотренного ролика продолжаем с сохранённой
    // позиции (±5 с). Завершённые (> 95 %) и почти начатые (< 5 с) — с начала.
    pl.pendingResumeSec = 0.0;
    for (const auto& h : history) {
        if (h.id != item.id) continue;
        if (h.duration >= 30.0 && h.time >= 5.0 && h.time <= h.duration * 0.95)
            pl.pendingResumeSec = h.time;
        break;
    }

    // libmpv поднимаем при первом видео: не тормозим старт и не падаем на нём.
    if (!pl.ready) {
        pl.ready = player::instance().init();
        platform::bootLog("player init: %s", pl.ready ? "ok" : "ОШИБКА");
    }

    std::string id = item.id;
    threads::spawn([this, id]() {
        yt::VideoDetails d;
        bool ok = yt.video(id, d);
        captureVisitor();
        std::lock_guard<std::mutex> lk(async.m);
        async.video = d;
        async.okVideo = ok;
        async.errVideo = ok ? "" : (yt.error().empty() ? "Не удалось получить потоки" : yt.error());
        async.videoGen++;
    });
}

void App::closePlayer() {
    auto& p = player::instance();
    double pos = p.position(), dur = p.duration();
    for (auto& h : history) {
        if (h.id != pl.item.id) continue;
        h.time = pos;
        if (dur > 0.0) h.duration = dur;
        h.watchedAt = (double)time(nullptr);
        break;
    }
    p.close();
    pl.visible = false;
    plFrame = Image{};
    pl.qualityMenuOpen = false;
    pl.moreMenuOpen = false;
    pl.closeDialogOpen = false;
    pl.menusOpen = false;
    saveConfig();
    rankFeed();     // вернулись с просмотра — лента учитывает новый сигнал
    // UX-11: возвращаемся на экран, откуда открыли видео (Home или Search),
    // а не всегда на Home.
    Screen target = Screen::Home;
    while (!navStack.empty()) {
        Screen p2 = navStack.back();
        navStack.pop_back();
        if (p2 == Screen::Splash || p2 == Screen::Player) continue;
        target = p2;
        break;
    }
    switchTo(target);
}

void App::switchPlayerQuality(int qi) {
    if (qi < 0 || qi > 3) return;
    static const char* kNames[4] = {"Авто", "720p", "480p", "360p"};
    cfg.quality = qi;
    saveConfig();
    if (!pl.visible) {
        toast(std::string("Качество: ") + kNames[qi]);
        return;
    }
    auto& p = player::instance();
    const double pos = p.position();
    const bool wasPaused = p.paused();
    const double startAt = pos > 1.0 ? pos : 0.0;
    if (!openPlayerStreams(targetHeightFor(qi), startAt)) {
        toast("Не удалось сменить качество: " + pl.error);
        return;
    }
    if (wasPaused) p.setPaused(true);
    toast(std::string("Качество: ") + kNames[qi]);
    platform::bootLog("PLR-14: качество -> %s, позиция %.1f c", kNames[qi], startAt);
}

void App::setDpiEnabled(bool on) {
    cfg.dpiEnabled = on;
    dpi::config().enabled = on;
    if (on) {
        if (dpi::startSocks(1080) && cfg.proxy.empty())
            net::setProxy("socks5h://127.0.0.1:1080");
        dpi::startRelay(8899);
        toast("Обход DPI включён");
    } else {
        dpi::stopSocks();
        toast("Обход DPI выключен");
    }
    // Внешний прокси имеет приоритет и не сбрасывается этим переключателем:
    // ретранслятор видео пользуется им же.
    if (!cfg.proxy.empty()) net::setProxy(cfg.proxy);
    else if (!on) net::setProxy("");
    else if (dpi::socksPort() > 0)
        net::setProxy("socks5h://127.0.0.1:" + std::to_string(dpi::socksPort()));
    saveConfig();
}

bool App::setProxy(const std::string& raw, std::string& err) {
    std::string norm;
    if (!validateProxy(raw, norm, err)) return false;

    cfg.proxy = norm;
    saveConfig();
    if (norm.empty()) {
        // Внешний прокси отключён — возвращаемся к встроенному обходу, если он
        // включён (иначе прямое соединение).
        if (cfg.dpiEnabled && dpi::socksPort() > 0)
            net::setProxy("socks5h://127.0.0.1:" + std::to_string(dpi::socksPort()));
        else
            net::setProxy("");
        toast("Внешний прокси отключён");
        platform::bootLog("UX-09: внешний прокси сброшен");
    } else {
        net::setProxy(norm);
        toast("Прокси: " + norm);
        platform::bootLog("UX-09: внешний прокси %s", norm.c_str());
    }
    return true;
}

// UX-08: кольцевой буфер последних 5 ошибок. Подряд одинаковые не повторяем.
void App::recordDiagError(const std::string& e) {
    if (e.empty()) return;
    if (!diagErrors.empty()) {
        const std::string& last = diagErrors.back();
        if (last.size() > e.size() && last.compare(last.size() - e.size(), e.size(), e) == 0)
            return;
    }
    char ts[16] = "";
    std::time_t now = std::time(nullptr);
    std::tm* tmv = std::localtime(&now);
    if (tmv) std::strftime(ts, sizeof(ts), "%H:%M:%S", tmv);
    diagErrors.push_back(std::string(ts) + " " + e);
    if (diagErrors.size() > 5) diagErrors.erase(diagErrors.begin());
}

// UX-08: «Скопировать лог в файл». На Switch нет буфера обмена, поэтому
// диагностику (режим, порты, RTT, bootstrap, последние ошибки) пишем в
// sdmc:/config/TubeNX/diagnostics.txt и дописываем хвост boot-лога.
void App::exportDiagnostics() {
    static const char* kPath = "sdmc:/config/TubeNX/diagnostics.txt";
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/TubeNX", 0777);
    FILE* f = fopen(kPath, "w");
    if (!f) {
        toast("Не удалось записать диагностику");
        return;
    }
    char ts[32] = "";
    std::time_t now = std::time(nullptr);
    std::tm* tmv = std::localtime(&now);
    if (tmv) std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tmv);
    fprintf(f, "TubeNX — диагностика\n%s\n\n", ts);
    fprintf(f, "DPI: enabled=%d mode=%s socks=%d relay=%d\n", cfg.dpiEnabled ? 1 : 0,
            dpi::modeName(dpi::config().mode), dpi::socksPort(), dpi::relayPort());
    // CFG-07: краткая сводка настроек (без секретов) — чтобы по одному файлу
    // было видно, с каким конфигом воспроизводится проблема.
    fprintf(f,
            "config: v2 dark=%d seed=%08X quality=%d speed=%d adelay=%d autoplay=%d reco=%d "
            "chip=%d set=%d history=%d\n",
            cfg.dark ? 1 : 0, cfg.seed, cfg.quality, cfg.speed, cfg.audioDelayMs,
            cfg.autoplay ? 1 : 0, cfg.localReco ? 1 : 0, cfg.homeChip, settingsFocus,
            (int)history.size());
    fprintf(f, "storage: read_only=%d\n", storageReadOnly ? 1 : 0);
    fprintf(f, "probe: ok=%d rtt=%d ms\n", probeOk ? 1 : 0, probeMs);
    fprintf(f, "bootstrap: bootDone=%d feedOk=%d\n", bootDone ? 1 : 0, feedOk ? 1 : 0);
    fprintf(f, "proxy: '%s'\n", redactUserInfo(net::proxy()).c_str());
    fprintf(f, "feed error: '%s'\n", feedError.c_str());
    fprintf(f, "player error: '%s'\n", pl.error.c_str());
    fprintf(f, "\nпоследние ошибки:\n");
    if (diagErrors.empty()) fprintf(f, "  (нет)\n");
    for (const auto& e : diagErrors) fprintf(f, "  %s\n", e.c_str());
    FILE* log = fopen("sdmc:/switch/TubeNX-boot.log", "r");
    if (log) {
        fputs("\n--- boot log ---\n", f);
        char buf[1024];
        while (fgets(buf, sizeof(buf), log)) fputs(buf, f);
        fclose(log);
    }
    fclose(f);
    platform::bootLog("UX-08: диагностика сохранена в %s", kPath);
    toast(std::string("Диагностика: ") + kPath, 3.5f);
}

void App::probe(bool useBypass) {
    probing = true;
    threads::spawn([this, useBypass]() {
        dpi::ProbeResult pr = dpi::probe("www.youtube.com", 443, useBypass);
        std::lock_guard<std::mutex> lk(async.m);
        async.probeMs = pr.ok ? pr.rttMs : -1;
        async.probeOk = pr.ok;
        async.probeGen++;
    });
}

// DPI-06: «Подобрать автоматически» — перебираем Off→Split→MultiSplit→
// TlsRecordSplit на generate_204 и запоминаем первый рабочий режим.
void App::autoSelectDpi() {
    if (dpiAutoSelecting) return;
    dpiAutoSelecting = true;
    toast("Подбираем режим обхода…");
    threads::spawn([this]() {
        dpi::AutoSelectResult r = dpi::autoSelectMode();
        std::lock_guard<std::mutex> lk(async.m);
        async.dpiAutoOk = r.found;
        async.dpiAutoMode = r.found ? (int)r.mode : -1;
        async.dpiAutoTried = r.tried;
        async.dpiAutoErr = r.lastError;
        async.dpiAutoGen++;
    });
}

// «Повторить» с офлайн-сплэша (§5.4): сбрасываем старт и заново проверяем сеть.
// Проба в фоне увеличит probeGen, и update() снова запустит загрузку ленты.
void App::retryBoot() {
    bootDone = false;
    feedOk = false;
    feedLoading = false;
    feedError.clear();
    probeMs = -1;
    probeOk = false;
    splashT = 0.f;   // снова выдержать минимальное время показа
    splashStatus = "Проверяем соединение…";
    probe(cfg.dpiEnabled);
}


// ---------------------------------------------------------------- локальные рекомендации
namespace {

// Канал сравниваем без регистра и хвостовых пробелов: в ленте и истории
// названия приходят из разных рендереров и могут отличаться мелочами.
std::string normChannel(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char ch : s) {
        unsigned char u = (unsigned char)ch;
        if (u >= 'A' && u <= 'Z') o.push_back((char)(u - 'A' + 'a'));
        else if (u != '\r' && u != '\n') o.push_back(ch);
    }
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

uint64_t idHash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

void App::rebuildAffinity() {
    affinity_.clear();
    if (!cfg.localReco) return;
    double now = (double)time(nullptr);
    for (const auto& h : history) {
        if (h.channel.empty()) continue;
        double days = h.watchedAt > 0.0 ? std::max(0.0, (now - h.watchedAt) / 86400.0) : 0.0;
        float recency = (float)(1.0 / (1.0 + days / 10.0));   // неделя ≈ половина веса
        float w = 1.f;
        if (h.liked) w += 2.f;                                // лайк — сильный сигнал
        if (h.duration > 60.0) {
            double p = h.time / h.duration;
            if (p >= 0.9) w *= 0.6f;                          // досмотрел — не навязывать
            else if (p > 0.02) w *= 1.3f;                     // бросил на середине — интересно
        }
        affinity_[normChannel(h.channel)] += w * recency;
    }
}

std::vector<std::pair<std::string, float>> App::topChannels(int n) {
    if (affinity_.empty()) rebuildAffinity();
    std::vector<std::pair<std::string, float>> v(affinity_.begin(), affinity_.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (n > 0 && (int)v.size() > n) v.resize((size_t)n);
    // Ключи — нормализованные (нижний регистр); подставляем исходное название
    // канала из истории, чтобы в настройках был читаемый текст.
    for (auto& kv : v) {
        for (const auto& h : history) {
            if (!h.channel.empty() && normChannel(h.channel) == kv.first) {
                kv.first = h.channel;
                break;
            }
        }
    }
    return v;
}

float App::channelAffinity(const std::string& channel) {
    if (affinity_.empty()) rebuildAffinity();
    auto it = affinity_.find(normChannel(channel));
    return it == affinity_.end() ? 0.f : it->second;
}

void App::rankFeed() {
    if (feed.sections.empty()) return;
    rebuildAffinity();
    if (!cfg.localReco) return;

    auto& items = feed.sections.front().items;
    if (items.size() < 2) return;

    std::unordered_map<std::string, const HistoryEntry*> hist;
    for (const auto& h : history) hist[h.id] = &h;

    auto score = [&](const yt::VideoItem& v) -> float {
        float s = 10.f;
        auto a = affinity_.find(normChannel(v.channel));
        if (a != affinity_.end()) s += std::min(30.f, a->second) * 2.4f;

        auto h = hist.find(v.id);
        if (h != hist.end()) {
            const HistoryEntry* e = h->second;
            if (e->liked) s += 90.f;
            if (e->duration > 60.0) {
                double p = e->time / e->duration;
                if (p > 0.02 && p < 0.9) s += 45.f;   // «продолжить просмотр»
                else if (p >= 0.9) s -= 35.f;         // уже посмотрено
            } else if (e->time > 5.0) {
                s += 20.f;
            }
        }
        s += (float)(idHash(v.id) % 600) / 100.f;      // стабильный «шум», лента не застывает
        if (v.live) s += 3.f;
        return s;
    };

    std::stable_sort(items.begin(), items.end(),
                     [&](const yt::VideoItem& a, const yt::VideoItem& b) { return score(a) > score(b); });

    // YouTube иногда дублирует одно видео в разных рядах — убираем повторы.
    std::unordered_map<std::string, int> seen;
    std::vector<yt::VideoItem> uniq;
    uniq.reserve(items.size());
    for (auto& v : items) {
        if (v.id.empty() || seen[v.id]++) continue;
        uniq.push_back(v);
    }
    items.swap(uniq);
}

std::vector<const HistoryEntry*> App::continueWatching() const {
    std::vector<const HistoryEntry*> out;
    if (!cfg.localReco) return out;
    for (const auto& h : history) {
        if (h.id.empty() || h.title.empty()) continue;
        // UX-10: «Продолжить» — только реально недосмотренное. Завершённые
        // (короче 30 с или прогресс > 95 %) и едва начатые (< 5 с) не берём.
        if (h.duration >= 30.0) {
            if (h.time < 5.0 || h.time > h.duration * 0.95) continue;
        } else if (h.duration > 0.0) {
            continue;   // слишком короткое — продолжать нечего
        } else if (h.time < 5.0) {
            continue;   // длительность неизвестна: нужен заметный прогресс
        }
        out.push_back(&h);
        if (out.size() >= 8) break;
    }
    return out;
}

bool App::isLiked(const std::string& videoId) const {
    for (const auto& h : history)
        if (h.id == videoId) return h.liked;
    return false;
}

void App::toggleLike(const std::string& videoId) {
    if (videoId.empty()) return;
    for (auto& h : history) {
        if (h.id != videoId) continue;
        h.liked = !h.liked;
        h.watchedAt = (double)time(nullptr);
        toast(h.liked ? "Добавлено в любимые" : "Убрано из любимых");
        saveConfig();
        rankFeed();
        return;
    }
    HistoryEntry h;
    h.id = videoId;
    h.title = pl.details.title.empty() ? pl.item.title : pl.details.title;
    h.channel = pl.details.channel.empty() ? pl.item.channel : pl.details.channel;
    h.watchedAt = (double)time(nullptr);
    h.liked = true;
    history.insert(history.begin(), h);
    if (history.size() > 100) history.resize(100);
    toast("Добавлено в любимые");
    saveConfig();
    rankFeed();
}

// ---------------------------------------------------------------- update
void App::update(float dt) {
    uiTime_ += dt;
    // UX-12: один тост на экране; по истечении берём следующий из очереди.
    if (toastT > 0.f) {
        toastT = std::max(0.f, toastT - dt);
        if (toastT <= 0.f) toastText.clear();
    }
    if (toastText.empty() && !toastQueue.empty()) {
        toastText = toastQueue.front().text;
        toastT = toastTotal = toastQueue.front().seconds;
        toastQueue.erase(toastQueue.begin());
    }
    if (probing) probeSpin = std::fmod(probeSpin + dt * 3.f, 6.28318f);

    // UX-02: сторож загрузки. Сетевые бюджеты (net::timeouts) закрывают
    // зависшие запросы, но если ответ всё же потерялся — показываем ошибку с
    // «Повторить», а не бесконечный скелетон.
    if (feedLoading) {
        feedLoadT += dt;
        if (feedLoadT > 30.f) {
            feedLoading = false;
            feedLoadT = 0.f;
            if (feedError.empty()) feedError = "YouTube не отвечает. Проверьте соединение и обход DPI.";
            recordDiagError(feedError);
            platform::bootLog("UX-02: сторож ленты сработал (30 с)");
        }
    } else {
        feedLoadT = 0.f;
    }
    if (searchLoading) {
        searchLoadT += dt;
        if (searchLoadT > 20.f) {
            searchLoading = false;
            searchLoadT = 0.f;
            if (searchError.empty()) searchError = "YouTube не отвечает. Проверьте соединение и обход DPI.";
            recordDiagError(searchError);
            platform::bootLog("UX-02: сторож поиска сработал (20 с)");
        }
    } else {
        searchLoadT = 0.f;
    }
    if (pl.loading) {
        videoLoadT += dt;
        if (videoLoadT > 25.f) {
            pl.loading = false;
            videoLoadT = 0.f;
            if (pl.error.empty()) pl.error = "YouTube не отвечает. Проверьте соединение и обход DPI.";
            platform::bootLog("UX-02: сторож видео сработал (25 с)");
        }
    } else {
        videoLoadT = 0.f;
    }

    // ---- сплэш ----
    if (screen == Screen::Splash) {
        splashT += dt;
        int pg;
        {
            std::lock_guard<std::mutex> lk(async.m);
            pg = async.probeGen.load();
            if (pg != 0) {
                probeMs = async.probeMs;
                probeOk = async.probeOk;
            }
        }
        if (pg != 0 && pg != bootProbeGen) {
            bootProbeGen = pg;
            bootDone = true;
            loadHome(false);
            splashStatus = "Загрузка ленты…";
        }
        // Страховка: если проба почему-то не вернулась (сеть мертва), всё равно
        // пускаем в интерфейс — там видно ошибку и можно зайти в настройки,
        // вместо бесконечного сплэша. §5.4: сеть не держит переход дольше 3 с.
        if (!bootDone && splashT > 3.f) {
            bootDone = true;
            splashStatus = "Сеть отвечает медленно…";
            loadHome(false);
            // UX-14: на первом запуске не тащим в Home — сначала онбординг.
            if (!cfg.firstRun) go(Screen::Home);
        }
        // Офлайн (проба сети провалилась) сплэш не покидает сам: показывает
        // «Нет сети» и кнопки «Настроить»/«Повторить». Если сеть есть, а лента
        // не загрузилась — уходим на Home с карточкой ошибки.
        // UX-14: пока идёт онбординг, автоматический переход выключен —
        // дальше ведёт кнопка «Продолжить».
        if (bootDone && splashT > 2.4f && !feedLoading && (feedOk || probeOk) && !cfg.firstRun)
            go(Screen::Home);
    }

    // ---- результаты фоновых задач ----
    // PL-02: пришёл результат — его надо показать (без этого статичный экран
    // остался бы старым до страховочного кадра через секунду).
    if (async.feedGen.load() != seenFeedGen || async.searchGen.load() != seenSearchGen ||
        async.videoGen.load() != seenVideoGen || async.refreshGen.load() != seenRefreshGen ||
        async.probeGen.load() != seenProbeGen || async.suggestGen.load() != seenSuggestGen ||
        async.visitorGen.load() != seenVisitorGen || async.dpiAutoGen.load() != seenDpiAutoGen)
        dirty_ = true;
    {
        std::lock_guard<std::mutex> lk(async.m);
        if (async.feedGen.load() != seenFeedGen) {
            seenFeedGen = async.feedGen.load();
            feed = async.feed;
            feedOk = async.okFeed;
            feedError = async.errFeed;
            feedCont = async.feed.continuation;
            feedLoading = false;
            if (!feedOk) feedError = feedError.empty() ? "Сеть недоступна. Проверьте обход DPI." : feedError;
            if (!feedError.empty()) recordDiagError(feedError);
            platform::bootLog("feed: ok=%d sections=%zu err='%s'",
                              feedOk ? 1 : 0, feed.sections.size(), feedError.c_str());
            rankFeed();
        }
        if (async.searchGen.load() != seenSearchGen) {
            seenSearchGen = async.searchGen.load();
            searchFeed = async.search;
            searchLoading = false;
            searchError = async.errSearch;
            if (!searchError.empty()) recordDiagError(searchError);
            searchCont = async.search.continuation;
        }
        if (async.videoGen.load() != seenVideoGen) {
            seenVideoGen = async.videoGen.load();
            if (async.okVideo) {
                pl.details = async.video;
                platform::bootLog("video: ok=1 id=%s formats=%zu title='%s'", pl.item.id.c_str(),
                                  pl.details.formats.size(), pl.details.title.c_str());
                if (openPlayerStreams(targetHeightFor(cfg.quality), pl.pendingResumeSec)) pl.error.clear();
                pl.pendingResumeSec = 0.0;
                pl.loading = false;
            } else {
                platform::bootLog("video: ok=0 id=%s status=%s err='%s'", pl.item.id.c_str(),
                                  async.video.playabilityStatus.c_str(), async.errVideo.c_str());
                // YT-05: сохраняем статус/причину — экран плеера покажет по ним
                // понятный заголовок и действие (вход/возраст/регион).
                pl.details.playabilityStatus = async.video.playabilityStatus;
                pl.details.playabilityReason = async.video.playabilityReason;
                pl.error = async.errVideo.empty() ? "Ошибка загрузки видео" : async.errVideo;
                pl.loading = false;
                recordDiagError(pl.error);
            }
        }
        if (async.refreshGen.load() != seenRefreshGen) {   // YT-08
            seenRefreshGen = async.refreshGen.load();
            if (screen == Screen::Player && pl.visible && async.okRefresh &&
                async.refreshed.id == pl.item.id) {
                pl.details = async.refreshed;
                if (openPlayerStreams(targetHeightFor(cfg.quality), refreshPos)) {
                    pl.error.clear();
                    plLoggedPlaying = false;
                    toast("Поток обновлён");
                    platform::bootLog("YT-08: поток переоткрыт с позиции %.1f c", refreshPos);
                }
            } else if (screen == Screen::Player && pl.visible) {
                toast("Не удалось обновить поток");
                platform::bootLog("YT-08: refreshStreams не удался");
            }
        }
        if (async.probeGen.load() != seenProbeGen) {
            seenProbeGen = async.probeGen.load();
            probeMs = async.probeMs;
            probeOk = async.probeOk;
            probing = false;
            if (screen == Screen::Settings && seenProbeGen > 1)
                toast(probeOk ? ("YouTube доступен, " + std::to_string(probeMs) + " мс") : "YouTube недоступен");
        }
        if (async.suggestGen.load() != seenSuggestGen) {
            seenSuggestGen = async.suggestGen.load();
            // UX-04: применяем подсказки только если ответ относится к текущему
            // запросу — иначе поздний ответ старого запроса подменяет список.
            if (async.suggQuery == query) {
                suggestions = async.sugg;
                if (suggestions.size() > 8) suggestions.resize(8);
            }
        }
        // YT-02: сохраняем visitorData, добытый при bootstrap (файл — из UI-потока).
        if (async.visitorGen.load() != seenVisitorGen) {
            seenVisitorGen = async.visitorGen.load();
            if (!async.visitor.empty() &&
                (cfg.visitor != async.visitor || cfg.visitorSavedAt != async.visitorSavedAt)) {
                cfg.visitor = async.visitor;
                cfg.visitorSavedAt = async.visitorSavedAt;
                saveConfig();
                platform::bootLog("yt: visitor сохранён (%zu байт, savedAt=%lld)",
                                  cfg.visitor.size(), cfg.visitorSavedAt);
            }
        }
        // DPI-06: результат автоподбора режима.
        if (async.dpiAutoGen.load() != seenDpiAutoGen) {
            seenDpiAutoGen = async.dpiAutoGen.load();
            dpiAutoSelecting = false;
            if (async.dpiAutoOk && async.dpiAutoMode >= 0) {
                cfg.dpiMode = async.dpiAutoMode;
                dpi::setMode((dpi::Mode)cfg.dpiMode);
                saveConfig();
                toast(std::string("Режим обхода: ") + dpi::modeName((dpi::Mode)cfg.dpiMode));
                platform::bootLog("dpi auto: выбран режим %s (проверено %d)",
                                  dpi::modeName((dpi::Mode)cfg.dpiMode), async.dpiAutoTried);
            } else {
                toast(async.dpiAutoErr == "video playing" ? "Нельзя менять режим во время видео"
                                                          : "Не удалось подобрать режим");
                platform::bootLog("dpi auto: нет рабочего режима (проверено %d, err='%s')",
                                  async.dpiAutoTried, async.dpiAutoErr.c_str());
            }
        }
    }

    // подсказки поиска с задержкой (UX-04: debounce 280 мс, только последний запрос)
    if (screen == Screen::Search && showSuggestions && query.size() >= 2) {
        suggestTimer += dt;
        if (suggestTimer > 0.28f) {
            suggestTimer = -1000.f;   // один раз на запрос
            std::string q = query;
            // YT-10: отменяем предыдущий запрос и ставим новый токен. Если
            // пользователь продолжает печатать, старый ответ уже не нужен.
            auto token = std::make_shared<net::CancelToken>();
            {
                std::lock_guard<std::mutex> lk(async.m);
                if (async.suggestCancel) async.suggestCancel->cancel();
                async.suggestCancel = token;
            }
            threads::spawn([this, q, token]() {
                auto s = yt.suggestions(q, token.get());
                std::lock_guard<std::mutex> lk(async.m);
                if (token->cancelled()) return;   // результат устарел
                async.sugg = s;
                async.suggQuery = q;   // UX-04: помечаем, к какому запросу
                if (async.sugg.size() > 8) async.sugg.resize(8);
                async.suggestGen++;
            });
        }
    }

    // ---- плеер ----
    if (screen == Screen::Player) {
        auto& p = player::instance();
        if (!pl.loading) {
            bool newFrame = p.update(plFrame);
            if (newFrame) plFrameDirty_ = true;   // PL-02
            if (!pl.error.empty() || (!p.playing() && p.position() <= 0.01 && !p.paused())) {
                std::string e = p.error();
                if (!e.empty() && pl.error != e) {
                    // Частая причина на российских провайдерах — блокировка самого
                    // CDN видео (googlevideo) по IP: обход по SNI тогда бесполезен,
                    // нужен внешний прокси. Подсказываем это прямо на экране.
                    if (cfg.proxy.empty() &&
                        (e.find("loading failed") != std::string::npos ||
                         e.find("502") != std::string::npos))
                        e += " Если провайдер блокирует серверы видео, укажите прокси в Настройках.";
                    pl.error = e;
                }
            }
        }
        if (pl.menusOpen) {
            // UX-05: открытое меню качества/«Ещё»/диалог закрытия не даём
            // спрятать контролы — иначе пункты исчезают из-под фокуса.
            pl.controls = true;
            pl.idle = 0.f;
        } else if (pl.controls) {
            pl.idle += dt;
            if (pl.idle > 3.0f) pl.controls = false;   // UX-05: автоскрытие 3 с
        }

        // UX-06: снимаем удержание прогресса, когда mpv догнал цель seek (или
        // по таймауту) — тогда полоса снова следует за реальным time-pos.
        if (pl.seekTarget >= 0.0) {
            pl.seekHoldT += dt;
            if (std::fabs(p.position() - pl.seekTarget) < 2.0 || pl.seekHoldT > 2.0f)
                pl.seekTarget = -1.0;
        }

        // Ошибка плеера видна на экране, но раньше не попадала в лог — из-за
        // этого сбой видео нельзя было разобрать постфактум.
        if (pl.error != lastPlayerError) {
            lastPlayerError = pl.error;
            if (!pl.error.empty()) recordDiagError(pl.error);
            platform::bootLog("player error: '%s'", pl.error.empty() ? "(сброшена)" : pl.error.c_str());
        }
        if (!plLoggedPlaying && p.playing() && p.position() > 0.05) {
            plLoggedPlaying = true;
            platform::bootLog("player: воспроизведение пошло, pos=%.2f dur=%.2f", p.position(),
                              p.duration());
        }

        // UX-10: позиция сохраняется раз в 5 с и сразу при паузе (сон/выход
        // в hbmenu). Тогда «Продолжить» попадёт в ±5 с даже без закрытия плеера.
        if (!pl.loading && pl.visible) {
            const bool pausedNow = p.paused();
            histSaveT += dt;
            if (histSaveT >= 5.f || (pausedNow && !lastPaused)) {
                histSaveT = 0.f;
                const double pos = p.position(), dur = p.duration();
                for (auto& h : history) {
                    if (h.id != pl.item.id) continue;
                    if (pos > 0.0) h.time = pos;
                    if (dur > 0.0) h.duration = dur;
                    h.watchedAt = (double)time(nullptr);
                    break;
                }
                saveHistory();
            }
            lastPaused = pausedNow;
        }

        // YT-08: ссылка на поток истекла (403/410) — один повтор: заново
        // запрашиваем /player и открываем тот же ролик с прежней позиции.
        if (!streamRefreshTried && !pl.loading && pl.visible && !pl.error.empty() &&
            (pl.error.find("403") != std::string::npos ||
             pl.error.find("410") != std::string::npos)) {
            streamRefreshTried = true;
            refreshPos = p.position();
            platform::bootLog("YT-08: поток истёк ('%s'), обновляем ссылки", pl.error.c_str());
            toast("Ссылка на поток истекла — обновляем…");
            const std::string id = pl.item.id;
            threads::spawn([this, id]() {
                yt::VideoDetails fresh;
                const bool ok = yt.refreshStreams(id, fresh);
                std::lock_guard<std::mutex> lk(async.m);
                async.refreshed = fresh;
                async.okRefresh = ok;
                async.refreshGen++;
            });
        }
    }

    // ---- предзагрузка обложек ----
    for (auto& s : feed.sections)
        for (auto& v : s.items) images.request(v.thumbnailUrl);
    for (auto& s : searchFeed.sections)
        for (auto& v : s.items) images.request(v.thumbnailUrl);
    for (auto& h : history)
        if (!h.id.empty()) images.request(yt::thumbUrl(h.id));
    images.pump();

    // ---- PL-02: кадровый ритм ----
    // Перерисовка нужна, пока на экране что-то двигается или догружается.
    // Всё, что меняется от кадра к кадру, перечислено здесь; статичный экран
    // кадр не рисует (главный цикл пропускает draw+flush).
    if (toastT > 0.f || probing || dpiAutoSelecting || screen == Screen::Splash ||
        feedLoading || searchLoading || pl.loading)
        dirty_ = true;
    if (plFrameDirty_) {
        plFrameDirty_ = false;
        dirty_ = true;
    }
    // Обложки догружаются в фоне — пока есть незавершённые, картинка меняется.
    // Как только очередь опустела, нужен ещё один кадр: последняя обложка
    // могла прийти между кадрами (иначе плейсхолдер висел бы до watchdog).
    const int pend = (screen == Screen::Home || screen == Screen::Search)
                         ? images.stats().pending
                         : 0;
    if (pend > 0 || pend != lastImagePending_) dirty_ = true;
    lastImagePending_ = pend;
    if (screen == Screen::Player && pl.visible) {
        auto& p = player::instance();
        const int st = (int)p.state();
        if (st != lastPlayerState_) {
            lastPlayerState_ = st;
            dirty_ = true;
        }
        // Играющее видео даёт новый кадр, но состояние/метрики меняются и без
        // него. Открытые контролы сбрасывают свой таймер только в drawPlayer(),
        // поэтому на экране с контролами кадр не пропускаем.
        if (p.playing() || p.buffering() || pl.controls) dirty_ = true;
    }
}

// ---------------------------------------------------------------- сетка
std::vector<const yt::VideoItem*> App::flatItems(const yt::Feed& f) const {
    std::vector<const yt::VideoItem*> out;
    for (auto& s : f.sections)
        for (auto& v : s.items) out.push_back(&v);
    return out;
}

App::GridMetrics App::metricsFor(float width) const {
    // Раскладка сетки — всегда 4 колонки (docs/impl-tz.md §5.3):
    // handheld 4×296, dock 4×276; thumbH = floor(cardW*9/16), cardH = thumbH + 90.
    GridMetrics m;
    m.cols = 4;
    m.gap = 16.f;
    m.rowGap = 24.f;   // .grid { gap: 24px 16px }
    m.cardW = (width - (m.cols - 1) * m.gap) / (float)m.cols;
    m.thumbH = std::floor(m.cardW * 9.f / 16.f);
    m.cardH = m.thumbH + 90.f;
    return m;
}

float App::gridContentHeight(const yt::Feed& f, float width) const {
    auto m = metricsFor(width);
    int n = (int)flatItems(f).size();
    int rows = (n + m.cols - 1) / m.cols;
    return rows * (m.cardH + m.rowGap) + 24.f;
}

void App::drawVideo(ui::Ui& u, const Rect& dst) {
    auto& c = u.canvas();
    c.fillRect(dst, Color::rgb(0x000000));
    if (!plFrame.valid()) return;
    float va = (float)plFrame.w / (float)plFrame.h;
    float ra = dst.w / dst.h;
    Rect r = dst;
    if (va > ra) { r.h = dst.w / va; r.y = dst.y + (dst.h - r.h) * 0.5f; }
    else { r.w = dst.h * va; r.x = dst.x + (dst.w - r.w) * 0.5f; }
    c.image(plFrame, r, 1.f);
}

} // namespace tnx::app