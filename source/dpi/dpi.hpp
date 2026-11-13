// dpi/dpi.hpp — обход DPI в духе ByeByeDPI: фрагментация TLS ClientHello,
// локальный SOCKS5 для curl и HTTP-ретранслятор для плеера.
#pragma once

#include <cstdint>
#include <string>

namespace tnx::dpi {

enum class Mode {
    Off = 0,
    Split,      // разбить ClientHello на 2 части (самый совместимый)
    MultiSplit, // много мелких фрагментов — против агрессивного DPI
    SplitDisorder, // фрагменты в обратном порядке + задержка
    TlsRecordSplit, // разбить по границам TLS-записей
};

struct Config {
    bool enabled = true;
    Mode mode = Mode::Split;
    int fragmentSize = 1;   // размер первого фрагмента в байтах (1..16).
                            // 2 — нельзя: на этой сети ClientHello, разрезанный
                            // ровно после 2 байт, DPI сбрасывает (RST), а после
                            // 1 и 3 байт проходит нормально.
    int fragments = 6;      // количество фрагментов для MultiSplit
    int delayMs = 1;        // пауза между фрагментами
    bool splitHttpHost = false; // для http:// запросов разрывать Host
    bool bypassOnlyBlocked = false;
    // Ретранслятору видео по умолчанию нужен локальный SOCKS. Если же
    // приложение решило, что напрямую (с подменой SNI) всё работает, прокси
    // не навязываем — иначе видео пойдёт «через обход» без нужды.
    bool relayViaLocalSocks = true;
};

Config& config();

struct Stats {
    uint64_t connections = 0;
    uint64_t fragmented = 0;
    uint64_t relayedBytes = 0;
    uint64_t errors = 0;
    float lastHandshakeMs = 0.f;
    bool active = false;      // прокси запущен
    bool working = false;     // последний probe успешен
    uint64_t savedBytes = 0;
};
Stats& stats();

void init();

// SOCKS5-прокси на 127.0.0.1:port. curl ходит через него: socks5h://127.0.0.1:1080
bool startSocks(int port = 1080);
void stopSocks();
int socksPort();

// Локальный HTTP-ретранслятор для mpv: http://127.0.0.1:8899/u?u=<upstream-url>
bool startRelay(int port = 8899);
void stopRelay();
int relayPort();
std::string relayUrl(const std::string& upstreamUrl);

struct ProbeResult {
    bool ok = false;
    int rttMs = 0;
    long status = 0;
    std::string error;
};
ProbeResult probe(const char* host, int port = 443, bool useBypass = true);

// Человекочитаемое имя режима (для экрана настроек).
const char* modeName(Mode m);
void setMode(Mode m);

} // namespace tnx::dpi