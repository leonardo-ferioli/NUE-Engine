/*
 * NUE engine - asset containers: open, walk, extract, convert and pack the resource files of a game.
 *
 * What this module is for. A recompiled game ships its data in container files of its own (NFS Underground 2: .BUN chunk streams and .LZC =
 * JDLZ compressed .BUN). The engine must be able to open them, show what is inside, convert the contents (first target: textures, through the
 * texture module, include/nue/texture.h) and write NEW resources: the engine's own .nuepack (memory mappable, per platform texture variants,
 * docs/NUEPACK.md) or the original game format again (a changed .BUN/.LZC the PC game can load).
 *
 * Layers (docs/ASSET_FORMATS.md has the byte layouts, docs/NUEPACK.md the .nuepack format):
 *   1. sources      readable byte ranges: memory, or a file of the engine VFS (include/nue/storage.h) read with positioned reads
 *   2. codecs       byte stream compressors (name, identify, decompress, compress). JDLZ is built in; games can register more
 *   3. formats      plugins: probe a source, report its node tree (chunks) and its resources (textures...), serialise it again.
 *                   NFS BUN/LZC is the first plugin and uses only this public interface
 *   4. containers   what the application holds: a node tree over a source, a list of resources, pending edits, writers
 *   5. nuepack      the engine's resource package (writer + reader) and its VFS style mount with asynchronous reads
 *
 * Rules that hold for every function here
 *   - No global state: everything hangs from a nue_assets_context (the registry). Several contexts can live side by side.
 *   - Memory comes from the engine allocator (nue_alloc) and is accounted in the "assets" memory budget (include/nue/memory.h): there is no
 *     built-in size limit; a request that does not fit the budget fails with NUE_ERR_OUT_OF_MEMORY instead of crashing.
 *   - Files are little-endian on disk whatever the host: every read and write goes through explicit byte accessors (no struct casts).
 *   - Output is deterministic: the same input and options give the same bytes (no timestamps, no pointers, no thread dependent order).
 *   - Thread: a nue_assets_context and the codec / format registries are [any] once registration is finished. A container is [one thread at
 *     a time]; reading payloads of an opened container with nue_asset_node_read is [any] when no edit is made at the same time.
 *   - Every struct passed across the ABI starts with `uint32_t struct_size` (NUE_STRUCT_INIT), as everywhere in the engine.
 *   - Fallible functions return nue_result (base.h); details through nue_get_last_error_message().
 */
#ifndef NUE_ASSETS_H
#define NUE_ASSETS_H

#include "nue/base.h"
#include "nue/memory.h"
#include "nue/storage.h"
#include "nue/texture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The module is a separate static (or shared) library: it has its own export macro, independent of the libnue one. */
#if defined(NUE_ASSETS_BUILD_SHARED) && defined(_WIN32)
#  define NUE_ASSETS_API __declspec(dllexport)
#elif defined(NUE_ASSETS_USE_SHARED) && defined(_WIN32)
#  define NUE_ASSETS_API __declspec(dllimport)
#elif defined(NUE_ASSETS_BUILD_SHARED) || defined(NUE_ASSETS_USE_SHARED)
#  define NUE_ASSETS_API __attribute__((visibility("default")))
#else
#  define NUE_ASSETS_API
#endif

#define NUE_ASSETS_API_VERSION 1u
#define NUE_ASSET_NO_NODE      0xFFFFFFFFu

NUE_DEFINE_HANDLE(nue_assets_context);
NUE_DEFINE_HANDLE(nue_asset_container);
struct nue_asset_source;                    /* opaque readable byte range */

/* ============================================================================================== sources */

/* A byte range that can be read at any offset. Plugins receive one in nue_asset_format.open; applications build them with the
 * constructors below. Reading past the end returns the bytes that exist (*out_read smaller, NUE_OK_PARTIAL). [any] */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_source_read(const struct nue_asset_source* source, uint64_t offset, void* buffer, size_t size, size_t* out_read);
NUE_ASSETS_API uint64_t NUE_CALL nue_asset_source_size(const struct nue_asset_source* source);
/* Direct pointer to a byte range when the source is in memory (or the file is memory mapped): NULL when it is not available (then use read). */
NUE_ASSETS_API const void* NUE_CALL nue_asset_source_peek(const struct nue_asset_source* source, uint64_t offset, size_t size);

/* ============================================================================================== codecs */

typedef struct nue_asset_compress_params {
    uint32_t struct_size;
    int32_t level;                          /* 0 = fastest ... 9 = smallest; default 6. Same level and input -> same output */
    uint32_t flags;
    uint32_t reserved[5];
} nue_asset_compress_params;

