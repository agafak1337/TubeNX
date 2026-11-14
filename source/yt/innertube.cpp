// yt/innertube.cpp — клиент внутреннего API YouTube (InnerTube) без API-ключа.
// Разбор ответов на libjansson, транспорт — net::get/net::post (libcurl).
//
// Поток «InnerTube/данные» (M2): YT-01..YT-09. Изменения помечены аббревиатурами
// в комментариях. Логи идут через platform::logInfo/logWarn (на хосте — заглушки).
#include "yt/innertube.hpp"

#include "net/http.hpp"
#include "platform.hpp"

#include <jansson.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tnx::yt {
namespace {

// Публичный ключ веб-клиента. Используется, если вытащить ключ из HTML не удалось.
constexpr const char* kPublicApiKey = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";

// TTL visitorData (YT-02): через 30 дней считаем его протухшим.
constexpr int64_t kVisitorTtlSeconds = 30 * 24 * 3600;

// UA браузера для bootstrap-страницы (индекс 2 матрицы раньше был WEB).
constexpr const char* kWebUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

// --- Логи ---------------------------------------------------------------
void ytLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ytLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    platform::logInfo("%s", buf);
}

void ytWarn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ytWarn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    platform::logWarn("%s", buf);
}

int64_t nowEpoch() {
    return (int64_t)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

std::string lowerCopy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// YT-04: редиректы и Set-Cookie видны в r.headers (libcurl идёт за редиректами).
void logResponseHeaders(const char* tag, const net::Response& r) {
    for (const auto& h : r.headers) {
        const std::string name = lowerCopy(h.first);
        if (name == "location") {
            ytLog("yt: %s redirect -> %s", tag, h.second.c_str());
        } else if (name == "set-cookie") {
            ytLog("yt: %s set-cookie: %.48s", tag, h.second.c_str());
        }
    }
}

// Описание поддерживаемых клиентов InnerTube (YT-03).
// kind: 0 — Android, 1 — iOS, 2 — Web, 3 — TV.
struct ClientInfo {
    const char* name;
    const char* ver;
    int id;            // X-YouTube-Client-Name
    const char* ua;
    int kind;
};

// Порядок = порядок fallback (YT-03): ANDROID_VR → ANDROID → IOS → TVHTML5.
const ClientInfo kClients[4] = {
    {"ANDROID_VR", "1.62.27", 28,
     "com.google.android.apps.youtube.vr.oculus/1.62.27 (Linux; U; Android 12; en_US; Quest 3 "
     "Build/SQ3A.220605.009.A1) gzip",
     0},
    {"ANDROID", "20.10.38", 3,
     "com.google.android.youtube/20.10.38 (Linux; U; Android 12) gzip", 0},
    {"IOS", "20.10.4", 5,
     "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_1_0 like Mac OS X)", 1},
    {"TVHTML5", "7.20241126.10.00", 7,
     "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034494-gold (unlike Gecko) "
     "v8/8.8.278.8-jit gles Starboard/17, TubeNX TV",
     3},
};

const ClientInfo& clientInfo(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
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

bool jsonTrue(json_t* obj, const char* key) {
    if (!json_is_object(obj)) return false;
    return json_is_true(json_object_get(obj, key));
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

// FNV-1a: короткий отпечаток строки (для логов; сам visitor не логируется).
std::string shortFingerprint(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%08llx/len%zu", (unsigned long long)(h & 0xffffffffu),
                  s.size());
    return std::string(buf);
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

// YT-01: извлечение API-ключа из HTML/JS (bootstrap или sw.js_data).
bool extractApiKey(const std::string& body, std::string& key) {
    if (extractEmbeddedString(body, "INNERTUBE_API_KEY", key) && key.size() > 10) return true;
    if (extractEmbeddedString(body, "INNERTUBE_CONTEXT_CLIENT_CONFIG", key) && key.size() > 10)
        return true;
    key.clear();
    return false;
}

// YT-01: извлечение visitorData из HTML/JS, включая compact sw.js_data.
std::string extractVisitor(const std::string& body) {
    std::string v;
    if (extractEmbeddedString(body, "VISITOR_DATA", v) && !v.empty()) return pctDecode(v);
    if (extractEmbeddedString(body, "visitorData", v) && !v.empty()) return pctDecode(v);
    return scanVisitorToken(body);
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

// context{} запроса InnerTube. Возвращает ЗНАЧЕНИЕ поля "context" (без ключа).
std::string ctxJson(const std::string& region, const std::string& hl, const std::string& visitor,
                    int client) {
    const ClientInfo& ci = clientInfo(client);
    std::string s = "{\"client\":{\"clientName\":" + jsonQuote(ci.name) +
                    ",\"clientVersion\":" + jsonQuote(ci.ver) + ",\"hl\":" + jsonQuote(hl) +
                    ",\"gl\":" + jsonQuote(region);
    if (!visitor.empty()) s += ",\"visitorData\":" + jsonQuote(visitor);
    if (ci.kind == 0) {
        s += ",\"androidSdkVersion\":32,\"osName\":\"Android\",\"osVersion\":\"12\"";
    } else if (ci.kind == 1) {
        s += ",\"osName\":\"iPhone\",\"osVersion\":\"18.1.0.22B83\",\"deviceModel\":\"iPhone16,2\"";
    } else if (ci.kind == 3) {
        s += ",\"clientScreen\":\"TV\",\"osName\":\"Tizen\",\"osVersion\":\"6.0\"";
    } else {
        s += ",\"userAgent\":" + jsonQuote(ci.ua);
    }
    s += "},\"user\":{\"lockedSafetyMode\":false}}";
    return s;
}

// YT-04: обязательные заголовки InnerTube; X-Goog-* и Referer сверяются с клиентом.
net::Headers apiHeaders(const std::string& visitor, const std::string& hl, int client) {
    const ClientInfo& ci = clientInfo(client);
    net::Headers h;
    h.reserve(10);
    h.push_back({"Content-Type", "application/json"});
    h.push_back({"X-YouTube-Client-Name", std::to_string(ci.id)});
    h.push_back({"X-YouTube-Client-Version", ci.ver});
    h.push_back({"X-Goog-Api-Format-Version", "2"});
    if (!visitor.empty()) h.push_back({"X-Goog-Visitor-Id", visitor});
    h.push_back({"Origin", "https://www.youtube.com"});
    h.push_back({"Referer", "https://www.youtube.com/"});
    h.push_back({"Accept-Language", hl});
    h.push_back({"User-Agent", ci.ua});
    h.push_back({"Accept", "application/json"});
    return h;
}

// YT-02: короткий отпечаток visitor для логов (сам visitor не пишем целиком).
std::string visitorFingerprint(const std::string& visitor) {
    if (visitor.empty()) return "-";
    return shortFingerprint(visitor);
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

// YT-07: разбор массива форматов. live — все потоки прямого эфира помечаются
// флагом live; drmInfos/drmFamilies — флагом drm.
bool parseStreamArray(json_t* arr, std::vector<StreamFormat>& out, bool live) {
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

        s.live = live;
        json_t* drm = json_object_get(node, "drmInfos");
        if (!json_is_array(drm)) drm = json_object_get(node, "drmFamilies");
        s.drm = json_is_array(drm) && json_array_size(drm) > 0;

        out.push_back(std::move(s));
    }
    return true;
}

// YT-05: разбор /player. При не-OK playabilityStatus заполняет статус/причину и
// возвращает понятную ошибку (reason от YouTube приоритетнее нашего текста).
bool parsePlayerJson(const std::string& text, const std::string& id, VideoDetails& out,
                     std::string& err) {
    JsonDoc doc(text);
    if (!doc.ok()) {
        err = "некорректный JSON от YouTube";
        return false;
    }

    const std::string status = jsonStringAt(doc.root, "playabilityStatus.status");
    const std::string reason = jsonStringAt(doc.root, "playabilityStatus.reason");
    out.playabilityStatus = status;
    out.playabilityReason = reason;
    if (status != "OK") {
        err = !reason.empty() ? reason : InnerTube::playabilityText(status);
        if (err.empty()) err = status.empty() ? "видео недоступно" : "видео недоступно (" + status + ")";
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
    out.live = jsonTrue(vd, "isLiveContent") || jsonTrue(vd, "isLive");

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

    parseStreamArray(jsonPath(doc.root, "streamingData.formats"), out.formats, out.live);
    parseStreamArray(jsonPath(doc.root, "streamingData.adaptiveFormats"), out.formats, out.live);

    err.clear();
    return true;
}

} // namespace

// --- FeedPager (YT-06) ---

void FeedPager::reset() {
    seen_.clear();
    pages_ = 0;
    busy_.store(false);
}

bool FeedPager::beginPage() {
    if (busy_.load() || pages_ >= kMaxPages) return false;
    busy_.store(true);
    return true;
}

void FeedPager::endPage() {
    if (busy_.load()) {
        busy_.store(false);
        ++pages_;
    }
}

void FeedPager::abortPage() { busy_.store(false); }

size_t FeedPager::dedup(Feed& feed) {
    size_t added = 0;
    for (Section& s : feed.sections) {
        if (s.chips) continue;
        std::vector<VideoItem> keep;
        keep.reserve(s.items.size());
        for (VideoItem& it : s.items) {
            if (it.id.empty()) continue;
            if (seen_.insert(it.id).second) {
                keep.push_back(std::move(it));
                ++added;
            }
        }
        s.items = std::move(keep);
    }
    feed.sections.erase(
        std::remove_if(feed.sections.begin(), feed.sections.end(),
                       [](const Section& s) { return !s.chips && s.items.empty(); }),
        feed.sections.end());
    return added;
}

// --- InnerTube: матрица клиентов (YT-03) ---

int InnerTube::clientCount() { return (int)(sizeof(kClients) / sizeof(kClients[0])); }

std::string InnerTube::clientName(int idx) { return clientInfo(idx).name; }

std::string InnerTube::clientVersionStr(int idx) { return clientInfo(idx).ver; }

// YT-05
std::string InnerTube::playabilityText(const std::string& status) {
    if (status.empty() || status == "OK") return {};
    if (status == "LOGIN_REQUIRED") return "Требуется вход в аккаунт";
    if (status == "AGE_VERIFICATION" || status == "AGE_CHECK_REQUIRED")
        return "Требуется подтверждение возраста";
    if (status == "UNPLAYABLE") return "Видео недоступно";
    if (status == "LIVE_STREAM_OFFLINE") return "Прямой эфир не идёт";
    if (status == "GEO_BLOCKED" || status == "GEO_BLOCK")
        return "Видео заблокировано в вашем регионе";
    if (status == "CONTENT_CHECK_REQUIRED") return "Требуется подтверждение содержимого";
    if (status == "ERROR") return "Ошибка YouTube";
    return "Видео недоступно (" + status + ")";
}

// YT-08
bool InnerTube::isExpiredStatus(long httpStatus) { return httpStatus == 403 || httpStatus == 410; }

// --- InnerTube: сессия ---

bool InnerTube::init(const std::string& region, const std::string& hl) {
    if (!region.empty()) region_ = region;
    if (!hl.empty()) hl_ = hl;
    if (hl_.empty()) hl_ = "ru";
    if (region_.empty()) region_ = "RU";

    error_.clear();

    // YT-01: region — независимый шаг (из параметра/конфига, не из сети).
    ytLog("yt: init region=%s hl=%s", region_.c_str(), hl_.c_str());

    // YT-02: visitor из конфига; устаревший (TTL 30 дней) отбрасываем.
    if (visitorFromConfig_ && !visitorData_.empty()) {
        if (visitorSavedAtEpoch_ > 0) {
            const int64_t age = nowEpoch() - visitorSavedAtEpoch_;
            if (age > kVisitorTtlSeconds) {
                ytLog("yt: visitor из конфига устарел (%lld ч), ротация",
                      (long long)(age / 3600));
                visitorData_.clear();
                visitorFromConfig_ = false;
                visitorSource_.clear();
            } else {
                visitorSource_ = "config";
            }
        } else {
            visitorSource_ = "config";
        }
    }

    // YT-04: bootstrap всегда с consent-cookie; cookie не переиспользуется между
    // клиентами (мы не заводим cookie-jar — только явный заголовок).
    net::Headers boot;
    boot.push_back({"Accept-Language", hl_});
    boot.push_back({"Accept", "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8"});
    boot.push_back({"Cookie", "CONSENT=YES+; SOCS=CAI"});
    // Для bootstrap-страницы всегда представляемся браузером: в мобильной
    // разметке INNERTUBE_API_KEY может отсутствовать.
    boot.push_back({"User-Agent", kWebUa});

    // Шаг 1: bootstrap-страница `/`. 400/ошибка — НЕ фатально (YT-01).
    net::Response r = net::get("https://www.youtube.com/", boot, net::kTimeoutBootstrapMs, 6u << 20);
    logResponseHeaders("bootstrap/", r);
    if (r.ok() && !r.body.empty()) {
        std::string k;
        if (extractApiKey(r.body, k)) {
            apiKey_ = k;
            apiKeySource_ = "home";
        }
        const std::string v = extractVisitor(r.body);
        if (!v.empty() && visitorSource_.empty()) {
            visitorData_ = v;
            visitorSource_ = "home";
        }
    } else {
        ytWarn("yt: bootstrap GET / -> %s (продолжаем: ключ/visitor из sw.js_data)",
               httpError(r).c_str());
        error_ = httpError(r);
    }

    // Шаг 2: sw.js_data → публичный ключ + visitor (YT-01).
    if (apiKey_.empty() || visitorSource_.empty()) {
        net::Response r2 =
            net::get("https://www.youtube.com/sw.js_data", boot, net::kTimeoutBootstrapMs, 4u << 20);
        logResponseHeaders("sw.js_data", r2);
        if (r2.ok() && !r2.body.empty()) {
            if (apiKey_.empty()) {
                std::string k;
                if (extractApiKey(r2.body, k)) {
                    apiKey_ = k;
                    apiKeySource_ = "sw.js_data";
                }
            }
            if (visitorSource_.empty()) {
                const std::string v = extractVisitor(r2.body);
                if (!v.empty()) {
                    visitorData_ = v;
                    visitorSource_ = "sw.js_data";
                }
            }
        } else if (!r2.error.empty()) {
            ytLog("yt: sw.js_data -> %s", httpError(r2).c_str());
        }
    }

    // Шаг 3: жёсткие fallback-и — публичный ключ и локально сгенерированный visitor.
    if (apiKey_.empty() || apiKey_.size() <= 10) {
        apiKey_ = kPublicApiKey;
        apiKeySource_ = "public";
    }
    if (visitorData_.empty()) {
        visitorData_ = genVisitorData();
        visitorSource_ = "generated";
    }

    if (apiKeySource_ != "home" && visitorSource_ == "generated" && error_.empty())
        error_ = "нет соединения с YouTube";

    ytLog("yt: apiKey source=%s, visitor source=%s id=%s", apiKeySource_.c_str(),
          visitorSource_.c_str(), visitorFingerprint(visitorData_).c_str());

    // Всё, что не пришло из конфига, считается свежим — UI может сохранить.
    if (visitorSource_ != "config") visitorSavedAtEpoch_ = nowEpoch();
    return true; // офлайн-режим: UI должен показать ошибку, но клиент создан
}

void InnerTube::setVisitorData(const std::string& v, int64_t savedAtEpoch) {
    visitorData_ = v;
    visitorSavedAtEpoch_ = savedAtEpoch;
    visitorFromConfig_ = !v.empty();
    visitorSource_ = v.empty() ? std::string() : std::string("config");
}

std::string InnerTube::visitorLogId() const { return visitorFingerprint(visitorData_); }

VisitorState InnerTube::visitorState() const {
    VisitorState s;
    s.data = visitorData_;
    s.savedAtEpoch = visitorSavedAtEpoch_;
    return s;
}

void InnerTube::rotateVisitor() {
    visitorData_ = genVisitorData();
    visitorSource_ = "rotated";
    visitorFromConfig_ = false;
    visitorSavedAtEpoch_ = nowEpoch();
    ytLog("yt: visitor ротирован (id=%s)", visitorFingerprint(visitorData_).c_str());
}

void InnerTube::setClientVersion(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= clientCount()) idx = clientCount() - 1;
    clientVer_ = idx;
    browseClient_ = idx;
}

bool InnerTube::ensureSession() {
    if (apiKey_.empty() || visitorData_.empty()) init(region_, hl_);
    if (apiKey_.empty()) apiKey_ = kPublicApiKey;
    if (visitorData_.empty()) visitorData_ = genVisitorData();
    return true;
}

// YT-02 + YT-03: единая точка запроса InnerTube.
net::Response InnerTube::apiPost(const std::string& endpoint,
                                 const std::function<std::string(int)>& bodyFor, int clientIdx,
                                 bool allowFallback) {
    int ci = clientIdx;
    bool rotated = false;
    net::Response r;
    for (int guard = 0; guard < 10; ++guard) {
        const std::string body = bodyFor(ci);
        // NET-11: отмена применяется к лентам/поиску, но не к /player
        // (плеер открывается по уже выбранному видео и не должен зависеть
        // от токена, выставленного экраном поиска).
        net::CancelToken* cancel = (endpoint == "player") ? nullptr : cancelToken();
        r = net::post(apiUrl(endpoint, apiKey_), body, apiHeaders(visitorData_, hl_, ci),
                      net::kTimeoutBrowseMs, net::limits::kApi, 3, nullptr, cancel);
        if (r.ok()) return r;

        // NET-11: отмену не превращаем в перебор клиентов/повторов.
        if (cancel && cancel->cancelled()) {
            ytWarn("yt: %s отменён (NET-11)", endpoint.c_str());
            break;
        }

        // YT-02: 400 на InnerTube часто означает протухший/чужой visitorData.
        if (r.status == 400 && !rotated) {
            ytWarn("yt: %s -> HTTP 400, ротация visitor (%s)", endpoint.c_str(),
                   visitorFingerprint(visitorData_).c_str());
            rotateVisitor();
            rotated = true;
            continue;
        }

        // YT-03: клиент отвалился — переходим к следующему в матрице.
        if (allowFallback) {
            const int next = (ci + 1 < clientCount()) ? ci + 1 : -1;
            if (next >= 0) {
                const std::string why = r.error.empty() ? ("HTTP " + std::to_string(r.status))
                                                        : r.error;
                ytWarn("yt: %s client=%s -> %s, fallback -> %s", endpoint.c_str(),
                       clientInfo(ci).name, why.c_str(), clientInfo(next).name);
                ci = next;
                browseClient_ = next;
                continue;
            }
        }
        break;
    }
    return r;
}

// --- InnerTube: ленты ---

Feed InnerTube::parseFeed(const std::string& json, bool isSearch) {
    Feed f = parseFeedImpl(json, isSearch);
    if (!f.ok && !f.error.empty()) error_ = f.error;
    return f;
}

Feed InnerTube::home() {
    ensureSession();
    pager_.reset();
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"browseId\":\"FEwhat_to_watch\"}";
    };
    net::Response r = apiPost("browse", bodyFor, browseClient_, true);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (f.ok) {
        (void)pager_.dedup(f);
        error_.clear();
    }
    return f;
}

Feed InnerTube::trending() {
    ensureSession();
    pager_.reset();
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"browseId\":\"FEtrending\"}";
    };
    net::Response r = apiPost("browse", bodyFor, browseClient_, true);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (f.ok) {
        (void)pager_.dedup(f);
        error_.clear();
    }
    return f;
}

Feed InnerTube::search(const std::string& q, const std::string& continuation) {
    ensureSession();
    const bool cont = !continuation.empty();
    if (!cont) {
        pager_.reset();
    } else if (!pager_.beginPage()) {
        Feed f;
        f.error = pager_.limitReached() ? "конец ленты (10 страниц)" : "подгрузка уже идёт";
        if (pager_.limitReached()) f.continuation.clear();
        error_ = f.error;
        return f;
    }

    auto bodyFor = [&](int c) {
        std::string b = std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c);
        if (cont) b += ",\"continuation\":" + jsonQuote(continuation);
        else b += ",\"query\":" + jsonQuote(q);
        b += "}";
        return b;
    };
    net::Response r = apiPost("search", bodyFor, browseClient_, true);
    if (!r.ok()) {
        if (cont) pager_.abortPage();
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, true);
    if (f.ok) {
        const size_t added = pager_.dedup(f);
        if (cont) {
            pager_.endPage();
            if (added == 0) f.continuation.clear();   // явный конец (YT-06)
        }
        error_.clear();
    } else if (cont) {
        pager_.abortPage();
    }
    return f;
}

