// yt/innertube.cpp — клиент внутреннего API YouTube (InnerTube) без API-ключа.
// Разбор ответов на libjansson, транспорт — net::get/net::post (libcurl).
#include "yt/innertube.hpp"

#include "net/http.hpp"

#include <jansson.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tnx::yt {
namespace {

// Публичный ключ веб-клиента. Используется, если вытащить ключ из HTML не удалось.
constexpr const char* kPublicApiKey = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";

// Описание поддерживаемых клиентов InnerTube.
struct ClientInfo {
    const char* name;
    const char* ver;
    int id;            // X-YouTube-Client-Name
    const char* ua;
    int kind;          // 0 — Android, 1 — iOS, 2 — Web
};

const ClientInfo kClients[3] = {
    {"ANDROID_VR", "1.62.27", 28,
     "com.google.android.apps.youtube.vr.oculus/1.62.27 (Linux; U; Android 12; en_US; Quest 3 "
     "Build/SQ3A.220605.009.A1) gzip",
     0},
    {"ANDROID", "20.10.38", 3,
     "com.google.android.youtube/20.10.38 (Linux; U; Android 12) gzip", 0},
    {"WEB", "2.20241126.01.00", 1,
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/131.0.0.0 Safari/537.36",
     2},
};

const ClientInfo& clientInfo(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    return kClients[idx];
}

// RAII-обёртка над json_t: гарантирует decref при любом выходе.
struct JsonDoc {
    json_t* root = nullptr;

    explicit JsonDoc(const std::string& text) {
        json_error_t err;
        std::memset(&err, 0, sizeof(err));
        root = json_loads(text.c_str(), 0, &err);
    }
    ~JsonDoc() {
        if (root) json_decref(root);
    }
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    bool ok() const { return root != nullptr; }
};

// Доступ по пути "a.b[0].c[-1].d". Отсутствующие узлы дают nullptr.
json_t* jsonPath(json_t* root, const std::string& path) {
    if (!root) return nullptr;
    json_t* cur = root;
    size_t i = 0;
    while (i < path.size() && cur) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            size_t close = path.find(']', i);
            if (close == std::string::npos) return nullptr;
            if (!json_is_array(cur)) return nullptr;
            std::string idx = path.substr(i + 1, close - i - 1);
            json_int_t n = 0;
            if (!idx.empty() && idx[0] == '-') {
                n = (json_int_t)json_array_size(cur) - std::atoll(idx.c_str() + 1);
            } else {
                n = (json_int_t)std::atoll(idx.c_str());
            }
            if (n < 0 || n >= (json_int_t)json_array_size(cur)) return nullptr;
            cur = json_array_get(cur, (size_t)n);
            i = close + 1;
            continue;
        }
        size_t j = i;
        while (j < path.size() && path[j] != '.' && path[j] != '[') ++j;
        if (!json_is_object(cur)) return nullptr;
        std::string name = path.substr(i, j - i);
        cur = json_object_get(cur, name.c_str());
        i = j;
    }
    return cur;
}

// Текст из узла InnerTube: строка, {"simpleText":...} или {"runs":[{text},...]}.
std::string jsonText(json_t* node) {
    if (!node) return {};
    if (json_is_string(node)) return std::string(json_string_value(node));
    if (!json_is_object(node)) return {};

    json_t* simple = json_object_get(node, "simpleText");
    if (json_is_string(simple)) return std::string(json_string_value(simple));

    json_t* runs = json_object_get(node, "runs");
    if (json_is_array(runs)) {
        std::string out;
        size_t i;
        json_t* run;
        json_array_foreach(runs, i, run) {
            if (json_is_string(run)) {
                out += json_string_value(run);
            } else if (json_is_object(run)) {
                json_t* t = json_object_get(run, "text");
                if (json_is_string(t)) out += json_string_value(t);
            }
        }
        if (!out.empty()) return out;
    }

    // Некоторые заголовки завёрнуты в content/text/title/label.
    for (const char* key : {"content", "text", "title", "label"}) {
        json_t* v = json_object_get(node, key);
        if (json_is_string(v)) return std::string(json_string_value(v));
        if (json_is_object(v)) {
            std::string t = jsonText(v);
            if (!t.empty()) return t;
        }
    }
    return {};
}

std::string jsonStringAt(json_t* root, const std::string& path) {
    return jsonText(jsonPath(root, path));
}

std::string firstText(json_t* obj, std::initializer_list<const char*> keys) {
    if (!json_is_object(obj)) return {};
    for (const char* key : keys) {
        std::string s = jsonText(json_object_get(obj, key));
        if (!s.empty()) return s;
    }
    return {};
}

int64_t jsonInt64(json_t* node) {
    if (!node) return 0;
    if (json_is_integer(node)) return (int64_t)json_integer_value(node);
    if (json_is_real(node)) return (int64_t)json_real_value(node);
    if (json_is_string(node)) return std::atoll(json_string_value(node));
    return 0;
}

