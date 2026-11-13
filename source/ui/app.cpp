// ui/app.cpp — состояние, навигация, фоновые запросы, конфиг.
#include "core/threads.hpp"
#include "ui/app.hpp"

#include "core/canvas.hpp"
#include "dpi/dpi.hpp"
#include "net/http.hpp"
#include "platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace tnx::app {

static const char* kConfigPath = "sdmc:/config/TubeNX/config.ini";
static const char* kHistoryPath = "sdmc:/config/TubeNX/history.txt";

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

// Приводим строку прокси к виду scheme://host:port. Без схемы считаем SOCKS5
// с разрешением имён на стороне прокси (socks5h) — так работает и с
// доменными именами, и с IP.
std::string normalizeProxy(std::string p) {
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t' || p.back() == '\n' || p.back() == '\r'))
        p.pop_back();
    if (p.empty()) return p;
    if (p.find("://") == std::string::npos) p = "socks5h://" + p;
    return p;
}

// ---------------------------------------------------------------- конфиг
void App::loadConfig() {
    FILE* f = fopen(kConfigPath, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            std::string k = line;
            std::string v = eq + 1;
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
            if (k == "dark") cfg.dark = atoi(v.c_str()) != 0;
            else if (k == "seed") cfg.seed = (uint32_t)strtoul(v.c_str(), nullptr, 16);
            else if (k == "quality") cfg.quality = atoi(v.c_str());
            else if (k == "speed") cfg.speed = atoi(v.c_str());
            else if (k == "adelay") cfg.audioDelayMs = atoi(v.c_str());
            else if (k == "dpi") cfg.dpiEnabled = atoi(v.c_str()) != 0;
            else if (k == "dpi_mode") cfg.dpiMode = atoi(v.c_str());
            else if (k == "autoplay") cfg.autoplay = atoi(v.c_str()) != 0;
            else if (k == "reco") cfg.localReco = atoi(v.c_str()) != 0;
            else if (k == "proxy") {
                // В значении не должно быть переводов строк (формат ini).
                std::string p = v;
                for (char& ch : p)
                    if (ch == '\n' || ch == '\r') ch = ' ';
                cfg.proxy = p;
            }
        }
        fclose(f);
    }
    // история
    f = fopen(kHistoryPath, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && history.size() < 100) {
            HistoryEntry h;
            char id[64] = {0}, title[512] = {0}, ch[256] = {0};
            double t = 0, d = 0, at = 0;
            int liked = 0;
            int n = sscanf(line, "%63s\t%511[^\t]\t%255[^\t]\t%lf\t%lf\t%lf\t%d", id, title, ch, &t, &d,
                           &at, &liked);
            if (n >= 3) {
                h.id = id; h.title = title; h.channel = ch; h.time = t; h.duration = d;
                h.watchedAt = at; h.liked = liked != 0;
                history.push_back(h);
            }
        }
        fclose(f);
    }
}

void App::saveConfig() {
    mkdir("sdmc:/config/TubeNX", 0777);
    FILE* f = fopen(kConfigPath, "w");
    if (!f) return;
    fprintf(f, "dark=%d\n", cfg.dark ? 1 : 0);
    fprintf(f, "seed=%08X\n", cfg.seed);
    fprintf(f, "quality=%d\n", cfg.quality);
    fprintf(f, "speed=%d\n", cfg.speed);
    fprintf(f, "adelay=%d\n", cfg.audioDelayMs);
    fprintf(f, "dpi=%d\n", cfg.dpiEnabled ? 1 : 0);
    fprintf(f, "dpi_mode=%d\n", cfg.dpiMode);
    fprintf(f, "autoplay=%d\n", cfg.autoplay ? 1 : 0);
    fprintf(f, "reco=%d\n", cfg.localReco ? 1 : 0);
    if (!cfg.proxy.empty()) fprintf(f, "proxy=%s\n", cfg.proxy.c_str());
    fclose(f);

    f = fopen(kHistoryPath, "w");
    if (f) {
        for (auto& h : history)
            fprintf(f, "%s\t%s\t%s\t%.1f\t%.1f\t%.0f\t%d\n", h.id.c_str(), h.title.c_str(),
                    h.channel.c_str(), h.time, h.duration, h.watchedAt, h.liked ? 1 : 0);
        fclose(f);
    }
}

void App::applyTheme() { theme = md3::Theme::make(cfg.seed, cfg.dark); splashBgReady = false; }

