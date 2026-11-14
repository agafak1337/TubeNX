// player/player.cpp — бэкенд воспроизведения на libmpv (software-рендер в Image).
//
// Архитектура:
//   * mpv core живёт в своих внутренних потоках (demux/decode/ao), наружу торчит
//     только mpv_handle и mpv_render_context (API рендера).
//   * Все вызовы mpv_* (команды, свойства, события, render) выполняются из
//     главного (UI) потока. Отдельного «плеерного» потока нет: события
//     вычитываются неблокирующим mpv_wait_event(0) в pumpEvents(), кадр
//     забирается по флагу MPV_RENDER_UPDATE_FRAME в update().
//   * Двойная буферизация не нужна: mpv_render_context_render() пишет кадр в
//     промежуточный rgb0-буфер (R,G,B,X), который затем конвертируется в
//     Image::px формата 0xAABBGGRR. Буфер и render-контекст защищены
//     std::mutex — render-контекст нельзя пересоздавать/рендерить одновременно,
//     а mpv может дёргать свои callback'и из собственных потоков.
//   * Аудио отдаётся через AO-драйвер порта (audren API, драйвер называется
//     "hos"); при его отсутствии mpv падает на sdl/null. Реально выбранный AO
//     и наличие аудиодорожки фиксируются в Impl::audioOk — основа для
//     UI-предупреждения (в player.hpp отдельного геттера нет).
//
// Потокобезопасность: публичные методы Player вызываются из главного потока.

