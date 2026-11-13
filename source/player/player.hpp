// player/player.hpp — воспроизведение через libmpv (software-рендер) + audren.
#pragma once

#include "core/canvas.hpp"

#include <string>

namespace tnx::player {

class Player {
public:
    bool init();
    void shutdown();

    bool open(const std::string& url, const std::string& title = "");
    // Второй поток (DASH): подключить внешнюю аудиодорожку к уже открытому видео.
    bool addAudio(const std::string& url);
    void close();
    bool loaded() const;

    void setPaused(bool p);
    void togglePause();
    bool paused() const;

    void seek(double seconds);
    void seekRelative(double delta);
    void setVolume(int v);          // 0..130
    int volume() const;
    void setSpeed(double s);
    // Сдвиг звука относительно видео, секунды: + позже, − раньше.
    void setAudioDelay(double sec);

    bool playing() const;
    bool buffering() const;
    double position() const;
    double duration() const;
    bool hasVideo() const;
    std::string error() const;
    std::string title() const;

    // Вызывать раз в кадр: обновляет кадр в Image (RGBA8888), если появился новый.
    bool update(Image& out);
    void setTargetSize(int w, int h);   // размер рендера (обычно 960x540)
    int videoWidth() const;
    int videoHeight() const;

    // Состояние для UI
    struct Status {
        bool seekable = false;
        bool eof = false;
        int dropped = 0;
        float decodeFps = 0.f;
        int cacheMs = 0;
    };
    Status status() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

Player& instance();

} // namespace tnx::player