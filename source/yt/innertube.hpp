// yt/innertube.hpp — клиент InnerTube (внутренний API YouTube), без API-ключей.
//
// Поток «InnerTube/данные» (M2): YT-01..YT-09.
// Изменения M2:
//   * YT-01 — bootstrap-страница `/` не обязательна: ключ/visitor/region
//     добываются независимо (home → sw.js_data → публичный ключ/генерация).
//   * YT-02 — visitorData можно загрузить из конфига и переиспользовать
//     (setVisitorData + visitorLogId; запись в конфиг — зона UI).
//   * YT-03 — матрица клиентов (ANDROID_VR→ANDROID→IOS→TVHTML5) с fallback.
//   * YT-05 — playabilityStatus разбирается и маппится в понятный текст.
//   * YT-06 — пагинация с дедупом, лимитом 10 страниц и защитой от гонок.
//   * YT-07 — детерминированный выбор потоков (StreamSelection).
//   * YT-08 — refreshStreams() с одним повтором для истёкших ссылок.
//   * YT-09 — генерация URL обложек по id/качеству.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tnx::net {
struct Response;   // определён в net/http.hpp; нужен только как возвращаемый тип
class CancelToken; // NET-11: токен отмены (net/http.hpp)
} // namespace tnx::net

namespace tnx::yt {

struct VideoItem {
    std::string id;            // videoId
    std::string title;
    std::string channel;
    std::string channelId;
    std::string meta;          // "1,2 млн просмотров • 3 дня назад"
    std::string duration;      // "12:34" или "LIVE"
    std::string thumbnailUrl;
    int64_t viewCount = 0;
    int lengthSeconds = 0;
    bool live = false;
};

struct Section {
    std::string title;
    std::string subtitle;
    std::vector<VideoItem> items;
    bool chips = false;        // ряд фильтров
    std::vector<std::string> chipLabels;
};

struct StreamFormat {
    int itag = 0;
    std::string url;
    std::string mime;
    std::string qualityLabel;   // "720p"
    std::string audioQuality;   // "AUDIO_QUALITY_MEDIUM"
    std::string codecs;
    int64_t contentLength = 0;
    int bitrate = 0;
    int width = 0, height = 0;
    bool hasVideo = false, hasAudio = false;
    bool live = false;          // поток прямого эфира (YT-07: не выбирать)
    bool drm = false;           // защищён DRM (YT-07: не выбирать)
};

struct VideoDetails {
    std::string id, title, description, channel, channelId, channelThumb;
    std::string thumbnailUrl, publishedText, viewText, likeText, shareUrl;
    int64_t viewCount = 0;
    int lengthSeconds = 0;
    bool live = false;                 // прямой эфир (YT-07)
    // YT-05: статус воспроизводимости из /player.
    std::string playabilityStatus;     // "OK", "LOGIN_REQUIRED", ...
    std::string playabilityReason;     // текст от YouTube (если дан)
    std::vector<StreamFormat> formats;
    std::vector<VideoItem> related;
};

struct Feed {
    std::vector<Section> sections;
    std::string continuation;
    bool ok = false;
    std::string error;
};

// YT-02: то, что UI кладёт в конфиг и отдаёт назад (запись/чтение — зона UI).
// data — сам visitorData; savedAtEpoch — секунды Unix для TTL/ротации.
struct VisitorState {
    std::string data;
    int64_t savedAtEpoch = 0;
};

// YT-07: детерминированный выбор потоков.
//   Split — видео и звук отдельными дорожками (video+audio заполнены);
//   Muxed — один поток со звуком и видео (muxed заполнен);
//   None  — ничего пригодного нет.
enum class StreamPick { None, Split, Muxed };

struct StreamSelection {
    StreamPick mode = StreamPick::None;
    StreamFormat muxed;
    StreamFormat video;
    StreamFormat audio;
    std::string reason;   // человекочитаемое пояснение для лога
};

// YT-06: пагинация ленты. Дедуп по videoId, лимит страниц, защита от
// параллельных continuation. Живёт внутри InnerTube, но тестируется на хосте.
class FeedPager {
public:
    static constexpr int kMaxPages = 10;

    void reset();
    bool busy() const { return busy_.load(); }
    int pages() const { return pages_; }
    bool limitReached() const { return pages_ >= kMaxPages; }

    // Занять «страницу». false — уже идёт подгрузка или достигнут лимит.
    bool beginPage();
    // Успешная страница: снять флаг и увеличить счётчик.
    void endPage();
    // Неудачная страница: снять флаг, счётчик не трогать.
    void abortPage();

    // Оставить в ленте только ранее не встречавшиеся videoId.
    // Возвращает число добавленных (при 0 — продолжения больше нет).
    size_t dedup(Feed& feed);

private:
    std::unordered_set<std::string> seen_;
    std::atomic<bool> busy_{false};
    int pages_ = 0;
};

class InnerTube {
public:
    bool init(const std::string& region = "RU", const std::string& hl = "ru");