typedef struct nue_asset_codec {
    uint32_t struct_size;
    const char* name;                       /* unique, lower case: "jdlz" */
    void* userdata;
    /* NUE_OK when `data` starts with a stream of this codec; out_uncompressed = size after decoding, out_stream = size of the whole stream
     * (header included) when the header says it. NUE_ERR_UNSUPPORTED when it is not this codec. Needs at most 64 bytes. */
    nue_result (NUE_CALL *identify)(void* userdata, const void* data, size_t size, uint64_t* out_uncompressed, uint64_t* out_stream);
    /* Decode a whole stream into dst (dst_size >= the identified uncompressed size). NUE_ERR_CORRUPT on malformed data; never reads or writes
     * out of bounds. */
    nue_result (NUE_CALL *decompress)(void* userdata, const void* src, size_t src_size, void* dst, size_t dst_size, size_t* out_written);
    /* NULL when the codec cannot compress. dst_capacity >= compress_bound(src_size). */
    nue_result (NUE_CALL *compress)(void* userdata, const void* src, size_t src_size, const nue_asset_compress_params* params,
                                     void* dst, size_t dst_capacity, size_t* out_written);
    size_t (NUE_CALL *compress_bound)(void* userdata, size_t src_size);
} nue_asset_codec;

/* JDLZ (the LZ77 variant of EA's Need for Speed games, "JDLZ" streams): usable without a context. */
NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_identify(const void* data, size_t size, uint32_t* out_uncompressed, uint32_t* out_stream);
NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_decompress(const void* src, size_t src_size, void* dst, size_t dst_size, size_t* out_written);
NUE_ASSETS_API size_t NUE_CALL nue_jdlz_compress_bound(size_t src_size);
NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_compress(const void* src, size_t src_size, int32_t level, void* dst, size_t dst_capacity, size_t* out_written);

/* HUFF (EA's order-0 Huffman with a run/literal escape; used by some textures of the NFSU2 car packs). Decompression only. The stream size reported by
 * identify includes the 16 byte header. */
NUE_ASSETS_API nue_result NUE_CALL nue_huff_identify(const void* data, size_t size, uint32_t* out_uncompressed, uint32_t* out_stream);
NUE_ASSETS_API nue_result NUE_CALL nue_huff_decompress(const void* src, size_t src_size, void* dst, size_t dst_size, size_t* out_written);

/* ============================================================================================== node tree (what plugins report) */

typedef enum nue_asset_node_kind {
    NUE_ASSET_NODE_BLOB = 0,                /* opaque bytes: kept as they are */
    NUE_ASSET_NODE_GROUP,                   /* has children */
    NUE_ASSET_NODE_PADDING,                 /* alignment filler: recomputed when the layout changes */
    NUE_ASSET_NODE_TAIL,                    /* bytes after the last node that no node covers */
    NUE_ASSET_NODE_DERIVED                  /* payload is generated by the format writer from resources (e.g. texture pixel data) */
} nue_asset_node_kind;

enum {                                      /* nue_asset_node_info.flags */
    NUE_ASSET_NODE_OPAQUE   = 1u << 0,      /* looked like a group but its content did not parse: kept as one blob */
    NUE_ASSET_NODE_EDITED   = 1u << 1,      /* payload replaced by the application (query only) */
    NUE_ASSET_NODE_HAS_NAME = 1u << 2
};

typedef struct nue_asset_node_info {
    uint32_t struct_size;
    uint32_t index;                         /* position in the pre-order list, assigned by the container */
    uint32_t parent;                        /* NUE_ASSET_NO_NODE for the roots */
    uint32_t first_child, next_sibling;     /* NUE_ASSET_NO_NODE when none (set by the container) */
    uint32_t depth;
    uint32_t kind;                          /* nue_asset_node_kind */
    uint32_t flags;
    uint32_t header_size;                   /* bytes of the node's own header in front of the payload (NFS: 8) */
    uint32_t align;                         /* PADDING: alignment (power of two) the following node's start keeps; 0 otherwise */
    uint32_t reserved;
    uint64_t id;                            /* format specific type id (NFS: the 32 bit chunk id) */
    uint64_t offset;                        /* payload offset in the container's source */
    uint64_t size;                          /* payload size (current: the replaced size when edited) */
    const char* name;                       /* NULL when the format has none; valid while the container lives */
} nue_asset_node_info;

/* ============================================================================================== resources (what a format means by its nodes) */

typedef enum nue_asset_resource_kind {
    NUE_ASSET_RES_BLOB = 0,
    NUE_ASSET_RES_TEXTURE,                  /* has an image: nue_asset_resource_get_image */
    NUE_ASSET_RES_TEXTURE_PACK,             /* group of textures (NFS: a TPK) */
    NUE_ASSET_RES_OTHER,                    /* anything the plugin wants to list (sound...) */
    NUE_ASSET_RES_MESH                      /* has a mesh: nue_asset_resource_get_mesh. In the resource info: width = vertices, height = triangles */
} nue_asset_resource_kind;

