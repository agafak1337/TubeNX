// ui/app.hpp — состояние приложения и навигация.
#pragma once

#include "core/canvas.hpp"
#include "core/font.hpp"
#include "dpi/dpi.hpp"
#include "md3/theme.hpp"
#include "md3/ui.hpp"
#include "net/images.hpp"
#include "player/player.hpp"
#include "ui/proxy.hpp"
#include "yt/innertube.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tnx::net {
class CancelToken;   // NET-11: токен отмены (net/http.hpp)
} // namespace tnx::net

namespace tnx::app {

enum class Screen { Splash, Home, Search, Player, Settings };

struct Config {
    bool dark = true;
    uint32_t seed = 0xFFC3002F;
    int quality = 1;       // 0 авто, 1 720p, 2 480p, 3 360p
    int speed = 100;
    // Сдвиг звука относительно видео, мс: + звук позже, − раньше.
    int audioDelayMs = 0;
    bool dpiEnabled = true;
    int dpiMode = 1;       // индекс в списке режимов
    // YT-02: visitorData персистентный — иначе каждая сессия считалась бы
    // «новичком». Храним и время сохранения (секунды Unix) для TTL/ротации.
    std::string visitor;
    long long visitorSavedAt = 0;
    bool autoplay = true;
    int homeChip = 0;      // UX-01: выбранный чип главной (сохраняется)
    bool localReco = true; // локальные рекомендации (история на этой консоли)
    // UX-14: первый запуск — короткий онбординг на сплэше. Сбрасывается
    // кнопкой «Продолжить»; при наличии истории пропускается.
    bool firstRun = true;
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

#include "ui/proxy.hpp"

// Приводим строку прокси к виду scheme://host:port (пустая — отключить).
// UX-09: normalizeProxy/validateProxy живут в ui/proxy.hpp (host-тестируемо).

class App {
public:
    void init();
    void shutdown();
    void update(float dt);
    void draw(ui::Ui& u);

    // --- конфиг/тема ---
    Config cfg;
    // CFG-02: ключи, которых нет в Config (новая версия/ручная правка) —
    // сохраняем обратно, чтобы обновление их не стирало.
    std::vector<std::pair<std::string, std::string>> extraConfig;
    // CFG-06: SD недоступна на запись — настройки/история живут в памяти.
    bool storageReadOnly = false;
    md3::Theme theme;
    Fonts fonts;
    net::ImageCache images;
    yt::InnerTube yt;

    // --- навигация ---
    Screen screen = Screen::Splash;
    Screen prevScreen = Screen::Home;
    // UX-11: стек экранов. `B` возвращает строго туда, откуда пришли, а не
    // всегда на Home. Ограничен, чтобы не расти бесконечно.
    std::vector<Screen> navStack;
    void go(Screen s);
    void back();
    // Смена экрана без записи в стек (для возврата по стеку и служебных
    // переходов). Только здесь — побочные эффекты ухода с экрана.
    void switchTo(Screen s);

    // --- данные ---
    yt::Feed feed;              // главная/поиск
    std::string feedCont;
    bool feedLoading = false;
    bool feedOk = false;
    // UX-02: сторож загрузки — не показываем бесконечный скелетон.
    float feedLoadT = 0.f;
    float searchLoadT = 0.f;
    float videoLoadT = 0.f;
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
    // NET-11: id последнего запущенного поиска/ленты. Поздний ответ от
    // устаревшего запроса не применяем (в транспорте его отменяет CancelToken,
    // когда yt:: его пробрасывает).
    std::atomic<int> searchReqId{0};
    std::atomic<int> feedReqId{0};
    // NET-11: токены отмены текущих запросов. Живут в App (указатель должен
    // пережить запрос); при новом запросе/уходе с экрана — cancel().
    std::shared_ptr<net::CancelToken> searchTok;
    std::shared_ptr<net::CancelToken> feedTok;

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
        // UX-10: позиция, с которой продолжить при открытии (0 — с начала).
        double pendingResumeSec = 0.0;
        // UX-05: открыто меню/диалог плеера — контролы не прячем.
        bool menusOpen = false;
        // UX-06: удерживаем прогресс на цели seek, пока mpv не догонит, чтобы
        // полоса не откатывалась назад на 1–2 кадра.
        double seekTarget = -1.0;
        float seekHoldT = 0.f;
        // UX-05: меню/диалог плеера (перенесены из screens.cpp, чтобы update и
        // openVideo могли сбрасывать их при смене видео).
        bool qualityMenuOpen = false;
        bool moreMenuOpen = false;
        bool closeDialogOpen = false;
    } pl;

