// net/sni.cpp — обход SNI-блокировки YouTube.
//
// Провайдерские DPI (в т.ч. РКН) вырезают TLS-рукопожатие, если в ClientHello
// встречается заблокированное имя — сравнение при этом регистрозависимое.
// Проверено на реальной сети: `www.youtube.com` -> таймаут, `WWW.YOUTUBE.COM`
// -> нормальный TLS 1.3 и ответ сервера. По RFC 6066 SNI регистронезависим,
// поэтому верхний регистр не ломает ни TLS, ни проверку сертификата.
//
// libcurl и ffmpeg в devkitPro-порту ходят в TLS через системный сервис
// `ssl` (libnx), а не через mbedTLS: имя хоста задаётся одним вызовом
// sslConnectionSetHostName(). Перехватываем его линкером (--wrap) и
// поднимаем регистр — этого достаточно и для API, и для видео.
#include <switch.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>

#include "platform.hpp"

namespace {

// Хосты, которые точно режут по SNI. Остальные не трогаем: пусть идут как есть.
const char* const kBlockedSuffixes[] = {
    "youtube.com", "youtube-nocookie.com", "youtu.be", "ytimg.com",
    "googlevideo.com", "ggpht.com", "googleapis.com", "gvt1.com", "gvt2.com",
};

// Отдельный, более грубый приём для видео: у `*.googlevideo.com` регистр не
// помогает (проверено: и нижний, и верхний — таймаут), зато серверы Google
// обслуживают эти URL по заголовку Host. Поэтому в SNI отправляем чужое, но
// разрешённое имя, а Host остаётся настоящим:
//   SNI=www.google.com + Host: rr4---sn-....googlevideo.com -> 206 video/mp4
// Проверено на живой сети (1.7-2.5 МиБ/с, стабильно).
// Сертификат при этом выписан на *.google.com, поэтому в ретрансляторе для
// таких хостов проверка имени отключена (см. dpi.cpp).
const char* const kSpoofSuffixes[] = {"googlevideo.com"};
const char* const kSpoofName = "www.google.com";

// Имя можно менять на время одного соединения: ретранслятор перебирает
// кандидатов, если провайдер начнёт резать и первое. Переменная thread_local:
// libcurl работает в отдельном потоке на каждое соединение, так что гонок нет.
thread_local const char* g_spoofOverride = nullptr;

bool endsWithNoCase(const char* host, const char* suf) {
    const size_t n = strlen(host), m = strlen(suf);
    if (n < m) return false;
    const char* tail = host + (n - m);
    for (size_t i = 0; i < m; i++) {
        unsigned char a = (unsigned char)tail[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (a != (unsigned char)suf[i]) return false;
    }
    return true;
}

bool needsSpoof(const char* host) {
    if (!host || !*host) return false;
    for (const char* suf : kSpoofSuffixes)
        if (endsWithNoCase(host, suf)) return true;
    return false;
}

bool needsUpper(const char* host) {
    if (!host || !*host) return false;
    const size_t n = strlen(host);
    for (const char* suf : kBlockedSuffixes) {
        const size_t m = strlen(suf);
        if (n < m) continue;
        const char* tail = host + (n - m);
        bool eq = true;
        for (size_t i = 0; i < m; i++) {
            unsigned char a = (unsigned char)tail[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (a != (unsigned char)suf[i]) {
                eq = false;
                break;
            }
        }
        if (eq) return true;
    }
    return false;
}

} // namespace

extern "C" Result __real_sslConnectionSetHostName(SslConnection* c, const char* str, u32 str_bufsize);

// Выбор имени для следующих рукопожатий в текущем потоке. nullptr — умолчание.
extern "C" void tubeSpoofSetName(const char* name) {
    g_spoofOverride = name;
}

extern "C" Result __wrap_sslConnectionSetHostName(SslConnection* c, const char* str, u32 str_bufsize) {
    if (!str || !*str) return __real_sslConnectionSetHostName(c, str, str_bufsize);

    // Диагностика: первые 24 рукопожатия видно, какое имя реально уходит в сеть.
    // Без этого в логе не отличить «обход не применился» от «обход не помог».
    static std::atomic<int> logged{0};
    const bool logIt = logged.fetch_add(1) < 24;

    // Видео: подменяем имя целиком (см. kSpoofSuffixes выше).
    if (needsSpoof(str)) {
        const char* name = g_spoofOverride ? g_spoofOverride : kSpoofName;
        const size_t n = strlen(name);
        u32 sz = str_bufsize ? str_bufsize : (u32)n;
        if (sz > n) sz = (u32)n;
        if (sz > 0xff) sz = 0xff;
        if (logIt) tnx::platform::bootLog("sni: %s -> %s (подмена)", str, name);
        return __real_sslConnectionSetHostName(c, name, sz);
    }

    if (!needsUpper(str)) {
        if (logIt) tnx::platform::bootLog("sni: %s -> без изменений", str);
        return __real_sslConnectionSetHostName(c, str, str_bufsize);
    }

    char buf[256];
    size_t n = strlen(str);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)str[i]);
    buf[n] = 0;