typedef struct nue_asset_resource_info {
    uint32_t struct_size;
    uint32_t index;
    uint32_t kind;                          /* nue_asset_resource_kind */
    uint32_t node;                          /* owner node (NUE_ASSET_NO_NODE if none) */
    uint32_t parent;                        /* resource index of the pack it belongs to, NUE_ASSET_NO_NODE if none */
    uint32_t flags;                         /* NUE_ASSET_RESFLAG_* */
    uint64_t hash;                          /* format specific name hash / key */
    const char* name;                       /* may be empty */
    uint32_t format;                        /* nue_tex_format of the stored pixels (textures), else 0 */
    uint32_t width, height, depth, mip_levels;
    uint32_t native_format;                 /* the game's own code (NFS: the compression byte); informational */
    uint32_t sub_index;                     /* position inside the owner (NFS: texture number inside its pack) */
    uint32_t derived_node;                  /* TEXTURE_PACK: node whose payload the writer regenerates from the textures (NUE_ASSET_NO_NODE if none) */
    uint32_t reserved;
    uint64_t data_size;                     /* bytes of the stored pixel data (textures) or payload (blobs) */
} nue_asset_resource_info;

enum {                                      /* nue_asset_resource_info.flags */
    NUE_ASSET_RESFLAG_PALETTED = 1u << 0,   /* stored as indices + palette; get_image expands to BGRA8 */
    NUE_ASSET_RESFLAG_EDITED   = 1u << 1,
    NUE_ASSET_RESFLAG_READONLY = 1u << 2    /* can be read and extracted but not replaced (the writer does not rebuild this layout): set_image / set_native refuse */
};

/* A triangle mesh in plain arrays (what a model exporter or a renderer wants). Positions are in the game's own units and axes (NFS: Z up). */
typedef struct nue_asset_mesh_run {
    uint32_t index_offset, index_count;     /* range of `indices` drawn with this material (a multiple of 3) */
    uint64_t material_hash;                 /* the game's material / texture hash for the run (0 = none) */
    uint64_t shader_hash;
} nue_asset_mesh_run;

enum { NUE_ASSET_MESH_NORMALS = 1u << 0, NUE_ASSET_MESH_UVS = 1u << 1, NUE_ASSET_MESH_COLORS = 1u << 2 };

typedef struct nue_asset_mesh {
    uint32_t struct_size;
    const char* name;
    uint32_t flags;                         /* NUE_ASSET_MESH_*: which optional arrays exist */
    uint32_t vertex_count, index_count;     /* triangle list, three indices per triangle */
    const float* positions;                 /* 3 floats per vertex */
    const float* normals;                   /* 3 per vertex or NULL */
    const float* uvs;                       /* 2 per vertex or NULL (origin top left, as Direct3D) */
    const uint8_t* colors;                  /* 4 bytes RGBA per vertex or NULL */
    const uint32_t* indices;
    uint32_t run_count;
    const nue_asset_mesh_run* runs;         /* material runs tiling `indices` */
    float transform[16];                    /* local matrix as stored (row major); identity when the file has none */
    float bbox_min[3], bbox_max[3];
} nue_asset_mesh;

/* Texture pixels the way the game file stores them (for lossless transfer, e.g. into a .nuepack and back). */
typedef struct nue_asset_native_texture {
    uint32_t struct_size;
    uint32_t native_format;                 /* the game's own code (NFS: the compression byte 0x22 = DXT1...) */
    uint32_t tex_format;                    /* nue_tex_format of the stored pixels (palette indices: NUE_TEX_R8_UNORM) */
    uint32_t width, height, depth, mip_levels;
    uint32_t flags;                         /* NUE_ASSET_RESFLAG_PALETTED: `palette_size` bytes (256 x B,G,R,A) follow the pixel levels */
    uint32_t palette_size;
    const void* data;                       /* all mip levels, level 0 first, then the palette */
    uint64_t size;
} nue_asset_native_texture;

/* ============================================================================================== format plugins */

struct nue_asset_open_ctx;
struct nue_asset_write_ctx;

typedef struct nue_asset_open_ctx_ops {
    uint32_t struct_size;
    /* Append a node (pre-order: a parent is added before its children; parent = NUE_ASSET_NO_NODE for roots). The container fills index, depth and the
     * links. Returns the node index in *out_index. */
    nue_result (NUE_CALL *add_node)(struct nue_asset_open_ctx* ctx, const nue_asset_node_info* node, uint32_t* out_index);
    /* A node added earlier as a GROUP whose content did not parse becomes an opaque BLOB (flag OPAQUE) and its children are dropped. Only valid while no
     * node was added after its subtree. */
    nue_result (NUE_CALL *truncate_children)(struct nue_asset_open_ctx* ctx, uint32_t node);
    void (NUE_CALL *set_state)(struct nue_asset_open_ctx* ctx, void* state);
    void* (NUE_CALL *get_state)(const struct nue_asset_open_ctx* ctx);
} nue_asset_open_ctx_ops;

typedef struct nue_asset_open_ctx {
    uint32_t struct_size;
    const nue_asset_open_ctx_ops* ops;
    nue_asset_container container;          /* the container being built (node queries work for the nodes added so far) */
    const struct nue_asset_source* source;
    uint32_t flags;                         /* NUE_ASSET_OPEN_* */
    void* internal;
} nue_asset_open_ctx;

typedef struct nue_asset_resource_ops {
    uint32_t struct_size;
    nue_result (NUE_CALL *add_resource)(void* builder, const nue_asset_resource_info* resource, uint32_t* out_index);
} nue_asset_resource_ops;

