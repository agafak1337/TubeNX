// player/player.hpp — воспроизведение через libmpv (software-рендер) + audren.
#pragma once

#include "core/canvas.hpp"

#include <string>

namespace tnx::player {

class Player {
public:
    // PLR-01: явная машина состояний открытия. FetchingDetails/FetchingStreams
    // выполняет UI до open(); плеер ведёт остальные переходы и логирует их.
    enum class State {
        Idle,
        Opening,
        FileLoaded,
        Playing,
        Paused,
        Ended,
        Error,
    };

    bool init();
    void shutdown();

    // PLR-14: startAt > 0 — открыть и перемотать после FILE_LOADED (смена
    // качества без выхода из плеера, сохранение позиции).
    bool open(const std::string& url, const std::string& title = "", double startAt = 0.0);
    // Второй поток (DASH): подключить внешнюю аудиодорожку к уже открытому видео.
    bool addAudio(const std::string& url);
    void close();
    bool loaded() const;

    void setPaused(bool p);
    void togglePause();
    bool paused() const;
    State state() const;
    const char* stateName() const;

    void seek(double seconds);
    void seekRelative(double delta);
    void setVolume(int v);          // 0..130
    int volume() const;
    void setMuted(bool m);          // PLR-09
    bool muted() const;
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

    // PLR-07/13: есть ли выбранная аудиодорожка и не играет ли тихий null-AO.
    bool audioTrackPresent() const;
    // Пустая строка — звук в порядке; иначе текст для UI («Нет звука: …»).
    std::string audioWarning() const;
    // PLR-07: аудио нет, но файл загружен — UI может перезапустить muxed 18.
    bool needsMuxedFallback() const;

    // PL-03/PLR-11: сон/пробуждение. В фоновом режиме (HOME/сон) ставим паузу,
    // на пробуждении переинициализируем аудио и продолжаем с той же позиции.
    void onSleep();
    void onWake();

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
        double avsync = 0.0;      // PLR-04/12: расхождение A/V, секунды
        long long vbitrate = 0;   // PLR-12: битрейт видео, бит/с
        long long abitrate = 0;   // PLR-12: битрейт аудио, бит/с
    };
    Status status() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

Player& instance();

} // namespace tnx::player