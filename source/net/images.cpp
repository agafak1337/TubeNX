// net/images.cpp — асинхронная загрузка обложек и декодирование через stb_image.
#include "core/threads.hpp"
#include "net/images.hpp"

#include "net/http.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "third_party/stb_image.h"

namespace tnx::net {

// --- Fallback-цепочка качеств обложки (NET-02) ------------------------------

const char* const kThumbQualities[5] = {"maxresdefault", "sddefault", "hqdefault",
                                        "mqdefault", "default"};

namespace {

// Позиция качества в URL обложки: слева '/', справа '.'. Порядок перебора —
// от самого длинного токена к короткому, иначе "default" поймается внутри
// "hqdefault".
struct ThumbLoc {
    int index = -1;
    size_t pos = std::string::npos;
};

ThumbLoc locateThumbQuality(const std::string& url) {
    for (int i = 0; i < 5; ++i) {
        const std::string tok = kThumbQualities[i];
        size_t pos = url.find(tok);
        while (pos != std::string::npos) {
            const bool leftOk = pos > 0 && url[pos - 1] == '/';
            const size_t after = pos + tok.size();
            const bool rightOk = after < url.size() && url[after] == '.';
            if (leftOk && rightOk) return ThumbLoc{i, pos};
            pos = url.find(tok, pos + 1);
        }
    }
    return ThumbLoc{};
}

constexpr int kFailTtlMs = 10 * 60 * 1000; // отрицательный результат — 10 минут

} // namespace

std::string thumbnailAtQuality(const std::string& url, const char* quality) {
    if (!quality) return std::string();
    const ThumbLoc loc = locateThumbQuality(url);
    if (loc.index < 0) return std::string();
    const std::string& tok = kThumbQualities[loc.index];
    return url.substr(0, loc.pos) + quality + url.substr(loc.pos + tok.size());
}

std::vector<std::string> thumbnailCandidates(const std::string& url) {
    std::vector<std::string> out;
    const ThumbLoc loc = locateThumbQuality(url);
    if (loc.index < 0) {
        out.push_back(url); // не ytimg — не трогаем
        return out;
    }
    for (int i = loc.index; i < 5; ++i) {
        out.push_back(thumbnailAtQuality(url, kThumbQualities[i]));
    }
    return out;
}

// --- Декодирование ---

Image decodeImage(const unsigned char* data, size_t len, int maxWidth) {
    Image out;
    if (!data || len == 0) return out;

    int w = 0, h = 0, comp = 0;
    unsigned char* raw = stbi_load_from_memory(data, (int)len, &w, &h, &comp, STBI_rgb_alpha);
    if (!raw || w <= 0 || h <= 0) {
        if (raw) stbi_image_free(raw);
        return out;
    }

    if (maxWidth > 0 && w > maxWidth) {
        const int dw = maxWidth;
        int dh = (int)((long long)h * dw / w);
        if (dh < 1) dh = 1;

        out.w = dw;
        out.h = dh;
        out.px.resize((size_t)dw * (size_t)dh);

        // Box-фильтр: каждый пиксель результата — среднее по своему блоку.
        for (int y = 0; y < dh; ++y) {
            int sy0 = (int)((long long)y * h / dh);
            int sy1 = (int)((long long)(y + 1) * h / dh);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > h) sy1 = h;

            for (int x = 0; x < dw; ++x) {
                int sx0 = (int)((long long)x * w / dw);
                int sx1 = (int)((long long)(x + 1) * w / dw);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > w) sx1 = w;

                uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
                uint32_t count = 0;
                for (int sy = sy0; sy < sy1; ++sy) {
                    const unsigned char* row = raw + (size_t)sy * (size_t)w * 4;
                    for (int sx = sx0; sx < sx1; ++sx) {
                        const unsigned char* p = row + (size_t)sx * 4;
                        sr += p[0];
                        sg += p[1];
                        sb += p[2];
                        sa += p[3];
                        ++count;
                    }
                }
                if (count == 0) count = 1;
                const uint32_t r = (uint32_t)((sr + count / 2) / count);
                const uint32_t g = (uint32_t)((sg + count / 2) / count);
                const uint32_t b = (uint32_t)((sb + count / 2) / count);
                const uint32_t a = (uint32_t)((sa + count / 2) / count);
                // 0xAABBGGRR: в памяти little-endian это R,G,B,A.
                out.px[(size_t)y * (size_t)dw + (size_t)x] = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
    } else {
        out.w = w;
        out.h = h;
        out.px.resize((size_t)w * (size_t)h);
        for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i) {
            const unsigned char* p = raw + i * 4;
            out.px[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                        ((uint32_t)p[3] << 24);
        }
    }

    stbi_image_free(raw);
    return out;
}

// --- Кэш ---

struct ImageCache::Impl {
    struct Entry {
        std::shared_ptr<Image> img;
        bool failed = false;
        bool placeholder = false; // все размеры 404 — не ошибка сети
        size_t bytes = 0;
        uint64_t lastUsed = 0;   // PERF-03: LRU-метка
        std::chrono::steady_clock::time_point failAt{}; // когда зафиксирован провал
    };