typedef struct nue_asset_sink nue_asset_sink;       /* where a writer puts bytes (opaque) */
/* Append bytes at the current end of the output. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_sink_write(nue_asset_sink* sink, const void* data, size_t size);
/* Current length of the output (the offset the next byte lands at). */
NUE_ASSETS_API uint64_t NUE_CALL nue_asset_sink_position(const nue_asset_sink* sink);
/* Overwrite bytes already written (offsets, sizes patched after the fact). The range must lie inside what was written. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_sink_patch(nue_asset_sink* sink, uint64_t offset, const void* data, size_t size);

typedef struct nue_asset_format {
    uint32_t struct_size;
    const char* name;                       /* unique, lower case: "nfs-bun" */
    const char* description;
    const char* extensions;                 /* ";bun;lzc" separated list, hints only (probe decides) */
    void* userdata;
    /* Cheap check on the first bytes: 0 = not mine, 1..100 = confidence. head holds up to 4096 bytes (fewer when the file is shorter). */
    int32_t (NUE_CALL *probe)(void* userdata, const void* head, size_t head_size, uint64_t total_size);
    /* Walk the source and add the nodes (see nue_asset_open_ctx_ops). NUE_ERR_CORRUPT when it is not a valid file of this format. */
    nue_result (NUE_CALL *open)(void* userdata, nue_asset_open_ctx* ctx);
    /* Free what set_state stored. */
    void (NUE_CALL *close)(void* userdata, void* state);
    /* Build the resource list from the nodes (called after open, and after a container was assembled from nodes elsewhere, e.g. from a nuepack). */
    nue_result (NUE_CALL *index)(void* userdata, nue_asset_container container, void* state, const nue_asset_resource_ops* ops, void* builder);
    /* Pixels of a texture resource: a new image (nue_tex_image_destroy it). NULL when the format has no textures. */
    nue_result (NUE_CALL *get_image)(void* userdata, nue_asset_container container, void* state, uint32_t resource, nue_tex_image* out_image);
    /* The stored pixels of a texture resource in the game's own encoding. NULL when the format cannot (get_image still works). */
    nue_result (NUE_CALL *get_native)(void* userdata, nue_asset_container container, void* state, uint32_t resource, nue_asset_native_texture* out, void** out_memory);
    /* A mesh resource as arrays. *out_memory owns every pointer inside *out (nue_alloc; free with nue_free). NULL when the format has no meshes. */
    nue_result (NUE_CALL *get_mesh)(void* userdata, nue_asset_container container, void* state, uint32_t resource, nue_asset_mesh* out, void** out_memory);
    /* Serialise the whole container (current nodes + edits) into the sink, rebuilding every offset, size and padding that depends on them. */
    nue_result (NUE_CALL *write)(void* userdata, nue_asset_container container, void* state, uint32_t flags, nue_asset_sink* sink);
    /* Codec the format normally travels in (by file extension, e.g. ".lzc" -> "jdlz"); NULL if none. Used when writing a container that was not read from a
     * compressed file. */
    const char* (NUE_CALL *default_codec)(void* userdata, const char* extension_with_dot);
} nue_asset_format;

/* ============================================================================================== context (registry) */

typedef struct nue_assets_context_desc {
    uint32_t struct_size;
    uint32_t flags;                         /* NUE_ASSETS_CTX_* */
    const char* budget_name;                /* memory budget name; default "assets" */
    nue_budget_policy budget_policy;        /* all zero = the engine default for NUE_BUDGET_IMPORT_STAGING */
    uint32_t reserved[4];
} nue_assets_context_desc;

enum {
    NUE_ASSETS_CTX_NO_BUILTINS = 1u << 0,   /* start with empty registries: nothing is registered (not even jdlz and nfs-bun) */
    NUE_ASSETS_CTX_NO_BUDGET   = 1u << 1    /* do not use the engine memory budget (allocation is only limited by the machine) */
};

/* desc may be NULL (defaults, built-ins registered). With the engine not initialised the budget is simply not used. */
NUE_ASSETS_API nue_result NUE_CALL nue_assets_context_create(const nue_assets_context_desc* desc, nue_assets_context* out);
NUE_ASSETS_API void NUE_CALL nue_assets_context_destroy(nue_assets_context ctx);       /* every container made from it must be closed first */
NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_codec(nue_assets_context ctx, const nue_asset_codec* codec);     /* copied; NUE_ERR_ALREADY_EXISTS if the name is taken */
NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_format(nue_assets_context ctx, const nue_asset_format* format);
NUE_ASSETS_API uint32_t NUE_CALL nue_assets_codec_count(nue_assets_context ctx);
NUE_ASSETS_API const char* NUE_CALL nue_assets_codec_name(nue_assets_context ctx, uint32_t index);
NUE_ASSETS_API uint32_t NUE_CALL nue_assets_format_count(nue_assets_context ctx);
NUE_ASSETS_API const char* NUE_CALL nue_assets_format_name(nue_assets_context ctx, uint32_t index);
/* Run a codec of the context by name on memory. compress allocates *out with nue_alloc (free with nue_free). */
NUE_ASSETS_API nue_result NUE_CALL nue_assets_codec_encode(nue_assets_context ctx, const char* codec, const void* src, size_t size, int32_t level, void** out, size_t* out_size);
NUE_ASSETS_API nue_result NUE_CALL nue_assets_codec_decode(nue_assets_context ctx, const char* codec, const void* src, size_t size, void** out, size_t* out_size);