// ---------------------------------------------------------------- инициализация
void App::init() {
    using platform::bootLog;
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
    dpi::config().enabled = cfg.dpiEnabled;
    dpi::setMode((dpi::Mode)cfg.dpiMode);
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
        bootLog("proxy: внешний %s", cfg.proxy.c_str());
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

    chips = {"Главная", "В тренде", "Музыка", "Игры", "Новости", "Подкасты"};

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
            net::Response r = net::get(probeUrl, {}, 9000, 64u << 10, 1, &ov);
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
                net::get("https://www.youtube.com/generate_204", {}, 9000, 64u << 10, 1, &direct);
            net::Response img = net::get("https://i.ytimg.com/vi/JJW4L_TcIYg/mqdefault.jpg",
                                         {}, 9000, 1u << 20, 1, &direct);
            directUseful = (page.status == 204 || page.status == 200) && img.status == 200 &&
                           img.body.size() > 2000;
            platform::bootLog("tls: напрямую проба=%ld (%zu байт), обложка=%ld (%zu байт) -> %s",
                              page.status, page.body.size(), img.status, img.body.size(),
                              directUseful ? "без прокси" : "через SOCKS");
        }

        if (!ok && !viaProxy.empty()) {
            ok = attempt("proxy", 0, viaProxy);
            if (!ok) ok = attempt("proxy", 1, viaProxy);
            if (!ok) ok = attempt("proxy", 2, viaProxy);
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
                net::get("https://speed.cloudflare.com/__down?bytes=1500000", {}, 20000,
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
                          net::proxy().c_str());

        yt.init("RU", "ru");
        std::lock_guard<std::mutex> lk(async.m);
        async.probeGen++;      // сигнал «загрузка завершена» для сплэша
    });
    probeSpin = 0.f;
}

void App::shutdown() {
    saveConfig();
    images.stop();
    player::instance().close();
    dpi::stopRelay();
    dpi::stopSocks();
}

// ---------------------------------------------------------------- навигация
void App::go(Screen s) {
    // Уходя со сплэша, освобождаем 3.7 МБ кэша фона: он больше не нужен.
    if (s != Screen::Splash && screen == Screen::Splash && !splashBg.px.empty()) {
        splashBg.px.clear();
        splashBg.px.shrink_to_fit();
        splashBgReady = false;
    }
    if (screen != s) prevScreen = screen;
    screen = s;
    navIndex = s == Screen::Home ? 0 : (s == Screen::Search ? 1 : (s == Screen::Settings ? 2 : navIndex));
}

void App::back() {
    switch (screen) {
    case Screen::Player: closePlayer(); break;
    case Screen::Search:
    case Screen::Settings: go(Screen::Home); break;
    case Screen::Home: platform::requestExit(); break;
    default: break;
    }
}

void App::toast(const std::string& s, float seconds) {
    toastText = s;
    toastT = seconds;
}