#include "player/player.hpp"
#include "platform.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace tnx::player {
namespace {

// Строковые константы лежат в изменяемой памяти: mpv_render_param.data — void*.
char kRenderApiSw[] = MPV_RENDER_API_TYPE_SW;
char kRenderFormat[] = "rgb0";

constexpr int kDefaultWidth = 960;
constexpr int kDefaultHeight = 540;
constexpr int kMinWidth = 64;
constexpr int kMinHeight = 36;
constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1080;

// Ограничение спама в boot-лог: не больше стольких строк mpv на один open().
constexpr int kMaxMpvLogLines = 80;
constexpr size_t kMaxMpvLogBytes = 180;
constexpr size_t kMaxTitleBytes = 80;

// Строка лога mpv приходит с завершающим '\n'; убираем хвостовые переводы строк.
std::string cleanLogLine(const char* text) {
    if (!text) return std::string();
    std::string s(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// mpv может прислать многострочное сообщение, а bootLog пишет по строке на
// запись — внутренние переводы строк заменяем пробелами.
std::string sanitizeLogLine(const char* text) {
    std::string s = cleanLogLine(text);
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

// Обрезка длинной строки для лога. Режем по байтам, но не посередине
// utf8-последовательности: отступаем от границы до ведущего байта.
std::string truncateLog(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

// Только схема и хост URL: в query лежат токены, в пути — подпись, их в лог
// писать нельзя. Для локального ретранслятора вернёт http://127.0.0.1:8899.
std::string urlSchemeHost(const std::string& url) {
    size_t p = url.find("://");
    if (p == std::string::npos) return std::string("(без схемы)");
    size_t start = p + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    // user:pass@host — креденшелы тоже не логируем.
    size_t at = url.rfind('@', end);
    if (at != std::string::npos && at >= start) start = at + 1;
    return url.substr(0, end);
}

const char* endFileReasonName(int reason) {
    switch (reason) {
    case MPV_END_FILE_REASON_EOF: return "EOF";
    case MPV_END_FILE_REASON_STOP: return "STOP";
    case MPV_END_FILE_REASON_QUIT: return "QUIT";
    case MPV_END_FILE_REASON_ERROR: return "ERROR";
    case MPV_END_FILE_REASON_REDIRECT: return "REDIRECT";
    default: return "?";
    }
}

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Строка rgb0 должна быть выровнена на 64 байта — иначе mpv уходит в медленный
// путь (копирование всего кадра) на SIMD-скалере.
int alignUp(int v, int a) {
    return (v + a - 1) / a * a;
}

// PLR-01: имена состояний для лога/UI.
const char* stateNameOf(int s) {
    switch (static_cast<Player::State>(s)) {
    case Player::State::Idle: return "Idle";
    case Player::State::Opening: return "Opening";
    case Player::State::FileLoaded: return "FileLoaded";
    case Player::State::Playing: return "Playing";
    case Player::State::Paused: return "Paused";
    case Player::State::Ended: return "Ended";
    case Player::State::Error: return "Error";
    }
    return "?";
}

// PLR-05: дебаунс seek — не чаще раза в 200 мс.
constexpr int kSeekDebounceMs = 200;
// PLR-01: сколько шаг Opening может висеть без file-loaded/ошибки.
constexpr int kOpenTimeoutSec = 45;
// PLR-12: период строки метрик и лимит строк на сессию плеера.
constexpr int kMetricsPeriodMs = 5000;
constexpr int kMetricsMaxLines = 200;

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Player::Impl {
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;
    bool initialized = false;

    // Кадровый буфер. frameMtx защищает frameBuf/stride/renderCtx: рендер и
    // пересоздание контекста (setTargetSize) не должны пересекаться, а mpv
    // зовёт update-callback из своего потока.
    mutable std::mutex frameMtx;
    std::vector<uint8_t> frameBuf;  // rgb0, stride * targetH + запас
    int targetW = kDefaultWidth;
    int targetH = kDefaultHeight;
    size_t stride = 0;
    bool forceRedraw = false;       // после ресайза нужен один внеочередной кадр

    // Логическое состояние. Всё меняется в главном потоке; stateMtx защищает
    // строки/флаги от гипотетического параллельного чтения из UI.
    mutable std::mutex stateMtx;
    std::string displayTitle;       // заголовок, переданный в open()
    std::string lastError;          // фатальная ошибка (loadfile/END_FILE)
    std::string lastLogError;       // последняя строка уровня error из лога mpv
    std::string lastReportedError;  // последняя ошибка, уже отданная в bootLog
    std::string pendingAudio;       // audio-add, отложенный до FILE_LOADED
    int mpvLogCount = 0;            // строк mpv-лога, записанных за текущий open()
    bool mpvLogLimitReported = false;
    bool fileLoaded = false;
    bool starting = false;          // loadfile отправлен, FILE_LOADED ещё не пришёл
    bool startSeen = false;         // START_FILE для ТЕКУЩЕГО loadfile уже пришёл
    int audioRetries = 0;           // повтор audio-add после неудачи
    bool eofReached = false;
    bool audioOk = false;
    double audioDelay = 0.0;        // сдвиг звука, с (+ позже, − раньше)

    // PLR-01: машина состояний и generation открытия.
    State st = State::Idle;
    uint64_t gen = 0;
    // PLR-14: позиция, к которой нужно перемотать после FILE_LOADED.
    double pendingSeek = 0.0;
    // PLR-01: начало шага Opening для таймаута.
    std::chrono::steady_clock::time_point openStartedAt{};
    // PLR-07: первая аудиодорожка найдена после file-loaded.
    bool audioTrackSeen = false;
    // PL-03/PLR-11.
    bool sleeping = false;
    bool wasPlayingBeforeSleep = false;
    // PLR-05.
    std::chrono::steady_clock::time_point lastSeekAt{};
    // PLR-12.
    std::chrono::steady_clock::time_point lastMetricsAt{};
    int metricLines = 0;

    void transition(State next, const char* why);
    void selectTracksFromFile();
    void logMetrics();
    void setMediaPlayback(bool on);

    void setError(const std::string& msg);
    bool getFlag(const char* name, bool& out) const;
    bool getInt(const char* name, int64_t& out) const;
    bool getDouble(const char* name, double& out) const;
    std::string getString(const char* name) const;

    void pumpEvents();
    void refreshAudioState();
    bool applyAudioAdd(const std::string& url);
    bool createRenderContextLocked();
};

void Player::Impl::setError(const std::string& msg) {
    bool report = false;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        lastError = msg;
        // Одинаковые подряд ошибки не повторяем: одна строка на причину.
        if (msg != lastReportedError) {
            lastReportedError = msg;
            report = true;
        }
    }
    if (report) platform::bootLog("player error: %s", msg.c_str());
}

// PLR-01: переход машины состояний. Вызывается без stateMtx (пишет лог).
void Player::Impl::transition(State next, const char* why) {
    State prev;
    uint64_t genNow = 0;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        prev = st;
        if (prev == next) return;
        st = next;
        if (next == State::Opening) openStartedAt = std::chrono::steady_clock::now();
        genNow = gen;
    }
    if (why && *why) {
        platform::bootLog("player: state %s -> %s (%s, gen %llu)",
                          stateNameOf(static_cast<int>(prev)), stateNameOf(static_cast<int>(next)),
                          why, static_cast<unsigned long long>(genNow));
    } else {
        platform::bootLog("player: state %s -> %s (gen %llu)",
                          stateNameOf(static_cast<int>(prev)), stateNameOf(static_cast<int>(next)),
                          static_cast<unsigned long long>(genNow));
    }
}

// PLR-07: после file-loaded явно выбираем первую video/audio дорожку.
void Player::Impl::selectTracksFromFile() {
    if (!mpv) return;
    int64_t count = 0;
    if (!getInt("track-list/count", count) || count <= 0) return;

    int64_t firstVideo = -1, firstAudio = -1;
    int audioTracks = 0;
    for (int64_t i = 0; i < count; ++i) {
        char key[64];
        std::snprintf(key, sizeof(key), "track-list/%lld/type", static_cast<long long>(i));
        const std::string type = getString(key);
        std::snprintf(key, sizeof(key), "track-list/%lld/id", static_cast<long long>(i));
        int64_t id = 0;
        if (!getInt(key, id)) continue;
        if (type == "video" && firstVideo < 0) firstVideo = id;
        if (type == "audio") {
            ++audioTracks;
            if (firstAudio < 0) firstAudio = id;
        }
    }

    if (firstVideo >= 0) {
        mpv_set_property(mpv, "vid", MPV_FORMAT_INT64, &firstVideo);
    }
    if (firstAudio >= 0) {
        mpv_set_property(mpv, "aid", MPV_FORMAT_INT64, &firstAudio);
    }
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        audioTrackSeen = audioTracks > 0;
    }
    if (audioTracks == 0) {
        platform::bootLog("player: в файле нет аудиодорожки (multiplexed fallback)");
    } else {
        platform::bootLog("player: дорожки vid=%lld aid=%lld",
                          static_cast<long long>(firstVideo), static_cast<long long>(firstAudio));
    }
}

// PLR-12: раз в 5 секунд — одна строка метрик (не больше 200 строк за сессию).
void Player::Impl::logMetrics() {
    const auto now = std::chrono::steady_clock::now();
    if (lastMetricsAt != std::chrono::steady_clock::time_point{} &&
        now - lastMetricsAt < std::chrono::milliseconds(kMetricsPeriodMs)) {
        return;
    }
    lastMetricsAt = now;
    if (metricLines >= kMetricsMaxLines) return;
    ++metricLines;

    double pos = 0.0, cache = 0.0, av = 0.0;
    int64_t dropped = 0, vbitrate = 0, abitrate = 0;
    getDouble("time-pos", pos);
    getDouble("demuxer-cache-time", cache);
    getDouble("avsync", av);
    getInt("frame-drop-count", dropped);
    getInt("video-bitrate", vbitrate);
    getInt("audio-bitrate", abitrate);
    platform::bootLog(
        "player metrics: pos=%.1f cache=%.1f dropped=%lld avsync=%+.3f vbitrate=%lld abitrate=%lld",
        pos, cache, static_cast<long long>(dropped), av, static_cast<long long>(vbitrate),
        static_cast<long long>(abitrate));
}

void Player::Impl::setMediaPlayback(bool on) { platform::setMediaPlaybackState(on); }

bool Player::Impl::getFlag(const char* name, bool& out) const {
    if (!mpv) return false;
    int v = 0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &v) < 0) return false;
    out = (v != 0);
    return true;
}

bool Player::Impl::getInt(const char* name, int64_t& out) const {
    if (!mpv) return false;
    int64_t v = 0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v) < 0) return false;
    out = v;
    return true;
}

bool Player::Impl::getDouble(const char* name, double& out) const {
    if (!mpv) return false;
    double v = 0.0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &v) < 0) return false;
    out = v;
    return true;
}

