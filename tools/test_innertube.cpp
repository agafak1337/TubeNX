// tools/test_innertube.cpp — хостовый тест парсера InnerTube (без сети).
// Сборка:
//   g++ -std=c++17 -DTNX_HOST -I../source -I$DEVKITPRO/portlibs/switch/include
//       test_innertube.cpp ../source/yt/innertube.cpp -o test_innertube
//       -L/usr/lib/x86_64-linux-gnu -l:libjansson.so.4
// Запуск из корня проекта: ./test_innertube tools/sample_yt.json
#include "net/http.hpp"
#include "yt/innertube.hpp"

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
} // namespace tnx::yt

// Заглушки сети: тест не ходит в интернет.
namespace tnx::net {

Response get(const std::string& url, const Headers&, int, size_t) {
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

Response post(const std::string&, const std::string&, const Headers&, int, size_t) {
    Response r;
    r.status = 0;
    r.error = "host-test: сеть отключена";
    return r;
}

std::string urlEncode(const std::string& s) {
    return s;
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

    // --- 4. pickStreams ---
    std::printf("\n4) pickStreams\n");
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

    StreamFormat muxed, videoTrack, audioTrack;
    bool ok = InnerTube::pickStreams(vd, &muxed, &videoTrack, &audioTrack);
    check(ok, "pickStreams вернул true для muxed");
    checkEqInt(muxed.itag, 22, "выбран muxed itag 22 (720p)");
    checkEq(videoTrack.url, "https://example/136", "адаптивное видео — avc1 720p");
    checkEqInt(audioTrack.itag, 140, "адаптивный звук — m4a/AAC с наибольшим битрейтом");
    check(audioTrack.hasAudio && !audioTrack.hasVideo, "аудиодорожка без видео");

    // Без itag 22 → muxed 18.
    vd.formats.erase(vd.formats.begin() + 1);
    ok = InnerTube::pickStreams(vd, &muxed, &videoTrack, &audioTrack);
    check(ok, "pickStreams без itag 22");
    checkEqInt(muxed.itag, 18, "выбран muxed itag 18 (360p)");

    // Только adaptive → muxed пуст, но video+audio есть.
    VideoDetails adaptive;
    adaptive.formats.push_back(makeFormat(137, "video/mp4; codecs=\"avc1.640028\"",
                                          "https://example/137", 1920, 1080, 4500000, true, false));
    adaptive.formats.push_back(makeFormat(136, "video/mp4; codecs=\"avc1.64001F\"",
                                          "https://example/136", 1280, 720, 2500000, true, false));
    adaptive.formats.push_back(makeFormat(134, "video/mp4; codecs=\"avc1.4D401E\"",
                                          "https://example/134", 854, 480, 1200000, true, false));
    adaptive.formats.push_back(makeFormat(140, "audio/mp4; codecs=\"mp4a.40.2\"",
                                          "https://example/140", 0, 0, 128000, false, true));
    StreamFormat onlyMuxed, onlyVideo, onlyAudio;
    ok = InnerTube::pickStreams(adaptive, &onlyMuxed, &onlyVideo, &onlyAudio);
    check(ok, "pickStreams true для adaptive video+audio");
    checkEqInt(onlyVideo.itag, 136, "видео 720p предпочтительнее 1080p");
    checkEqInt(onlyAudio.itag, 140, "звук m4a/aac");

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
    checkEqInt((long long)pd.formats.size(), 3, "формат с signatureCipher пропущен");

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
    check(InnerTube::pickStreams(pd, &pm, &pv, &pa), "pickStreams по разобранному player");
    checkEqInt(pm.itag, 18, "muxed из player — itag 18");
    checkEqInt(pv.itag, 137, "видео из player — itag 137");
    checkEqInt(pa.itag, 140, "звук из player — itag 140");

    std::string badErr;
    VideoDetails badDetails;
    check(!host_parse_player(
              R"({"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Войдите в аккаунт"}})",
              "x", badDetails, badErr),
          "недоступное видео → false");
    checkEq(badErr, "Войдите в аккаунт", "причина недоступности");

    // --- 6. Подсказки поиска (JSONP) и офлайн-init ---
    std::printf("\n6) suggestions / init\n");
    InnerTube client;
    std::vector<std::string> sugg = client.suggestions("switch");
    checkEqInt((long long)sugg.size(), 3, "3 подсказки из JSONP");
    if (!sugg.empty()) checkEq(sugg[0], "switch homebrew", "первая подсказка");

    check(client.init("RU", "ru"), "init без сети возвращает true");
    check(!client.error().empty(), "init заполнил error_ при сетевой ошибке");
    checkEqInt(client.clientVersion(), 0, "клиент по умолчанию ANDROID_VR");
    client.setClientVersion(2);
    checkEqInt(client.clientVersion(), 2, "setClientVersion(2)");
    client.setClientVersion(99);
    checkEqInt(client.clientVersion(), 2, "setClientVersion ограничен сверху");
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

    std::printf("\nИтог: %d проверок, %d ошибок\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}