// Первый объект по ключу в глубину (порядок документа).
json_t* findFirstObject(json_t* node, const char* key, int depth = 0) {
    if (!node || depth > 64) return nullptr;
    if (json_is_object(node)) {
        json_t* direct = json_object_get(node, key);
        if (direct) return direct;
        const char* k;
        json_t* v;
        json_object_foreach(node, k, v) {
            json_t* found = findFirstObject(v, key, depth + 1);
            if (found) return found;
        }
    } else if (json_is_array(node)) {
        size_t i;
        json_t* v;
        json_array_foreach(node, i, v) {
            json_t* found = findFirstObject(v, key, depth + 1);
            if (found) return found;
        }
    }
    return nullptr;
}

// "1 234 567 просмотров" / "1,2 млн просмотров" / "12 тыс." → число.
int64_t parseCountText(const std::string& text) {
    if (text.empty()) return 0;

    std::string low;
    low.reserve(text.size());
    for (char c : text) low += (char)std::tolower((unsigned char)c);

    double mult = 1.0;
    if (low.find("тыс") != std::string::npos) mult = 1e3;
    else if (low.find("млрд") != std::string::npos) mult = 1e9;
    else if (low.find("млн") != std::string::npos) mult = 1e6;

    if (mult <= 1.0) {
        int64_t v = 0;
        for (char c : text) {
            if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        }
        return v;
    }

    // Сокращённая форма: первое число с одним десятичным разделителем.
    std::string num;
    bool sep = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            num += c;
        } else if ((c == ',' || c == '.') && !sep) {
            sep = true;
            num += '.';
        } else if (!num.empty()) {
            break;
        }
    }
    if (num.empty()) return 0;
    return (int64_t)std::llround(std::atof(num.c_str()) * mult);
}

// "12:34" / "1:02:03" → секунды.
int parseClock(const std::string& text) {
    int parts[3] = {0, 0, 0};
    int n = 0;
    int cur = 0;
    bool digit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            digit = true;
        } else if (c == ':' && n < 2) {
            parts[n++] = cur;
            cur = 0;
            digit = false;
        } else if (digit) {
            break;
        }
    }
    if (!digit) return 0;
    parts[n] = cur;
    if (n == 0) return parts[0];
    if (n == 1) return parts[0] * 60 + parts[1];
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

// "2024-11-26" → "26 нояб. 2024 г."
std::string humanDate(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return iso;
    if (m < 1 || m > 12 || d < 1 || y < 1900) return iso;
    static const char* months[] = {"янв.", "февр.", "мар.", "апр.", "мая",  "июн.",
                                   "июл.", "авг.",  "сент.", "окт.", "нояб.", "дек."};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %s %d г.", d, months[m - 1], y);
    return std::string(buf);
}

std::string pctDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += (char)(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string base64Url(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(len * 4 / 3 + 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        bool two = (i + 1 < len);
        if (two) v |= (uint32_t)data[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (two) out += tbl[(v >> 6) & 63];
    }
    return out;
}

// Локальный visitorData на случай, если сеть недоступна.
std::string genVisitorData() {
    uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    seed ^= (uint64_t)(uintptr_t)&seed * 0x9E3779B97F4A7C15ull;
    std::mt19937_64 rng(seed);
    unsigned char buf[18]; // 18 байт → 24 символа base64url без паддинга
    for (unsigned char& b : buf) b = (unsigned char)(rng() >> 24);
    return base64Url(buf, sizeof(buf));
}

// Ищет в тексте HTML/JS пару "ключ":"значение".
bool extractEmbeddedString(const std::string& src, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = src.find(needle, pos)) != std::string::npos) {
        size_t p = pos + needle.size();
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
        if (p < src.size() && src[p] == ':') {
            ++p;
            while (p < src.size() &&
                   (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == '\r')) {
                ++p;
            }
            if (p < src.size() && src[p] == '"') {
                ++p;
                std::string val;
                bool esc = false;
                while (p < src.size()) {
                    char c = src[p++];
                    if (esc) {
                        val += c;
                        esc = false;
                        continue;
                    }
                    if (c == '\\') {
                        esc = true;
                        continue;
                    }
                    if (c == '"') break;
                    val += c;
                }
                if (!val.empty()) {
                    out = std::move(val);
                    return true;
                }
            }
        }
        pos += needle.size();
    }
    return false;
}

// sw.js_data: visitorData лежит голой строкой вида "Cgt...".
std::string scanVisitorToken(const std::string& src) {
    size_t pos = src.find("\"Cgt");
    if (pos == std::string::npos) return {};
    size_t start = pos + 1;
    size_t end = src.find('"', start);
    if (end == std::string::npos || end <= start) return {};
    return pctDecode(src.substr(start, end - start));
}

// Экранирование строки для JSON-запроса.
std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += "\"";
    return out;
}