std::string Player::Impl::getString(const char* name) const {
    if (!mpv) return std::string();
    char* raw = mpv_get_property_string(mpv, name);
    if (!raw) return std::string();
    std::string s(raw);
    mpv_free(raw);
    return s;
}

void Player::Impl::pumpEvents() {
    if (!mpv) return;

    std::string audioToAdd;
    // PLR-01: переходы применяем после отпускания stateMtx (transition пишет лог).
    std::vector<std::pair<State, const char*>> stateChanges;
    bool doSelectTracks = false;
    bool doPendingSeek = false;
    double seekTo = 0.0;
    // Строки для boot-лога копим здесь, а пишем уже без stateMtx: bootLog
    // делает файловый ввод-вывод, держать на нём мьютекс состояния не нужно.
    std::vector<std::string> logLines;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        for (;;) {
            mpv_event* ev = mpv_wait_event(mpv, 0.0);
            if (!ev || ev->event_id == MPV_EVENT_NONE) break;

            switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                const auto* lm = static_cast<const mpv_event_log_message*>(ev->data);
                if (!lm) break;
                const std::string line = sanitizeLogLine(lm->text);
                if (line.empty()) break;

                if (lm->log_level <= MPV_LOG_LEVEL_ERROR) {
                    lastLogError = lm->prefix ? (std::string(lm->prefix) + ": " + line) : line;
                }
                // warn и выше — в boot-лог: именно эти строки объясняют, почему
                // не открылся поток/декодер/аудио. Спам ограничен на open().
                if (lm->log_level <= MPV_LOG_LEVEL_WARN) {
                    if (mpvLogCount < kMaxMpvLogLines) {
                        ++mpvLogCount;
                        std::string entry = "mpv[";
                        entry += lm->prefix ? lm->prefix : "?";
                        entry += "]: ";
                        entry += truncateLog(line, kMaxMpvLogBytes);
                        logLines.push_back(std::move(entry));
                    } else if (!mpvLogLimitReported) {
                        mpvLogLimitReported = true;
                        logLines.emplace_back("mpv: лог-лимит исчерпан");
                    }
                }
                break;
            }
            case MPV_EVENT_START_FILE:
                eofReached = false;
                startSeen = true;
                break;
            case MPV_EVENT_FILE_LOADED: {
                // FILE_LOADED без START_FILE — «хвост» предыдущего файла:
                // open() уже выдал новый loadfile, и audio-add попал бы в
                // старую запись плейлиста, а новое видео осталось без звука.
                if (!startSeen) {
                    logLines.emplace_back("mpv: stale file-loaded (нет start-file), игнор");
                    break;
                }
                startSeen = false;
                fileLoaded = true;
                starting = false;
                eofReached = false;
                lastError.clear();
                lastLogError.clear();
                audioOk = false;
                // DASH: аудио, добавленное до готовности файла, ставим сейчас.
                audioToAdd.swap(pendingAudio);
                if (audioToAdd.empty()) doSelectTracks = true;
                if (pendingSeek > 0.0) {
                    doPendingSeek = true;
                    seekTo = pendingSeek;
                    pendingSeek = 0.0;
                }
                stateChanges.push_back({State::FileLoaded, "file-loaded"});
                logLines.emplace_back("mpv: file loaded");
                // Свойства могут быть недоступны (аудио-only, битый поток) —
                // это не ошибка, просто не пишем строку.
                int64_t vw = 0;
                int64_t vh = 0;
                if (getInt("video-params/w", vw) && getInt("video-params/h", vh) && vw > 0 && vh > 0) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "mpv: video-params w=%lld h=%lld",
                                  static_cast<long long>(vw), static_cast<long long>(vh));
                    logLines.emplace_back(buf);
                }
                const std::string vcodec = getString("video-codec");
                if (!vcodec.empty()) logLines.push_back("mpv: video-codec='" + truncateLog(vcodec, 80) + "'");
                const std::string acodec = getString("audio-codec-name");
                if (!acodec.empty()) logLines.push_back("mpv: audio-codec-name='" + truncateLog(acodec, 80) + "'");
                break;
            }
            case MPV_EVENT_END_FILE: {
                const auto* ef = static_cast<const mpv_event_end_file*>(ev->data);
                const int reason = ef ? ef->reason : -1;
                const int err = ef ? ef->error : 0;
                char buf[320];
                std::snprintf(buf, sizeof(buf), "mpv: end-file reason=%d(%s) err=%d '%s'",
                              reason, endFileReasonName(reason), err,
                              err != 0 ? mpv_error_string(err) : "");
                logLines.emplace_back(buf);
                if (!lastLogError.empty()) {
                    logLines.push_back("mpv: end-file lastLogError='" +
                                       truncateLog(lastLogError, kMaxMpvLogBytes) + "'");
                }
                fileLoaded = false;
                audioOk = false;
                if (ef && ef->reason == MPV_END_FILE_REASON_EOF) {
                    eofReached = true;
                    starting = false;
                    stateChanges.push_back({State::Ended, "eof"});
                } else if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) {
                    eofReached = false;
                    starting = false;
                    std::string detail = lastLogError;
                    if (detail.empty() && ef->error != 0) detail = mpv_error_string(ef->error);
                    lastError = detail.empty() ? std::string("ошибка воспроизведения") : detail;
                    stateChanges.push_back({State::Error, "end-file error"});
                } else {
                    // STOP/REDIRECT: старый файл выгружается при "replace".
                    // starting не сбрасываем — новый файл ещё не загружен,
                    // иначе addAudio(), вызванный сразу после open(), потеряется.
                    eofReached = false;
                }
                break;
            }
            case MPV_EVENT_SHUTDOWN:
                logLines.emplace_back("mpv: shutdown");
                fileLoaded = false;
                starting = false;
                stateChanges.push_back({State::Idle, "shutdown"});
                break;
            case MPV_EVENT_PLAYBACK_RESTART: {
                // PLR-05/06: после загрузки/seek mpv возобновил воспроизведение.
                bool isPaused = false;
                if (getFlag("pause", isPaused) && isPaused) {
                    stateChanges.push_back({State::Paused, "playback-restart"});
                } else if (fileLoaded) {
                    stateChanges.push_back({State::Playing, "playback-restart"});
                }
                break;
            }
            default:
                // Остальные события (property-change, seek, video-reconfig и
                // т.п.) нам не нужны: состояние читаем свойствами по запросу.
                break;
            }
        }
    }

    for (const auto& change : stateChanges) transition(change.first, change.second);

    // PLR-01: таймаут шага открытия. На мёртвой сети mpv не всегда отдаёт
    // end-file с ошибкой, а UI не должен вечно показывать «Загрузка».
    bool openTimedOut = false;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        if (st == State::Opening && openStartedAt.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - openStartedAt >
                std::chrono::seconds(kOpenTimeoutSec)) {
            starting = false;
            lastError = "таймаут открытия потока";
            openTimedOut = true;
        }
    }
    if (openTimedOut) {
        transition(State::Error, "open timeout");
        platform::bootLog("player: Opening дольше %d c — поток остановлен", kOpenTimeoutSec);
        const char* args[] = {"stop", nullptr};
        mpv_command(mpv, args);
    }

    if (doSelectTracks) selectTracksFromFile();
    if (doPendingSeek && mpv) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", seekTo);
        const char* args[] = {"seek", buf, "absolute+keyframes", nullptr};
        const int rc = mpv_command(mpv, args);
        platform::bootLog("player: seek после open → %.1f c, rc=%d", seekTo, rc);
    }

    for (const std::string& line : logLines) platform::bootLog("%s", line.c_str());
    if (!audioToAdd.empty() && !applyAudioAdd(audioToAdd)) {
        // Не теряем дорожку: одна повторная попытка на следующем FILE_LOADED.
        std::lock_guard<std::mutex> lk(stateMtx);
        if (audioRetries < 1) {
            ++audioRetries;
            pendingAudio = audioToAdd;
        }
    }
}

