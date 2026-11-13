// net/images.cpp — асинхронная загрузка обложек и декодирование через stb_image.
#include "core/threads.hpp"
#include "net/images.hpp"

#include "net/http.hpp"

#include <algorithm>
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
        size_t bytes = 0;
    };

    struct Result {
        std::string url;
        std::shared_ptr<Image> img;
        bool failed = false;
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

    void worker();
    void applyReady();
    void enforceLimit(int limitMb);

    static void removeFromOrder(std::deque<std::string>& order, const std::string& url) {
        order.erase(std::remove(order.begin(), order.end(), url), order.end());
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
        if (!url.empty()) {
            // Обложка — косметика: не занимаем единственную SSL-сессию
            // пула на 60 с (20 с x 3 попытки). Один быстрый заход, ошибку
            // кэшируем и рисуем заглушку.
            Response resp = net::get(url, {{"Accept", "image/*"}}, 8000, 8u << 20, 1);
            if (resp.ok() && !resp.body.empty()) {
                Image decoded = decodeImage((const unsigned char*)resp.body.data(), resp.body.size(),
                                            480);
                if (decoded.valid()) {
                    image = std::make_shared<Image>(std::move(decoded));
                    failed = false;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (stopping) return;
            ready.push_back(Result{url, std::move(image), failed});
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
        if (r.failed || !r.img) {
            Entry e;
            e.failed = true;
            e.bytes = 0;
            cache.emplace(r.url, std::move(e));
            order.push_back(r.url);
            continue;
        }
        Entry e;
        e.bytes = r.img->px.size() * sizeof(uint32_t);
        e.img = std::move(r.img);
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

    while (bytes > limit && !order.empty()) {
        const std::string victim = std::move(order.front());
        order.pop_front();
        auto it = cache.find(victim);
        if (it == cache.end()) continue;
        bytes -= it->second.bytes;
        if (it->second.img) graveyard.push_back(it->second.img);
        cache.erase(it);
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
    if (impl_->cache.find(url) != impl_->cache.end()) return;
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
    if (it->second.failed) return nullptr;
    return it->second.img ? it->second.img.get() : nullptr;
}

bool ImageCache::failed(const std::string& url) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->cache.find(url);
    return it != impl_->cache.end() && it->second.failed;
}

void ImageCache::pump() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->applyReady();
    impl_->enforceLimit(limitMb_);
}

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