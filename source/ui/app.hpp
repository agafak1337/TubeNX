// ui/app.hpp — состояние приложения и навигация.
#pragma once

#include "core/canvas.hpp"
#include "core/font.hpp"
#include "dpi/dpi.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"
#include "net/images.hpp"
#include "player/player.hpp"
#include "yt/innertube.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tnx::app {

enum class Screen { Splash, Home, Search, Player, Settings };

struct Config {
    bool dark = true;
    uint32_t seed = 0xFFB3261E;
    int quality = 1;       // 0 авто, 1 720p, 2 480p, 3 360p
    int speed = 100;
    // Сдвиг звука относительно видео, мс: + звук позже, − раньше.
    int audioDelayMs = 0;
    bool dpiEnabled = true;
    int dpiMode = 1;       // индекс в списке режимов
    bool autoplay = true;
    bool localReco = true; // локальные рекомендации (история на этой консоли)
    // Внешний прокси для видео: socks5h://host:port или http://host:port.
    // Нужен там, где провайдер блокирует сам CDN видео (googlevideo) — обход
    // по SNI в этом случае не помогает, трафик должен идти через туннель.
    std::string proxy;
};

struct HistoryEntry {
    std::string id, title, channel;
    double time = 0;        // позиция просмотра, с
    double duration = 0;    // длительность, с (0 — неизвестна)
    double watchedAt = 0;   // unix-время последнего просмотра
    bool liked = false;     // локальный «лайк»
};

// Приводим строку прокси к виду scheme://host:port (пустая — отключить).
std::string normalizeProxy(std::string p);

class App {
public:
    void init();
    void shutdown();
    void update(float dt);
    void draw(ui::Ui& u);

    // --- конфиг/тема ---
    Config cfg;
    md3::Theme theme;
    Fonts fonts;
    net::ImageCache images;
    yt::InnerTube yt;

    // --- навигация ---
    Screen screen = Screen::Splash;
    Screen prevScreen = Screen::Home;
    void go(Screen s);
    void back();

    // --- данные ---
    yt::Feed feed;              // главная/поиск
    std::string feedCont;
    bool feedLoading = false;
    bool feedOk = false;
    std::string feedError;
    int homeChip = 0;
    bool homeIsTrending = false;
    std::vector<std::string> chips;

    // поиск
    std::string query;
    std::vector<std::string> suggestions;
    bool showSuggestions = false;
    float suggestTimer = 0.f;
    yt::Feed searchFeed;
    std::string searchCont;
    bool searchLoading = false;
    std::string searchError;
    std::string lastQuery;

    // плеер
    struct PlayerUi {
        yt::VideoDetails details;
        yt::VideoItem item;
        bool visible = false;
        bool loading = false;
        std::string error;
        bool controls = true;
        float idle = 0.f;
        bool seeking = false;
        float seekPos = 0.f;
        bool fromHome = true;
        bool ready = false;             // libmpv инициализирован
        std::string qualityLabel;
        bool pip = false;
    } pl;

    // диагностика плеера: чтобы ошибка и факт старта видео попадали в лог
    std::string lastPlayerError;
    bool plLoggedPlaying = false;

    // очередь/история
    std::vector<HistoryEntry> history;

    // splash
    float splashT = 0.f;
    std::string splashStatus = "Запуск TubeNX…";
    bool splashProbed = false;
    int probeMs = -1;
    bool probeOk = false;
    bool bootDone = false;
    // Кэш статического фона сплэша: градиенты на 1280×720 стоили ~130 мс на
    // кадр, из-за чего сплэш шёл на 6 fps и отбирал ядро у сети. Фон рисуется
    // один раз и дальше просто копируется.
    Image splashBg;
    bool splashBgReady = false;

    // настройки/диагностика
    bool probing = false;
    float probeSpin = 0.f;
    int settingsFocus = 0;

    // тосты
    std::string toastText;
    float toastT = 0.f;

    void toast(const std::string& s, float seconds = 2.5f);

    // --- действия ---
    void loadHome(bool trending);
    void loadMore();
    void startSearch(const std::string& q, bool openKeyboard = false);
    void openVideo(const yt::VideoItem& item);
    void closePlayer();
    void probe(bool useBypass = true);
    void applyTheme();
    void saveConfig();
    void loadConfig();
    void setDpiEnabled(bool on);

    // --- локальные рекомендации (без сети и без аккаунта) ---
    // Пересортировать первую секцию ленты по аффинити каналов и истории.
    void rankFeed();
    // История, которую есть смысл продолжить (не досмотрено, не в самом начале).
    std::vector<const HistoryEntry*> continueWatching() const;
    void toggleLike(const std::string& videoId);
    bool isLiked(const std::string& videoId) const;
    float channelAffinity(const std::string& channel);
    int recoSignals() const { return (int)affinity_.size(); }

    // --- рисование экранов (screens.cpp) ---
    void drawSplash(ui::Ui& u);
    void drawHome(ui::Ui& u);
    void drawSearch(ui::Ui& u);
    void drawPlayer(ui::Ui& u);
    void drawSettings(ui::Ui& u);
    void drawChrome(ui::Ui& u);     // нижняя навигация
    void drawToasts(ui::Ui& u);
    void drawGrid(ui::Ui& u, uint64_t id, const Rect& area, const yt::Feed& f, bool loading,
                  const std::string& err, const char* emptyText, float scroll, float contentH);
    // Горизонтальная полоса «Продолжить просмотр» из локальной истории.
    void drawContinueStrip(ui::Ui& u, const Rect& r, const std::vector<const HistoryEntry*>& items);

    // разбор фида в плоский список видео (для сетки)
    std::vector<const yt::VideoItem*> flatItems(const yt::Feed& f) const;
    float gridContentHeight(const yt::Feed& f, float width) const;

    // сетка
    struct GridMetrics {
        int cols = 3;
        float gap = 16.f;
        float cardW = 0.f;
        float thumbH = 0.f;
        float cardH = 0.f;
    };
    GridMetrics metricsFor(float width) const;

    int navIndex = 0;
    std::string clock;

    // Результаты фоновых сетевых задач (поток → UI).
    struct Async {
        std::mutex m;
        std::atomic<int> feedGen{0}, searchGen{0}, videoGen{0}, probeGen{0}, suggestGen{0};
        yt::Feed feed, search;
        yt::VideoDetails video;
        std::vector<std::string> sugg;
        std::string errFeed, errSearch, errVideo;
        bool okFeed = false, okSearch = false, okVideo = false;
        int probeMs = -1;
        bool probeOk = false;
    } async;
    int seenFeedGen = 0, seenSearchGen = 0, seenVideoGen = 0, seenProbeGen = 0, seenSuggestGen = 0;

    std::unordered_map<std::string, float> affinity_;   // норм. канал → вес
    void rebuildAffinity();

    Image plFrame;              // текущий кадр видео (RGBA)
    void drawVideo(ui::Ui& u, const Rect& dst);

private:
    float uiTime_ = 0.f;
};

// вспомогательное: миллисекунды «сейчас» для анимаций
float nowSeconds();

} // namespace tnx::app