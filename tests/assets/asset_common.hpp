// Helpers shared by the asset tests: deterministic data, the opt-in game directory, small file utilities.
#pragma once
#include "nue_test.h"
#include "nue/assets.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace asstest {

inline uint32_t rng_next(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

// Game files are never part of the repository: tests that read them need NUE_TEST_GAME_DIR (the game's data folder) and skip otherwise.
inline const char* game_dir() { const char* d = std::getenv("NUE_TEST_GAME_DIR"); return d && d[0] ? d : nullptr; }
inline std::string game_path(const char* rel) { return std::string(game_dir()) + "/" + rel; }

inline bool load_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

inline void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

}  // namespace asstest