/* ============================================================================================== containers */

typedef uint32_t nue_asset_open_flags;
enum {
    NUE_ASSET_OPEN_LENIENT   = 1u << 0,     /* a damaged tail or an unparsable group becomes an opaque blob instead of failing */
    NUE_ASSET_OPEN_NO_UNWRAP = 1u << 1,     /* do not look for a codec wrapper: open the bytes as they are */
    NUE_ASSET_OPEN_LOAD_ALL  = 1u << 2,     /* read the whole file into memory at once (faster for many small reads; the default for compressed files) */
    NUE_ASSET_OPEN_FORMAT    = 1u << 3      /* use `format_name` instead of probing */
};

typedef struct nue_asset_open_desc {
    uint32_t struct_size;
    nue_asset_open_flags flags;
    const char* path;                       /* VFS path ("data:GLOBAL/GLOBALA.BUN") or native path; or NULL with memory */
    const void* memory;                     /* with size: open from memory (the caller keeps it alive unless NUE_ASSET_OPEN_COPY) */
    size_t memory_size;
    const char* name_hint;                  /* extension hint when opening from memory (".bun") */
    const char* format_name;                /* with NUE_ASSET_OPEN_FORMAT */
    uint32_t reserved[4];
} nue_asset_open_desc;

#define NUE_ASSET_OPEN_COPY (1u << 4)       /* memory: copy the bytes into container owned memory */

NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_open(nue_assets_context ctx, const nue_asset_open_desc* desc, nue_asset_container* out);
NUE_ASSETS_API void NUE_CALL nue_asset_container_close(nue_asset_container container);

typedef struct nue_asset_container_info {
    uint32_t struct_size;
    const char* format;                     /* plugin name */
    const char* codec;                      /* wrapper codec name ("jdlz") or NULL */
    uint32_t node_count, resource_count;
    uint64_t file_size;                     /* bytes on disk / in the given memory */
    uint64_t content_size;                  /* bytes of the decoded content the nodes refer to (== file_size without a codec) */
    nue_bool dirty;                         /* an edit is pending: writing rebuilds the file */
    nue_bool lenient_repairs;               /* the container opened with NUE_ASSET_OPEN_LENIENT and something had to be kept opaque */
} nue_asset_container_info;
NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_get_info(nue_asset_container container, nue_asset_container_info* out);

/* ---- node tree ---- */
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_node_count(nue_asset_container container);
NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_get(nue_asset_container container, uint32_t index, nue_asset_node_info* out);
/* Copy payload bytes [offset, offset+size) of a node (edits included). *out_read may be smaller at the end of the payload. [any] */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_read(nue_asset_container container, uint32_t index, uint64_t offset, void* buffer, size_t size, size_t* out_read);
/* Replace the payload of a node (any kind but GROUP); the bytes are copied. A format's writer fixes everything that depends on the size. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_set_payload(nue_asset_container container, uint32_t index, const void* data, size_t size);
/* First node of the given id at or after `start` (pre-order), NUE_ASSET_NO_NODE if none. */
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_node_find(nue_asset_container container, uint64_t id, uint32_t start);
/* Direct pointer to the current payload bytes when they are in memory (edit buffer or in-memory source); NULL otherwise. */
NUE_ASSETS_API const void* NUE_CALL nue_asset_node_peek(nue_asset_container container, uint32_t index, uint64_t offset, size_t size);

/* ---- resources ---- */
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_resource_count(nue_asset_container container);
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get(nue_asset_container container, uint32_t index, nue_asset_resource_info* out);
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_resource_find(nue_asset_container container, const char* name_or_null, uint64_t hash_or_zero);   /* case-insensitive name; NUE_ASSET_NO_NODE if none */
/* The pixels of a texture resource (the edited image when an edit is pending). Destroy with nue_tex_image_destroy. Palettised textures come back as BGRA8. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get_image(nue_asset_container container, uint32_t index, nue_tex_image* out_image);
/* Replace a texture: the image is cloned. Any format the texture module knows; the writer converts it to what the target format stores. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_set_image(nue_asset_container container, uint32_t index, nue_tex_image image);
/* The pending edit of a resource (NULL when none): for format writers. Owned by the container. */
NUE_ASSETS_API nue_tex_image NUE_CALL nue_asset_resource_edit(nue_asset_container container, uint32_t index);