Feed InnerTube::related(const std::string& videoId) {
    ensureSession();
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"videoId\":" + jsonQuote(videoId) + "}";
    };
    net::Response r = apiPost("next", bodyFor, browseClient_, true);
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
    pager_.reset();
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"browseId\":" + jsonQuote(channelId) + "}";
    };
    net::Response r = apiPost("browse", bodyFor, browseClient_, true);
    if (!r.ok()) {
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (f.ok) {
        (void)pager_.dedup(f);
        error_.clear();
    }
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
    // YT-06: не запускаем параллельные continuation и не превышаем лимит страниц.
    if (!pager_.beginPage()) {
        Feed f;
        f.error = pager_.limitReached() ? "конец ленты (10 страниц)" : "подгрузка уже идёт";
        if (pager_.limitReached()) f.continuation.clear();
        error_ = f.error;
        return f;
    }

    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"continuation\":" + jsonQuote(continuation) + "}";
    };
    net::Response r = apiPost("browse", bodyFor, browseClient_, true);
    if (!r.ok()) {
        pager_.abortPage();
        Feed f;
        f.error = httpError(r);
        error_ = f.error;
        return f;
    }
    Feed f = parseFeed(r.body, false);
    if (f.ok) {
        const size_t added = pager_.dedup(f);
        pager_.endPage();
        // Нет новых видео или токен не сменился — продолжения больше нет.
        if (added == 0 || f.continuation == continuation) f.continuation.clear();
        error_.clear();
    } else {
        pager_.abortPage();
    }
    return f;
}