bool Player::Impl::applyAudioAdd(const std::string& url) {
    if (!mpv || url.empty()) return false;
    const char* args[] = {"audio-add", url.c_str(), "select", nullptr};
    int rc = mpv_command(mpv, args);
    platform::bootLog("player audio-add: rc=%d%s url=%s", rc,
                      rc < 0 ? mpv_error_string(rc) : "", urlSchemeHost(url).c_str());
    if (rc < 0) {
        setError(std::string("audio-add: ") + mpv_error_string(rc));
        return false;
    }
    // "select" уже выбрал дорожку. aid=auto перезапускает авто-выбор и на
    // файле со встроенным звуком вернёт встроенную дорожку — ставим только
    // если выбор действительно пуст.
    const std::string aidNow = getString("aid");
    if (aidNow.empty() || aidNow == "no") mpv_set_property_string(mpv, "aid", "auto");
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        audioTrackSeen = true;
    }
    refreshAudioState();
    return true;
}

void Player::Impl::refreshAudioState() {
    bool loadedNow = false;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        loadedNow = fileLoaded;
    }
    bool ok = false;
    if (mpv && loadedNow) {
        const std::string ao = getString("current-ao");
        int64_t channels = 0;
        const bool hasTrack = getInt("audio-params/channel-count", channels) && channels > 0;
        ok = hasTrack && !ao.empty() && ao != "null" && ao != "no";
    }
    std::lock_guard<std::mutex> lk(stateMtx);
    audioOk = ok;
}

bool Player::Impl::createRenderContextLocked() {
    if (!mpv) return false;
    if (renderCtx) return true;

    targetW = clampInt(targetW, kMinWidth, kMaxWidth);
    targetH = clampInt(targetH, kMinHeight, kMaxHeight);
    stride = static_cast<size_t>(alignUp(targetW * 4, 64));
    // +64 байта: scaler вправе писать в padding после последней строки.
    frameBuf.assign(stride * static_cast<size_t>(targetH) + 64, 0);

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, kRenderApiSw},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* ctx = nullptr;
    int rc = mpv_render_context_create(&ctx, mpv, params);
    if (rc < 0) {
        renderCtx = nullptr;
        setError(std::string("render context: ") + mpv_error_string(rc));
        return false;
    }
    renderCtx = ctx;
    forceRedraw = true;
    return true;
}

// ---------------------------------------------------------------------------
// Жизненный цикл
// ---------------------------------------------------------------------------

