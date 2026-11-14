// md3/icons.hpp — АВТОГЕНЕРАЦИЯ: имена Material Symbols → кодпоинты.
// Файл создан tools/gen_icons.py; шрифт — resources/fonts/MaterialSymbols.ttf (сабсет).
#pragma once
#include <cstdint>
#include <cstring>

namespace tnx::md3 {

struct IconEntry { const char* name; uint32_t cp; };
inline const IconEntry kIcons[] = {
    {"account_circle", 0xf20b},
    {"add", 0xe145},
    {"arrow_back", 0xe5c4},
    {"arrow_forward", 0xe5c8},
    {"arrow_upward", 0xe5d8},
    {"bolt", 0xea0b},
    {"cast", 0xe307},
    {"check", 0xe668},
    {"chevron_left", 0xe5cb},
    {"chevron_right", 0xe5cc},
    {"close", 0xe5cd},
    {"cloud_download", 0xe2c0},
    {"cloud_off", 0xe2c1},
    {"dark_mode", 0xe51c},
    {"delete", 0xe92e},
    {"download", 0xf090},
    {"error", 0xf8b6},
    {"expand_more", 0xe5cf},
    {"filter_list", 0xe152},
    {"forward_10", 0xe056},
    {"fullscreen", 0xe5d0},
    {"fullscreen_exit", 0xe5d1},
    {"gpp_good", 0xf013},
    {"high_quality", 0xe024},
    {"history", 0xe8b3},
    {"home", 0xe9b2},
    {"info", 0xe88e},
    {"keyboard_arrow_up", 0xe316},
    {"language", 0xea07},
    {"light_mode", 0xe518},
    {"live_tv", 0xe63a},
    {"lock", 0xe899},
    {"logout", 0xe9ba},
    {"menu", 0xe5d2},
    {"more_vert", 0xe5d4},
    {"movie", 0xe404},
    {"music_note", 0xe405},
    {"open_in_new", 0xe89e},
    {"pause", 0xe034},
    {"person", 0xf0d3},
    {"play_arrow", 0xe037},
    {"playlist_play", 0xe05f},
    {"refresh", 0xe5d5},
    {"replay", 0xe042},
    {"replay_10", 0xe059},
    {"restart_alt", 0xf053},
    {"schedule", 0xefd6},
    {"search", 0xef7a},
    {"settings", 0xe8b8},
    {"share", 0xe80d},
    {"skip_next", 0xe044},
    {"skip_previous", 0xe045},
    {"sort", 0xe164},
    {"speed", 0xe9e4},
    {"sports", 0xea30},
    {"subscriptions", 0xe064},
    {"subtitles", 0xe048},
    {"sync", 0xe627},
    {"thumb_down", 0xf578},
    {"thumb_up", 0xf577},
    {"trending_up", 0xe8e5},
    {"tune", 0xe429},
    {"verified", 0xef76},
    {"volume_off", 0xe04f},
    {"volume_up", 0xe050},
    {"vpn_key", 0xe0da},
    {"wifi", 0xe63e},
    {"wifi_off", 0xe648},
};

inline uint32_t iconCodepoint(const char* name) {
    if (!name) return 0;
    for (const auto& e : kIcons)
        if (std::strcmp(e.name, name) == 0) return e.cp;
    return 0;
}

} // namespace tnx::md3
