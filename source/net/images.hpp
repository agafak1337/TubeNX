// net/images.hpp — асинхронная загрузка и декодирование обложек.
#pragma once

#include "core/canvas.hpp"

#include <string>
#include <vector>

namespace tnx::net {

class ImageCache {
public:
    void start(int workers = 3);
    void stop();

    void request(const std::string& url);
    const Image* get(const std::string& url);   // nullptr — ещё грузится
    bool failed(const std::string& url);
    // Скелетон/плейсхолдер, пока грузится.
    void pump();
    void clear();
    void setMemoryLimitMB(int mb) { limitMb_ = mb; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int limitMb_ = 48;
};

// Декодирование изображения из памяти (JPEG/PNG/WebP), с даунскейлом до maxWidth.
Image decodeImage(const unsigned char* data, size_t len, int maxWidth = 480);

} // namespace tnx::net