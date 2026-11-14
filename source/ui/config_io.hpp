// ui/config_io.hpp — файловые операции конфига и истории (CFG-01/02/04/06).
//
// Вынесено из app.cpp, чтобы атомарную запись, восстановление из .bak и
// экранирование полей можно было прогнать host-тестом без Switch
// (tools/test_config.cpp). Функции чистые и не знают про Config.
#pragma once

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH__
// devkitPro/newlib при -std=c++17 не объявляет fileno в <cstdio>/<unistd.h>.
extern "C" int fileno(FILE*);
#endif

namespace tnx::app::configio {

// CFG-01: атомарная запись. Данные пишутся в *.tmp, сбрасываются на диск,
// прежний файл уходит в *.bak, и только потом tmp переименовывается на место.
// Если питание пропадёт в любой момент, на SD останется либо старый, либо
// новый целый файл — но не обрезанный.
inline bool writeAtomic(const char* path, const std::string& data) {
    const std::string tmp = std::string(path) + ".tmp";
    const std::string bak = std::string(path) + ".bak";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (ok) ok = fflush(f) == 0;
    if (ok) {
        const int fd = fileno(f);
        if (fd >= 0) fsync(fd);   // на FAT может быть no-op, но не вредит
    }
    fclose(f);
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    // Прежний файл — в .bak (если его не было, rename просто не сработает).
    remove(bak.c_str());
    rename(path, bak.c_str());
    if (rename(tmp.c_str(), path) != 0) {
        // Не удалось заменить — хотя бы вернуть старый на место.
        rename(bak.c_str(), path);
        remove(tmp.c_str());
        return false;
    }
    return true;
}

// Читает файл целиком. false — файла нет или он не открылся.
inline bool readWholeFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// CFG-01: читает основной файл, а если он пуст/битый — *.bak.
// `required` — символ, который обязан встретиться в валидном файле
// (для ini это '='); 0 — достаточно непустого файла.
// Возвращает true, если данные взяты из .bak.
inline bool readWithBak(const char* path, std::string& out, char required) {
    out.clear();
    if (readWholeFile(path, out) && !out.empty() &&
        (required == 0 || out.find(required) != std::string::npos))
        return false;
    std::string bak;
    if (readWholeFile((std::string(path) + ".bak").c_str(), bak) && !bak.empty()) {
        out.swap(bak);
        return true;
    }
    out.clear();
    return false;
}

// CFG-04: экранирование полей истории. Табы/переводы строк в названиях
// роликов раньше рвали строку TSV и съедали запись.
inline std::string escapeField(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
        case '\\': o += "\\\\"; break;
        case '\t': o += "\\t"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        default: o += ch; break;
        }
    }
    return o;
}

inline std::string unescapeField(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            if (n == 't') o += '\t';
            else if (n == 'n') o += '\n';
            else if (n == 'r') o += '\r';
            else o += n;   // «\\» и любой неизвестный код — как есть
        } else {
            o += s[i];
        }
    }
    return o;
}

// Строка ini/лога не должна содержать переводов строк.
inline std::string oneLine(const std::string& s) {
    std::string o = s;
    for (char& ch : o)
        if (ch == '\n' || ch == '\r') ch = ' ';
    return o;
}

// CFG-07/LOG-07: в диагностике не должно быть пароля прокси (user:pass@host).
inline std::string redactUserInfo(const std::string& u) {
    const size_t sep = u.find("://");
    const size_t at = u.find('@');
    if (sep != std::string::npos && at != std::string::npos && at > sep)
        return u.substr(0, sep + 3) + "***@" + u.substr(at + 1);
    return u;
}

// CFG-06: проверка, что каталог доступен на запись. На консоли без SD
// sdmc:/config может отсутствовать или быть read-only.
inline bool probeWritable(const char* dir) {
    mkdir(dir, 0777);
    const std::string probe = std::string(dir) + "/.write-test";
    FILE* f = fopen(probe.c_str(), "wb");
    if (!f) return false;
    const bool ok = fputs("ok", f) >= 0;
    fclose(f);
    remove(probe.c_str());
    return ok;
}

} // namespace tnx::app::configio