bool Player::init() {
    if (impl_ && impl_->initialized && impl_->mpv) return true;
    if (!impl_) impl_ = new (std::nothrow) Impl();
    if (!impl_) return false;
    Impl& im = *impl_;
    if (im.mpv) {
        im.initialized = true;
        return true;
    }

    im.mpv = mpv_create();
    if (!im.mpv) {
        platform::bootLog("player: mpv_create failed");
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    auto setOption = [&im](const char* name, const char* value) {
        // Неизвестная опция не фатальна: часть сборок mpv не знает ytdl и др.
        mpv_set_option_string(im.mpv, name, value);
    };

    // Видео: только software-рендер через render API, без hwdec.
    setOption("vo", "libmpv");
    setOption("hwdec", "no");
    setOption("vid", "auto");          // не "1": id дорожки в DASH может быть любым
    setOption("video-sync", "audio");  // мастер-клок — звук
    setOption("framedrop", "vo");      // при отставании дропаем на уровне VO
    // Компенсация задержки вывода: кадр показываем на 50 мс позже звука
    // (значение mpv по умолчанию). Если убрать — видео «убегает» вперёд.
    setOption("video-timing-offset", "0.05");
    setOption("interpolation", "no");
    // Дешёвые скалеры: SW-рендер 720p на 4x Cortex-A57 не терпит lanczos.
    setOption("scale", "bilinear");
    setOption("dscale", "bilinear");
    setOption("cscale", "bilinear");
    setOption("correct-downscaling", "no");
    setOption("linear-downscaling", "no");
    setOption("sigmoid-upscaling", "no");
    setOption("dither-depth", "no");

    // Звук: AO-драйвер порта называется "hos" (внутри — audren API). Список
    // задаёт порядок: audren (на будущее), hos, sdl, null.
    setOption("ao", "audren,hos,sdl,null");
    setOption("audio-fallback-to-null", "yes");
    setOption("audio-channels", "stereo");
    // Задержка звука на консоли складывается из буферов audren. У ao_hos
    // значения по умолчанию — 4 буфера по 8192 сэмпла: 4*8192/48000 = 683 мс,
    // из-за чего звук отставал почти на секунду. 4 по 1024 = 85 мс.
    // rc проверяем отдельно: имена под-опций AO собираются динамически
    // ("ao-" + драйвер + "-" + опция) — если имя не совпало, mpv молча
    // вернётся к 8192 сэмплам, и звук снова отстанет на полсекунды.
    const int rcAoSamples = mpv_set_option_string(im.mpv, "ao-hos-num-samples", "1024");
    const int rcAoBuffers = mpv_set_option_string(im.mpv, "ao-hos-num-buffers", "4");
    // Небольшой запас, чтобы AAC-декодер не рвал поток, но без лишней секунды.
    setOption("audio-buffer", "0.15");
    setOption("audio-display", "no");  // не тащить обложки как видеодорожку

    // Сеть/кеш: локальный ретранслятор на 127.0.0.1, поэтому 20 c readahead
    // дают запас на паузы UI и не рвут поток.
    setOption("cache", "yes");
    setOption("demuxer-max-bytes", "32MiB");
    setOption("demuxer-readahead-secs", "20");
    setOption("demuxer-max-back-bytes", "8MiB");
    // PLR-03: не ждать вечно мёртвый ретранслятор/сеть; ffmpeg переподключается
    // сам (reconnect) и пауза кеша не считается ошибкой.
    setOption("network-timeout", "15");
    setOption("demuxer-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    setOption("cache-pause", "yes");
    setOption("cache-pause-wait", "1");

    // Декодер: 4 потока lavc — по числу доступных ядер.
    setOption("vd-lavc-threads", "4");

    // Минимум побочных эффектов на консоли: никаких конфигов, скриптов, OSD.
    setOption("ytdl", "no");
    setOption("terminal", "no");
    setOption("msg-level", "all=warn");
    setOption("config", "no");
    setOption("load-scripts", "no");
    setOption("osc", "no");
    setOption("osd-level", "0");
    setOption("input-default-bindings", "no");
    setOption("input-terminal", "no");
    setOption("idle", "yes");
    setOption("keep-open", "no");
    setOption("sub-auto", "no");
    setOption("audio-file-auto", "no");
    setOption("force-window", "no");

    int rc = mpv_initialize(im.mpv);
    if (rc < 0) {
        platform::bootLog("player: mpv_initialize rc=%d '%s'", rc, mpv_error_string(rc));
        mpv_terminate_destroy(im.mpv);
        im.mpv = nullptr;
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    // "warn": в boot-лог попадают предупреждения сети/демуксера/декодера —
    // именно они объясняют, почему видео не пошло. Спам ограничен в pumpEvents.
    mpv_request_log_messages(im.mpv, "warn");
    platform::bootLog("player: ao_hos 4x1024 сэмпла (rc %d/%d)%s", rcAoSamples, rcAoBuffers,
                      (rcAoSamples < 0 || rcAoBuffers < 0) ? " — опция не принята!" : "");
    im.initialized = true;
    im.pumpEvents();

    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(im.frameMtx);
        ok = im.createRenderContextLocked();
    }
    if (!ok) {
        // Без render-контекста показывать нечего: инициализация провалилась.
        platform::bootLog("player: init failed (render context)");
        im.initialized = false;
        mpv_terminate_destroy(im.mpv);
        im.mpv = nullptr;
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    const std::string mpvVersion = im.getString("mpv-version");
    platform::bootLog("player: mpv-version='%s'", mpvVersion.c_str());
    platform::bootLog("player: mpv инициализирован");
    return true;
}

void Player::shutdown() {
    Impl* im = impl_;
    if (!im) return;
    impl_ = nullptr;
    platform::setMediaPlaybackState(false);

    // Сначала render-контекст (mpv требует освободить его до destroy ядра),
    // причём при остановленном рендере — поэтому под frameMtx.
    {
        std::lock_guard<std::mutex> lk(im->frameMtx);
        if (im->renderCtx) {
            mpv_render_context_free(im->renderCtx);
            im->renderCtx = nullptr;
        }
        im->frameBuf.clear();
        im->frameBuf.shrink_to_fit();
    }
    if (im->mpv) {
        mpv_terminate_destroy(im->mpv);
        im->mpv = nullptr;
    }
    im->initialized = false;
    delete im;
}

// ---------------------------------------------------------------------------
// Открытие / закрытие
// ---------------------------------------------------------------------------

bool Player::open(const std::string& url, const std::string& title, double startAt) {
    if (url.empty()) return false;
    if (!init()) return false;

    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        ++im.gen;
        im.displayTitle = title;
        im.lastError.clear();
        im.lastLogError.clear();
        im.lastReportedError.clear();
        im.pendingAudio.clear();
        im.mpvLogCount = 0;
        im.mpvLogLimitReported = false;
        im.fileLoaded = false;
        im.starting = true;
        im.startSeen = false;
        im.audioRetries = 0;
        im.eofReached = false;
        im.audioOk = false;
        im.audioTrackSeen = false;
        im.pendingSeek = startAt > 0.0 ? startAt : 0.0;
    }
    im.transition(State::Opening, "loadfile");
    im.setMediaPlayback(true);

    // loadfile асинхронен: FILE_LOADED придёт в pumpEvents(). URL обычно ведёт
    // на локальный ретранслятор (http://127.0.0.1:8899/...). https тоже
    // пробуем — ffmpeg без TLS вернёт ошибку, она всплывёт в error().
    const char* args[] = {"loadfile", url.c_str(), "replace", nullptr};
    int rc = mpv_command(im.mpv, args);
    // URL не логируем целиком: в query токены. Только схема+хост.
    platform::bootLog("player open: rc=%d url_scheme_host=%s title='%s'", rc,
                      urlSchemeHost(url).c_str(), truncateLog(title, kMaxTitleBytes).c_str());
    if (rc >= 0) {
        // audio-delay живёт в mpv между файлами, но применяем явно: значение
        // могло измениться в настройках, пока плеер был закрыт.
        double delay = im.audioDelay;
        mpv_set_property(im.mpv, "audio-delay", MPV_FORMAT_DOUBLE, &delay);
        if (delay != 0.0)
            platform::bootLog("player: сдвиг звука %+d мс", (int)std::lround(delay * 1000.0));
    }
    if (rc < 0) {
        im.setError(std::string("loadfile: ") + mpv_error_string(rc));
        {
            std::lock_guard<std::mutex> lk(im.stateMtx);
            im.starting = false;
        }
        im.transition(State::Error, "loadfile failed");
        im.setMediaPlayback(false);
        return false;
    }
    return true;
}

bool Player::addAudio(const std::string& url) {
    if (url.empty()) return false;
    if (!impl_ || !impl_->initialized || !impl_->mpv) return false;

    Impl& im = *impl_;
    im.pumpEvents();

    const std::string host = urlSchemeHost(url);
    bool loadedNow = false;
    bool startingNow = false;
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        loadedNow = im.fileLoaded;
        startingNow = im.starting;
        if (!loadedNow && startingNow) {
            // Видео ещё грузится — применим сразу после FILE_LOADED.
            im.pendingAudio = url;
            deferred = true;
        }
    }
    if (deferred) {
        platform::bootLog("player addAudio: rc=deferred url=%s", host.c_str());
        return true;
    }
    if (!loadedNow) {
        platform::bootLog("player addAudio: rc=-1 url=%s (файл не загружен)", host.c_str());
        return false;
    }

    const bool ok = im.applyAudioAdd(url);
    // Точный rc пишет applyAudioAdd, здесь — итог для вызывающего.
    platform::bootLog("player addAudio: rc=%s url=%s", ok ? "ok" : "fail", host.c_str());
    return ok;
}

void Player::close() {
    if (!impl_ || !impl_->mpv) return;

    Impl& im = *impl_;
    im.pumpEvents();
    const auto t0 = std::chrono::steady_clock::now();

    const char* args[] = {"stop", nullptr};
    mpv_command(im.mpv, args);
    // PLR-10: даём mpv выдать END_FILE/STOP, но не блокируем UI дольше ~200 мс.
    for (int i = 0; i < 20; ++i) {
        im.pumpEvents();
        bool loadedNow = false;
        {
            std::lock_guard<std::mutex> lk(im.stateMtx);
            loadedNow = im.fileLoaded;
        }
        if (!loadedNow) break;
    }

    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        im.displayTitle.clear();
        im.lastError.clear();
        im.lastLogError.clear();
        im.pendingAudio.clear();
        im.fileLoaded = false;
        im.starting = false;
        im.eofReached = false;
        im.audioOk = false;
        im.audioTrackSeen = false;
        im.pendingSeek = 0.0;
        im.sleeping = false;
        im.wasPlayingBeforeSleep = false;
    }
    im.transition(State::Idle, "close");
    im.setMediaPlayback(false);
    {
        std::lock_guard<std::mutex> lk(im.frameMtx);
        im.forceRedraw = false;
        std::fill(im.frameBuf.begin(), im.frameBuf.end(), 0);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    platform::bootLog("player close: %lld мс", static_cast<long long>(ms));
}

bool Player::loaded() const {
    if (!impl_) return false;
    impl_->pumpEvents();
    std::lock_guard<std::mutex> lk(impl_->stateMtx);
    return impl_->fileLoaded;
}

// ---------------------------------------------------------------------------
// Управление
// ---------------------------------------------------------------------------

void Player::setPaused(bool p) {
    if (!impl_ || !impl_->mpv) return;
    Impl& im = *impl_;
    im.pumpEvents();
    int flag = p ? 1 : 0;
    mpv_set_property(im.mpv, "pause", MPV_FORMAT_FLAG, &flag);
    bool loadedNow = false;
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        loadedNow = im.fileLoaded;
    }
    if (p) {
        im.transition(State::Paused, "pause");
    } else if (loadedNow) {
        im.transition(State::Playing, "resume");
    }
    // PL-12: на паузе hbmenu не должен показывать «медиа играет».
    im.setMediaPlayback(!p && loadedNow && !im.sleeping);
}

