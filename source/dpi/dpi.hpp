// dpi/dpi.hpp — обход DPI в духе ByeByeDPI: фрагментация TLS ClientHello,
// локальный SOCKS5 для curl и HTTP-ретранслятор для плеера.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    // DPI-06: режим подобран автопробой (probeMode/autoSelectMode). Пока флаг
    // стоит, при ошибках retry-цикл может переспросить провайдера и сменить
    // режим (только когда видео не играет).
    bool modeAutoPicked = false;
    // Порог накопленных ошибок ретранслятора, после которого имеет смысл
    // перепроверить режим (DPI-06). 0 — не перепроверять автоматически.
    int autoReprobeErrors = 3;
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
    // DPI-16: расширенные метрики для /healthz и экрана диагностики.
    float rateKbps = 0.f;         // скорость последнего окна ретранслятора
    int activeSessions = 0;       // активных upstream-сессий ретранслятора
    uint64_t sslSessions = 0;     // всего SSL-сессий, поднятых ретранслятором
    std::string lastError;        // последняя человекочитаемая ошибка
};
Stats& stats();

// DPI-16: потокобезопасный снимок статистики (копия под мьютексом). Нужен
// там, где счётчики читают из другого потока, чем пишут (диагностика/тесты).
Stats statsSnapshot();

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

// ---------------------------------------------------------------------------
// DPI-06: автовыбор режима фрагментации.
// ---------------------------------------------------------------------------

// Результат одной пробы режима.
struct ModeProbe {
    bool ok = false;
    Mode mode = Mode::Off;
    long status = 0;
    int rttMs = 0;
    std::string error;
};

// Проба ОДНОГО режима: временно ставит его в конфиг, делает запрос на
// generate_204 через SOCKS5 и возвращает результат. Режим в конфиге
// восстанавливается. На хосте без сети вернёт ok=false.
ModeProbe probeMode(Mode m);

// Результат перебора режимов.
struct AutoSelectResult {
    bool found = false;      // найден рабочий режим
    Mode mode = Mode::Off;   // выбранный режим (при found)
    int tried = 0;           // сколько режимов реально проверено
    int workingIndex = -1;   // индекс в порядке перебора (Off..TlsRecordSplit)
    std::string lastError;   // последняя ошибка пробы, если ничего не нашлось
};

// Чистая логика перебора (без сети): зовёт probeFn по порядку
// Off -> Split -> MultiSplit -> TlsRecordSplit и берёт первый рабочий.
// Именно её проверяет host-тест, поэтому она не трогает конфиг и сокеты.
AutoSelectResult selectWorkingMode(const std::function<bool(Mode)>& probeFn);

// Полный автоподбор: selectWorkingMode() + probeMode() + запись в config().
// Возвращает выбранный режим. Пока идёт видео — режим НЕ меняется
// (found=false, lastError="video playing").
AutoSelectResult autoSelectMode();

// DPI-06: во время видео менять режим нельзя. Устанавливает плеер.
void setVideoPlaying(bool playing);
bool videoPlaying();

// DPI-06: перепроверка при ошибках. true — пора повторить автоподбор
// (режим был выбран автопробой, накопилось >= autoReprobeErrors ошибок и
// видео не играет). Сброс порога делает autoSelectMode().
bool shouldReprobeOnErrors();

// ---------------------------------------------------------------------------
// DPI-09..DPI-12, DPI-16: ретранслятор. Общие помощники вынесены из
// libcurl-ветки и собираются под -DTNX_HOST (DPI-15): их проверяет host-тест.
// ---------------------------------------------------------------------------

// DPI-09: максимальная длина URL ретранслятора. Длиннее — 414.
constexpr size_t kMaxRelayUrl = 8192;

// ---------------------------------------------------------------------------
// DPI-15: доступ к внутренним парсерам для host-теста/fuzz. Не для прода.
// ---------------------------------------------------------------------------
namespace test {

std::string pctEncodeTest(const std::string& s);
std::string pctDecodeTest(const std::string& s);
bool parseRelayUpstreamTest(const std::string& path, std::string& upstream);
bool relayUrlTooLongTest(const std::string& url);

bool validByteRangeTest(const std::string& value);

int curlCodeToHttpTest(int curlCode);
std::string httpErrorHeadersTest(int curlCode, int retryAfterSec);

bool sendResponseTest(int fd, int code, const char* contentType, const std::string& body,
                      const std::string& extraHeaders);

std::string healthzTest(const Stats& s, int video, int audio, int other, int slots);

// Разбор HTTP-запроса из уже прочитанного буфера. Возвращает -1 при ошибке,
// иначе число разобранных заголовков.
int parseHttpRequestTest(const std::string& raw, std::string& method, std::string& path);
// То же, но читает из сокета.
int readHttpRequestTest(int fd, std::string& method, std::string& path);
// SOCKS5-рукопожатие. Для fuzz: setSocksUpstreamDisabledTest(true) отключает
// реальное подключение к цели, поэтому битый ввод не уходит в сеть.
int socksHandshakeTest(int client, bool* fragmentedOut);
void setSocksUpstreamDisabledTest(bool disabled);

// DPI-13: открыть слушатель на wantPort (fallback на эфемерный). -1 при сбое.
int bindLocalListenerTest(int wantPort, int& actualPort);

// DPI-06: управление порогом перепроверки для тестов.
void setAutoBaselineErrorsTest(uint64_t errors);
uint64_t autoBaselineErrorsTest();
// Возвращает workingIndex выбранного режима или -1, если пробы все false.
int selectWorkingModeTest(bool offOk, bool splitOk, bool multiOk, bool tlsOk);

} // namespace test


// DPI-05: чистый план фрагментации первого пакета (TLS ClientHello / HTTP).
// Не трогает сокеты и не спит — из одного источника собирает и отправку
// (sendFirstPacket), и host-тест (tools/test_dpi.cpp). Инвариант: если
// склеить chunks по порядку, получаются ровно исходные len байт.
struct FragmentPlan {
    std::vector<std::string> chunks; // фрагменты в порядке отправки
    std::vector<int> delaysMs;       // пауза ПЕРЕД соответствующим фрагментом
    bool applied = false;            // режим реально резал пакет (было >1 send)
};

FragmentPlan planFragments(const char* data, size_t len, const Config& cfg);

} // namespace tnx::dpi