std::vector<std::string> InnerTube::suggestions(const std::string& q,
                                                net::CancelToken* cancel) {
    std::vector<std::string> out;
    if (q.empty()) return out;
    if (cancel && cancel->cancelled()) return out;

    // YT-10: 3 с — подсказки не блокируют ввод. Запрос кодируется urlEncode
    // (ё/&/пробел уходят как %D1%91/%26/%20), q в лог не пишется целиком:
    // это пользовательский ввод, ограничиваем длину и экранируем.
    constexpr int kSuggestTimeoutMs = 3000;
    const std::string url = "https://suggestqueries-clients6.youtube.com/complete/search"
                            "?client=youtube&ds=yt&hl=" +
                            hl_ + "&q=" + net::urlEncode(q);
    const net::Headers headers = {
        {"Accept-Language", hl_},
        {"User-Agent", clientInfo(clientVer_).ua},
    };
    const auto t0 = std::chrono::steady_clock::now();
    net::Response r = net::get(url, headers, kSuggestTimeoutMs, 1u << 20, 1, nullptr, cancel);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    if (!r.ok() || r.body.empty()) {
        // Ошибку/отмену видно в логе, но не чаще 40 раз за запуск: каждое
        // нажатие клавиши не должно превращаться в строку на SD.
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 40) {
            const std::string qlog = net::escapeForLog(q, 48);
            platform::bootLog("suggest: '%s' -> статус %ld, %lld мс%s%s", qlog.c_str(), r.status, ms,
                              r.error.empty() ? "" : ", ошибка: ",
                              r.error.empty() ? "" : r.error.c_str());
        }
        return out;
    }

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

