// tools/host_stubs.cpp — заглушки платформенных зависимостей для offscreen-предпросмотра UI
// на обычном Linux-хосте (без Switch). Позволяет рендерить экраны в PNG и проверять дизайн.
#include "dpi/dpi.hpp"
#include "net/http.hpp"
#include "net/images.hpp"
#include "platform.hpp"
#include "player/player.hpp"
#include "yt/innertube.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tnx::net {

static std::string g_proxy;
static std::string g_ca;
static Verify g_verify = Verify::Full;

bool init() { return true; }
void shutdown() {}
void setProxy(const std::string& url) { g_proxy = url; }
std::string proxy() { return g_proxy; }
void setCaBundlePath(const std::string& p) { g_ca = p; }
void setUserAgent(const std::string&) {}
void setVerify(Verify v) { g_verify = v; }
Verify verify() { return g_verify; }

Response get(const std::string&, const Headers&, int, size_t, int, const Overrides*) {
    return Response{};
}
Response post(const std::string&, const std::string&, const Headers&, int, size_t, int,
              const Overrides*) {
    return Response{};
}
bool fetchToFile(const std::string&, const std::string&, const std::function<void(long long, long long)>&) {
    return false;
}
std::string urlEncode(const std::string& s) { return s; }
std::string urlDecode(const std::string& s) { return s; }

// --- синтетические обложки, чтобы оценить композицию ---
static std::vector<Image> g_thumbs;
static bool g_reserved = false;
static std::vector<std::string> g_urls;

static void makeThumb(Image& img, uint32_t c1, uint32_t c2) {
    img.w = 480;
    img.h = 270;
    img.px.assign((size_t)img.w * img.h, 0);
    Canvas cv;
    cv.attach(img.px.data(), img.w, img.h, img.w);
    Color a = Color::rgb(c1 & 0xFFFFFF), b = Color::rgb(c2 & 0xFFFFFF);
    cv.fillLinearGradient({0, 0, 480, 270}, CornerRadii(0), a, b, true);
    cv.fillCircle(380, 60, 90, Color::rgb(0xFFFFFF).withAlpha(0.10f));
    cv.fillCircle(90, 240, 120, Color::rgb(0x000000).withAlpha(0.12f));
    cv.fillRoundRect({190, 105, 100, 60}, 16, Color::rgb(0xFFFFFF).withAlpha(0.25f));
}

const Image* ImageCache::get(const std::string& url) {
    if (!g_reserved) { g_thumbs.reserve(256); g_reserved = true; }
    for (size_t i = 0; i < g_urls.size(); i++)
        if (g_urls[i] == url) return &g_thumbs[i];
    g_urls.push_back(url);
    g_thumbs.emplace_back();
    static const uint32_t pairs[][2] = {
        {0xB3261E, 0x7D5260}, {0x1E3A8A, 0x0F766E}, {0x9A3412, 0x7C2D12},
        {0x365314, 0x166534}, {0x4C1D95, 0x831843}, {0x155E75, 0x164E63},
    };
    int idx = (int)g_thumbs.size() - 1;
    makeThumb(g_thumbs.back(), pairs[idx % 6][0], pairs[idx % 6][1]);
    return &g_thumbs.back();
}
bool ImageCache::failed(const std::string&) { return false; }
void ImageCache::request(const std::string&) {}
void ImageCache::pump() {}
void ImageCache::clear() {}
void ImageCache::start(int) {}
void ImageCache::stop() {}
Image decodeImage(const unsigned char*, size_t, int) { return Image{}; }

} // namespace tnx::net

namespace tnx::dpi {

static Config g_cfg;
static Stats g_stats;
Config& config() { return g_cfg; }
Stats& stats() { return g_stats; }
void init() {}
bool startSocks(int) { return true; }
void stopSocks() {}
int socksPort() { return 1080; }
bool startRelay(int) { return true; }
void stopRelay() {}
int relayPort() { return 8899; }
std::string relayUrl(const std::string& u) { return "http://127.0.0.1:8899/u?u=" + u; }
ProbeResult probe(const char*, int, bool) { return ProbeResult{true, 96, 200, ""}; }
const char* modeName(Mode) { return "Split"; }
void setMode(Mode m) { g_cfg.mode = m; }

} // namespace tnx::dpi