    Feed home();                                        // главная (browse FEwhat_to_watch)
    Feed trending();                                    // в тренде
    Feed search(const std::string& q, const std::string& continuation = "");
    Feed related(const std::string& videoId);
    Feed channel(const std::string& channelId);
    // YT-10: таймаут 3 с и отмена по токену — подсказки не должны ни
    // блокировать ввод, ни тянуться после смены запроса.
    std::vector<std::string> suggestions(const std::string& q,
                                         net::CancelToken* cancel = nullptr);
    bool video(const std::string& id, VideoDetails& out);
    Feed continueFeed(const std::string& continuation);

    // Выбор потоков. preferHeight — желаемая высота адаптивного видео
    // (720 по умолчанию: 1080p на 4x A57 в software-декодере не тянет).
    static bool pickStreams(const VideoDetails& v, StreamFormat* muxed, StreamFormat* video,
                            StreamFormat* audio, int preferHeight = 720);
    // YT-07: таблица приоритетов 136+140 → 18 → 22 → 135+140 с пояснением.
    static StreamSelection selectStreams(const VideoDetails& v, int preferHeight = 720);

    // YT-08: повторный /player и переселект (не более одного повтора).
    bool refreshStreams(const std::string& videoId, VideoDetails& out);
    // 403/410 — ссылка на googlevideo истекла (контракт для плеера).
    static bool isExpiredStatus(long httpStatus);

    // YT-05: маппинг кода playabilityStatus в понятный текст.
    static std::string playabilityText(const std::string& status);

    // YT-02: персистентность visitorData. savedAtEpoch — секунды Unix, когда
    // visitor был сохранён в конфиг (0 — неизвестно). Устаревший visitor
    // отбрасывается при инициализации.
    void setVisitorData(const std::string& v, int64_t savedAtEpoch = 0);
    const std::string& visitorData() const { return visitorData_; }
    // Снимок для конфига: visitor + время сохранения (UI пишет сам).
    VisitorState visitorState() const;
    // Короткий отпечаток для логов: сам visitorData не логируется целиком.
    std::string visitorLogId() const;
    bool visitorFromConfig() const { return visitorFromConfig_; }

    // NET-11: токен отмены для InnerTube-запросов. UI выставляет токен перед
    // новым поиском/лентой и вызывает cancel() при повторном запросе или уходе
    // с экрана; транспорт net::post прекращает запрос. nullptr — без отмены.
    // Указатель должен жить дольше запроса (обычно член UI-состояния).
    void setCancelToken(net::CancelToken* t) { cancel_.store(t, std::memory_order_relaxed); }
    net::CancelToken* cancelToken() const { return cancel_.load(std::memory_order_relaxed); }

    // YT-03: матрица клиентов и fallback.
    static int clientCount();
    static std::string clientName(int idx);
    static std::string clientVersionStr(int idx);
    // Health-check по browse с автоматическим переключением клиента.
    bool healthCheck();
    int browseClient() const { return browseClient_; }
    void resetClientFallback() { browseClient_ = kBrowseStart; }

    // YT-06: пагинация (дедуп/лимит/гонки).
    FeedPager& pager() { return pager_; }
    void resetContinuation() { pager_.reset(); }

    const std::string& error() const { return error_; }
    void setClientVersion(int idx);   // индекс в матрице клиентов
    int clientVersion() const { return clientVer_; }

private:
    static constexpr int kBrowseStart = 0;

    std::string region_ = "RU", hl_ = "ru", visitorData_, apiKey_, error_;
    std::string apiKeySource_, visitorSource_;
    bool visitorFromConfig_ = false;
    int64_t visitorSavedAtEpoch_ = 0;
    int clientVer_ = 0;        // клиент для browse/search/next
    int browseClient_ = 0;     // текущий клиент матрицы (после fallback)
    FeedPager pager_;
    // NET-11: активный токен отмены (к player-запросам не применяется).
    std::atomic<net::CancelToken*> cancel_{nullptr};

    std::string player(const std::string& videoId);
    bool fetchVideo(const std::string& id, VideoDetails& out, bool withRelated);
    Feed parseFeed(const std::string& json, bool isSearch);
    // Единая точка запроса InnerTube: поворот visitor на 400 и fallback клиента.
    net::Response apiPost(const std::string& endpoint,
                          const std::function<std::string(int)>& bodyFor, int clientIdx,
                          bool allowFallback);
    bool ensureSession();
    void rotateVisitor();
};

// YT-09: URL обложки по id и качеству.
//   0=default 1=mqdefault 2=hqdefault 3=sddefault 4=maxresdefault
std::string thumbUrl(const std::string& videoId, int quality = 2);
// Цепочка качеств от высокого к низкому (maxresdefault → … → default).
std::vector<std::string> thumbCandidates(const std::string& videoId, int fromQuality = 4);

std::string formatViews(int64_t n);
std::string formatDuration(int seconds);

} // namespace tnx::yt