/* The geometry of a mesh resource. Free *out_memory with nue_free when done. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get_mesh(nue_asset_container container, uint32_t index, nue_asset_mesh* out, void** out_memory);

/* ---- native texture data (type declared above) ---- */
/* *out_memory receives the buffer that out->data points into (nue_alloc; free with nue_free). */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get_native(nue_asset_container container, uint32_t index, nue_asset_native_texture* out, void** out_memory);
/* Replace a texture with data in the game's own encoding (copied). Clears an image edit of the same resource. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_set_native(nue_asset_container container, uint32_t index, const nue_asset_native_texture* native);
/* The pending native edit (NULL when none): for format writers. Owned by the container. */
NUE_ASSETS_API const nue_asset_native_texture* NUE_CALL nue_asset_resource_native_edit(nue_asset_container container, uint32_t index);

/* ---- writing ---- */
typedef uint32_t nue_asset_write_flags;
enum {
    NUE_ASSET_WRITE_REBUILD  = 1u << 0,     /* always serialise from the node tree, even when nothing was edited (the default reuses the original bytes) */
    NUE_ASSET_WRITE_NO_CODEC = 1u << 1,     /* write the plain content without the wrapper codec (.LZC -> .BUN) */
    NUE_ASSET_WRITE_FORCE_CODEC = 1u << 2,  /* wrap in the codec even when the file was not compressed; the codec name goes in write_desc.codec */
    NUE_ASSET_WRITE_RECOMPRESS = 1u << 3,   /* with a codec: compress again even if the content is unchanged (default keeps the original compressed bytes) */
    NUE_ASSET_WRITE_REGENERATE = 1u << 4    /* with REBUILD: also regenerate derived data (texture pack tables and pixel layout) from the resources, even when nothing was edited:
                                             * a verification mode, the result must equal the original */
};

typedef struct nue_asset_write_desc {
    uint32_t struct_size;
    nue_asset_write_flags flags;
    const char* path;                       /* VFS or native path (atomic: temp file + rename) */
    void** out_data;                        /* or: memory result, allocated with nue_alloc (free with nue_free) */
    size_t* out_size;
    const char* codec;                      /* with FORCE_CODEC; NULL = the format's default for `path`'s extension */
    int32_t codec_level;                    /* 0 = default */
    uint32_t reserved[4];
} nue_asset_write_desc;
NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_write(nue_asset_container container, const nue_asset_write_desc* desc);
/* The container that is being written: the writer needs the pixels of edited and unedited resources alike. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_content_hash(nue_asset_container container, uint8_t out_sha256[32]);   /* of the current serialised content */

/* ============================================================================================== texture helpers */

/* Dump the texture of a resource as DDS / PNG / KTX2 (kind = NUE_TEX_FILE_*). The image is converted to RGBA8 for PNG. */
NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_save_texture(nue_asset_container container, uint32_t index, const char* path, nue_tex_file_kind kind);

/* ============================================================================================== model export */

typedef enum nue_asset_model_format { NUE_ASSET_MODEL_GLB = 1, NUE_ASSET_MODEL_OBJ } nue_asset_model_format;

enum {                                      /* nue_asset_model_desc.flags */
    NUE_ASSET_MODEL_NO_TEXTURES = 1u << 0,  /* geometry only */
    NUE_ASSET_MODEL_KEEP_Z_UP   = 1u << 1,  /* do not turn the game's Z up axes into Y up (GLB: a rotation on the root node; OBJ: swapped axes) */
    NUE_ASSET_MODEL_BEST_LOD    = 1u << 2   /* parts named ..._A, ..._B ...: keep only the most detailed of each */
};

typedef struct nue_asset_model_desc {
    uint32_t struct_size;
    const nue_asset_container* containers;  /* the meshes of all of them are exported; textures are looked up (by hash) in all of them */
    uint32_t container_count;
    uint32_t format;                        /* nue_asset_model_format */
    uint32_t flags;
    const char* name_filter;                /* only meshes whose name contains this text (case-insensitive); NULL = all */
    uint32_t max_meshes;                    /* 0 = no limit */
    const char* path;                       /* output: .glb (one file) or .obj (+ .mtl and a folder of PNG textures next to it) */
    uint32_t reserved[4];
} nue_asset_model_desc;

typedef struct nue_asset_model_stats {
    uint32_t struct_size;
    uint32_t meshes, textures, meshes_skipped;      /* skipped: could not be read (reported through the last error message) */
    uint64_t vertices, triangles, bytes_written;
} nue_asset_model_stats;

NUE_ASSETS_API nue_result NUE_CALL nue_asset_export_model(const nue_asset_model_desc* desc, nue_asset_model_stats* out_stats);

/* ============================================================================================== .nuepack (docs/NUEPACK.md) */

NUE_DEFINE_HANDLE(nue_pack);                /* an opened package (read) */
NUE_DEFINE_HANDLE(nue_pack_builder);        /* a package being built (write) */

#define NUE_PACK_MAGIC 0x5045554Eu          /* "NUEP" little endian */
#define NUE_PACK_VERSION_MAJOR 1u
#define NUE_PACK_VERSION_MINOR 0u

/* Variants: one entry per resource per platform class. The "source" variant keeps the pixels the way the game file had them, so a pack
 * can be exported back (nue_pack_export_container). */