// context{} запроса InnerTube.
std::string ctxJson(const std::string& region, const std::string& hl, const std::string& visitor,
                    int client) {
    const ClientInfo& ci = clientInfo(client);
    // Возвращает ЗНАЧЕНИЕ поля "context" (без самого ключа): все вызывающие
    // собирают тело как {"context": + ctxJson(...) + ,...}. Раньше здесь был
    // ещё один {"context": внутри — тело получалось битым JSON, и YouTube
    // отвечал 400 Bad Request.
    std::string s = "{\"client\":{\"clientName\":" + jsonQuote(ci.name) +
                    ",\"clientVersion\":" + jsonQuote(ci.ver) + ",\"hl\":" + jsonQuote(hl) +
                    ",\"gl\":" + jsonQuote(region);
    if (!visitor.empty()) s += ",\"visitorData\":" + jsonQuote(visitor);
    if (ci.kind == 0) {
        s += ",\"androidSdkVersion\":32,\"osName\":\"Android\",\"osVersion\":\"12\"";
    } else if (ci.kind == 1) {
        s += ",\"osName\":\"iPhone\",\"osVersion\":\"18.1.0.22B83\",\"deviceModel\":\"iPhone16,2\"";
    } else {
        s += ",\"userAgent\":" + jsonQuote(ci.ua);
    }
    s += "},\"user\":{\"lockedSafetyMode\":false}}";
    return s;
}

net::Headers apiHeaders(const std::string& visitor, const std::string& hl, int client) {
    const ClientInfo& ci = clientInfo(client);
    net::Headers h;
    h.reserve(8);
    h.push_back({"Content-Type", "application/json"});
    h.push_back({"X-YouTube-Client-Name", std::to_string(ci.id)});
    h.push_back({"X-YouTube-Client-Version", ci.ver});
    if (!visitor.empty()) h.push_back({"X-Goog-Visitor-Id", visitor});
    h.push_back({"Origin", "https://www.youtube.com"});
    h.push_back({"Accept-Language", hl});
    h.push_back({"User-Agent", ci.ua});
    h.push_back({"Accept", "application/json"});
    return h;
}

std::string apiUrl(const std::string& endpoint, const std::string& key) {
    return "https://www.youtube.com/youtubei/v1/" + endpoint + "?key=" + key +
           "&prettyPrint=false";
}

std::string httpError(const net::Response& r) {
    if (!r.error.empty()) return r.error;
    return "HTTP " + std::to_string(r.status);
}

std::string codecsFromMime(const std::string& mime) {
    size_t p = mime.find("codecs=");
    if (p == std::string::npos) return {};
    p += 7;
    while (p < mime.size() && (mime[p] == '"' || mime[p] == ' ')) ++p;
    size_t e = mime.find('"', p);
    if (e == std::string::npos) e = mime.find(';', p);
    if (e == std::string::npos) e = mime.size();
    return mime.substr(p, e - p);
}

std::string absoluteUrl(std::string url) {
    if (url.rfind("//", 0) == 0) return "https:" + url;
    return url;
}

bool isVideoRendererKey(const std::string& key) {
    return key == "videoRenderer" || key == "gridVideoRenderer" || key == "reelItemRenderer" ||
           key == "compactVideoRenderer" || key == "videoWithContextRenderer";
}

bool isShelfKey(const std::string& key) {
    return key == "shelfRenderer" || key == "richShelfRenderer" || key == "reelShelfRenderer";
}

// --- Разбор одного видео-рендерера ---

VideoItem parseVideoRenderer(json_t* vr) {
    VideoItem v;
    if (!json_is_object(vr)) return v;

    v.id = jsonText(json_object_get(vr, "videoId"));
    if (v.id.empty()) {
        json_t* ne = json_object_get(vr, "navigationEndpoint");
        v.id = jsonText(jsonPath(ne, "reelWatchEndpoint.videoId"));
        if (v.id.empty()) v.id = jsonText(jsonPath(ne, "watchEndpoint.videoId"));
    }

    v.title = jsonText(json_object_get(vr, "title"));
    if (v.title.empty()) v.title = jsonText(json_object_get(vr, "headline"));
    if (v.title.empty()) v.title = jsonText(json_object_get(vr, "videoInfo"));

    v.channel = firstText(vr, {"ownerText", "longBylineText", "shortBylineText", "channelName",
                               "author"});
    json_t* byline = json_object_get(vr, "ownerText");
    if (!byline) byline = json_object_get(vr, "longBylineText");
    if (!byline) byline = json_object_get(vr, "shortBylineText");
    v.channelId = jsonText(jsonPath(byline, "runs[0].navigationEndpoint.browseEndpoint.browseId"));
    if (v.channelId.empty()) v.channelId = jsonText(json_object_get(vr, "channelId"));

    json_t* thumbs = jsonPath(vr, "thumbnail.thumbnails");
    if (json_is_array(thumbs) && json_array_size(thumbs) > 0) {
        json_t* last = json_array_get(thumbs, (size_t)json_array_size(thumbs) - 1);
        v.thumbnailUrl = absoluteUrl(jsonText(json_object_get(last, "url")));
    }
    if (v.thumbnailUrl.empty() && !v.id.empty()) v.thumbnailUrl = thumbUrl(v.id, 2);

    const std::string viewsText = firstText(vr, {"shortViewCountText", "viewCountText"});
    v.viewCount = parseCountText(viewsText);
    if (v.viewCount == 0) v.viewCount = jsonInt64(json_object_get(vr, "viewCount"));

    const std::string published = firstText(vr, {"publishedTimeText"});
    if (!viewsText.empty() || !published.empty()) {
        v.meta = viewsText;
        if (!viewsText.empty() && !published.empty()) v.meta += " • ";
        v.meta += published;
    }

    std::string lengthText = firstText(vr, {"lengthText"});
    json_t* overlay = findFirstObject(vr, "thumbnailOverlayTimeStatusRenderer");
    if (lengthText.empty()) lengthText = jsonText(json_object_get(overlay, "text"));
    const std::string overlayStyle = jsonText(json_object_get(overlay, "style"));

    v.live = (overlayStyle == "LIVE" || lengthText == "LIVE");

    if (!v.live) {
        json_t* badges = json_object_get(vr, "badges");
        if (json_is_array(badges)) {
            size_t i;
            json_t* badge;
            json_array_foreach(badges, i, badge) {
                json_t* meta = json_object_get(badge, "metadataBadgeRenderer");
                const std::string style = jsonText(json_object_get(meta, "style"));
                const std::string label = jsonText(json_object_get(meta, "label"));
                if (style == "BADGE_STYLE_TYPE_LIVE_NOW" || label == "LIVE" ||
                    label == "ПРЯМОЙ ЭФИР") {
                    v.live = true;
                    break;
                }
            }
        }
    }

    if (v.live) {
        v.duration = "LIVE";
        v.lengthSeconds = 0;
    } else if (!lengthText.empty()) {
        v.duration = lengthText;
        v.lengthSeconds = parseClock(lengthText);
    }
    return v;
}