    // str_bufsize у сервиса ограничен 0xff — не выходим за него.
    u32 sz = str_bufsize ? str_bufsize : (u32)n;
    if (sz > n) sz = (u32)n;
    if (sz > 0xff) sz = 0xff;
    if (logIt) tnx::platform::bootLog("sni: %s -> %s (регистр)", str, buf);
    return __real_sslConnectionSetHostName(c, buf, sz);
}
// ---------------------------------------------------------------------------
// Диагностика TLS-стека.
//
// На консоли рукопожатие иногда занимает секунды, и тогда curl упирается в
// connect-таймаут. Чтобы понять, кто именно тормозит — сервис ssl, импорт
// CA-бандла (у нас 148 сертификатов!) или сеть — измеряем каждый вызов.
// ---------------------------------------------------------------------------
namespace {

long long msNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::atomic<int> g_slowLogged{0};

void noteSlow(const char* what, long long ms, long rc) {
    if (ms < 250 && rc == 0) return;
    if (g_slowLogged.fetch_add(1) >= 60) return;
    tnx::platform::bootLog("ssl: %s %lld мс, rc=0x%x", what, ms, (unsigned)rc);
}

std::atomic<int> g_pkiCalls{0};
std::atomic<long long> g_pkiMs{0};

} // namespace

extern "C" Result __real_sslCreateContext(SslContext* c, u32 ssl_version);
extern "C" Result __wrap_sslCreateContext(SslContext* c, u32 ssl_version) {
    const long long t0 = msNow();
    Result rc = __real_sslCreateContext(c, ssl_version);
    noteSlow("CreateContext", msNow() - t0, rc);
    return rc;
}

extern "C" Result __real_sslContextCreateConnection(SslContext* c, SslConnection* conn);
extern "C" Result __wrap_sslContextCreateConnection(SslContext* c, SslConnection* conn) {
    const long long t0 = msNow();
    Result rc = __real_sslContextCreateConnection(c, conn);
    noteSlow("CreateConnection", msNow() - t0, rc);
    return rc;
}

extern "C" int __real_socketSslConnectionSetSocketDescriptor(SslConnection* c, int sockfd);
extern "C" int __wrap_socketSslConnectionSetSocketDescriptor(SslConnection* c, int sockfd) {
    const long long t0 = msNow();
    int rc = __real_socketSslConnectionSetSocketDescriptor(c, sockfd);
    noteSlow("SetSocketDescriptor", msNow() - t0, rc);
    return rc;
}

extern "C" Result __real_sslContextImportServerPki(SslContext* c, const void* buffer, u32 size,
                                                   SslCertificateFormat format, u64* id);
extern "C" Result __wrap_sslContextImportServerPki(SslContext* c, const void* buffer, u32 size,
                                                   SslCertificateFormat format, u64* id) {
    const long long t0 = msNow();
    Result rc = __real_sslContextImportServerPki(c, buffer, size, format, id);
    const long long dt = msNow() - t0;
    const int n = g_pkiCalls.fetch_add(1) + 1;
    const long long total = g_pkiMs.fetch_add(dt) + dt;
    // Бандл целиком — 148 сертификатов. Если импорт идёт на каждое соединение,
    // в логе будет видно «148 вызовов» подряд.
    if (n % 40 == 0 || dt >= 100) {
        tnx::platform::bootLog("ssl: ImportServerPki #%d, %lld мс (всего %lld мс), size=%u",
                               n, dt, total, (unsigned)size);
    }
    return rc;
}

extern "C" Result __real_sslConnectionDoHandshake(SslConnection* c, u32* out_size,
                                                  u32* total_certs, void* server_certbuf,
                                                  u32 server_certbuf_size);
extern "C" Result __wrap_sslConnectionDoHandshake(SslConnection* c, u32* out_size,
                                                  u32* total_certs, void* server_certbuf,
                                                  u32 server_certbuf_size) {
    const long long t0 = msNow();
    Result rc = __real_sslConnectionDoHandshake(c, out_size, total_certs, server_certbuf,
                                                server_certbuf_size);
    noteSlow("DoHandshake", msNow() - t0, rc);
    return rc;
}

// libcurl просит у сервиса ssl 3 сессии (Curl_libnx_init: sslInitialize(3)).
// Мы грузим сеть из нескольких потоков одновременно (обложки, InnerTube,
// ретранслятор видео), поэтому просим максимум — 4. Больше сервис не умеет.
extern "C" Result __real_sslInitialize(u32 num_sessions);
extern "C" Result __wrap_sslInitialize(u32 num_sessions) {
    const u32 want = num_sessions < 4 ? 4 : num_sessions;
    const long long t0 = msNow();
    Result rc = __real_sslInitialize(want);
    tnx::platform::bootLog("ssl: Initialize(%u) -> 0x%x за %lld мс", want, (unsigned)rc,
                           msNow() - t0);
    return rc;
}