    struct Result {
        std::string url;
        std::shared_ptr<Image> img;
        bool failed = false;
        bool placeholder = false;
    };

    std::mutex mtx;
    std::condition_variable cv;

    std::deque<std::string> queue;                 // ожидающие загрузки
    std::unordered_set<std::string> pending;       // запрошено, но результат ещё не забран
    std::unordered_map<std::string, Entry> cache;  // url → готовое изображение/фейл
    std::deque<std::string> order;                 // порядок вставки для вытеснения
    std::vector<Result> ready;                     // результаты от рабочих потоков
    std::vector<std::shared_ptr<Image>> graveyard; // вытесненные, но ещё может быть в кадре

    std::vector<threads::Thread> threads;
    bool stopping = false;

    size_t bytes = 0;
    uint64_t useTick = 0;       // PERF-03: монотонная метка использования
    uint64_t evicted = 0;       // PERF-03: сколько записей вытеснено

    void worker();
    void applyReady();
    void enforceLimit(int limitMb);

    static void removeFromOrder(std::deque<std::string>& order, const std::string& url) {
        order.erase(std::remove(order.begin(), order.end(), url), order.end());
    }

    // Отрицательный результат живёт ~10 минут. Готовое изображение — вечно
    // (до вытеснения по памяти).
    static bool isExpired(const Entry& e) {
        if (e.img) return false;
        if (e.failAt == std::chrono::steady_clock::time_point{}) return false;
        return std::chrono::steady_clock::now() - e.failAt >
               std::chrono::milliseconds(kFailTtlMs);
    }
};

void ImageCache::Impl::worker() {
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping) return;
            url = std::move(queue.front());
            queue.pop_front();
        }

        std::shared_ptr<Image> image;
        bool failed = true;
        bool placeholder = false;
        if (!url.empty()) {
            // NET-02: обложка — косметика. Не занимаем SSL-сессию надолго
            // (8 с, 1 попытка) и перебираем качества от текущего к меньшим:
            // 404 на размере — это НЕ ошибка сети.
            failed = false;
            bool netError = false;
            for (const std::string& cand : thumbnailCandidates(url)) {
                Response resp = net::get(cand, {{"Accept", "image/*"}},
                                         net::timeouts().images, net::limits::kImages, 1);
                if (!resp.error.empty()) {
                    // Сеть отвалилась — остальные качества не помогут.
                    netError = true;
                    break;
                }
                if (resp.status >= 400 || resp.body.empty()) continue; // 404 → ниже
                Image decoded = decodeImage((const unsigned char*)resp.body.data(),
                                            resp.body.size(), 480);
                if (decoded.valid()) {
                    image = std::make_shared<Image>(std::move(decoded));
                    break;
                }
                // Битое тело — пробуем следующий размер.
            }
            if (!image) {
                if (netError) {
                    failed = true; // ошибка сети: рисуем заглушку-ошибку
                } else {
                    placeholder = true; // id валиден, размеров нет
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (stopping) return;
            ready.push_back(Result{url, std::move(image), failed, placeholder});
        }
    }
}

// Вызывается под захваченным mtx.
void ImageCache::Impl::applyReady() {
    // Освобождаем изображения, вытесненные в прошлом кадре: указатели, выданные
    // через get() в предыдущем кадре, к этому моменту уже не используются.
    graveyard.clear();

    for (Result& r : ready) {
        pending.erase(r.url);
        auto it = cache.find(r.url);
        if (it != cache.end()) {
            // Запись могла появиться раньше (повторный запрос) — обновляем.
            bytes -= it->second.bytes;
            removeFromOrder(order, r.url);
            cache.erase(it);
        }
        if (r.failed || (!r.img && !r.placeholder)) {
            Entry e;
            e.failed = true;
            e.bytes = 0;
            e.failAt = std::chrono::steady_clock::now();
            cache.emplace(r.url, std::move(e));
            order.push_back(r.url);
            continue;
        }
        if (r.placeholder || !r.img) {
            Entry e;
            e.placeholder = true;
            e.bytes = 0;
            e.failAt = std::chrono::steady_clock::now();
            cache.emplace(r.url, std::move(e));
            order.push_back(r.url);
            continue;
        }
        Entry e;
        e.bytes = r.img->px.size() * sizeof(uint32_t);
        e.img = std::move(r.img);
        e.lastUsed = ++useTick;
        bytes += e.bytes;
        cache.emplace(r.url, std::move(e));
        order.push_back(r.url);
    }
    ready.clear();
}