// Собирает видео из произвольного контейнера в порядке появления, без дублей.
void collectVideos(json_t* node, std::vector<VideoItem>& out, std::set<std::string>& seen,
                   int depth = 0) {
    if (!node || depth > 64) return;

    if (json_is_array(node)) {
        size_t i;
        json_t* v;
        json_array_foreach(node, i, v) collectVideos(v, out, seen, depth + 1);
        return;
    }
    if (!json_is_object(node)) return;

    for (const char* key : {"videoRenderer", "gridVideoRenderer", "reelItemRenderer",
                            "compactVideoRenderer", "videoWithContextRenderer"}) {
        json_t* vr = json_object_get(node, key);
        if (!json_is_object(vr)) continue;
        VideoItem item = parseVideoRenderer(vr);
        if (!item.id.empty() && seen.insert(item.id).second) out.push_back(std::move(item));
    }

    const char* key;
    json_t* val;
    json_object_foreach(node, key, val) {
        if (isVideoRendererKey(key) || isShelfKey(key)) continue;
        collectVideos(val, out, seen, depth + 1);
    }
}

// Ряд фильтров (chipCloudRenderer) → подписи.
void collectChips(json_t* node, std::vector<std::string>& out, int depth = 0) {
    if (!node || depth > 48) return;
    if (json_is_array(node)) {
        size_t i;
        json_t* v;
        json_array_foreach(node, i, v) collectChips(v, out, depth + 1);
        return;
    }
    if (!json_is_object(node)) return;

    json_t* cloud = json_object_get(node, "chipCloudRenderer");
    if (json_is_object(cloud)) {
        json_t* chips = json_object_get(cloud, "chips");
        if (json_is_array(chips)) {
            size_t i;
            json_t* chip;
            json_array_foreach(chips, i, chip) {
                json_t* cc = json_object_get(chip, "chipCloudChipRenderer");
                if (!json_is_object(cc)) cc = json_object_get(chip, "chipRenderer");
                if (!json_is_object(cc)) continue;
                std::string label = jsonText(json_object_get(cc, "text"));
                if (label.empty()) label = jsonText(json_object_get(cc, "label"));
                if (!label.empty()) out.push_back(std::move(label));
            }
        }
        return;
    }

    const char* key;
    json_t* val;
    json_object_foreach(node, key, val) collectChips(val, out, depth + 1);
}

// Полки (shelfRenderer) со своими заголовками.
void collectShelves(json_t* node, std::vector<Section>& out, std::set<std::string>& seen,
                    int depth = 0) {
    if (!node || depth > 64) return;
    if (json_is_array(node)) {
        size_t i;
        json_t* v;
        json_array_foreach(node, i, v) collectShelves(v, out, seen, depth + 1);
        return;
    }
    if (!json_is_object(node)) return;

    for (const char* key : {"shelfRenderer", "richShelfRenderer", "reelShelfRenderer"}) {
        json_t* shelf = json_object_get(node, key);
        if (!json_is_object(shelf)) continue;
        Section s;
        s.title = firstText(shelf, {"title"});
        if (s.title.empty()) s.title = jsonStringAt(shelf, "header.shelfHeaderRenderer.title");
        s.subtitle = firstText(shelf, {"subtitle"});
        if (s.subtitle.empty()) s.subtitle = jsonStringAt(shelf, "header.shelfHeaderRenderer.subtitle");
        collectVideos(shelf, s.items, seen, depth + 1);
        if (!s.items.empty()) out.push_back(std::move(s));
        return; // внутренние полки этой полки уже собраны
    }

    const char* key;
    json_t* val;
    json_object_foreach(node, key, val) collectShelves(val, out, seen, depth + 1);
}