    // диагностика плеера: чтобы ошибка и факт старта видео попадали в лог
    std::string lastPlayerError;
    bool plLoggedPlaying = false;

    // UX-08: экран диагностики DPI — режим, порты, RTT, bootstrap и последние
    // 5 ошибок (кольцевой буфер) + экспорт лога в файл.
    std::vector<std::string> diagErrors;
    void recordDiagError(const std::string& e);
    void exportDiagnostics();

    // UX-10: периодическое сохранение позиции просмотра.
    float histSaveT = 0.f;
    bool lastPaused = false;

    // очередь/история
    std::vector<HistoryEntry> history;

    // splash
    float splashT = 0.f;
    std::string splashStatus = "Запуск TubeNX…";
    bool splashProbed = false;
    int probeMs = -1;
    bool probeOk = false;
    bool bootDone = false;
    int bootProbeGen = 0;   // на какой пробе уже отработал старт (для «Повторить»)
    // Кэш статического фона сплэша: градиенты на 1280×720 стоили ~130 мс на
    // кадр, из-за чего сплэш шёл на 6 fps и отбирал ядро у сети. Фон рисуется
    // один раз и дальше просто копируется.
    Image splashBg;
    bool splashBgReady = false;

    // настройки/диагностика
    bool probing = false;
    float probeSpin = 0.f;
    int settingsFocus = 0;
    // DPI-06: идёт автоподбор режима фрагментации (кнопка «Подобрать автоматически»).
    bool dpiAutoSelecting = false;

    // тосты
    std::string toastText;
    float toastT = 0.f;
    float toastTotal = 0.f;
    // UX-12: очередь тостов — показываем по одному, не наслаивая.
    struct ToastItem {
        std::string text;
        float seconds = 2.5f;
    };
    std::vector<ToastItem> toastQueue;

    void toast(const std::string& s, float seconds = 2.5f);

    // --- действия ---
    void loadHome(bool trending);
    void loadMore();
    void startSearch(const std::string& q, bool openKeyboard = false);
    void openVideo(const yt::VideoItem& item);
    void closePlayer();
    // PLR-14: сменить качество на лету — перезапуск потока с той же позиции.
    void switchPlayerQuality(int qi);
    void probe(bool useBypass = true);
    // DPI-06: перебрать режимы фрагментации (Off→Split→MultiSplit→TlsRecordSplit)
    // пробой generate_204, запомнить первый рабочий в конфиг. Не меняет режим,
    // пока идёт видео (так решает dpi::autoSelectMode).
    void autoSelectDpi();
    // Повторная проверка соединения с офлайн-сплэша (§5.4): сбрасывает
    // состояние загрузки и заново запускает пробу и загрузку ленты.
    void retryBoot();
    void applyTheme();
    void saveConfig();
    void loadConfig();
    // CFG-07: сброс настроек к значениям по умолчанию (история сохраняется).
    void resetSettings();
    // UX-10: сохранить только историю (раз в 5 с во время просмотра).
    void saveHistory();
    // UX-10: убрать запись истории (кнопка «×» на карточке «Продолжить»).
    void removeHistory(const std::string& id);
    void setDpiEnabled(bool on);
    // UX-09: применить внешний прокси (пустая строка — отключить). Валидирует,
    // пишет конфиг и переключает транспорт. Возвращает false и заполняет err.
    bool setProxy(const std::string& raw, std::string& err);

    // --- локальные рекомендации (без сети и без аккаунта) ---
    // Пересортировать первую секцию ленты по аффинити каналов и истории.
    void rankFeed();
    // История, которую есть смысл продолжить (не досмотрено, не в самом начале).
    std::vector<const HistoryEntry*> continueWatching() const;
    void toggleLike(const std::string& videoId);
    bool isLiked(const std::string& videoId) const;
    float channelAffinity(const std::string& channel);
    int recoSignals() const { return (int)affinity_.size(); }
    // YT-11: 2–3 канала, сильнее всего влияющих на порядок ленты (для строки
    // «почему так отсортировано» в настройках). Имена — как в истории.
    std::vector<std::pair<std::string, float>> topChannels(int n);

