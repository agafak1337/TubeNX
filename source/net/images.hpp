// net/images.hpp — асинхронная загрузка и декодирование обложек.
#pragma once

#include "core/canvas.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tnx::net {

// Состояние обложки (NET-02). Placeholder — id валиден, но ни одного размера
// нет: это НЕ ошибка сети, рисовать тональный плейсхолдер.
enum class ThumbState { Missing, Loading, Ready, Placeholder, Failed };

class ImageCache {
public:
    void start(int workers = 3);
    void stop();

    void request(const std::string& url);
    const Image* get(const std::string& url);   // nullptr — ещё грузится/нет картинки
    bool failed(const std::string& url);
    // true — все размеры обложки отсутствуют (404), но сеть в порядке.
    bool placeholder(const std::string& url);
    ThumbState state(const std::string& url);
    // Скелетон/плейсхолдер, пока грузится.
    void pump();
    void clear();
    void setMemoryLimitMB(int mb) { limitMb_ = mb; }

    // PERF-03: метрики кэша (LRU, лимит соблюдается).
    struct Stats {
        size_t bytes = 0;
        int entries = 0;
        int pending = 0;
        unsigned long long evicted = 0;
        int limitMb = 48;
    };
    Stats stats();

#ifdef TNX_HOST
    // PERF-03: тестовый вход в кэш без сети (host-тест LRU/лимита).
    void testPut(const std::string& url, int w, int h);
#endif

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int limitMb_ = 48;
};

// Декодирование изображения из памяти (JPEG/PNG/WebP), с даунскейлом до maxWidth.
Image decodeImage(const unsigned char* data, size_t len, int maxWidth = 480);

// --- Fallback-цепочка качеств обложки (NET-02) ------------------------------
// Порядок: maxresdefault -> sddefault -> hqdefault -> mqdefault -> default.
extern const char* const kThumbQualities[5];
// Подменить качество в URL обложки i.ytimg.com (расширение .jpg/.webp
// сохраняется). Возвращает "" если URL не похож на обложку.
std::string thumbnailAtQuality(const std::string& url, const char* quality);
// Цепочка кандидатов: от текущего качества исходного URL и ниже. Для не-ytimg
// URL возвращает единственный элемент — сам url.
std::vector<std::string> thumbnailCandidates(const std::string& url);

} // namespace tnx::net