// Токен продолжения ленты.
std::string findContinuation(json_t* root) {
    if (json_t* cc = findFirstObject(root, "continuationCommand")) {
        std::string t = jsonText(json_object_get(cc, "token"));
        if (!t.empty()) return t;
    }
    if (json_t* nc = findFirstObject(root, "nextContinuationData")) {
        std::string t = jsonText(json_object_get(nc, "continuation"));
        if (!t.empty()) return t;
    }
    return {};
}

// Общий разбор лент: главная, тренды, поиск, канал, подгрузка.
Feed parseFeedImpl(const std::string& text, bool isSearch) {
    Feed feed;
    JsonDoc doc(text);
    if (!doc.ok()) {
        feed.error = "некорректный JSON от YouTube";
        return feed;
    }

    feed.continuation = findContinuation(doc.root);

    std::vector<std::string> chips;
    collectChips(doc.root, chips);
    if (!chips.empty()) {
        Section chipSection;
        chipSection.chips = true;
        chipSection.chipLabels = std::move(chips);
        feed.sections.push_back(std::move(chipSection));
    }

    std::set<std::string> seen;

    json_t* main = findFirstObject(doc.root, "richGridRenderer");
    if (!main) main = findFirstObject(doc.root, "sectionListRenderer");
    if (!main) main = findFirstObject(doc.root, "continuationContents");
    if (!main) main = doc.root;

    Section primary;
    primary.title = isSearch ? "Результаты" : "";
    collectVideos(main, primary.items, seen);
    if (!primary.items.empty()) feed.sections.push_back(std::move(primary));

    std::vector<Section> shelves;
    collectShelves(doc.root, shelves, seen);
    for (Section& s : shelves) feed.sections.push_back(std::move(s));

    feed.ok = true;
    return feed;
}

// Разбор /next: похожие видео в secondaryResults.
Feed parseRelatedImpl(const std::string& text) {
    Feed feed;
    JsonDoc doc(text);
    if (!doc.ok()) {
        feed.error = "некорректный JSON от YouTube";
        return feed;
    }

    json_t* secondary = findFirstObject(doc.root, "secondaryResults");
    if (!secondary) secondary = doc.root;

    std::set<std::string> seen;
    Section s;
    s.title = "Похожие";
    collectVideos(secondary, s.items, seen);
    if (!s.items.empty()) feed.sections.push_back(std::move(s));
    feed.continuation = findContinuation(secondary);
    feed.ok = true;
    return feed;
}

bool parseStreamArray(json_t* arr, std::vector<StreamFormat>& out) {
    if (!json_is_array(arr)) return false;
    size_t i;
    json_t* node;
    json_array_foreach(arr, i, node) {
        if (!json_is_object(node)) continue;
        StreamFormat s;

        s.url = jsonText(json_object_get(node, "url"));
        // signatureCipher требует расшифровки — такие форматы пропускаем.
        if (s.url.empty()) continue;
        s.url = absoluteUrl(s.url);

        s.itag = (int)jsonInt64(json_object_get(node, "itag"));
        s.mime = jsonText(json_object_get(node, "mimeType"));
        s.qualityLabel = jsonText(json_object_get(node, "qualityLabel"));
        s.audioQuality = jsonText(json_object_get(node, "audioQuality"));
        s.bitrate = (int)jsonInt64(json_object_get(node, "bitrate"));
        s.contentLength = jsonInt64(json_object_get(node, "contentLength"));
        s.width = (int)jsonInt64(json_object_get(node, "width"));
        s.height = (int)jsonInt64(json_object_get(node, "height"));

        s.codecs = jsonText(json_object_get(node, "codecs"));
        if (s.codecs.empty()) s.codecs = codecsFromMime(s.mime);

        s.hasVideo = s.mime.find("video/") != std::string::npos;
        // Muxed-поток имеет mime "video/mp4", а звук виден только в codecs.
        const bool muxedAudio = s.mime.find("mp4a") != std::string::npos ||
                                s.mime.find("opus") != std::string::npos ||
                                s.mime.find("ac-3") != std::string::npos ||
                                s.mime.find("ec-3") != std::string::npos;
        s.hasAudio = s.mime.find("audio/") != std::string::npos || (s.hasVideo && muxedAudio);

        out.push_back(std::move(s));
    }
    return true;
}