// /player одним клиентом. Полный перебор клиентов — в fetchVideo().
std::string InnerTube::player(const std::string& videoId) {
    ensureSession();
    const int cl = 1;
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"videoId\":" + jsonQuote(videoId) +
               ",\"contentCheckOk\":true,\"racyCheckOk\":true"
               ",\"playbackContext\":{\"contentPlaybackContext\":"
               "{\"html5Preference\":\"HTML5_PREF_WANTS\"}}}";
    };
    net::Response r = apiPost("player", bodyFor, cl, false);
    if (!r.ok()) {
        if (isExpiredStatus(r.status)) {
            ytWarn("yt: /player %s -> HTTP %ld (ссылка могла истечь)", videoId.c_str(), r.status);
        }
        error_ = httpError(r);
        return {};
    }
    return r.body;
}

// YT-03 для /player: ANDROID первым (на этой сети отдаёт прямые ссылки), затем
// IOS → TVHTML5 → ANDROID_VR. Клиент-специфичные отказы (LOGIN_REQUIRED и т.п.)
// повод попробовать следующего; терминальные (возраст, регион) — нет.
bool InnerTube::fetchVideo(const std::string& id, VideoDetails& out, bool withRelated) {
    static const int kPlayerChain[] = {1, 2, 3, 0};
    std::string lastErr;
    VideoDetails lastTmp;

    for (int ci : kPlayerChain) {
        auto bodyFor = [&](int c) {
            return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
                   ",\"videoId\":" + jsonQuote(id) +
                   ",\"contentCheckOk\":true,\"racyCheckOk\":true"
                   ",\"playbackContext\":{\"contentPlaybackContext\":"
                   "{\"html5Preference\":\"HTML5_PREF_WANTS\"}}}";
        };
        net::Response r = apiPost("player", bodyFor, ci, false);
        if (!r.ok()) {
            lastErr = httpError(r);
            if (isExpiredStatus(r.status)) {
                ytWarn("yt: /player %s -> HTTP %ld (ссылка могла истечь)", id.c_str(), r.status);
            }
            continue;
        }

        VideoDetails tmp;
        std::string perr;
        if (parsePlayerJson(r.body, id, tmp, perr)) {
            out = tmp;
            if (withRelated) {
                Feed rel = related(id);
                if (rel.ok) {
                    for (const Section& sec : rel.sections) {
                        for (const VideoItem& item : sec.items) out.related.push_back(item);
                    }
                }
            }
            error_.clear();
            return true;
        }

        lastTmp = tmp;
        lastErr = perr;
        ytWarn("yt: /player client=%s не отдал %s: %s (status=%s)", clientInfo(ci).name, id.c_str(),
               perr.c_str(), tmp.playabilityStatus.c_str());
        const std::string& st = tmp.playabilityStatus;
        const bool retryOther = (st == "LOGIN_REQUIRED" || st == "ERROR" || st == "UNPLAYABLE");
        if (!retryOther) break;
    }

    out = lastTmp;
    if (out.playabilityReason.empty()) out.playabilityReason = lastErr;
    error_ = lastErr;
    return false;
}