void Player::togglePause() {
    setPaused(!paused());
}

bool Player::paused() const {
    if (!impl_ || !impl_->mpv) return false;
    impl_->pumpEvents();
    bool p = false;
    if (!impl_->getFlag("pause", p)) return false;
    return p;
}

void Player::seek(double seconds) {
    if (!impl_ || !impl_->mpv) return;
    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        // До FILE_LOADED duration=0, и seek 0.0 только сбивает загрузку.
        if (!im.fileLoaded) return;
    }
    // PLR-05: дебаунс 200 мс — серия нажатий не заваливает mpv seek'ами.
    const auto now = std::chrono::steady_clock::now();
    if (im.lastSeekAt != std::chrono::steady_clock::time_point{} &&
        now - im.lastSeekAt < std::chrono::milliseconds(kSeekDebounceMs)) {
        return;
    }
    im.lastSeekAt = now;

    double dur = 0.0;
    if (im.getDouble("duration", dur) && dur > 0.0) seconds = std::min(seconds, dur);
    if (seconds < 0.0) seconds = 0.0;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    // В mpv "absolute" по умолчанию = exact (hr). Дешёвый seek по ключевым
    // кадрам надо просить явно, иначе software-декодер 720p захлёбывается.
    const char* args[] = {"seek", buf, "absolute+keyframes", nullptr};
    int rc = mpv_command(im.mpv, args);
    platform::bootLog("player seek: %.1f c, rc=%d", seconds, rc);
    if (rc < 0) im.setError(std::string("seek: ") + mpv_error_string(rc));
}

void Player::seekRelative(double delta) {
    if (!impl_ || !impl_->mpv) return;
    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        if (!im.fileLoaded) return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (im.lastSeekAt != std::chrono::steady_clock::time_point{} &&
        now - im.lastSeekAt < std::chrono::milliseconds(kSeekDebounceMs)) {
        return;
    }
    im.lastSeekAt = now;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", delta);
    const char* args[] = {"seek", buf, "relative", nullptr};
    int rc = mpv_command(im.mpv, args);
    platform::bootLog("player seek relative: %+.1f c, rc=%d", delta, rc);
    if (rc < 0) im.setError(std::string("seek: ") + mpv_error_string(rc));
}