typedef enum nue_pack_variant {
    NUE_PACK_VARIANT_SOURCE = 0,            /* as in the original file (DXT, BGRA8, P8 -> indices + palette) */
    NUE_PACK_VARIANT_BC,                    /* desktop: BC1..BC7 (or what the source already is) */
    NUE_PACK_VARIANT_ASTC,                  /* Android/Apple ASTC LDR */
    NUE_PACK_VARIANT_ETC2,                  /* Android ETC2 */
    NUE_PACK_VARIANT_PVRTC,                 /* old Apple GPUs */
    NUE_PACK_VARIANT_RGBA8,                 /* universal fallback */
    NUE_PACK_VARIANT_COUNT
} nue_pack_variant;
NUE_ASSETS_API const char* NUE_CALL nue_pack_variant_name(nue_pack_variant variant);                 /* "source", "bc", "astc", "etc2", "pvrtc", "rgba8" */
NUE_ASSETS_API nue_pack_variant NUE_CALL nue_pack_variant_from_name(const char* name);               /* COUNT if unknown */
NUE_ASSETS_API nue_pack_variant NUE_CALL nue_pack_variant_for_device_class(nue_tex_device_class device_class);
/* The variant a device can use as it is: the first of bc, astc, etc2, pvrtc whose representative formats (BC1/BC3, ASTC 4x4, ETC2 RGBA8, PVRTC1 4bpp) the
 * capabilities mark as sampled and filtered, else rgba8. caps = what the GPU probe reported (nue_tex_caps_from_gpu_caps / nue_tex_context_get_caps) or a class
 * preset (nue_tex_caps_for_class). */
NUE_ASSETS_API nue_pack_variant NUE_CALL nue_pack_variant_for_caps(const nue_tex_caps* caps);

typedef struct nue_pack_builder_desc {
    uint32_t struct_size;
    uint32_t flags;
    const char* source_name;                /* free text stored in the header ("GLOBAL/GlobalB.lzc") */
    uint32_t reserved[4];
} nue_pack_builder_desc;

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_create(nue_assets_context ctx, const nue_pack_builder_desc* desc, nue_pack_builder* out);
NUE_ASSETS_API void NUE_CALL nue_pack_builder_destroy(nue_pack_builder builder);

typedef struct nue_pack_convert_options {
    uint32_t struct_size;
    uint32_t variants;                      /* bit mask of 1 << nue_pack_variant; 0 = SOURCE only. SOURCE is always written */
    nue_tex_quality quality;                /* encoder quality for the transcoded variants */
    uint32_t flags;                         /* NUE_PACK_CONVERT_* */
    uint32_t max_dimension;                 /* drop top mips of larger textures in the non-source variants (0 = keep) */
    nue_tex_executor executor;              /* parallel encoding; NULL = the calling thread */
    uint32_t reserved[4];
} nue_pack_convert_options;
enum {
    NUE_PACK_CONVERT_NO_COMPRESS = 1u << 0, /* store the chunks raw (default: JDLZ for blobs larger than 256 bytes when it makes them smaller) */
    NUE_PACK_CONVERT_KEEP_TREE   = 1u << 1  /* default on for NFS containers: keep the node tree so the pack can be exported back (set automatically) */
};

/* Add everything of an opened container: the node tree (opaque blobs, padding as markers), and every texture in the SOURCE variant plus the
 * variants asked for. Returns NUE_OK_PARTIAL when some texture could not be transcoded (it is stored as RGBA8 in that variant). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_container(nue_pack_builder builder, nue_asset_container container, const nue_pack_convert_options* options);
/* Add one loose file (stored as a blob under `name`). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_blob(nue_pack_builder builder, const char* name, const void* data, size_t size, uint32_t flags);
/* Add one loose texture (DDS/PNG/KTX2 bytes or an image) with its variants. */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_image(nue_pack_builder builder, const char* name, nue_tex_image image, const nue_pack_convert_options* options);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_write(nue_pack_builder builder, const char* path_or_null, void** out_data, size_t* out_size);

typedef struct nue_pack_info {
    uint32_t struct_size;
    uint32_t version_major, version_minor;
    uint32_t entry_count, tree_node_count;
    uint64_t total_size;
    uint32_t variant_mask;
    const char* source_name;
    nue_bool has_tree;                      /* exportable back to a game container */
    nue_bool verified_header;
    char format_name[32];                   /* the game format the tree came from ("nfs-bun") */
    char codec_name[16];                    /* the codec the original file used ("jdlz") or "" */
} nue_pack_info;

typedef uint32_t nue_pack_open_flags;
enum { NUE_PACK_OPEN_VERIFY = 1u << 0 /* check every entry hash while opening */, NUE_PACK_OPEN_MAP = 1u << 1 /* memory map the file (default: read it) */ };