bool InnerTube::video(const std::string& id, VideoDetails& out) {
    out = VideoDetails{};
    if (id.empty()) {
        error_ = "пустой идентификатор видео";
        return false;
    }
    ensureSession();
    return fetchVideo(id, out, true);
}

// YT-08: 1 повтор /player и переселект.
bool InnerTube::refreshStreams(const std::string& videoId, VideoDetails& out) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        VideoDetails fresh;
        if (fetchVideo(videoId, fresh, false)) {
            out = fresh;
            return true;
        }
        ytWarn("yt: refreshStreams %s — попытка %d/2 не удалась: %s", videoId.c_str(), attempt + 1,
               error_.c_str());
    }
    return false;
}

// YT-03: health-check по browse с fallback.
bool InnerTube::healthCheck() {
    ensureSession();
    auto bodyFor = [&](int c) {
        return std::string("{\"context\":") + ctxJson(region_, hl_, visitorData_, c) +
               ",\"browseId\":\"FEwhat_to_watch\"}";
    };
    net::Response r = apiPost("browse", bodyFor, browseClient_, true);
    if (!r.ok()) {
        error_ = httpError(r);
        return false;
    }
    error_.clear();
    return true;
}

// --- YT-07: детерминированный выбор потоков ---

StreamSelection InnerTube::selectStreams(const VideoDetails& v, int preferHeight) {
    StreamSelection sel;
    if (preferHeight <= 0) preferHeight = 720;

    auto mp4 = [](const StreamFormat& f) { return f.mime.find("mp4") != std::string::npos; };
    auto avc = [](const StreamFormat& f) {
        return f.codecs.find("avc1") != std::string::npos ||
               f.mime.find("avc1") != std::string::npos;
    };
    auto aac = [](const StreamFormat& f) {
        return f.codecs.find("mp4a") != std::string::npos ||
               f.mime.find("mp4a") != std::string::npos ||
               f.mime.find("audio/mp4") != std::string::npos;
    };
    auto base = [](const StreamFormat& f) { return !f.url.empty() && !f.live && !f.drm; };
    auto isVideoOnly = [&](const StreamFormat& f) {
        return base(f) && f.hasVideo && !f.hasAudio;
    };
    auto isMuxed = [&](const StreamFormat& f) {
        return base(f) && f.hasVideo && f.hasAudio && mp4(f) && avc(f);
    };
    auto isAudio = [&](const StreamFormat& f) { return base(f) && f.hasAudio && !f.hasVideo; };

    auto pickAudio = [&]() -> const StreamFormat* {
        const StreamFormat* best = nullptr;
        long long bestScore = std::numeric_limits<long long>::min();
        for (const StreamFormat& f : v.formats) {
            if (!isAudio(f)) continue;
            long long score = 0;
            if (aac(f)) score += 1LL << 40;        // AAC лучше Opus для mpv/аудио
            if (f.itag == 140) score += 1LL << 38; // стандартный itag 140
            score += f.bitrate;
            score -= f.itag;                       // детерминизм: меньший itag
            if (score > bestScore) {
                bestScore = score;
                best = &f;
            }
        }
        return best;
    };
    auto pickVideoNear = [&](int target, int minHeight) -> const StreamFormat* {
        const StreamFormat* best = nullptr;
        long long bestScore = std::numeric_limits<long long>::min();
        for (const StreamFormat& f : v.formats) {
            if (!isVideoOnly(f)) continue;
            if (minHeight > 0 && f.height > 0 && f.height < minHeight) continue;
            long long score = 0;
            if (avc(f)) score += 1LL << 32;   // H.264 — software-декодер
            if (mp4(f)) score += 1LL << 30;
            const int h = f.height > 0 ? f.height : target;
            const int diff = h > target ? h - target : target - h;
            score -= (long long)diff * 10000;
            score -= f.itag;                  // детерминизм: меньший itag
            if (score > bestScore) {
                bestScore = score;
                best = &f;
            }
        }
        return best;
    };
    auto findMuxed = [&](int itag) -> const StreamFormat* {
        for (const StreamFormat& f : v.formats) {
            if (isMuxed(f) && f.itag == itag) return &f;
        }
        return nullptr;
    };
    auto pickBestMuxed = [&]() -> const StreamFormat* {
        const StreamFormat* best = nullptr;
        long long bestScore = std::numeric_limits<long long>::min();
        for (const StreamFormat& f : v.formats) {
            if (!isMuxed(f)) continue;
            long long score = 0;
            if (f.itag == 18) score += 3000;
            else if (f.itag == 22) score += 2000;
            else if (f.itag == 17) score += 1000;
            score -= f.height;    // меньшее разрешение легче для software-декодера
            score -= f.itag;
            if (score > bestScore) {
                bestScore = score;
                best = &f;
            }
        }
        return best;
    };

    const StreamFormat* a = pickAudio();
    // Верхняя ступень — адаптивное видео у желаемой высоты (для 720 это >=600,
    // чтобы 480p/360p не вытесняли muxed 18 согласно таблице YT-07).
    int topMin = preferHeight - 120;
    if (topMin < 240) topMin = 240;
    const StreamFormat* vTop = pickVideoNear(preferHeight, topMin);
    const StreamFormat* vAny = pickVideoNear(480, 0);
    const StreamFormat* m18 = findMuxed(18);
    const StreamFormat* mBest = pickBestMuxed();

    // Таблица приоритетов (YT-07): 136+140 → 18 → 22 → 135+140 → …
    if (vTop && a) {
        sel.mode = StreamPick::Split;
        sel.video = *vTop;
        sel.audio = *a;
        sel.reason = "split itag=" + std::to_string(vTop->itag) + "+" + std::to_string(a->itag) +
                     " (" + vTop->qualityLabel + " AVC+AAC)";
    } else if (m18) {
        sel.mode = StreamPick::Muxed;
        sel.muxed = *m18;
        sel.reason = "muxed itag=18 (" + m18->qualityLabel + ")";
    } else if (mBest) {
        sel.mode = StreamPick::Muxed;
        sel.muxed = *mBest;
        sel.reason = "muxed itag=" + std::to_string(mBest->itag) + " (" + mBest->qualityLabel + ")";
        if (a) sel.audio = *a;   // таблица «22+140»
    } else if (vAny && a) {
        sel.mode = StreamPick::Split;
        sel.video = *vAny;
        sel.audio = *a;
        sel.reason = "split-low itag=" + std::to_string(vAny->itag) + "+" + std::to_string(a->itag);
    } else if (vAny) {
        sel.mode = StreamPick::Split;
        sel.video = *vAny;
        sel.reason = "video-only itag=" + std::to_string(vAny->itag) + " (без звука)";
    }

    // Лог кандидатов и выбора (YT-07).
    std::string cands;
    for (const StreamFormat& f : v.formats) {
        if (!base(f)) continue;
        if (!cands.empty()) cands += ",";
        cands += std::to_string(f.itag);
    }
    std::string chosen = "none";
    if (sel.mode == StreamPick::Split) {
        chosen = "split:" + std::to_string(sel.video.itag);
        if (!sel.audio.url.empty()) chosen += "+" + std::to_string(sel.audio.itag);
    } else if (sel.mode == StreamPick::Muxed) {
        chosen = "muxed:" + std::to_string(sel.muxed.itag);
    }
    ytLog("streams: candidates=[%s] chosen=%s (%s)", cands.c_str(), chosen.c_str(),
          sel.reason.c_str());
    return sel;
}