// ---------------------------------------------------------------- данные
void App::loadHome(bool trending) {
    homeIsTrending = trending;
    feedLoading = true;
    feedOk = false;
    feedError.clear();
    platform::bootLog("loadHome: %s", trending ? "trending" : "home");
    std::string cont;
    threads::spawn([this, trending]() {
        yt::Feed f = trending ? yt.trending() : yt.home();
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
    std::string cont = feedCont;
    threads::spawn([this, cont]() {
        yt::Feed more = yt.continueFeed(cont);
        std::lock_guard<std::mutex> lk(async.m);
        yt::Feed base = async.feed;
        if (!base.sections.empty() && !more.sections.empty()) {
            // продолжаем первую секцию — так выглядит лента YouTube
            for (auto& s : more.sections) {
                if (s.items.empty()) continue;
                base.sections.front().items.insert(base.sections.front().items.end(), s.items.begin(),
                                                   s.items.end());
            }
            base.continuation = more.continuation;
        }
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
    threads::spawn([this, text]() {
        yt::Feed f = yt.search(text);
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

void App::openVideo(const yt::VideoItem& item) {
    pl.item = item;
    pl.visible = true;
    pl.loading = true;
    pl.error.clear();
    pl.controls = true;
    pl.idle = 0.f;
    pl.seeking = false;
    pl.details = yt::VideoDetails{};
    pl.qualityLabel.clear();
    lastPlayerError.clear();
    plLoggedPlaying = false;
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
    if (history.size() > 60) history.resize(60);

    // libmpv поднимаем при первом видео: не тормозим старт и не падаем на нём.
    if (!pl.ready) {
        pl.ready = player::instance().init();
        platform::bootLog("player init: %s", pl.ready ? "ok" : "ОШИБКА");
    }

    std::string id = item.id;
    threads::spawn([this, id]() {
        yt::VideoDetails d;
        bool ok = yt.video(id, d);
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
    saveConfig();
    rankFeed();     // вернулись с просмотра — лента учитывает новый сигнал
    go(Screen::Home);
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
        bool partially = h.duration > 60.0 ? (h.time > 3.0 && h.time < h.duration * 0.95)
                                           : (h.time > 3.0);
        if (!partially) continue;
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
    if (history.size() > 60) history.resize(60);
    toast("Добавлено в любимые");
    saveConfig();
    rankFeed();
}

// ---------------------------------------------------------------- update
void App::update(float dt) {
    uiTime_ += dt;
    if (toastT > 0.f) toastT = std::max(0.f, toastT - dt);
    if (probing) probeSpin = std::fmod(probeSpin + dt * 3.f, 6.28318f);

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
        if (pg != 0 && !bootDone) {
            bootDone = true;
            loadHome(false);
            splashStatus = "Загрузка ленты…";
        }
        // Страховка: если проба почему-то не вернулась (сеть мертва), всё равно
        // пускаем в интерфейс — там видно ошибку и можно зайти в настройки,
        // вместо бесконечного сплэша.
        if (!bootDone && splashT > 6.f) {
            bootDone = true;
            splashStatus = "Сеть отвечает медленно…";
            loadHome(false);
            go(Screen::Home);
        }
        if (bootDone && splashT > 2.4f && !feedLoading) go(Screen::Home);
    }

    // ---- результаты фоновых задач ----
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
            platform::bootLog("feed: ok=%d sections=%zu err='%s'",
                              feedOk ? 1 : 0, feed.sections.size(), feedError.c_str());
            rankFeed();
        }
        if (async.searchGen.load() != seenSearchGen) {
            seenSearchGen = async.searchGen.load();
            searchFeed = async.search;
            searchLoading = false;
            searchError = async.errSearch;
            searchCont = async.search.continuation;
        }
        if (async.videoGen.load() != seenVideoGen) {
            seenVideoGen = async.videoGen.load();
            if (async.okVideo) {
                pl.details = async.video;
                platform::bootLog("video: ok=1 id=%s formats=%zu title='%s'", pl.item.id.c_str(),
                                  pl.details.formats.size(), pl.details.title.c_str());
                yt::StreamFormat muxed, v, a;
                if (yt::InnerTube::pickStreams(pl.details, &muxed, &v, &a,
                                               targetHeightFor(cfg.quality))) {
                    platform::bootLog(
                        "streams: muxed itag=%d q='%s' url=%d | video itag=%d q='%s' url=%d | "
                        "audio itag=%d url=%d",
                        muxed.itag, muxed.qualityLabel.c_str(), muxed.url.empty() ? 0 : 1, v.itag,
                        v.qualityLabel.c_str(), v.url.empty() ? 0 : 1, a.itag, a.url.empty() ? 0 : 1);
                    if (muxed.url.empty() && v.url.empty()) {
                        pl.error = "Нет доступных потоков (нужен вход/PO-token)";
                        pl.loading = false;
                    } else {
                        auto& p = player::instance();
                        if (!pl.ready) {
                            pl.ready = p.init();
                            platform::bootLog("player init (late): %s", pl.ready ? "ok" : "ОШИБКА");
                        }
                        // Кадр рендерим сразу в разрешении экрана: тогда UI
                        // копирует его как есть (1:1), без bilinear-апскейла
                        // на каждый кадр. Масштабирование делает mpv (NEON).
                        p.setTargetSize(1280, 720);
                        p.setAudioDelay(cfg.audioDelayMs / 1000.0);
                        // Плееру нужны 2 SSL-сессии (видео + звук), а всего их 4.
                        // Закрываем простаивающие UI-соединения, иначе mpv
                        // ждёт рукопожатие и падает с «Failed to open».
                        net::closeIdleConnections();
                        // Всегда отдаём mpv локальный ретранслятор: он ходит в
                        // сеть через libcurl, где обход DPI (верхний регистр SNI
                        // + фрагментация) уже проверен. Так плеер не зависит от
                        // TLS-стека ffmpeg и настроек прокси.
                        const bool relay = dpi::relayPort() > 0;
                        // Адаптивные дорожки (видео 720p + отдельный AAC) лучше
                        // muxed itag 18 (360p), поэтому пробуем их первыми.
                        const bool adaptiveReady = !v.url.empty() && !a.url.empty();
                        const bool videoOnly = !v.url.empty() && a.url.empty() &&
                                               muxed.url.empty();
                        bool opened = false;
                        if (adaptiveReady || muxed.url.empty()) {
                            std::string vurl = relay ? dpi::relayUrl(v.url) : v.url;
                            platform::bootLog("player: open %s itag=%d q='%s' relay=%d src=%s",
                                              urlHost(vurl).c_str(), v.itag, v.qualityLabel.c_str(),
                                              relay ? 1 : 0, urlHost(v.url).c_str());
                            opened = p.open(vurl, pl.details.title);
                            if (opened && !a.url.empty()) {
                                std::string aurl = relay ? dpi::relayUrl(a.url) : a.url;
                                platform::bootLog("player: audio %s itag=%d",
                                                  urlHost(aurl).c_str(), a.itag);
                                p.addAudio(aurl);
                            }
                            pl.qualityLabel = v.qualityLabel.empty() ? "H.264" : v.qualityLabel;
                            if (opened && videoOnly) pl.qualityLabel += " (без звука)";
                        } else {
                            std::string url = relay ? dpi::relayUrl(muxed.url) : muxed.url;
                            platform::bootLog("player: open %s itag=%d q='%s' relay=%d src=%s",
                                              urlHost(url).c_str(), muxed.itag,
                                              muxed.qualityLabel.c_str(), relay ? 1 : 0,
                                              urlHost(muxed.url).c_str());
                            opened = p.open(url, pl.details.title);
                            pl.qualityLabel = muxed.qualityLabel.empty() ? "H.264" : muxed.qualityLabel;
                        }
                        if (!opened) {
                            // Раньше ошибка open() затиралась, и экран оставался
                            // чёрным без объяснения.
                            pl.error = "Не удалось открыть видео-поток";
                            pl.qualityLabel.clear();
                        } else {
                            pl.error.clear();
                        }
                        pl.loading = false;
                    }
                } else {
                    platform::bootLog("streams: pickStreams не нашёл потоков");
                    pl.error = "Видео недоступно для воспроизведения";
                    pl.loading = false;
                }
            } else {
                platform::bootLog("video: ok=0 id=%s err='%s'", pl.item.id.c_str(),
                                  async.errVideo.c_str());
                pl.error = async.errVideo.empty() ? "Ошибка загрузки видео" : async.errVideo;
                pl.loading = false;
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
            suggestions = async.sugg;
        }
    }

    // подсказки поиска с задержкой
    if (screen == Screen::Search && showSuggestions && query.size() >= 2) {
        suggestTimer += dt;
        if (suggestTimer > 0.35f) {
            suggestTimer = -1000.f;   // один раз на запрос
            std::string q = query;
            threads::spawn([this, q]() {
                auto s = yt.suggestions(q);
                std::lock_guard<std::mutex> lk(async.m);
                async.sugg = s;
                async.suggestGen++;
            });
        }
    }

    // ---- плеер ----
    if (screen == Screen::Player) {
        auto& p = player::instance();
        if (!pl.loading) {
            bool newFrame = p.update(plFrame);
            (void)newFrame;
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
        if (pl.controls) {
            pl.idle += dt;
            if (pl.idle > 4.5f) pl.controls = false;
        }

        // Ошибка плеера видна на экране, но раньше не попадала в лог — из-за
        // этого сбой видео нельзя было разобрать постфактум.
        if (pl.error != lastPlayerError) {
            lastPlayerError = pl.error;
            platform::bootLog("player error: '%s'", pl.error.empty() ? "(сброшена)" : pl.error.c_str());
        }
        if (!plLoggedPlaying && p.playing() && p.position() > 0.05) {
            plLoggedPlaying = true;
            platform::bootLog("player: воспроизведение пошло, pos=%.2f dur=%.2f", p.position(),
                              p.duration());
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
}

// ---------------------------------------------------------------- сетка
std::vector<const yt::VideoItem*> App::flatItems(const yt::Feed& f) const {
    std::vector<const yt::VideoItem*> out;
    for (auto& s : f.sections)
        for (auto& v : s.items) out.push_back(&v);
    return out;
}

App::GridMetrics App::metricsFor(float width) const {
    GridMetrics m;
    m.cols = width >= 1150.f ? 3 : (width >= 760.f ? 2 : 1);
    m.gap = 16.f;
    m.cardW = (width - (m.cols - 1) * m.gap) / (float)m.cols;
    m.thumbH = m.cardW * 9.f / 16.f;
    m.cardH = m.thumbH + 96.f;
    return m;
}

float App::gridContentHeight(const yt::Feed& f, float width) const {
    auto m = metricsFor(width);
    int n = (int)flatItems(f).size();
    int rows = (n + m.cols - 1) / m.cols;
    return rows * (m.cardH + m.gap) + 24.f;
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