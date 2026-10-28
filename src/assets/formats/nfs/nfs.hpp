// NFS plugin internals shared by nfs.cpp (chunk stream) and nfs_tpk.cpp (texture packs).
#pragma once
#include "nue/assets.h"
#include "nue/core.h"

#include "../../bytes.hpp"

#include <string>
#include <vector>

namespace nfs {

using nuea::lowbit;
using nuea::rd16;
using nuea::rd32;
using nuea::wr16;
using nuea::wr32;

constexpr uint32_t kIdTpk = 0xB3300000u, kIdTpkInfo = 0xB3310000u, kIdTpkHeader = 0x33310001u, kIdTpkHashes = 0x33310002u;
constexpr uint32_t kIdTpkRecords = 0x33310004u, kIdTpkFormats = 0x33310005u, kIdTpkDataGroup = 0xB3320000u, kIdTpkData = 0x33320002u;
constexpr uint32_t kRecSize = 0x7C, kFmtSize = 32;
constexpr uint64_t kDataAlign = 128, kMaxPadAlign = 32768;

// compression byte of a texture record
enum : uint8_t { kCtBgra8 = 0x20, kCtDxt1 = 0x22, kCtDxt3 = 0x24, kCtDxt5 = 0x26, kCtP8 = 0x08, kCtP8_16 = 0x80, kCtP8_64 = 0x81 };

struct TexRec {
    uint8_t raw[kRecSize];
    char name[25];
    uint32_t hash, cls;
    uint32_t placement, pal_off, image_size, pal_size, top_size;
    uint16_t width, height;
    uint8_t ct, mips;
};

struct Tpk {
    uint32_t node = 0xFFFFFFFFu, header_node = 0xFFFFFFFFu, hash_node = 0xFFFFFFFFu, rec_node = 0xFFFFFFFFu, fmt_node = 0xFFFFFFFFu;
    uint32_t data_group = 0xFFFFFFFFu, data_node = 0xFFFFFFFFu;
    uint64_t data_size = 0;                                  // payload size of the data chunk
    uint64_t base_rel = 0;                                   // first byte of pixel data, relative to the data payload (128 aligned in the file)
    std::string name;
    uint32_t hash = 0;
    std::vector<TexRec> tex;
};

struct TexPlan {
    uint8_t rec[kRecSize];
    bool edited = false;
    std::vector<uint8_t> image;                              // edited: the new pixel bytes (all mip levels)
    std::vector<uint8_t> pal_data;                           // edited with native data: the new palette
    bool pal_edited = false;
    uint64_t src_off = 0;                                    // unedited: payload relative offset of the image in the source
    uint64_t image_size = 0;
    uint32_t pal_size = 0;
    uint64_t pal_src = 0;
    uint32_t placement = 0, pal_off = 0;
};

struct TpkPlan {
    uint32_t tpk = 0xFFFFFFFFu, rec_node = 0xFFFFFFFFu, fmt_node = 0xFFFFFFFFu, data_node = 0xFFFFFFFFu;
    std::vector<TexPlan> tex;
    std::vector<uint8_t> rec_bytes, fmt_bytes;
    uint64_t src_base_rel = 0;
    uint64_t pool_total = 0;
};

// plugin entry points
int32_t NUE_CALL probe(void*, const void* head, size_t head_size, uint64_t total);
nue_result NUE_CALL open(void*, nue_asset_open_ctx* oc);
void NUE_CALL close(void*, void*);
nue_result NUE_CALL index(void*, nue_asset_container c, void*, const nue_asset_resource_ops* ops, void* builder);
nue_result NUE_CALL get_image(void*, nue_asset_container c, void*, uint32_t resource, nue_tex_image* out);
nue_result NUE_CALL get_native(void*, nue_asset_container c, void*, uint32_t resource, nue_asset_native_texture* out, void** out_memory);
nue_result NUE_CALL write(void*, nue_asset_container c, void*, uint32_t flags, nue_asset_sink* sink);
const char* NUE_CALL default_codec(void*, const char* ext);
nue_result index_meshes(nue_asset_container c, const nue_asset_resource_ops* ops, void* builder);
nue_result NUE_CALL get_mesh(void*, nue_asset_container c, void*, uint32_t resource, nue_asset_mesh* out, void** out_memory);

// texture packs
bool load_tpk(nue_asset_container c, uint32_t tpk_node, Tpk* out);
nue_result plan_tpk(nue_asset_container c, const Tpk& t, TpkPlan* out);
nue_result emit_tpk_data(nue_asset_container c, nue_asset_sink* sink, const TpkPlan& plan, uint64_t payload_pos);

bool parse_texrec(const uint8_t* p, TexRec* t);
nue_result texrec_to_image(const TexRec& x, const std::vector<uint8_t>& raw, const std::vector<uint8_t>& pal, nue_tex_image* out);
// car packs (TEXTURES.BIN: per texture compressed blobs)
nue_result index_car_pack(nue_asset_container c, uint32_t tpk_node, uint32_t pack_resource, const nue_asset_resource_ops* ops, void* builder);
nue_result car_load_texture(nue_asset_container c, uint32_t tpk_node, uint32_t sub_index, TexRec* x, std::vector<uint8_t>* image, std::vector<uint8_t>* palette);

nue_tex_format format_of_ct(uint8_t ct);                      // pixel format of the stored data (P8 -> R8_UNORM); UNKNOWN if not understood
bool ct_known(uint8_t ct);
bool ct_paletted(uint8_t ct);
uint64_t chain_size(uint8_t ct, uint32_t w, uint32_t h, uint32_t mips);

}  // namespace nfs