bool InnerTube::pickStreams(const VideoDetails& v, StreamFormat* muxed, StreamFormat* video,
                            StreamFormat* audio, int preferHeight) {
    const StreamSelection sel = selectStreams(v, preferHeight);
    if (muxed) *muxed = (sel.mode == StreamPick::Muxed) ? sel.muxed : StreamFormat{};
    if (video) *video = (sel.mode == StreamPick::Split) ? sel.video : StreamFormat{};
    if (audio) *audio = sel.audio;   // может быть пустым для video-only/muxed 18
    return sel.mode != StreamPick::None &&
           (!sel.video.url.empty() || !sel.muxed.url.empty());
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

// YT-09: URL обложки по id и качеству.
std::string thumbUrl(const std::string& videoId, int quality) {
    static const char* names[] = {"default.jpg", "mqdefault.jpg", "hqdefault.jpg", "sddefault.jpg",
                                  "maxresdefault.jpg"};
    if (quality < 0) quality = 0;
    if (quality > 4) quality = 4;
    return "https://i.ytimg.com/vi/" + videoId + "/" + names[quality];
}

std::vector<std::string> thumbCandidates(const std::string& videoId, int fromQuality) {
    std::vector<std::string> out;
    if (videoId.empty()) return out;
    if (fromQuality < 0) fromQuality = 0;
    if (fromQuality > 4) fromQuality = 4;
    for (int q = fromQuality; q >= 0; --q) out.push_back(thumbUrl(videoId, q));
    return out;
}

} // namespace tnx::yt

