#pragma once
#include "image.hpp"
#include <map>

namespace nuetex {

using KeyValues = std::map<std::string, std::string>;

nue_result dds_load(const uint8_t* data, size_t size, uint32_t flags, Image** out);
nue_result dds_save(const Image& im, uint32_t flags, std::vector<uint8_t>& out);
nue_result ktx2_load(const uint8_t* data, size_t size, uint32_t flags, Image** out, KeyValues* kv);
nue_result ktx2_save(const Image& im, const KeyValues* kv, std::vector<uint8_t>& out);
nue_result png_load(const uint8_t* data, size_t size, uint32_t flags, Image** out);
nue_result png_save(const Image& im, std::vector<uint8_t>& out);

bool read_file(const char* utf8_path, std::vector<uint8_t>& out);
bool write_file_atomic(const char* utf8_path, const void* data, size_t size);       // temp file + rename
bool make_dirs(const std::string& utf8_path);
bool remove_file(const char* utf8_path);

}  // namespace nuetex