void Player::setVolume(int v) {
    if (!impl_ || !impl_->mpv) return;
    double vol = static_cast<double>(clampInt(v, 0, 130));
    mpv_set_property(impl_->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

int Player::volume() const {
    if (!impl_ || !impl_->mpv) return 100;
    double v = 100.0;
    if (!impl_->getDouble("volume", v)) return 100;
    return clampInt(static_cast<int>(std::lround(v)), 0, 130);
}

void Player::setMuted(bool m) {
    if (!impl_ || !impl_->mpv) return;
    int flag = m ? 1 : 0;
    mpv_set_property(impl_->mpv, "mute", MPV_FORMAT_FLAG, &flag);
    platform::bootLog("player: mute=%d", flag);
}

bool Player::muted() const {
    if (!impl_ || !impl_->mpv) return false;
    bool m = false;
    return impl_->getFlag("mute", m) && m;
}

void Player::setAudioDelay(double sec) {
    if (!impl_ || !impl_->mpv) return;
    if (!(sec > -2.0)) sec = -2.0;
    if (sec > 2.0) sec = 2.0;
    impl_->audioDelay = sec;
    mpv_set_property(impl_->mpv, "audio-delay", MPV_FORMAT_DOUBLE, &sec);
    platform::bootLog("player: сдвиг звука %+d мс (PLR-04)", (int)std::lround(sec * 1000.0));
}

// PL-03/PLR-11: сон/фон. Пауза, аудио-состояние сбрасывается; на пробуждении
// переинициализируем дорожку (aid) и продолжаем с той же позиции.
void Player::onSleep() {
    if (!impl_ || !impl_->mpv) return;
    Impl& im = *impl_;
    im.pumpEvents();
    if (im.sleeping) return;
    const bool wasLoaded = loaded();
    im.wasPlayingBeforeSleep = wasLoaded && playing();
    im.sleeping = true;
    if (wasLoaded) setPaused(true);
    im.setMediaPlayback(false);
    platform::bootLog("player: sleep (was_playing=%d, pos=%.1f)",
                      im.wasPlayingBeforeSleep ? 1 : 0, position());
}

void Player::onWake() {
    if (!impl_ || !impl_->mpv) return;
    Impl& im = *impl_;
    im.pumpEvents();
    if (!im.sleeping) return;
    im.sleeping = false;
    const bool wasPlaying = im.wasPlayingBeforeSleep;
    im.wasPlayingBeforeSleep = false;
    if (!loaded()) {
        platform::bootLog("player: wake (файл не открыт)");
        return;
    }
    // Пересоздаём аудио-выход: после сна AO может остаться в мёртвом состоянии.
    const std::string aid = im.getString("aid");
    if (!aid.empty()) mpv_set_property_string(im.mpv, "aid", aid.c_str());
    if (wasPlaying) setPaused(false);
    im.setMediaPlayback(true);
    platform::bootLog("player: wake (resume=%d, pos=%.1f)", wasPlaying ? 1 : 0, position());
}

void Player::setSpeed(double s) {
    if (!impl_ || !impl_->mpv) return;
    if (!(s > 0.0)) return;
    s = std::max(0.25, std::min(s, 4.0));
    mpv_set_property(impl_->mpv, "speed", MPV_FORMAT_DOUBLE, &s);
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Машина состояний и аудио (PLR-01/07/13)
// ---------------------------------------------------------------------------

Player::State Player::state() const {
    if (!impl_) return State::Idle;
    impl_->pumpEvents();
    std::lock_guard<std::mutex> lk(impl_->stateMtx);
    return impl_->st;
}

const char* Player::stateName() const { return stateNameOf(static_cast<int>(state())); }

bool Player::audioTrackPresent() const {
    if (!impl_ || !impl_->mpv) return false;
    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        if (im.audioTrackSeen) return true;
    }
    int64_t channels = 0;
    return im.getInt("audio-params/channel-count", channels) && channels > 0;
}

std::string Player::audioWarning() const {
    if (!impl_ || !impl_->mpv) return std::string();
    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        if (!im.fileLoaded) return std::string();
    }
    if (!audioTrackPresent()) return "Нет звука: в потоке нет аудиодорожки";
    const std::string ao = im.getString("current-ao");
    if (ao == "null") return "Нет звука: аудиодрайвер null";
    return std::string();
}

bool Player::needsMuxedFallback() const {
    if (!impl_ || !impl_->mpv) return false;
    Impl& im = *impl_;
    im.pumpEvents();
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        if (!im.fileLoaded || !im.pendingAudio.empty()) return false;
    }
    return !audioTrackPresent();
}

bool Player::playing() const {
    if (!impl_ || !impl_->mpv) return false;
    Impl& im = *impl_;
    im.pumpEvents();

    bool isPaused = false;
    if (im.getFlag("pause", isPaused) && isPaused) return false;

    bool eof = false;
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        eof = im.eofReached;
    }
    bool eofProp = false;
    if (im.getFlag("eof-reached", eofProp) && eofProp) eof = true;
    if (eof) return false;

    double pos = 0.0;
    if (!im.getDouble("time-pos", pos)) return false;
    return pos > 0.0;
}

bool Player::buffering() const {
    if (!impl_ || !impl_->mpv) return false;
    Impl& im = *impl_;
    im.pumpEvents();

    bool loadedNow = false;
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        loadedNow = im.fileLoaded;
    }
    if (!loadedNow) return false;

    // На паузе кеш может не дозаполняться — это не «буферизация».
    bool isPaused = false;
    if (im.getFlag("pause", isPaused) && isPaused) return false;

    bool cachePaused = false;
    if (im.getFlag("paused-for-cache", cachePaused) && cachePaused) return true;

    int64_t state = 100;
    if (im.getInt("cache-buffering-state", state)) return state >= 0 && state < 100;
    return false;
}

double Player::position() const {
    if (!impl_ || !impl_->mpv) return 0.0;
    double pos = 0.0;
    if (!impl_->getDouble("time-pos", pos)) {
        // time-pos недоступен до первого кадра — пробуем playback-time.
        if (!impl_->getDouble("playback-time", pos)) return 0.0;
    }
    return pos > 0.0 ? pos : 0.0;
}

double Player::duration() const {
    if (!impl_ || !impl_->mpv) return 0.0;
    double dur = 0.0;
    if (!impl_->getDouble("duration", dur)) return 0.0;
    return dur > 0.0 ? dur : 0.0;
}

bool Player::hasVideo() const {
    if (!impl_ || !impl_->mpv) return false;
    return videoWidth() > 0 && videoHeight() > 0;
}