// --- Хостовые хуки (не влияют на сборку для Switch) ---
#ifdef TNX_HOST
namespace tnx::yt {
Feed host_parse_feed(const std::string& json, bool isSearch) {
    return parseFeedImpl(json, isSearch);
}
Feed host_parse_related(const std::string& json) { return parseRelatedImpl(json); }
bool host_parse_player(const std::string& json, const std::string& id, VideoDetails& out,
                       std::string& err) {
    out = VideoDetails{};
    return parsePlayerJson(json, id, out, err);
}
std::string host_playability_text(const std::string& status) {
    return InnerTube::playabilityText(status);
}
int host_client_count() { return InnerTube::clientCount(); }
std::string host_client_name(int idx) { return InnerTube::clientName(idx); }
int host_next_client(int idx) {
    return (idx + 1 < InnerTube::clientCount()) ? idx + 1 : -1;
}
std::string host_extract_key(const std::string& body) {
    std::string k;
    extractApiKey(body, k);
    return k;
}
std::string host_extract_visitor(const std::string& body) { return extractVisitor(body); }
StreamSelection host_select_streams(const VideoDetails& v, int preferHeight) {
    return InnerTube::selectStreams(v, preferHeight);
}
std::vector<std::string> host_thumb_candidates(const std::string& id, int q) {
    return thumbCandidates(id, q);
}
} // namespace tnx::yt
#endif