/* Open from a VFS / native path, or from memory (the caller keeps it alive). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_open_file(nue_assets_context ctx, const char* path, nue_pack_open_flags flags, nue_pack* out);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_open_memory(nue_assets_context ctx, const void* data, size_t size, nue_pack_open_flags flags, nue_pack* out);
NUE_ASSETS_API void NUE_CALL nue_pack_close(nue_pack pack);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_get_info(nue_pack pack, nue_pack_info* out);

typedef struct nue_pack_entry_info {
    uint32_t struct_size;
    uint32_t index;
    uint32_t kind;                          /* nue_asset_resource_kind */
    uint32_t variant;                       /* nue_pack_variant */
    uint32_t flags;
    uint32_t tex_format;                    /* nue_tex_format (textures) */
    uint32_t width, height, depth, mip_levels, layers, faces;
    uint64_t name_hash;                     /* FNV-1a 64 of the lower case name */
    const char* name;
    uint64_t stored_size, raw_size;         /* bytes in the pack / after decompression */
    uint64_t offset;                        /* in the pack file */
    uint32_t codec;                         /* 0 none, 1 jdlz */
    uint32_t crc32;
    uint8_t sha256[32];                     /* of the raw bytes */
} nue_pack_entry_info;

enum { NUE_PACK_ENTRY_PALETTED = 1u << 0, NUE_PACK_ENTRY_TREE_BLOB = 1u << 1 };

NUE_ASSETS_API uint32_t NUE_CALL nue_pack_entry_count(nue_pack pack);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_get(nue_pack pack, uint32_t index, nue_pack_entry_info* out);
/* Entry of a resource by name and variant; if the variant is missing the first available fallback is returned (SOURCE last). NUE_ASSET_NO_NODE if none. */
NUE_ASSETS_API uint32_t NUE_CALL nue_pack_entry_find(nue_pack pack, const char* name, nue_pack_variant preferred);
/* Raw bytes of an entry. Uncompressed entries are returned as a pointer into the pack (no copy: *out_owned = 0); compressed ones are decoded into a
 * new nue_alloc buffer (*out_owned = 1, free with nue_free). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_data(nue_pack pack, uint32_t index, const void** out_data, size_t* out_size, nue_bool* out_owned);
/* Texture entry as an image (a copy of the level data). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_image(nue_pack pack, uint32_t index, nue_tex_image* out_image);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_verify(nue_pack pack, uint32_t* out_bad_entries);

/* Rebuild an NFS style container from a pack that kept its tree: the result is an ordinary container (write it with nue_asset_container_write,
 * to .BUN or, with the pack's codec, to .LZC). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_export_container(nue_pack pack, nue_asset_container* out);

/* ---- VFS style mount: named entries read with asynchronous reads ---- */
NUE_DEFINE_HANDLE(nue_pack_mount);
NUE_DEFINE_HANDLE(nue_pack_file);

/* Mount an opened container (BUN/LZC...) read-only: "node/<index>" is the payload of a node, "<resource name>.dds" a texture resource as a DDS file. The
 * container must outlive the mount and must not be edited while the mount is in use. Plain (not compressed) files are read straight from disk with
 * asynchronous reads. */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create_container(nue_assets_context ctx, nue_asset_container container, nue_pack_mount* out);
/* Mount a pack file for reading by name. `preferred` picks the variant of textures. The pack stays on disk: reads use positioned/asynchronous file reads
 * (nue_file_read_at, nue_io_read_async) when the pack was opened from a path. */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create(nue_assets_context ctx, const char* path, nue_pack_variant preferred, nue_pack_mount* out);
/* The same, choosing the variant from the device capabilities (nue_pack_variant_for_caps). */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create_for_caps(nue_assets_context ctx, const char* path, const nue_tex_caps* caps, nue_pack_mount* out);
NUE_ASSETS_API void NUE_CALL nue_pack_mount_destroy(nue_pack_mount mount);
NUE_ASSETS_API nue_bool NUE_CALL nue_pack_mount_exists(nue_pack_mount mount, const char* name);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_size(nue_pack_mount mount, const char* name, uint64_t* out_size);
/* Open an entry for reading. Compressed entries are decoded once into memory when opened. */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_open(nue_pack_mount mount, const char* name, nue_pack_file* out);
NUE_ASSETS_API void NUE_CALL nue_pack_file_close(nue_pack_file file);
NUE_ASSETS_API uint64_t NUE_CALL nue_pack_file_size(nue_pack_file file);
NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_read_at(nue_pack_file file, uint64_t offset, void* buffer, size_t size, size_t* out_read);
/* Asynchronous read: same contract as nue_io_read_async (include/nue/storage.h); the returned request is polled/waited/released with the nue_io_* functions.
 * For entries kept in memory the read is done before the call returns: the callback has already run and *out_request is 0. */
NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_read_async(nue_pack_file file, uint64_t offset, void* buffer, size_t size, nue_io_callback callback, void* userdata, nue_io_request* out_request);

/* ============================================================================================== formats shipped with the module */

/* Register the NFS plugin (and jdlz) in a context created with NUE_ASSETS_CTX_NO_BUILTINS. */
NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_nfs(nue_assets_context ctx);

NUE_ASSETS_API const char* NUE_CALL nue_assets_version_string(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_ASSETS_H */