std::string Player::error() const {
    if (!impl_) return std::string();
    Impl& im = *impl_;
    im.pumpEvents();

    std::lock_guard<std::mutex> lk(im.stateMtx);
    if (!im.lastError.empty()) return im.lastError;
    // Логовые ошибки показываем только когда файл не загружен: иначе
    // восстановимая ошибка сети выглядела бы как фатальная.
    if (!im.fileLoaded && !im.lastLogError.empty()) return im.lastLogError;
    return std::string();
}

std::string Player::title() const {
    if (!impl_) return std::string();
    Impl& im = *impl_;
    im.pumpEvents();

    std::string t;
    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        t = im.displayTitle;
    }
    if (!t.empty()) return t;

    std::string mediaTitle = im.getString("media-title");
    if (!mediaTitle.empty()) return mediaTitle;
    return std::string();
}

Player::Status Player::status() const {
    Status s;
    if (!impl_ || !impl_->mpv) return s;
    Impl& im = *impl_;
    im.pumpEvents();

    bool flag = false;
    if (im.getFlag("seekable", flag)) s.seekable = flag;

    {
        std::lock_guard<std::mutex> lk(im.stateMtx);
        s.eof = im.eofReached;
    }
    if (im.getFlag("eof-reached", flag) && flag) s.eof = true;

    int64_t v = 0;
    if (im.getInt("frame-drop-count", v)) s.dropped = static_cast<int>(std::max<int64_t>(0, v));
    if (im.getInt("vo-drop-frame-count", v) && s.dropped == 0) {
        s.dropped = static_cast<int>(std::max<int64_t>(0, v));
    }

    double d = 0.0;
    if (im.getDouble("estimated-vf-fps", d)) s.decodeFps = static_cast<float>(d);

    if (im.getDouble("demuxer-cache-time", d) && d > 0.0) {
        s.cacheMs = static_cast<int>(std::min(d * 1000.0, 2147483000.0));
    }
    // PLR-04/12: расхождение A/V и битрейты — для лога и индикатора.
    if (im.getDouble("avsync", d)) s.avsync = d;
    if (im.getInt("video-bitrate", v)) s.vbitrate = v;
    if (im.getInt("audio-bitrate", v)) s.abitrate = v;
    return s;
}

// ---------------------------------------------------------------------------
// Видео: рендер в Image
// ---------------------------------------------------------------------------

bool Player::update(Image& out) {
    if (!impl_ || !impl_->mpv || !impl_->initialized) return false;
    Impl& im = *impl_;

    // События (END_FILE/LOG_MESSAGE) копятся в очереди mpv — вычитываем их
    // здесь, т.к. update() вызывается каждый кадр UI.
    im.pumpEvents();
    im.logMetrics(); // PLR-12: раз в 5 с, не больше 200 строк

    std::lock_guard<std::mutex> lk(im.frameMtx);
    if (!im.renderCtx) {
        // Например, после неудачного setTargetSize — пробуем восстановиться.
        if (!im.createRenderContextLocked()) return false;
    }

    const uint64_t flags = mpv_render_context_update(im.renderCtx);
    const bool newFrame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (!newFrame && !im.forceRedraw) return false;  // нет нового кадра — не тратим CPU
    im.forceRedraw = false;

    int size[2] = {im.targetW, im.targetH};
    size_t stride = im.stride;
    void* ptr = im.frameBuf.data();
    // Блокировка до целевого времени не нужна: mpv отдаёт кадр уже по таймингу
    // (video-timing-offset=0), а блокировать UI-поток нельзя.
    int blockForTarget = 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, kRenderFormat},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, ptr},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTarget},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    const int rc = mpv_render_context_render(im.renderCtx, params);
    if (rc < 0) {
        if (rc != MPV_ERROR_UNINITIALIZED) {
            im.setError(std::string("render: ") + mpv_error_string(rc));
        }
        return false;
    }

    const int w = im.targetW;
    const int h = im.targetH;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

    // rgb0: байты R,G,B,X. Image: 0xAABBGGRR (в памяти R,G,B,A) => младший
    // байт uint32 — R. Достаточно выставить непрозрачность: раскладка каналов
    // совпадает, поэтому копируем словами (раньше был memcpy на каждый
    // пиксель — на 1280x720 это заметные миллисекунды каждый кадр).
    const uint8_t* base = im.frameBuf.data();
    uint32_t* dst = out.px.data();
    for (int y = 0; y < h; ++y) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * im.stride);
        uint32_t* row = dst + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) row[x] = src[x] | 0xFF000000u;
    }
    return true;
}

void Player::setTargetSize(int w, int h) {
    if (!impl_) return;
    Impl& im = *impl_;
    w = clampInt(w, kMinWidth, kMaxWidth);
    h = clampInt(h, kMinHeight, kMaxHeight);

    std::lock_guard<std::mutex> lk(im.frameMtx);
    if (w == im.targetW && h == im.targetH && im.renderCtx) return;

    // SW-размер задаётся при создании render-контекста, поэтому пересоздаём
    // его целиком: stop не нужен, mpv сохраняет позицию и кеш.
    if (im.renderCtx) {
        mpv_render_context_free(im.renderCtx);
        im.renderCtx = nullptr;
    }
    im.targetW = w;
    im.targetH = h;
    im.forceRedraw = false;
    im.frameBuf.clear();

    if (im.mpv && im.initialized) im.createRenderContextLocked();
}

int Player::videoWidth() const {
    if (!impl_ || !impl_->mpv) return 0;
    int64_t v = 0;
    if (impl_->getInt("video-params/w", v) && v > 0) return static_cast<int>(v);
    if (impl_->getInt("width", v) && v > 0) return static_cast<int>(v);
    return 0;
}

int Player::videoHeight() const {
    if (!impl_ || !impl_->mpv) return 0;
    int64_t v = 0;
    if (impl_->getInt("video-params/h", v) && v > 0) return static_cast<int>(v);
    if (impl_->getInt("height", v) && v > 0) return static_cast<int>(v);
    return 0;
}

// ---------------------------------------------------------------------------
// Синглтон
// ---------------------------------------------------------------------------

Player& instance() {
    // Meyers-синглтон: потокобезопасная инициализация в C++11.
    // Деструктор не вызывает shutdown() — приложение обязано звать его явно
    // (в конце main), иначе libmpv может не успеть корректно завершить AO.
    static Player p;
    return p;
}

} // namespace tnx::player