    // --- рисование экранов (screens.cpp) ---
    void drawSplash(ui::Ui& u);
    void drawHome(ui::Ui& u);
    void drawSearch(ui::Ui& u);
    void drawPlayer(ui::Ui& u);
    void drawSettings(ui::Ui& u);
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
        int cols = 4;
        float gap = 16.f;       // межколоночный (app.css .grid gap 24/16)
        float rowGap = 24.f;    // межстрочный
        float cardW = 0.f;
        float thumbH = 0.f;
        float cardH = 0.f;
    };
    GridMetrics metricsFor(float width) const;

    std::string clock;

    // Результаты фоновых сетевых задач (поток → UI).
    struct Async {
        std::mutex m;
        std::atomic<int> feedGen{0}, searchGen{0}, videoGen{0}, probeGen{0}, suggestGen{0};
        std::atomic<int> refreshGen{0};   // YT-08: обновление истёкших ссылок
        std::atomic<int> dpiAutoGen{0}, visitorGen{0};
        yt::Feed feed, search;
        yt::VideoDetails video;
        std::vector<std::string> sugg;
        std::string suggQuery;   // UX-04: к какому запросу относятся подсказки
        // YT-10: токен текущего запроса подсказок — следующий ввод отменяет
        // предыдущий, а результат отменённого запроса не применяется.
        std::shared_ptr<net::CancelToken> suggestCancel;
        std::string errFeed, errSearch, errVideo;
        bool okFeed = false, okSearch = false, okVideo = false;
        yt::VideoDetails refreshed;       // YT-08: /player после 403/410
        bool okRefresh = false;
        int probeMs = -1;
        bool probeOk = false;
        // DPI-06: результат автоподбора режима.
        bool dpiAutoOk = false;
        int dpiAutoMode = -1;
        int dpiAutoTried = 0;
        std::string dpiAutoErr;
        // YT-02: снимок visitorData после init (пишем в конфиг из UI-потока).
        std::string visitor;
        long long visitorSavedAt = 0;
    } async;
    int seenFeedGen = 0, seenSearchGen = 0, seenVideoGen = 0, seenProbeGen = 0, seenSuggestGen = 0;
    int seenRefreshGen = 0;      // YT-08
    bool streamRefreshTried = false;
    double refreshPos = 0.0;
    int seenDpiAutoGen = 0, seenVisitorGen = 0;

    std::unordered_map<std::string, float> affinity_;   // норм. канал → вес
    void rebuildAffinity();

    // YT-02: снять текущий visitor после сетевого вызова (фоновый поток) и
    // отдать в UI-поток для записи в конфиг (ротация при 400 тоже сохранится).
    void captureVisitor();

    Image plFrame;              // текущий кадр видео (RGBA)
    void drawVideo(ui::Ui& u, const Rect& dst);

    // PL-02: снять флаг «нужна перерисовка». update() выставляет его, когда
    // изменилось состояние (экран, данные, ошибки, тосты) или идёт анимация
    // (спиннеры загрузки, тост, видео-кадр, догрузка обложек). Пока флага
    // нет, главный цикл не рисует кадр — статичный экран не жжёт CPU.
    bool takeDirty();

private:
    // PLR-14: подобрать потоки под высоту и открыть их в плеере. false —
    // потоков нет, причина в pl.error.
    bool openPlayerStreams(int targetHeight, double startAt);

    float uiTime_ = 0.f;
    bool dirty_ = true;          // PL-02
    bool plFrameDirty_ = false;  // PL-02: плеер отдал новый кадр
    int lastPlayerState_ = -1;   // PL-02: смена состояния плеера меняет картинку
    int lastImagePending_ = -1;  // PL-02: обложка догрузилась — нужен кадр
};

// вспомогательное: миллисекунды «сейчас» для анимаций
float nowSeconds();

} // namespace tnx::app