bool parsePlayerJson(const std::string& text, const std::string& id, VideoDetails& out,
                     std::string& err) {
    JsonDoc doc(text);
    if (!doc.ok()) {
        err = "некорректный JSON от YouTube";
        return false;
    }

    const std::string status = jsonStringAt(doc.root, "playabilityStatus.status");
    if (status != "OK") {
        err = jsonStringAt(doc.root, "playabilityStatus.reason");
        if (err.empty()) {
            err = status.empty() ? "видео недоступно" : "видео недоступно (" + status + ")";
        }
        return false;
    }

    json_t* vd = jsonPath(doc.root, "videoDetails");
    if (!json_is_object(vd)) {
        err = "в ответе нет videoDetails";
        return false;
    }

    out.id = jsonText(json_object_get(vd, "videoId"));
    if (out.id.empty()) out.id = id;
    out.title = jsonText(json_object_get(vd, "title"));
    out.description = jsonText(json_object_get(vd, "shortDescription"));
    out.channel = jsonText(json_object_get(vd, "author"));
    out.channelId = jsonText(json_object_get(vd, "channelId"));
    out.lengthSeconds = (int)jsonInt64(json_object_get(vd, "lengthSeconds"));

    json_t* thumbs = jsonPath(vd, "thumbnail.thumbnails");
    if (json_is_array(thumbs) && json_array_size(thumbs) > 0) {
        json_t* last = json_array_get(thumbs, (size_t)json_array_size(thumbs) - 1);
        out.thumbnailUrl = absoluteUrl(jsonText(json_object_get(last, "url")));
    }
    if (out.thumbnailUrl.empty()) out.thumbnailUrl = thumbUrl(out.id, 2);

    out.viewCount = jsonInt64(json_object_get(vd, "viewCount"));
    if (out.viewCount == 0) {
        out.viewCount = jsonInt64(jsonPath(doc.root, "microformat.playerMicroformatRenderer.viewCount"));
    }

    std::string publish = jsonStringAt(doc.root, "microformat.playerMicroformatRenderer.publishDate");
    if (publish.empty()) {
        publish = jsonStringAt(doc.root, "microformat.playerMicroformatRenderer.uploadDate");
    }
    out.publishedText = humanDate(publish);

    out.viewText = formatViews(out.viewCount);
    out.shareUrl = "https://youtu.be/" + out.id;

    parseStreamArray(jsonPath(doc.root, "streamingData.formats"), out.formats);
    parseStreamArray(jsonPath(doc.root, "streamingData.adaptiveFormats"), out.formats);

    err.clear();
    return true;
}

} // namespace

// --- InnerTube ---

bool InnerTube::init(const std::string& region, const std::string& hl) {
    if (!region.empty()) region_ = region;
    if (!hl.empty()) hl_ = hl;
    if (hl_.empty()) hl_ = "ru";
    if (region_.empty()) region_ = "RU";

    apiKey_ = kPublicApiKey;
    if (visitorData_.empty()) visitorData_ = genVisitorData();
    error_.clear();

    const net::Headers headers = {
        {"Accept-Language", hl_},
        {"Accept", "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8"},
        // Для bootstrap-страницы всегда представляемся браузером: в мобильной
        // разметке INNERTUBE_API_KEY может отсутствовать.
        {"User-Agent", kClients[2].ua},
    };

    net::Response r = net::get("https://www.youtube.com/", headers, 30000, 6u << 20);

    bool gotKey = false, gotVisitor = false;
    std::string key, visitor;

    if (r.ok() && !r.body.empty()) {
        gotKey = extractEmbeddedString(r.body, "INNERTUBE_API_KEY", key) && key.size() > 10;
        if (gotKey) apiKey_ = key;

        gotVisitor = extractEmbeddedString(r.body, "VISITOR_DATA", visitor);
        if (!gotVisitor || visitor.empty()) {
            gotVisitor = extractEmbeddedString(r.body, "visitorData", visitor);
        }
        if (gotVisitor && !visitor.empty()) visitorData_ = pctDecode(visitor);
    } else {
        error_ = httpError(r);
        if (error_.empty() || error_ == "HTTP 0") error_ = "нет соединения с YouTube";
    }

    // Резервный источник visitorData: компактный bootstrap-скрипт.
    if (!gotVisitor || visitor.empty()) {
        net::Response r2 = net::get("https://www.youtube.com/sw.js_data", headers, 30000, 4u << 20);
        if (r2.ok() && !r2.body.empty()) {
            std::string token = scanVisitorToken(r2.body);
            if (!token.empty()) visitorData_ = token;
            if (!gotKey) {
                std::string k2;
                if (extractEmbeddedString(r2.body, "INNERTUBE_API_KEY", k2) && k2.size() > 10) {
                    apiKey_ = k2;
                }
            }
        }
    }

    if (apiKey_.empty()) apiKey_ = kPublicApiKey;
    return true; // офлайн-режим: UI должен показать ошибку, но клиент создан
}

void InnerTube::setClientVersion(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    clientVer_ = idx;
}

bool InnerTube::ensureSession() {
    if (apiKey_.empty() || visitorData_.empty()) init(region_, hl_);
    if (apiKey_.empty()) apiKey_ = kPublicApiKey;
    return true;
}

Feed InnerTube::parseFeed(const std::string& json, bool isSearch) {
    Feed f = parseFeedImpl(json, isSearch);
    if (!f.ok && !f.error.empty()) error_ = f.error;
    return f;
}