namespace tnx::player {

struct Player::Impl {
    int dummy = 0;
};

static Player g_player;
Player& instance() { return g_player; }

bool Player::init() { return true; }
void Player::shutdown() {}
bool Player::open(const std::string&, const std::string&) { return true; }
bool Player::addAudio(const std::string&) { return true; }
void Player::close() {}
bool Player::loaded() const { return true; }
void Player::setPaused(bool) {}
void Player::togglePause() {}
bool Player::paused() const { return false; }
void Player::seek(double) {}
void Player::seekRelative(double) {}
void Player::setVolume(int v) { (void)v; }
int Player::volume() const { return 85; }
void Player::setSpeed(double) {}
void Player::setAudioDelay(double) {}
bool Player::playing() const { return true; }
bool Player::buffering() const { return false; }
double Player::position() const { return 214.0; }
double Player::duration() const { return 754.0; }
bool Player::hasVideo() const { return true; }
std::string Player::error() const { return ""; }
std::string Player::title() const { return "Preview"; }
bool Player::update(Image& out) {
    if (out.valid()) return false;
    out.w = 960;
    out.h = 540;
    out.px.assign((size_t)out.w * out.h, 0);
    Canvas cv;
    cv.attach(out.px.data(), out.w, out.h, out.w);
    cv.fillLinearGradient({0, 0, 960, 540}, CornerRadii(0), Color::rgb(0x111827), Color::rgb(0x312E81), false);
    cv.fillCircle(700, 170, 200, Color::rgb(0xFB7185).withAlpha(0.20f));
    cv.fillCircle(240, 420, 240, Color::rgb(0x22D3EE).withAlpha(0.16f));
    return true;
}
void Player::setTargetSize(int, int) {}
int Player::videoWidth() const { return 960; }
int Player::videoHeight() const { return 540; }
Player::Status Player::status() const { return Status{}; }

} // namespace tnx::player

namespace tnx::yt {

static std::vector<VideoItem> demoItems() {
    const char* titles[] = {
        "Lo-Fi Hip Hop Radio — beats to relax/study to",
        "Как работает Nintendo Switch: разбор архитектуры",
        "Material 3 Expressive: новый дизайн Google",
        "Обход DPI в 2026: что реально работает",
        "Тест 8 игр на Switch 2 в портативном режиме",
        "Хип-хоп микстейп 2026 — лучшие треки недели",
        "Собираем homebrew NRO с devkitPro за 10 минут",
        "Путешествие по Японии — 4K кинематографично",
        "Все секреты Material You на Android",
        "Стрим: пишем YouTube-клиент для Switch",
        "Обзор OLED-телевизора для консолей",
        "Готовим рамэн дома — рецепт из Осаки",
    };
    const char* channels[] = {"Lofi Girl", "Prog Tech", "Material Design", "Network Notes", "Digital Foundry",
                              "Music Box", "switchbrew", "Nomad Vision", "M3 Weekly", "Dev Stream",
                              "TV Lab", "Cook Easy"};
    const char* metas[] = {"1,2 млн просмотров • 3 дня назад", "84 тыс. просмотров • 2 недели назад",
                           "412 тыс. просмотров • 1 месяц назад", "2,7 млн просмотров • 5 дней назад",
                           "1,9 млн просмотров • 2 дня назад",   "330 тыс. просмотров • 1 неделя назад",
                           "12 тыс. просмотров • 4 часа назад",   "905 тыс. просмотров • 3 недели назад",
                           "76 тыс. просмотров • 6 дней назад",   "4,1 тыс. просмотров • 1 день назад",
                           "220 тыс. просмотров • 2 месяца назад", "58 тыс. просмотров • 9 дней назад"};
    std::vector<VideoItem> v;
    for (int i = 0; i < 12; i++) {
        VideoItem it;
        it.id = "demo" + std::to_string(i);
        it.title = titles[i];
        it.channel = channels[i];
        it.meta = metas[i];
        it.duration = i == 0 ? "LIVE" : (i % 3 == 0 ? "1:02:13" : (i % 3 == 1 ? "12:45" : "8:21"));
        it.live = (i == 0);
        it.thumbnailUrl = "thumb://" + std::to_string(i);
        v.push_back(it);
    }
    return v;
}

bool InnerTube::init(const std::string&, const std::string&) { return true; }
Feed InnerTube::home() {
    Feed f;
    f.ok = true;
    Section s;
    s.title = "Главная";
    s.items = demoItems();
    f.sections.push_back(s);
    return f;
}
Feed InnerTube::trending() { return home(); }
Feed InnerTube::search(const std::string&, const std::string&) { return home(); }
Feed InnerTube::related(const std::string&) { return home(); }
Feed InnerTube::channel(const std::string&) { return home(); }
Feed InnerTube::continueFeed(const std::string&) { return Feed{}; }
std::vector<std::string> InnerTube::suggestions(const std::string&) {
    return {"lofi hip hop radio", "material you дизайн", "nintendo switch homebrew", "обход dpi youtube"};
}
bool InnerTube::video(const std::string&, VideoDetails& out) {
    out.title = "Lo-Fi Hip Hop Radio — beats to relax/study to";
    out.channel = "Lofi Girl";
    return true;
}
bool InnerTube::pickStreams(const VideoDetails&, StreamFormat*, StreamFormat*, StreamFormat*, int) {
    return false;
}
std::string formatViews(int64_t) { return ""; }
std::string formatDuration(int) { return ""; }
std::string thumbUrl(const std::string& id, int) { return "thumb://" + id; }

} // namespace tnx::yt

namespace tnx::platform {

void bootLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

bool keyboard(std::string&, const char*, const char*, bool) { return false; }
void requestExit() {}
Info info() {
    Info i;
    i.firmware = "19.0.1";
    i.docked = true;
    i.wifi = true;
    i.battery = 87;
    return i;
}

} // namespace tnx::platform