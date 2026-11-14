// tools/test_innertube.cpp — хостовый тест парсера InnerTube (без сети).
// Сборка (M2):
//   [1] g++ -std=c++17 -O2 -Wall -Isource -Itools -DTNX_HOST
//   [2]     tools/test_innertube.cpp source/yt/innertube.cpp
//   [3]     -o /tmp/opencode/test_innertube_m2 -lpthread -l:libjansson.so.4
// Запуск из корня проекта: /tmp/opencode/test_innertube_m2 tools/sample_yt.json
//
// Проверяет ID YT-01..YT-09 (см. docs/refactor-tz.md §5.8): парсер лент/поиска,
// player, playabilityStatus, матрицу клиентов, дедуп/лимит continuation,
// детерминированный выбор потоков и извлечение ключа/visitor.
#include "net/http.hpp"
#include "platform.hpp"
#include "yt/innertube.hpp"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Хостовые хуки, определённые в innertube.cpp под -DTNX_HOST.
namespace tnx::yt {
Feed host_parse_feed(const std::string& json, bool isSearch);
Feed host_parse_related(const std::string& json);
bool host_parse_player(const std::string& json, const std::string& id, VideoDetails& out,
                       std::string& err);
std::string host_playability_text(const std::string& status);
int host_client_count();
std::string host_client_name(int idx);
int host_next_client(int idx);
std::string host_extract_key(const std::string& body);
std::string host_extract_visitor(const std::string& body);
StreamSelection host_select_streams(const VideoDetails& v, int preferHeight);
std::vector<std::string> host_thumb_candidates(const std::string& id, int q);
} // namespace tnx::yt

// Заглушки платформы (host не линкует main.cpp).
namespace tnx::platform {
void setLogLevel(LogLevel) {}
LogLevel logLevel() { return LogLevel::Info; }
bool debugEnabled() { return false; }
void logWrite(LogLevel, const char*, ...) {}
void logError(const char*, ...) {}
void logWarn(const char*, ...) {}
void logInfo(const char*, ...) {}
void logDebug(const char*, ...) {}
void bootLog(const char*, ...) {}
bool keyboard(std::string&, const char*, const char*, bool) { return false; }
void requestExit() {}
Info info() { return Info{}; }
} // namespace tnx::platform

// Заглушки сети: тест не ходит в интернет.
namespace tnx::net {

// NET-11: что транспорт получил последним (для проверки проводки токена).
CancelToken* g_lastPostCancel = nullptr;
std::string g_lastPostEndpoint;

Response get(const std::string& url, const Headers&, int, size_t, int, const Overrides*,
             CancelToken*) {
    Response r;
    r.status = 0;
    // Подсказки поиска приходят в JSONP — отдаём фикстуру.
    if (url.find("complete/search") != std::string::npos) {
        r.status = 200;
        r.body =
            "window.google.ac.h([\"switch\",[[\"switch homebrew\",0,[]],[\"switch cfw\",0,[]],"
            "[\"switch 2\",0,[]]],{\"k\":1}])";
        return r;
    }
    r.error = "host-test: сеть отключена";
    return r;
}

Response post(const std::string& url, const std::string&, const Headers&, int, size_t, int,
              const Overrides*, CancelToken* cancel) {
    g_lastPostCancel = cancel;
    g_lastPostEndpoint = url;
    Response r;
    r.status = 0;
    r.error = "host-test: сеть отключена";
    return r;
}

std::string urlEncode(const std::string& s) {
    return s;
}

// YT-10: подробный лог подсказок использует escapeForLog из http.cpp, который
// в host-тесте не линкуется — достаточно тривиального экранирования.
std::string escapeForLog(const std::string& s, size_t) {
    std::string out;
    for (char c : s) out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return out;
}

} // namespace tnx::net

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what, const std::string& detail = "") {
    ++g_checks;
    if (condition) {
        std::printf("  [ok]   %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
                    detail.c_str());
    }
}

void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what, "получено \"" + got + "\", ожидалось \"" + want + "\"");
}

void checkEqInt(long long got, long long want, const std::string& what) {
    check(got == want, what,
          "получено " + std::to_string(got) + ", ожидалось " + std::to_string(want));
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

tnx::yt::StreamFormat makeFormat(int itag, const std::string& mime, const std::string& url,
                                 int width, int height, int bitrate, bool hasVideo,
                                 bool hasAudio) {
    tnx::yt::StreamFormat f;
    f.itag = itag;
    f.mime = mime;
    f.url = url;
    f.width = width;
    f.height = height;
    f.bitrate = bitrate;
    f.hasVideo = hasVideo;
    f.hasAudio = hasAudio;
    f.qualityLabel = std::to_string(height) + "p";
    return f;
}

} // namespace