// Вызывается под захваченным mtx.
void ImageCache::Impl::enforceLimit(int limitMb) {
    if (limitMb <= 0) return; // 0 и меньше — без ограничения
    const size_t limit = (size_t)limitMb * 1024u * 1024u;

    // PERF-03: настоящий LRU — вытесняем запись с самой старой lastUsed.
    while (bytes > limit && !cache.empty()) {
        auto victim = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (victim == cache.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
        }
        if (victim == cache.end()) break;
        bytes -= victim->second.bytes;
        if (victim->second.img) graveyard.push_back(victim->second.img);
        removeFromOrder(order, victim->first);
        cache.erase(victim);
        ++evicted;
    }
}

void ImageCache::start(int workers) {
    if (!impl_) impl_ = new Impl();
    if (!impl_->threads.empty()) return; // уже запущены

    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stopping = false;
    }
    const int count = std::max(1, workers);
    impl_->threads.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        Impl* im = impl_;
        impl_->threads.emplace_back();
        impl_->threads.back().start([im] { im->worker(); });
    }
}

void ImageCache::stop() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stopping = true;
        impl_->queue.clear();
        impl_->pending.clear();
        impl_->ready.clear();
    }
    impl_->cv.notify_all();
    for (threads::Thread& t : impl_->threads) t.join();
    impl_->threads.clear();

    delete impl_;
    impl_ = nullptr;
}

void ImageCache::request(const std::string& url) {
    if (url.empty()) return;
    if (!impl_) impl_ = new Impl();

    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(url);
    if (it != impl_->cache.end()) {
        if (!Impl::isExpired(it->second)) return; // свежий результат — не грузим
        // Отрицательный результат истёк — удаляем и запрашиваем снова.
        impl_->bytes -= it->second.bytes;
        Impl::removeFromOrder(impl_->order, url);
        impl_->cache.erase(it);
    }
    if (impl_->pending.find(url) != impl_->pending.end()) return;

    impl_->pending.insert(url);
    impl_->queue.push_back(url);
    impl_->cv.notify_one();
}

const Image* ImageCache::get(const std::string& url) {
    if (!impl_) return nullptr;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(url);
    if (it == impl_->cache.end()) return nullptr;
    if (Impl::isExpired(it->second)) return nullptr;
    if (it->second.failed || it->second.placeholder) return nullptr;
    if (it->second.img) it->second.lastUsed = ++impl_->useTick; // PERF-03: LRU
    return it->second.img ? it->second.img.get() : nullptr;
}

ThumbState ImageCache::state(const std::string& url) {
    if (!impl_) return ThumbState::Missing;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->pending.find(url) != impl_->pending.end()) return ThumbState::Loading;
    auto it = impl_->cache.find(url);
    if (it == impl_->cache.end()) return ThumbState::Missing;
    if (Impl::isExpired(it->second)) return ThumbState::Missing;
    if (it->second.img) return ThumbState::Ready;
    if (it->second.placeholder) return ThumbState::Placeholder;
    return ThumbState::Failed;
}

bool ImageCache::placeholder(const std::string& url) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(url);
    return it != impl_->cache.end() && !Impl::isExpired(it->second) && it->second.placeholder;
}

bool ImageCache::failed(const std::string& url) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(url);
    return it != impl_->cache.end() && !Impl::isExpired(it->second) && it->second.failed;
}

void ImageCache::pump() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->applyReady();
    impl_->enforceLimit(limitMb_);
}

ImageCache::Stats ImageCache::stats() {
    Stats out;
    if (!impl_) {
        out.limitMb = limitMb_;
        return out;
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    out.bytes = impl_->bytes;
    out.entries = static_cast<int>(impl_->cache.size());
    out.pending = static_cast<int>(impl_->pending.size());
    out.evicted = impl_->evicted;
    out.limitMb = limitMb_;
    return out;
}

#ifdef TNX_HOST
void ImageCache::testPut(const std::string& url, int w, int h) {
    if (url.empty() || w <= 0 || h <= 0) return;
    if (!impl_) impl_ = new Impl();
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Entry e;
    e.img = std::make_shared<Image>();
    e.img->w = w;
    e.img->h = h;
    e.img->px.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0xFF000000u);
    e.bytes = e.img->px.size() * sizeof(uint32_t);
    e.lastUsed = ++impl_->useTick;
    auto old = impl_->cache.find(url);
    if (old != impl_->cache.end()) {
        impl_->bytes -= old->second.bytes;
        impl_->cache.erase(old);
        Impl::removeFromOrder(impl_->order, url);
    }
    impl_->bytes += e.bytes;
    impl_->cache.emplace(url, std::move(e));
    impl_->order.push_back(url);
    impl_->enforceLimit(limitMb_);
}
#endif

void ImageCache::clear() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->queue.clear();
    impl_->pending.clear();
    impl_->ready.clear();
    impl_->cache.clear();
    impl_->order.clear();
    impl_->graveyard.clear();
    impl_->bytes = 0;
}

} // namespace tnx::net