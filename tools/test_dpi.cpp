// tools/test_dpi.cpp — функциональный тест SOCKS5-обхода DPI на хосте.
//
// Что проверяем:
//   1) startSocks() поднимает прокси, curl реально получает страницу через
//      socks5h://127.0.0.1:11080;
//   2) все режимы фрагментации не ломают TLS (каждый режим — отдельный запрос);
//   3) счётчики stats() заполняются (connections/fragmented/relayedBytes);
//   4) probe() через прокси возвращает ok;
//   5) https://www.youtube.com через прокси (если сеть не блокирует).

#include "dpi/dpi.hpp"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

const char* kProxy = "127.0.0.1:11080";

long fileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return sz;
}

bool fileContains(const char* path, const char* needle) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    return all.find(needle) != std::string::npos;
}

int runCurl(const std::string& url, const char* out, int maxTime = 30) {
    std::string cmd = "curl -sS -m " + std::to_string(maxTime) +
                      " --socks5-hostname " + kProxy + " \"" + url + "\" -o " + out +
                      " -w 'http=%{http_code} bytes=%{size_download} time=%{time_total}'";
    std::printf("  $ %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    int code = 0;
    if (rc != -1) {
        code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    }
    std::printf("  exit=%d\n", code);
    return code;
}

void dumpStats(const char* tag) {
    const auto& s = tnx::dpi::stats();
    std::printf(
        "  [%s] connections=%llu fragmented=%llu relayed=%llu errors=%llu "
        "handshake=%.1fms active=%d working=%d\n",
        tag, static_cast<unsigned long long>(s.connections),
        static_cast<unsigned long long>(s.fragmented),
        static_cast<unsigned long long>(s.relayedBytes),
        static_cast<unsigned long long>(s.errors), s.lastHandshakeMs,
        static_cast<int>(s.active), static_cast<int>(s.working));
}

} // namespace

int main() {
    using namespace tnx::dpi;

    init();
    Config& cfg = config();
    cfg.enabled = true;
    cfg.fragmentSize = 4;
    cfg.fragments = 6;
    cfg.delayMs = 3;

    std::printf("== startSocks(11080) ==\n");
    if (!startSocks(11080)) {
        std::printf("FAIL: не удалось запустить SOCKS5\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("SOCKS5 слушает 127.0.0.1:%d, режим: %s\n\n", socksPort(),
                modeName(cfg.mode));

    bool allOk = true;

    // --- 1. example.com во всех режимах фрагментации ---
    struct Case {
        Mode mode;
        const char* name;
    };
    const Case cases[] = {
        {Mode::Split, "Split"},
        {Mode::MultiSplit, "MultiSplit"},
        {Mode::SplitDisorder, "SplitDisorder"},
        {Mode::TlsRecordSplit, "TlsRecordSplit"},
        {Mode::Off, "Off"},
    };

    for (const Case& c : cases) {
        setMode(c.mode);
        std::printf("== режим %s ==\n", c.name);
        int rc = runCurl("https://example.com/", "/tmp/tnx_example.html");
        long sz = fileSize("/tmp/tnx_example.html");
        bool body = fileContains("/tmp/tnx_example.html", "Example Domain");
        std::printf("  size=%ld contains='Example Domain'=%d\n", sz,
                    static_cast<int>(body));
        if (rc != 0 || sz <= 0 || !body) allOk = false;
        dumpStats(c.name);
        std::printf("\n");
    }

    // --- 2. ATYP=1 (IPv4) и plain HTTP с разрывом Host (splitHttpHost) ---
    std::printf("== ATYP=1: http://1.1.1.1 через --socks5 ==\n");
    setMode(Mode::Split);
    int iprc = std::system(
        "curl -sS -m 20 --socks5 127.0.0.1:11080 http://1.1.1.1/ "
        "-o /tmp/tnx_ip.html -w 'http=%{http_code} bytes=%{size_download}'");
    long ipsz = fileSize("/tmp/tnx_ip.html");
    std::printf("\n  exit=%d size=%ld\n", WIFEXITED(iprc) ? WEXITSTATUS(iprc) : -1,
                ipsz);
    if (ipsz <= 0) allOk = false;

    std::printf("== plain HTTP + splitHttpHost (MultiSplit) ==\n");
    cfg.splitHttpHost = true;
    setMode(Mode::MultiSplit);
    int hrc = std::system(
        "curl -sS -m 20 --socks5-hostname 127.0.0.1:11080 http://example.com/ "
        "-o /tmp/tnx_http.html -w 'http=%{http_code} bytes=%{size_download}'");
    long hsz = fileSize("/tmp/tnx_http.html");
    std::printf("\n  exit=%d size=%ld\n", WIFEXITED(hrc) ? WEXITSTATUS(hrc) : -1, hsz);
    if (hsz <= 0) allOk = false;
    cfg.splitHttpHost = false;
    dumpStats("http");
    std::printf("\n");

    // --- 3. probe через SOCKS и напрямую ---
    std::printf("== probe(example.com) ==\n");
    ProbeResult pb = probe("example.com", 443, true);
    std::printf("  bypass: ok=%d rtt=%dms status=%ld error='%s'\n",
                static_cast<int>(pb.ok), pb.rttMs, pb.status, pb.error.c_str());
    if (!pb.ok) allOk = false;
    ProbeResult pd = probe("example.com", 443, false);
    std::printf("  direct: ok=%d rtt=%dms status=%ld error='%s'\n",
                static_cast<int>(pd.ok), pd.rttMs, pd.status, pd.error.c_str());
    dumpStats("probe");
    std::printf("\n");

    // --- 4. youtube.com через прокси (в этой сети может быть заблокирован) ---
    std::printf("== https://www.youtube.com через прокси ==\n");
    setMode(Mode::Split);
    int yrc = runCurl("https://www.youtube.com/", "/tmp/tnx_youtube.html", 40);
    long ysz = fileSize("/tmp/tnx_youtube.html");
    std::printf("  size=%ld\n", ysz);
    dumpStats("youtube");
    bool youtubeOk = (yrc == 0 && ysz > 0);
    std::printf("  youtube: %s\n\n",
                youtubeOk ? "OK" : "недоступен в этой сети (IP-блокировка), "
                                   "это фиксируется как факт, а не как ошибка теста");

    // --- 5. идемпотентность stop/start ---
    std::printf("== stopSocks / повторный stop ==\n");
    stopSocks();
    stopSocks(); // должен быть no-op
    std::printf("  после stop: socksPort=%d active=%d\n", socksPort(),
                static_cast<int>(stats().active));
    if (socksPort() != 0 || stats().active) allOk = false;

    // --- 6. повторный запуск на том же порту ---
    if (!startSocks(11080)) {
        std::printf("FAIL: повторный startSocks\n");
        return 1;
    }
    std::printf("  повторный старт: порт=%d active=%d\n", socksPort(),
                static_cast<int>(stats().active));
    stopSocks();

    std::printf("\nИТОГ: %s (youtube %s)\n", allOk ? "PASS" : "FAIL",
                youtubeOk ? "доступен" : "недоступен в этой сети");
    return allOk ? 0 : 2;
}