Feed InnerTube::home() {
    ensureSession();
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_) +
                       ",\"browseId\":\"FEwhat_to_watch\"}";
    net::Response r =
        net::post(apiUrl("browse", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

Feed InnerTube::trending() {
    ensureSession();
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_) +
                       ",\"browseId\":\"FEtrending\"}";
    net::Response r =
        net::post(apiUrl("browse", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

Feed InnerTube::search(const std::string& q, const std::string& continuation) {
    ensureSession();
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_);
    if (!continuation.empty()) {
        body += ",\"continuation\":" + jsonQuote(continuation);
    } else {
        body += ",\"query\":" + jsonQuote(q);
    }
    body += "}";

    net::Response r =
        net::post(apiUrl("search", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, true);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

Feed InnerTube::related(const std::string& videoId) {
    ensureSession();
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_) +
                       ",\"videoId\":" + jsonQuote(videoId) + "}";
    net::Response r =
        net::post(apiUrl("next", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseRelatedImpl(r.body);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

Feed InnerTube::channel(const std::string& channelId) {
    ensureSession();
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_) +
                       ",\"browseId\":" + jsonQuote(channelId) + "}";
    net::Response r =
        net::post(apiUrl("browse", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

Feed InnerTube::continueFeed(const std::string& continuation) {
    ensureSession();
    if (continuation.empty()) {
        Feed f;
        f.error = "пустой токен продолжения";
        error_ = f.error;
        return f;
    }
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, clientVer_) +
                       ",\"continuation\":" + jsonQuote(continuation) + "}";
    net::Response r =
        net::post(apiUrl("browse", apiKey_), body, apiHeaders(visitorData_, hl_, clientVer_), 30000);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (!f.ok) error_ = f.error;
    else error_.clear();
    return f;
}

std::vector<std::string> InnerTube::suggestions(const std::string& q) {
    std::vector<std::string> out;
    if (q.empty()) return out;

    const std::string url = "https://suggestqueries-clients6.youtube.com/complete/search"
                            "?client=youtube&ds=yt&hl=" +
                            hl_ + "&q=" + net::urlEncode(q);
    const net::Headers headers = {
        {"Accept-Language", hl_},
        {"User-Agent", clientInfo(clientVer_).ua},
    };
    net::Response r = net::get(url, headers, 30000, 1u << 20);
    if (!r.ok() || r.body.empty()) return out;

    // Ответ — JSONP: window.google.ac.h([...]).
    const size_t first = r.body.find('[');
    const size_t last = r.body.rfind(']');
    if (first == std::string::npos || last == std::string::npos || last <= first) return out;

    JsonDoc doc(r.body.substr(first, last - first + 1));
    if (!doc.ok() || !json_is_array(doc.root)) return out;

    json_t* list = json_array_get(doc.root, 1);
    if (!json_is_array(list)) return out;

    size_t i;
    json_t* entry;
    json_array_foreach(list, i, entry) {
        json_t* word = nullptr;
        if (json_is_array(entry)) word = json_array_get(entry, 0);
        else if (json_is_string(entry)) word = entry;
        if (json_is_string(word)) {
            std::string s = json_string_value(word);
            if (!s.empty()) out.push_back(std::move(s));
        }
    }
    return out;
}

// /player берём отдельным клиентом: ANDROID 20.10.38 отдаёт прямые ссылки
// на потоки без входа в аккаунт и без PO-токена (проверено на этой сети),
// тогда как ANDROID_VR для плеера отвечает LOGIN_REQUIRED.
std::string InnerTube::player(const std::string& videoId) {
    ensureSession();
    const int cl = 1;
    std::string body = "{\"context\":" + ctxJson(region_, hl_, visitorData_, cl) +
                       ",\"videoId\":" + jsonQuote(videoId) +
                       ",\"contentCheckOk\":true,\"racyCheckOk\":true"
                       ",\"playbackContext\":{\"contentPlaybackContext\":"
                       "{\"html5Preference\":\"HTML5_PREF_WANTS\"}}}";
    net::Response r = net::post(apiUrl("player", apiKey_), body, apiHeaders(visitorData_, hl_, cl), 30000);
    if (!r.ok()) {
        error_ = httpError(r);
        return {};
    }
    return r.body;
}

bool InnerTube::video(const std::string& id, VideoDetails& out) {
    out = VideoDetails{};
    if (id.empty()) {
        error_ = "пустой идентификатор видео";
        return false;
    }

    const std::string raw = player(id);
    if (raw.empty()) return false; // error_ заполнен в player()

    std::string err;
    if (!parsePlayerJson(raw, id, out, err)) {
        error_ = err;
        return false;
    }

    // Похожие подтягиваем отдельным запросом; ошибка здесь не фатальна.
    Feed rel = related(id);
    if (rel.ok) {
        for (const Section& sec : rel.sections) {
            for (const VideoItem& item : sec.items) out.related.push_back(item);
        }
    }
    error_.clear();
    return true;
}

bool InnerTube::pickStreams(const VideoDetails& v, StreamFormat* muxed, StreamFormat* video,
                            StreamFormat* audio, int preferHeight) {
    const StreamFormat* bestMuxed = nullptr;
    const StreamFormat* bestVideo = nullptr;
    const StreamFormat* bestAudio = nullptr;

    auto isMp4 = [](const StreamFormat& f) {
        return f.mime.find("mp4") != std::string::npos;
    };
    auto isAvc = [](const StreamFormat& f) {
        return f.mime.find("avc1") != std::string::npos ||
               f.codecs.find("avc1") != std::string::npos;
    };

    // 1. Muxed: itag 22 (720p) → 18 (360p) → любой video+audio.
    int bestMuxedRank = -1;
    for (const StreamFormat& f : v.formats) {
        if (f.url.empty() || !f.hasVideo || !f.hasAudio) continue;
        int rank = 0;
        if (f.itag == 22) rank = 100;
        else if (f.itag == 18) rank = 90;
        else rank = 10;
        if (isMp4(f)) rank += 3;
        if (isAvc(f)) rank += 2;
        if (rank > bestMuxedRank) {
            bestMuxedRank = rank;
            bestMuxed = &f;
        }
    }

    // 2. Адаптивное видео: H.264/MP4, ближайшее к желаемой высоте.
    // Отдельная дорожка предпочтительнее muxed, но muxed остаётся запасным вариантом.
    if (preferHeight <= 0) preferHeight = 720;
    int bestVideoRank = -1000000;
    for (const StreamFormat& f : v.formats) {
        if (f.url.empty() || !f.hasVideo) continue;
        int rank = 0;
        // Адаптивная дорожка (без звука) выигрывает у muxed: там 720p/1080p.
        if (!f.hasAudio) rank += 400;
        if (isMp4(f)) rank += 100;
        if (isAvc(f)) rank += 50;
        // VP9/AV1 в software на 4×A57 не тянет: только как последний шанс,
        // иначе получаем «720p», который дёргается и отстаёт от звука.
        if (!isAvc(f)) rank -= 500;
        // Чем ближе высота к желаемой, тем лучше; 1080p дополнительно
        // штрафуем — software-декодер консоли его не вытягивает.
        int diff = f.height > 0 ? (f.height > preferHeight ? f.height - preferHeight
                                                          : preferHeight - f.height)
                                : 1000;
        rank -= diff / 2;
        if (f.height > 720) rank -= 40;
        if (rank > bestVideoRank) {
            bestVideoRank = rank;
            bestVideo = &f;
        }
    }

    // 3. Адаптивный звук: m4a/AAC с максимальным битрейтом.
    int bestAudioBitrate = -1;
    for (const StreamFormat& f : v.formats) {
        if (f.url.empty() || !f.hasAudio || f.hasVideo) continue;
        const bool aac = f.mime.find("audio/mp4") != std::string::npos ||
                         f.mime.find("mp4a") != std::string::npos ||
                         f.codecs.find("mp4a") != std::string::npos;
        const int score = (aac ? 1000000000 : 0) + f.bitrate;
        if (score > bestAudioBitrate) {
            bestAudioBitrate = score;
            bestAudio = &f;
        }
    }

    bool foundMuxed = false, foundVideo = false, foundAudio = false;
    if (muxed && bestMuxed) {
        *muxed = *bestMuxed;
        foundMuxed = true;
    }
    if (video && bestVideo) {
        *video = *bestVideo;
        foundVideo = true;
    }
    if (audio && bestAudio) {
        *audio = *bestAudio;
        foundAudio = true;
    }
    return foundMuxed || (foundVideo && foundAudio);
}

// --- Форматирование ---

std::string formatViews(int64_t n) {
    if (n < 0) n = 0;

    auto oneDecimal = [](double value) -> std::string {
        long long tenths = (long long)std::llround(value * 10.0);
        long long whole = tenths / 10;
        long long frac = tenths % 10;
        std::string s = std::to_string(whole);
        if (frac != 0) {
            s += ",";
            s += std::to_string(frac);
        }
        return s;
    };

    auto plural = [](long long value) -> const char* {
        const long long mod100 = value % 100;
        const long long mod10 = value % 10;
        if (mod10 == 1 && mod100 != 11) return "просмотр";
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "просмотра";
        return "просмотров";
    };

    if (n < 1000) {
        return std::to_string(n) + " " + plural(n);
    }
    if (n < 1000000) {
        return oneDecimal((double)n / 1000.0) + " тыс. просмотров";
    }
    if (n < 1000000000) {
        return oneDecimal((double)n / 1000000.0) + " млн просмотров";
    }
    return oneDecimal((double)n / 1000000000.0) + " млрд просмотров";
}

std::string formatDuration(int seconds) {
    if (seconds < 0) seconds = 0;
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    }
    return std::string(buf);
}

std::string thumbUrl(const std::string& videoId, int quality) {
    static const char* names[] = {"default.jpg", "mqdefault.jpg", "hqdefault.jpg", "sddefault.jpg"};
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;
    return "https://i.ytimg.com/vi/" + videoId + "/" + names[quality];
}

} // namespace tnx::yt

// --- Хостовый тест парсера (не влияет на сборку для Switch) ---
#ifdef TNX_HOST
namespace tnx::yt {
Feed host_parse_feed(const std::string& json, bool isSearch) {
    return parseFeedImpl(json, isSearch);
}
Feed host_parse_related(const std::string& json) {
    return parseRelatedImpl(json);
}
bool host_parse_player(const std::string& json, const std::string& id, VideoDetails& out,
                       std::string& err) {
    out = VideoDetails{};
    return parsePlayerJson(json, id, out, err);
}
} // namespace tnx::yt
#endif