int main(int argc, char** argv) {
    using namespace tnx::yt;

    const std::string samplePath = argc > 1 ? argv[1] : "tools/sample_yt.json";
    const std::string homeJson = readFile(samplePath);
    std::printf("Файл образца: %s (%zu байт)\n\n", samplePath.c_str(), homeJson.size());
    if (homeJson.empty()) {
        std::printf("Не удалось прочитать образец JSON\n");
        return 2;
    }

    // --- 1. Разбор главной страницы ---
    std::printf("1) parseFeed(home)\n");
    Feed home = host_parse_feed(homeJson, false);
    check(home.ok, "лента разобрана без ошибки", home.error);
    check(home.sections.size() >= 2, "есть основная секция и полка",
          "секций: " + std::to_string(home.sections.size()));

    size_t totalItems = 0;
    for (const Section& s : home.sections) {
        totalItems += s.items.size();
        std::printf("     секция \"%s\" (%zu видео)\n", s.title.c_str(), s.items.size());
        for (const VideoItem& v : s.items) {
            std::printf("       - %s | %s | %s | %s\n", v.id.c_str(), v.title.c_str(),
                        v.duration.c_str(), v.meta.c_str());
        }
    }
    check(totalItems >= 3, "извлечено 3+ видео", "видео: " + std::to_string(totalItems));

    // Главная секция — первая не-chips, с пустым заголовком.
    const Section* primary = nullptr;
    const Section* shelf = nullptr;
    for (const Section& s : home.sections) {
        if (s.chips) continue;
        if (s.title.empty() && !primary) primary = &s;
        else if (s.title == "Рекомендуем посмотреть") shelf = &s;
    }
    check(primary != nullptr, "основная секция имеет пустой заголовок");
    if (primary) checkEqInt((long long)primary->items.size(), 5, "в основной секции 5 видео");

    check(shelf != nullptr, "полка richShelfRenderer найдена");
    if (shelf) checkEqInt((long long)shelf->items.size(), 2, "в полке 2 видео");

    checkEq(home.continuation, "4qmFsgJVEhhVQ2hvbWVicmV3MDAwMDAwMDAx", "токен продолжения");

    // Поля первого видео.
    if (primary && !primary->items.empty()) {
        const VideoItem& v = primary->items[0];
        checkEq(v.id, "dQw4w9WgXcQ", "videoId");
        checkEq(v.title, "Switch homebrew: собираем TubeNX", "title из runs");
        checkEq(v.channel, "TechFan", "канал из longBylineText");
        checkEq(v.channelId, "UC1234567890abcdef", "channelId из navigationEndpoint");
        checkEq(v.duration, "12:34", "duration из lengthText");
        checkEqInt(v.lengthSeconds, 754, "lengthSeconds");
        checkEqInt(v.viewCount, 1234567, "viewCount из '1 234 567 просмотров'");
        checkEq(v.meta, "1 234 567 просмотров • 3 дня назад", "meta");
        check(v.thumbnailUrl.find("hq720.jpg?sqp=abc&rs=def") != std::string::npos,
              "thumbnailUrl — последний (крупнейший) из thumbnails");
        check(!v.live, "обычное видео не помечено как LIVE");
    }

    // Live-видео (thumbnailOverlayTimeStatusRenderer.style == LIVE).
    bool foundLive = false;
    for (const VideoItem& v : primary ? primary->items : std::vector<VideoItem>{}) {
        if (v.id == "aBcDeFgHiJk") {
            foundLive = true;
            check(v.live, "live-флаг по thumbnailOverlays");
            checkEq(v.duration, "LIVE", "duration = LIVE");
            checkEq(v.channelId, "UCnintendo0000000001", "channelId у live-видео");
            checkEqInt(v.viewCount, 24000, "viewCount из '24 тыс. зрителей'");
        }
    }
    check(foundLive, "live-видео найдено в ленте");

    // gridVideoRenderer и сокращённые числа.
    bool foundGrid = false, foundShort = false;
    for (const VideoItem& v : primary ? primary->items : std::vector<VideoItem>{}) {
        if (v.id == "gRiD1234567") {
            foundGrid = true;
            checkEqInt(v.viewCount, 2500000, "viewCount из '2,5 млн просмотров'");
            checkEq(v.duration, "1:02:03", "duration >= 1 часа");
            checkEqInt(v.lengthSeconds, 3723, "lengthSeconds для 1:02:03");
        }
        if (v.id == "kKk7jJj6hHh") {
            foundShort = true;
            checkEqInt(v.viewCount, 999, "viewCount из '999 просмотров'");
            check(v.thumbnailUrl.find("hqdefault.jpg") != std::string::npos,
                  "запасной thumbUrl для видео без thumbnail");
        }
    }
    check(foundGrid, "gridVideoRenderer разобран");
    check(foundShort, "videoRenderer без thumbnail разобран");

    // --- 2. Поиск: chips + результаты + продолжение ---
    std::printf("\n2) parseFeed(search)\n");
    const std::string searchJson = R"({
      "contents": {"twoColumnSearchResultsRenderer": {"primaryContents": {"sectionListRenderer": {
        "contents": [
          {"itemSectionRenderer": {"contents": [{"chipCloudRenderer": {"chips": [
            {"chipCloudChipRenderer": {"text": {"simpleText": "Все"}}},
            {"chipCloudChipRenderer": {"text": {"simpleText": "Видео"}}},
            {"chipCloudChipRenderer": {"text": {"simpleText": "Недавно загруженные"}}}
          ]}}]}},
          {"itemSectionRenderer": {"contents": [
            {"videoRenderer": {
              "videoId": "search00001",
              "title": {"simpleText": "Первое видео"},
              "ownerText": {"runs": [{"text": "Канал А",
                "navigationEndpoint": {"browseEndpoint": {"browseId": "UCsearchchannel1"}}}]},
              "viewCountText": {"simpleText": "10 тыс. просмотров"},
              "publishedTimeText": {"simpleText": "1 день назад"},
              "lengthText": {"simpleText": "1:02:03"},
              "thumbnail": {"thumbnails": [{"url": "https://i.ytimg.com/vi/search00001/hqdefault.jpg"}]}
            }},
            {"videoRenderer": {
              "videoId": "search00002",
              "title": {"runs": [{"text": "Второе "}, {"text": "видео"}]},
              "ownerText": {"runs": [{"text": "Канал Б"}]},
              "viewCountText": {"simpleText": "5 просмотров"},
              "lengthText": {"simpleText": "0:59"}
            }},
            {"videoRenderer": {
              "videoId": "search00003",
              "title": {"simpleText": "Третье видео"},
              "viewCountText": {"simpleText": "1,5 млрд просмотров"}
            }}
          ]}},
          {"continuationItemRenderer": {"continuationEndpoint": {
            "continuationCommand": {"token": "SEARCH_TOKEN_123"}}}}
        ]
      }}}}
    })";
    Feed search = host_parse_feed(searchJson, true);
    check(search.ok, "поиск разобран", search.error);
    check(!search.sections.empty() && search.sections[0].chips, "секция chips идёт первой");
    if (!search.sections.empty() && search.sections[0].chips) {
        checkEqInt((long long)search.sections[0].chipLabels.size(), 3, "3 фильтра в chips");
        if (search.sections[0].chipLabels.size() == 3) {
            checkEq(search.sections[0].chipLabels[2], "Недавно загруженные", "подпись фильтра");
        }
    }
    const Section* results = nullptr;
    for (const Section& s : search.sections) {
        if (!s.chips && s.title == "Результаты") results = &s;
    }
    check(results != nullptr, "секция результатов найдена");
    if (results) {
        checkEqInt((long long)results->items.size(), 3, "3 результата поиска");
        if (!results->items.empty()) {
            checkEq(results->items[0].title, "Первое видео", "title результата");
            checkEq(results->items[0].channelId, "UCsearchchannel1", "channelId результата");
            checkEq(results->items[0].meta, "10 тыс. просмотров • 1 день назад", "meta результата");
        }
        if (results->items.size() > 1) {
            checkEq(results->items[1].title, "Второе видео", "склейка runs в title");
        }
        if (results->items.size() > 2) {
            checkEqInt(results->items[2].viewCount, 1500000000LL, "viewCount из '1,5 млрд'");
        }
    }
    checkEq(search.continuation, "SEARCH_TOKEN_123", "продолжение поиска");

    // --- 3. Форматирование ---
    std::printf("\n3) formatViews / formatDuration / thumbUrl\n");
    checkEq(formatViews(0), "0 просмотров", "formatViews(0)");
    checkEq(formatViews(1), "1 просмотр", "formatViews(1)");
    checkEq(formatViews(2), "2 просмотра", "formatViews(2)");
    checkEq(formatViews(5), "5 просмотров", "formatViews(5)");
    checkEq(formatViews(11), "11 просмотров", "formatViews(11)");
    checkEq(formatViews(21), "21 просмотр", "formatViews(21)");
    checkEq(formatViews(1234), "1,2 тыс. просмотров", "formatViews(1234)");
    checkEq(formatViews(12345), "12,3 тыс. просмотров", "formatViews(12345)");
    checkEq(formatViews(1234567), "1,2 млн просмотров", "formatViews(1234567)");
    checkEq(formatViews(2000000), "2 млн просмотров", "formatViews(2000000)");
    checkEq(formatViews(2500000000LL), "2,5 млрд просмотров", "formatViews(2.5 млрд)");

    checkEq(formatDuration(0), "0:00", "formatDuration(0)");
    checkEq(formatDuration(59), "0:59", "formatDuration(59)");
    checkEq(formatDuration(754), "12:34", "formatDuration(754)");
    checkEq(formatDuration(3723), "1:02:03", "formatDuration(3723)");

    checkEq(thumbUrl("dQw4w9WgXcQ", 0), "https://i.ytimg.com/vi/dQw4w9WgXcQ/default.jpg",
            "thumbUrl quality 0");
    checkEq(thumbUrl("dQw4w9WgXcQ", 1), "https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg",
            "thumbUrl quality 1");
    checkEq(thumbUrl("dQw4w9WgXcQ", 2), "https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg",
            "thumbUrl quality 2");
    checkEq(thumbUrl("dQw4w9WgXcQ", 3), "https://i.ytimg.com/vi/dQw4w9WgXcQ/sddefault.jpg",
            "thumbUrl quality 3");
    checkEq(thumbUrl("dQw4w9WgXcQ", 4), "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg",
            "thumbUrl quality 4 (maxres)");

    // --- 4. YT-07: детерминированный выбор потоков ---
    std::printf("\n4) selectStreams (YT-07)\n");
    VideoDetails vd;
    vd.formats.push_back(makeFormat(18, "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
                                    "https://example/18", 640, 360, 500000, true, true));
    vd.formats.push_back(makeFormat(22, "video/mp4; codecs=\"avc1.64001F, mp4a.40.2\"",
                                    "https://example/22", 1280, 720, 2000000, true, true));
    vd.formats.push_back(makeFormat(137, "video/mp4; codecs=\"avc1.640028\"", "https://example/137",
                                    1920, 1080, 4500000, true, false));
    vd.formats.push_back(makeFormat(136, "video/mp4; codecs=\"avc1.64001F\"", "https://example/136",
                                    1280, 720, 2500000, true, false));
    vd.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"", "https://example/140",
                                    0, 0, 128000, false, true));
    vd.formats.push_back(makeFormat(139, "audio/mp4; codecs=\"mp4a.40.5\"", "https://example/139",
                                    0, 0, 48000, false, true));
    vd.formats.push_back(makeFormat(251, "audio/webm; codecs=\"opus\"", "https://example/251", 0, 0,
                                    160000, false, true));

    StreamSelection sel = host_select_streams(vd, 720);
    check(sel.mode == StreamPick::Split, "план 1: adaptive video+audio",
          "mode=" + std::to_string((int)sel.mode));
    checkEqInt(sel.video.itag, 136, "выбрано видео itag 136 (720p AVC)");
    checkEqInt(sel.audio.itag, 140, "выбран звук itag 140 (AAC)");

    StreamFormat muxed, videoTrack, audioTrack;
    bool ok = InnerTube::pickStreams(vd, &muxed, &videoTrack, &audioTrack, 720);
    check(ok, "pickStreams вернул true");
    check(muxed.url.empty(), "при split-плане muxed пуст");
    checkEq(videoTrack.url, "https://example/136", "pickStreams: видео 136");
    checkEqInt(audioTrack.itag, 140, "pickStreams: звук 140");

    // План 2: adaptive видео нет, есть только muxed 18.
    {
        VideoDetails v;
        v.formats.push_back(makeFormat(18, "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
                                       "https://example/18", 640, 360, 500000, true, true));
        v.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"", "https://example/140",
                                       0, 0, 128000, false, true));
        StreamSelection s = host_select_streams(v, 720);
        check(s.mode == StreamPick::Muxed, "план 2: нет adaptive → muxed");
        checkEqInt(s.muxed.itag, 18, "выбран muxed itag 18");
    }

    // План 3 таблицы: adaptive 480 (135) присутствует, но 18 идёт раньше 135+140.
    {
        VideoDetails v;
        v.formats.push_back(makeFormat(18, "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
                                       "https://example/18", 640, 360, 500000, true, true));
        v.formats.push_back(makeFormat(135, "video/mp4; codecs=\"avc1.4D401E\"", "https://example/135",
                                       854, 480, 1200000, true, false));
        v.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"", "https://example/140",
                                       0, 0, 128000, false, true));
        StreamSelection s = host_select_streams(v, 720);
        check(s.mode == StreamPick::Muxed, "18 приоритетнее 135+140");
        checkEqInt(s.muxed.itag, 18, "выбран muxed 18, а не adaptive 480");
    }

    // План 4: muxed нет → adaptive 480 + AAC.
    {
        VideoDetails v;
        v.formats.push_back(makeFormat(135, "video/mp4; codecs=\"avc1.4D401E\"", "https://example/135",
                                       854, 480, 1200000, true, false));
        v.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"", "https://example/140",
                                       0, 0, 128000, false, true));
        StreamSelection s = host_select_streams(v, 720);
        check(s.mode == StreamPick::Split, "нет muxed → adaptive 480+140");
        checkEqInt(s.video.itag, 135, "выбрано видео 135 (480p)");
    }

    // Детерминизм при равных баллах: меньший itag.
    {
        VideoDetails v;
        v.formats.push_back(makeFormat(298, "video/mp4; codecs=\"avc1.640020\"", "https://example/298",
                                       1280, 720, 2500000, true, false));
        v.formats.push_back(makeFormat(136, "video/mp4; codecs=\"avc1.64001F\"", "https://example/136",
                                       1280, 720, 2500000, true, false));
        v.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"", "https://example/140",
                                       0, 0, 128000, false, true));
        StreamSelection s = host_select_streams(v, 720);
        checkEqInt(s.video.itag, 136, "при равных баллах меньший itag");
    }

    // live/drm не выбираются.
    {
        VideoDetails v;
        StreamFormat live = makeFormat(136, "video/mp4; codecs=\"avc1.64001F\"", "https://example/136",
                                       1280, 720, 2500000, true, false);
        live.live = true;
        StreamFormat drm = makeFormat(18, "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
                                      "https://example/18", 640, 360, 500000, true, true);
        drm.drm = true;
        v.formats.push_back(live);
        v.formats.push_back(drm);
        StreamSelection s = host_select_streams(v, 720);
        check(s.mode == StreamPick::None, "live/drm-потоки пропущены");
        StreamFormat m, vi, au;
        check(!InnerTube::pickStreams(v, &m, &vi, &au, 720), "pickStreams false для live/drm");
    }

    // --- 5. Разбор player ---
    std::printf("\n5) parsePlayer\n");
    const std::string playerJson = R"({
      "playabilityStatus": {"status": "OK", "playableInEmbed": true},
      "streamingData": {
        "expiresInSeconds": "21540",
        "formats": [
          {"itag": 18, "url": "https://example/v18",
           "mimeType": "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
           "bitrate": 500000, "width": 640, "height": 360,
           "contentLength": "12345678", "qualityLabel": "360p",
           "audioQuality": "AUDIO_QUALITY_LOW"}
        ],
        "adaptiveFormats": [
          {"itag": 137, "url": "https://example/v137",
           "mimeType": "video/mp4; codecs=\"avc1.640028\"",
           "bitrate": 4500000, "width": 1920, "height": 1080,
           "contentLength": "987654321", "qualityLabel": "1080p"},
          {"itag": 136, "url": "https://example/v136",
           "mimeType": "video/mp4; codecs=\"avc1.64001F\"",
           "bitrate": 2500000, "width": 1280, "height": 720,
           "contentLength": "55555555", "qualityLabel": "720p"},
          {"itag": 140, "url": "https://example/a140",
           "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"",
           "bitrate": 128000, "contentLength": "3456789",
           "audioQuality": "AUDIO_QUALITY_MEDIUM"},
          {"itag": 22, "signatureCipher": "s=ABC&url=https%3A%2F%2Fexample%2Fv22",
           "mimeType": "video/mp4; codecs=\"avc1.64001F, mp4a.40.2\"",
           "bitrate": 2000000, "width": 1280, "height": 720}
        ]
      },
      "videoDetails": {
        "videoId": "player00001",
        "title": "Тестовое видео",
        "shortDescription": "Описание",
        "author": "Автор",
        "channelId": "UCplayer0000000001",
        "lengthSeconds": "754",
        "viewCount": "1234567",
        "thumbnail": {"thumbnails": [
          {"url": "https://i.ytimg.com/vi/player00001/hq720.jpg", "width": 720, "height": 404}
        ]}
      },
      "microformat": {"playerMicroformatRenderer": {
        "publishDate": "2024-11-26",
        "viewCount": "1234567",
        "ownerChannelName": "Автор"
      }}
    })";
    VideoDetails pd;
    std::string perr;
    bool pok = host_parse_player(playerJson, "player00001", pd, perr);
    check(pok, "player разобран", perr);
    checkEq(pd.title, "Тестовое видео", "название видео");
    checkEq(pd.channel, "Автор", "автор видео");
    checkEq(pd.channelId, "UCplayer0000000001", "channelId видео");
    checkEqInt(pd.lengthSeconds, 754, "длительность видео");
    checkEqInt(pd.viewCount, 1234567, "просмотры видео");
    checkEq(pd.viewText, "1,2 млн просмотров", "viewText");
    checkEq(pd.publishedText, "26 нояб. 2024 г.", "publishedText из publishDate");
    checkEq(pd.shareUrl, "https://youtu.be/player00001", "shareUrl");
    checkEq(pd.thumbnailUrl, "https://i.ytimg.com/vi/player00001/hq720.jpg", "обложка видео");
    checkEq(pd.playabilityStatus, "OK", "playabilityStatus = OK");
    checkEqInt((long long)pd.formats.size(), 4, "формат с signatureCipher пропущен");

    bool muxed18 = false, video137 = false, audio140 = false;
    for (const StreamFormat& f : pd.formats) {
        if (f.itag == 18) {
            muxed18 = f.hasVideo && f.hasAudio && f.height == 360;
            checkEq(f.codecs, "avc1.42001E, mp4a.40.2", "codecs из mimeType");
            checkEqInt(f.contentLength, 12345678, "contentLength из строки");
        }
        if (f.itag == 137) video137 = f.hasVideo && !f.hasAudio;
        if (f.itag == 140) audio140 = f.hasAudio && !f.hasVideo;
    }
    check(muxed18, "itag 18 распознан как muxed (video+audio)");
    check(video137, "itag 137 распознан как видео");
    check(audio140, "itag 140 распознан как аудио");

    StreamFormat pm, pv, pa;
    check(InnerTube::pickStreams(pd, &pm, &pv, &pa, 720), "pickStreams по разобранному player");
    checkEqInt(pv.itag, 136, "видео из player — itag 136 (720p)");
    checkEqInt(pa.itag, 140, "звук из player — itag 140");

    // TEST-05: фикстура прямого эфира. videoDetails.isLiveContent помечает все
    // потоки live, pickStreams их не выбирает (YT-07); бейдж LIVE_NOW в ленте
    // даёт live и duration=LIVE.
    const std::string liveJson = R"({
      "playabilityStatus": {"status": "OK"},
      "streamingData": {"adaptiveFormats": [
        {"itag": 300, "url": "https://example/live300",
         "mimeType": "video/mp4; codecs=\"avc1.640028\"",
         "bitrate": 3000000, "width": 1280, "height": 720, "qualityLabel": "720p"}
      ]},
      "videoDetails": {"videoId": "live0000001", "title": "Прямой эфир",
                       "author": "Канал", "isLiveContent": true, "lengthSeconds": "0"}
    })";
    VideoDetails ld;
    std::string lerr;
    check(host_parse_player(liveJson, "live0000001", ld, lerr), "фикстура live разобрана", lerr);
    check(ld.live, "isLiveContent -> live");
    check(!ld.formats.empty() && ld.formats[0].live, "потоки эфира помечены live");
    StreamFormat lm, lv, la;
    check(!InnerTube::pickStreams(ld, &lm, &lv, &la, 720), "эфир не выбирается как обычное видео");

    Feed liveFeed = host_parse_feed(
        R"({"contents":{"richGridRenderer":{"contents":[
          {"richItemRenderer":{"content":{"videoRenderer":{
            "videoId": "livefeed001", "title": {"simpleText": "Стрим"},
            "ownerText": {"runs": [{"text": "Канал"}]},
            "badges": [{"metadataBadgeRenderer": {"style": "BADGE_STYLE_TYPE_LIVE_NOW",
                                                  "label": "LIVE"}}],
            "thumbnail": {"thumbnails": [{"url": "https://i.ytimg.com/vi/livefeed001/hqdefault.jpg"}]}
          }}}}]}}})",
        false);
    check(liveFeed.ok, "лента с эфиром разобрана", liveFeed.error);
    bool liveItem = false;
    for (const auto& sec : liveFeed.sections)
        for (const auto& it : sec.items)
            if (it.live && it.duration == "LIVE") liveItem = true;
    check(liveItem, "бейдж LIVE_NOW -> live и duration=LIVE");

    // YT-05: playabilityStatus с reason и без.
    std::string badErr;
    VideoDetails badDetails;
    check(!host_parse_player(
              R"({"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Войдите в аккаунт"}})",
              "x", badDetails, badErr),
          "недоступное видео → false");
    checkEq(badErr, "Войдите в аккаунт", "причина недоступности (reason)");
    checkEq(badDetails.playabilityStatus, "LOGIN_REQUIRED", "сохранён статус");

    // --- 6. Подсказки поиска (JSONP) и офлайн-init ---
    std::printf("\n6) suggestions / init\n");
    InnerTube client;
    std::vector<std::string> sugg = client.suggestions("switch");
    checkEqInt((long long)sugg.size(), 3, "3 подсказки из JSONP");
    if (!sugg.empty()) checkEq(sugg[0], "switch homebrew", "первая подсказка");

    check(client.init("RU", "ru"), "init без сети возвращает true");
    check(!client.error().empty(), "init заполнил error_ при сетевой ошибке");
    check(!client.visitorData().empty(), "visitor сгенерирован локально");
    check(!client.visitorLogId().empty(), "visitorLogId не пуст");
    checkEqInt(client.clientVersion(), 0, "клиент по умолчанию ANDROID_VR");
    client.setClientVersion(2);
    checkEqInt(client.clientVersion(), 2, "setClientVersion(2)");
    client.setClientVersion(99);
    checkEqInt(client.clientVersion(), 3, "setClientVersion ограничен сверху (4 клиента)");
    VideoDetails offline;
    check(!client.video("player00001", offline), "video без сети → false");

    // --- 7. Пустой/битый JSON не роняет парсер ---
    std::printf("\n7) устойчивость\n");
    Feed broken = host_parse_feed("{не json", false);
    check(!broken.ok, "битый JSON → ok=false");
    check(!broken.error.empty(), "битый JSON → текст ошибки");
    Feed empty = host_parse_feed("{}", false);
    check(empty.ok, "пустой объект → ok=true без секций");
    check(empty.sections.empty(), "пустой объект → секций нет");

    // YT-12: лимиты парсера. Глубокая вложенность и обрезанный JSON не должны
    // ни ронять процесс, ни давать «полуразобранную» ленту.
    std::string deep(4000, '[');
    deep += std::string(4000, ']');
    Feed deepF = host_parse_feed(deep, false);
    check(!deepF.ok, "вложенность 4000 → ошибка, без падения");

    std::string truncated = "{\"contents\":[{\"videoRenderer\":{\"videoId\":\"abc\",\"tit";
    Feed truncF = host_parse_feed(truncated, false);
    check(!truncF.ok, "обрезанный JSON → ошибка, без падения");
    check(truncF.sections.empty(), "обрезанный JSON → секций нет");

    // Мусор вместо ожидаемых типов: videoRenderer массивом, runs строкой.
    Feed odd = host_parse_feed(
        "{\"contents\":[{\"videoRenderer\":[]},{\"title\":{\"runs\":\"x\"}}]}", false);
    check(odd.ok, "неверные типы узлов не мешают разбору");
    check(odd.sections.empty(), "неверные типы узлов → секций нет");

    // --- 8. YT-03: матрица клиентов и fallback ---
    std::printf("\n8) матрица клиентов (YT-03)\n");
    checkEqInt(host_client_count(), 4, "4 клиента в матрице");
    checkEq(host_client_name(0), "ANDROID_VR", "клиент 0");
    checkEq(host_client_name(1), "ANDROID", "клиент 1");
    checkEq(host_client_name(2), "IOS", "клиент 2");
    checkEq(host_client_name(3), "TVHTML5", "клиент 3");
    checkEqInt(host_next_client(0), 1, "fallback 0→1");
    checkEqInt(host_next_client(1), 2, "fallback 1→2");
    checkEqInt(host_next_client(2), 3, "fallback 2→3");
    checkEqInt(host_next_client(3), -1, "3 — последний, дальше нет");
    checkEqInt(client.browseClient(), 3, "browseClient после setClientVersion(99)");

    // --- 9. YT-05: маппинг playabilityStatus ---
    std::printf("\n9) playabilityStatus (YT-05)\n");
    checkEq(host_playability_text("OK"), "", "OK → пусто");
    checkEq(host_playability_text("LOGIN_REQUIRED"), "Требуется вход в аккаунт", "LOGIN_REQUIRED");
    checkEq(host_playability_text("AGE_VERIFICATION"), "Требуется подтверждение возраста",
            "AGE_VERIFICATION");
    checkEq(host_playability_text("UNPLAYABLE"), "Видео недоступно", "UNPLAYABLE");
    checkEq(host_playability_text("LIVE_STREAM_OFFLINE"), "Прямой эфир не идёт",
            "LIVE_STREAM_OFFLINE");
    checkEq(host_playability_text("GEO_BLOCKED"), "Видео заблокировано в вашем регионе",
            "GEO_BLOCKED");
    {
        std::string e;
        VideoDetails d;
        check(!host_parse_player(R"({"playabilityStatus":{"status":"GEO_BLOCKED"}})", "x", d, e),
              "GEO без reason → false");
        checkEq(e, "Видео заблокировано в вашем регионе", "маппинг без reason");
    }

    // --- 10. YT-06: continuation, дедуп, лимит, гонки ---
    std::printf("\n10) continuation/дедуп (YT-06)\n");
    FeedPager pager;
    checkEqInt(pager.pages(), 0, "страниц 0");
    check(pager.beginPage(), "первая страница занята");
    check(!pager.beginPage(), "параллельная continuation запрещена");
    pager.endPage();
    checkEqInt(pager.pages(), 1, "страниц 1");
    {
        Feed p1;
        Section s1;
        VideoItem a;
        a.id = "v1";
        s1.items.push_back(a);
        VideoItem b;
        b.id = "v2";
        s1.items.push_back(b);
        p1.sections.push_back(s1);
        checkEqInt((long long)pager.dedup(p1), 2, "первая страница: 2 новых");

        Feed p2;
        Section s2;
        VideoItem c;
        c.id = "v2";
        s2.items.push_back(c);
        VideoItem d;
        d.id = "v3";
        s2.items.push_back(d);
        p2.sections.push_back(s2);
        checkEqInt((long long)pager.dedup(p2), 1, "вторая страница: 1 новый (дедуп v2)");
        checkEqInt((long long)p2.sections[0].items.size(), 1, "дубликат удалён из ленты");
        if (!p2.sections[0].items.empty())
            checkEq(p2.sections[0].items[0].id, "v3", "оставлен новый v3");
    }
    for (int k = 0; k < 9; ++k) {
        if (pager.beginPage()) pager.endPage();
    }
    checkEqInt(pager.pages(), 10, "10 страниц");
    check(pager.limitReached(), "лимит страниц достигнут");
    check(!pager.beginPage(), "11-я страница запрещена");
    pager.reset();
    checkEqInt(pager.pages(), 0, "reset сбросил счётчик");
    check(pager.beginPage(), "после reset можно снова");

    // --- 11. YT-01/YT-02: извлечение ключа/visitor и персистентность ---
    std::printf("\n11) ключ/visitor (YT-01/YT-02)\n");
    const std::string homeHtml =
        R"({"INNERTUBE_API_KEY":"AIzaSyTESTKEY1234567890","VISITOR_DATA":"CgtTOKEN123"})";
    checkEq(host_extract_key(homeHtml), "AIzaSyTESTKEY1234567890", "ключ из home HTML");
    checkEq(host_extract_visitor(homeHtml), "CgtTOKEN123", "visitor из VISITOR_DATA");
    checkEq(host_extract_key("{}"), "", "нет ключа → пусто");
    const std::string swData = R"([["CgtSWTOKEN999",1,"x"],"other"])";
    checkEq(host_extract_visitor(swData), "CgtSWTOKEN999", "visitor из sw.js_data (Cgt-токен)");

    InnerTube c2;
    c2.setVisitorData("SAVED_VISITOR_ABC", 0);
    check(c2.visitorFromConfig(), "visitor помечен как из конфига");
    c2.init("RU", "ru");
    checkEq(c2.visitorData(), "SAVED_VISITOR_ABC", "visitor из конфига сохранён при запуске");
    check(c2.visitorFromConfig(), "после init visitor всё ещё из конфига");
    check(!c2.visitorLogId().empty(), "visitorLogId не пуст");
    check(c2.visitorLogId().find("SAVED_VISITOR_ABC") == std::string::npos,
          "логи не содержат visitor целиком");
    {
        VisitorState st = c2.visitorState();
        checkEq(st.data, "SAVED_VISITOR_ABC", "visitorState.data для конфига");
    }

    InnerTube c3;
    c3.setVisitorData("OLD_VISITOR", 1); // 1970 год → протух
    c3.init("RU", "ru");
    check(c3.visitorData() != "OLD_VISITOR", "устаревший visitor ротирован");
    check(!c3.visitorData().empty(), "новый visitor сгенерирован");

    // --- 12. YT-09: цепочка обложек по id ---
    std::printf("\n12) обложки (YT-09)\n");
    std::vector<std::string> chain = host_thumb_candidates("dQw4w9WgXcQ", 4);
    checkEqInt((long long)chain.size(), 5, "5 размеров в цепочке");
    if (chain.size() == 5) {
        checkEq(chain[0], "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg",
                "первый — maxresdefault");
        checkEq(chain[4], "https://i.ytimg.com/vi/dQw4w9WgXcQ/default.jpg", "последний — default");
    }
    check(host_thumb_candidates("", 4).empty(), "пустой id → пустая цепочка");

    // --- 13. YT-08: детект истёкших ссылок ---
    std::printf("\n13) истёкшие ссылки (YT-08)\n");
    check(InnerTube::isExpiredStatus(403), "403 — истёкшая ссылка");
    check(InnerTube::isExpiredStatus(410), "410 — истёкшая ссылка");
    check(!InnerTube::isExpiredStatus(200), "200 — не истёкшая");
    check(!InnerTube::isExpiredStatus(404), "404 — не истёкшая ссылка");

    // --- 14. NET-11: токен отмены доходит до транспорта ---
    std::printf("\n14) отмена запросов (NET-11)\n");
    {
        InnerTube c4;
        tnx::net::CancelToken tok;
        tnx::net::g_lastPostCancel = nullptr;
        c4.setCancelToken(&tok);
        check(c4.cancelToken() == &tok, "токен сохранён в InnerTube");
        (void)c4.home(); // apiPost("browse") → net::post с токеном
        check(tnx::net::g_lastPostCancel == &tok, "browse: токен передан в net::post");
        check(tnx::net::g_lastPostEndpoint.find("/browse") != std::string::npos,
              "browse: запрос ушёл на /browse");

        // /player не должен зависеть от токена экрана поиска.
        tnx::net::g_lastPostCancel = nullptr;
        VideoDetails vd;
        (void)c4.video("dQw4w9WgXcQ", vd);
        check(tnx::net::g_lastPostCancel == nullptr, "player: токен в net::post не передаётся");

        c4.setCancelToken(nullptr);
        check(c4.cancelToken() == nullptr, "токен можно снять");
    }

    std::printf("\nИтог: %d проверок, %d ошибок\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
