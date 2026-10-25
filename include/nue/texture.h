/*
 * NUE engine - texture formats, device capabilities, transcoding, codecs, container files, cache, import pipeline and memory budget.
 *
 * What this module is for. The recompiled games ship Direct3D 9 textures (DXT1/3/5 = BC1/BC2/BC3, sometimes ATI1/ATI2 = BC4/BC5) that many GPUs
 * cannot sample: Android GPUs mostly sample ETC2/ASTC (BC only on a few), Apple GPUs ASTC and PVRTC (never BC), desktop GPUs BC (not
 * ETC2/ASTC), and everything can fall back to RGBA8. This module understands every common format, tells which ones a device can use
 * (capabilities), decides what to convert a texture to (planner), does the conversion (codecs, multi-threaded), remembers the result on disk
 * (cache), converts everything once at first start (import pipeline) and keeps the memory under a budget (eviction hooks).
 *
 * Layers (docs/TEXTURE.md has the full design and the evidence for every choice):
 *   1. format registry   nue_tex_format + nue_tex_get_format_info + mappings to/from D3D9, DXGI, Vulkan, Metal, OpenGL(ES), bgfx, KTX2, DDS
 *   2. capabilities      nue_tex_caps: which formats a device samples / filters / renders to, limits; presets per platform class
 *   3. planner           nue_tex_make_plan(): source format + policy + caps -> target format, with an explanation log
 *   4. codecs            decode/encode/convert one level or a whole image; mip chains; metrics (PSNR / SSIM / delta E)
 *   5. files             DDS (D3D9 legacy FOURCC, DX10), KTX2 (uncompressed supercompression), PNG (read)
 *   6. cache + import    content-hash keyed on-disk cache; first-run import of all textures; on-demand asynchronous requests
 *   7. budget            texture memory accounting with LRU eviction hooks
 *   8. D3D9 seam         nue_tex_d3d9_*: what the Direct3D 9 layer calls at CreateTexture / UnlockRect / UpdateTexture time
 *
 * The module has no dependency on the rest of the engine (only base.h): it can be linked and tested on its own (library nue_texture).
 * Thread rules: every function is [any] unless stated; a nue_tex_image is not modified concurrently; contexts, executors, caches, budgets
 * and importers are internally synchronised. Callbacks run on the producing thread (worker threads for jobs) and must not call back into
 * the object that invoked them with a call that waits for it.
 */
#ifndef NUE_TEXTURE_H
#define NUE_TEXTURE_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this API and of the codec set. NUE_TEX_CODEC_VERSION is mixed into every cache key: bump it whenever a codec change can alter
 * the bytes it produces, so old cache entries are ignored (they are never wrong, only unused). */
#define NUE_TEX_API_VERSION   1u
#define NUE_TEX_CODEC_VERSION 2u      /* 2: fast 128-bit content hashes replace SHA-256 in cache keys and level index */

/* ============================================================================================== formats */

/* Stable numeric ids (they are written to cache files and may be stored by games). Names say what the bytes are:
 *   - byte-array formats (R8, RGBA8, BGRA8...) list channels in MEMORY order;
 *   - packed formats (RGB565, ARGB4, A2RGB10...) list channels from the MOST significant bit down, in a little-endian 16/32-bit word
 *     (so D3D9 A8R8G8B8 = BGRA8 bytes, D3D9 R5G6B5 = RGB565, D3D9 A4R4G4B4 = ARGB4, D3D9 A2B10G10R10 = A2BGR10).
 * sRGB variants are separate ids; formats without an sRGB twin (A8, L8, floats, BC4...) have none. */
typedef enum nue_tex_format {
    NUE_TEX_FORMAT_UNKNOWN = 0,

    /* uncompressed, byte arrays */
    NUE_TEX_R8_UNORM = 1, NUE_TEX_RG8_UNORM = 2, NUE_TEX_RGB8_UNORM = 3, NUE_TEX_BGR8_UNORM = 4,
    NUE_TEX_RGBA8_UNORM = 5, NUE_TEX_BGRA8_UNORM = 6, NUE_TEX_RGBX8_UNORM = 7, NUE_TEX_BGRX8_UNORM = 8,    /* X = padding byte, alpha reads as 1 */
    NUE_TEX_A8_UNORM = 9, NUE_TEX_L8_UNORM = 10, NUE_TEX_LA8_UNORM = 11,                                    /* LA8 = D3D9 A8L8: bytes L, A */
    NUE_TEX_L16_UNORM = 12, NUE_TEX_LA4_UNORM = 13,                                                         /* LA4 = D3D9 A4L4: A in the high nibble */
    NUE_TEX_R8_SRGB = 14, NUE_TEX_RG8_SRGB = 15, NUE_TEX_RGB8_SRGB = 16, NUE_TEX_BGR8_SRGB = 17,
    NUE_TEX_RGBA8_SRGB = 18, NUE_TEX_BGRA8_SRGB = 19, NUE_TEX_BGRX8_SRGB = 20,
    NUE_TEX_R8_SNORM = 21, NUE_TEX_RG8_SNORM = 22, NUE_TEX_RGBA8_SNORM = 23,                                /* D3D9 V8U8 = RG8_SNORM, Q8W8V8U8 = RGBA8_SNORM */
    NUE_TEX_R16_UNORM = 24, NUE_TEX_RG16_UNORM = 25, NUE_TEX_RGBA16_UNORM = 26,
    NUE_TEX_R16_SNORM = 27, NUE_TEX_RG16_SNORM = 28, NUE_TEX_RGBA16_SNORM = 29,
    NUE_TEX_R16_FLOAT = 30, NUE_TEX_RG16_FLOAT = 31, NUE_TEX_RGBA16_FLOAT = 32,
    NUE_TEX_R32_FLOAT = 33, NUE_TEX_RG32_FLOAT = 34, NUE_TEX_RGB32_FLOAT = 35, NUE_TEX_RGBA32_FLOAT = 36,
    /* uncompressed, packed words */
    NUE_TEX_RGB565 = 40, NUE_TEX_BGR565 = 41, NUE_TEX_RGBA4 = 42, NUE_TEX_BGRA4 = 43, NUE_TEX_ARGB4 = 44, NUE_TEX_XRGB4 = 45,
    NUE_TEX_RGB5A1 = 46, NUE_TEX_BGR5A1 = 47, NUE_TEX_A1RGB5 = 48, NUE_TEX_XRGB1555 = 49, NUE_TEX_RGB332 = 50, NUE_TEX_A8RGB332 = 51,
    NUE_TEX_A2RGB10 = 52, NUE_TEX_A2BGR10 = 53, NUE_TEX_RG11B10_FLOAT = 54, NUE_TEX_RGB9E5_FLOAT = 55,

    /* depth / stencil (registry and mappings only: never decoded or encoded) */
    NUE_TEX_D16_UNORM = 80, NUE_TEX_D24_UNORM_X8 = 81, NUE_TEX_D24_UNORM_S8_UINT = 82, NUE_TEX_D32_FLOAT = 83,
    NUE_TEX_D32_FLOAT_S8_UINT = 84, NUE_TEX_S8_UINT = 85,

    /* BC / S3TC / DXT / RGTC / BPTC. DXT1 = BC1 (alpha possible: 1-bit), DXT2/DXT4 = DXT3/DXT5 with premultiplied alpha, ATI1/BC4U = BC4, ATI2/BC5U = BC5 */
    NUE_TEX_BC1_UNORM = 100, NUE_TEX_BC1_SRGB = 101, NUE_TEX_BC2_UNORM = 102, NUE_TEX_BC2_SRGB = 103, NUE_TEX_BC2_PMA_UNORM = 104,
    NUE_TEX_BC3_UNORM = 105, NUE_TEX_BC3_SRGB = 106, NUE_TEX_BC3_PMA_UNORM = 107,
    NUE_TEX_BC4_UNORM = 108, NUE_TEX_BC4_SNORM = 109, NUE_TEX_BC5_UNORM = 110, NUE_TEX_BC5_SNORM = 111,
    NUE_TEX_BC6H_UFLOAT = 112, NUE_TEX_BC6H_SFLOAT = 113, NUE_TEX_BC7_UNORM = 114, NUE_TEX_BC7_SRGB = 115,

    /* ETC / EAC (ETC1 data is a subset of ETC2 RGB8) */
    NUE_TEX_ETC1_RGB8 = 130, NUE_TEX_ETC2_RGB8 = 131, NUE_TEX_ETC2_RGB8_SRGB = 132, NUE_TEX_ETC2_RGB8A1 = 133, NUE_TEX_ETC2_RGB8A1_SRGB = 134,
    NUE_TEX_ETC2_RGBA8 = 135, NUE_TEX_ETC2_RGBA8_SRGB = 136,
    NUE_TEX_EAC_R11_UNORM = 137, NUE_TEX_EAC_R11_SNORM = 138, NUE_TEX_EAC_RG11_UNORM = 139, NUE_TEX_EAC_RG11_SNORM = 140,

    /* ASTC 2D, 14 block sizes in this order: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12.
     * LDR UNORM = 160 + i, LDR sRGB = 176 + i, HDR (float) = 192 + i. 3D block sizes are not supported. */
    NUE_TEX_ASTC_4x4_UNORM = 160, NUE_TEX_ASTC_12x12_UNORM = 173,
    NUE_TEX_ASTC_4x4_SRGB = 176,  NUE_TEX_ASTC_12x12_SRGB = 189,
    NUE_TEX_ASTC_4x4_HDR = 192,   NUE_TEX_ASTC_12x12_HDR = 205,

    /* PVRTC (1 and 2; "2BPP"/"4BPP" = bits per pixel; PVRTC1 carries optional alpha in the same 64-bit block) */
    NUE_TEX_PVRTC1_2BPP = 224, NUE_TEX_PVRTC1_2BPP_SRGB = 225, NUE_TEX_PVRTC1_4BPP = 226, NUE_TEX_PVRTC1_4BPP_SRGB = 227,
    NUE_TEX_PVRTC2_2BPP = 228, NUE_TEX_PVRTC2_2BPP_SRGB = 229, NUE_TEX_PVRTC2_4BPP = 230, NUE_TEX_PVRTC2_4BPP_SRGB = 231,

    /* AMD / Qualcomm ATC (legacy Adreno/Mali-less phones; decode only) */
    NUE_TEX_ATC_RGB = 240, NUE_TEX_ATC_RGBA_EXPLICIT = 241, NUE_TEX_ATC_RGBA_INTERPOLATED = 242,

    NUE_TEX_FORMAT_COUNT = 256      /* size of tables indexed by nue_tex_format */
} nue_tex_format;

#define NUE_TEX_ASTC_BLOCK_SIZE_COUNT 14
#define NUE_TEX_ASTC_UNORM(i) ((nue_tex_format)(160 + (i)))
#define NUE_TEX_ASTC_SRGB(i)  ((nue_tex_format)(176 + (i)))
#define NUE_TEX_ASTC_HDR(i)   ((nue_tex_format)(192 + (i)))

typedef enum nue_tex_format_kind {
    NUE_TEX_KIND_UNCOMPRESSED = 1,      /* 1x1 blocks, byte array or packed word */
    NUE_TEX_KIND_BLOCK,                 /* block compressed */
    NUE_TEX_KIND_DEPTH                  /* depth and/or stencil */
} nue_tex_format_kind;

typedef enum nue_tex_numeric_type {
    NUE_TEX_TYPE_UNORM = 1, NUE_TEX_TYPE_SNORM, NUE_TEX_TYPE_FLOAT, NUE_TEX_TYPE_UINT, NUE_TEX_TYPE_SRGB
} nue_tex_numeric_type;

typedef enum nue_tex_alpha_mode {
    NUE_TEX_ALPHA_NONE = 0,             /* no alpha channel at all (alpha reads as 1) */
    NUE_TEX_ALPHA_PADDING,              /* the bits exist but are ignored (X8R8G8B8): alpha reads as 1 */
    NUE_TEX_ALPHA_STRAIGHT,             /* normal (non-premultiplied) alpha */
    NUE_TEX_ALPHA_PREMULTIPLIED,        /* colour already multiplied by alpha (DXT2 / DXT4) */
    NUE_TEX_ALPHA_PUNCHTHROUGH          /* 1-bit alpha (BC1 3-colour mode, ETC2 A1, A1R5G5B5) */
} nue_tex_alpha_mode;

typedef enum nue_tex_colorspace { NUE_TEX_COLORSPACE_LINEAR = 0, NUE_TEX_COLORSPACE_SRGB = 1 } nue_tex_colorspace;

enum {                                  /* nue_tex_format_info.flags */
    NUE_TEX_FINFO_COMPRESSED   = 1u << 0,
    NUE_TEX_FINFO_HDR          = 1u << 1,    /* values can exceed [0,1] (float formats, BC6H, ASTC HDR) */
    NUE_TEX_FINFO_SIGNED       = 1u << 2,
    NUE_TEX_FINFO_LOSSY        = 1u << 3,    /* block codecs: encoding is lossy */
    NUE_TEX_FINFO_LEGACY       = 1u << 4,    /* only produced by old content (D3D9 bump maps, ATC, PVRTC2...) */
    NUE_TEX_FINFO_HAS_DECODER  = 1u << 5,    /* nue_tex_decode_level handles it */
    NUE_TEX_FINFO_HAS_ENCODER  = 1u << 6,    /* nue_tex_encode_level handles it */
    NUE_TEX_FINFO_REQUIRES_POT = 1u << 7,    /* sampled only when width and height are powers of two (PVRTC1 on PowerVR) */
    NUE_TEX_FINFO_REQUIRES_SQUARE = 1u << 8
};

typedef struct nue_tex_format_info {
    uint32_t struct_size;
    nue_tex_format format;
    const char* name;                   /* "BC1_UNORM", "ASTC_6x6_SRGB" ... */
    nue_tex_format_kind kind;
    nue_tex_numeric_type type;
    nue_tex_colorspace colorspace;
    nue_tex_alpha_mode alpha;
    uint32_t flags;                     /* NUE_TEX_FINFO_* */
    uint8_t block_width, block_height;  /* texels per block (1,1 for uncompressed) */
    uint8_t block_bytes;                /* bytes per block (bytes per texel for uncompressed) */
    uint8_t min_width, min_height;      /* smallest stored level in texels (PVRTC: 8; everything else: one block) */
    uint8_t channels;                   /* 1..4 colour/alpha channels (depth formats: 1 or 2) */
    uint8_t channel_bits[4];            /* r,g,b,a bits (uncompressed: exact; compressed: nominal) */
    char layout[8];                     /* channel order in memory/word as text: "BGRA", "RGB565" ... */
    float bits_per_pixel;               /* 8 * block_bytes / (block_width * block_height) */
    nue_tex_format srgb_twin;           /* the sRGB variant of a linear format, or the format itself / UNKNOWN */
    nue_tex_format linear_twin;
    uint32_t reserved[4];
} nue_tex_format_info;

/* [any] The registry. NULL for ids that are not formats. The returned pointer is static. */
NUE_API const nue_tex_format_info* NUE_CALL nue_tex_get_format_info(nue_tex_format format);
NUE_API const char* NUE_CALL nue_tex_format_name(nue_tex_format format);                  /* "?" for unknown ids */
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_name(const char* name);               /* case insensitive; UNKNOWN if not found */
NUE_API uint32_t NUE_CALL nue_tex_format_list(nue_tex_format* out, uint32_t capacity);    /* every valid format, ascending id; returns the count */
NUE_API nue_bool NUE_CALL nue_tex_format_is_compressed(nue_tex_format format);
NUE_API nue_tex_format NUE_CALL nue_tex_format_to_srgb(nue_tex_format format);            /* UNKNOWN when there is no sRGB twin */
NUE_API nue_tex_format NUE_CALL nue_tex_format_to_linear(nue_tex_format format);

/* Geometry. Sizes of a level of `width` x `height` x `depth` texels as stored (rounded up to whole blocks, to min_width/min_height).
 * Level data is rows of blocks, row pitch = blocks_x * block_bytes, no padding between rows; volume slices follow one another. */
NUE_API uint32_t NUE_CALL nue_tex_mip_extent(uint32_t base, uint32_t level);              /* max(1, base >> level) */
NUE_API uint32_t NUE_CALL nue_tex_mip_count(uint32_t width, uint32_t height, uint32_t depth);
NUE_API uint32_t NUE_CALL nue_tex_row_pitch(nue_tex_format format, uint32_t width);       /* bytes of one row of blocks; 0 for unknown */
NUE_API uint32_t NUE_CALL nue_tex_rows(nue_tex_format format, uint32_t height);           /* rows of blocks */
NUE_API uint64_t NUE_CALL nue_tex_level_size(nue_tex_format format, uint32_t width, uint32_t height, uint32_t depth);
/* Whole chain: levels 0..mip_levels-1 (mip_levels 0 = full chain) times layers times faces. */
NUE_API uint64_t NUE_CALL nue_tex_chain_size(nue_tex_format format, uint32_t width, uint32_t height, uint32_t depth,
                                             uint32_t mip_levels, uint32_t layers, uint32_t faces);

/* ---------------------------------------------------------------------------------------------- format mappings
 * All mappings return a format/id of 0 / NUE_TEX_FORMAT_UNKNOWN when the other API has no equivalent; `exact` (may be NULL) is set to
 * NUE_FALSE when the result is the closest match rather than a byte-identical one (e.g. D3D9 D24FS8 -> D24S8). */

/* Direct3D 9: D3DFMT value or FOURCC ('DXT1', 'ATI1'...). Colour space is linear: D3D9 selects sRGB through sampler state, see nue_tex_format_to_srgb. */
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_d3d9(uint32_t d3dfmt, nue_bool* exact);
NUE_API uint32_t NUE_CALL nue_tex_format_to_d3d9(nue_tex_format format);
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_dxgi(uint32_t dxgi_format);           /* DXGI_FORMAT */
NUE_API uint32_t NUE_CALL nue_tex_format_to_dxgi(nue_tex_format format);
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_vk(uint32_t vk_format);               /* VkFormat (also the KTX2 vkFormat field) */
NUE_API uint32_t NUE_CALL nue_tex_format_to_vk(nue_tex_format format);
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_metal(uint32_t mtl_pixel_format);     /* MTLPixelFormat */
NUE_API uint32_t NUE_CALL nue_tex_format_to_metal(nue_tex_format format);

typedef struct nue_tex_gl_format {
    uint32_t internal_format;           /* sized or compressed internal format (GL_RGBA8, GL_COMPRESSED_RGBA_ASTC_4x4_KHR ...) */
    uint32_t format, type;              /* upload format/type for uncompressed formats; 0 for compressed */
} nue_tex_gl_format;
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_gl(uint32_t internal_format);
NUE_API nue_result NUE_CALL nue_tex_format_to_gl(nue_tex_format format, nue_tex_gl_format* out);   /* NUE_ERR_UNSUPPORTED when GL has none */

/* bgfx: bgfx::TextureFormat::Enum (the sRGB flag lives in the texture flags there, so sRGB formats map to the same id as linear ones). */
NUE_API nue_tex_format NUE_CALL nue_tex_format_from_bgfx(uint32_t bgfx_texture_format, nue_bool srgb);
NUE_API uint32_t NUE_CALL nue_tex_format_to_bgfx(nue_tex_format format, nue_bool* out_srgb);   /* bgfx::TextureFormat::Count (105) if none */
#define NUE_TEX_BGFX_FORMAT_COUNT 105u

/* KTX2 identifies formats by VkFormat: nue_tex_format_from_vk / to_vk. DDS by DXGI or FOURCC: from_dxgi / from_d3d9. */

/* ============================================================================================== capabilities */

enum {                                  /* per-format capability flags (nue_tex_caps.format_flags[format]) */
    NUE_TEX_FCAP_SAMPLE       = 1u << 0,    /* can be sampled by shaders, kept in this format in video memory */
    NUE_TEX_FCAP_FILTER       = 1u << 1,    /* linear / trilinear / anisotropic filtering */
    NUE_TEX_FCAP_RENDER       = 1u << 2,    /* can be a render target */
    NUE_TEX_FCAP_SRGB_SAMPLE  = 1u << 3,    /* hardware sRGB decode when sampled (needed for *_SRGB ids; otherwise decode in the shader) */
    NUE_TEX_FCAP_EMULATED     = 1u << 4,    /* the driver accepts it but decompresses to RGBA8 at upload: no memory saved */
    NUE_TEX_FCAP_MIP_AUTOGEN  = 1u << 5,
    NUE_TEX_FCAP_3D           = 1u << 6,
    NUE_TEX_FCAP_CUBE         = 1u << 7,
    NUE_TEX_FCAP_MSAA         = 1u << 8
};
#define NUE_TEX_FCAP_USABLE (NUE_TEX_FCAP_SAMPLE | NUE_TEX_FCAP_FILTER)

typedef enum nue_tex_device_class {
    NUE_TEX_DEVICE_SOFTWARE = 0,        /* RGBA8 only (software renderer, noop) */
    NUE_TEX_DEVICE_DESKTOP_BC,          /* Windows / Linux desktop GPUs: BC1-7, no ETC2/ASTC */
    NUE_TEX_DEVICE_DESKTOP_BC_ETC2,     /* Intel iGPU / Mesa style: BC + ETC2 (GL 4.3 / ES3 emulation, often EMULATED) */
    NUE_TEX_DEVICE_ANDROID_ASTC,        /* typical Android phone, GLES 3.0+/Vulkan: ETC2 + ASTC LDR */
    NUE_TEX_DEVICE_ANDROID_ETC2,        /* old/low-end Android: ETC2 only */
    NUE_TEX_DEVICE_ANDROID_ETC2_BC,     /* Android with BC (Tegra / x86 Intel): ETC2 + BC (+ ASTC) */
    NUE_TEX_DEVICE_APPLE_ASTC,          /* iOS A8+ : ASTC LDR (HDR from A13), ETC2, PVRTC; never BC */
    NUE_TEX_DEVICE_APPLE_PVRTC,         /* iOS A7 and older: PVRTC only (ASTC starts with A8) */
    NUE_TEX_DEVICE_APPLE_SILICON_MAC,   /* Apple GPU on macOS: BC + ASTC (+ ETC2) */
    NUE_TEX_DEVICE_COUNT
} nue_tex_device_class;

typedef struct nue_tex_caps {
    uint32_t struct_size;
    nue_tex_device_class device_class;
    uint32_t format_flags[NUE_TEX_FORMAT_COUNT];    /* NUE_TEX_FCAP_*; 0 = cannot be used */
    uint32_t max_size_2d, max_size_3d, max_size_cube;
    uint32_t max_array_layers;
    uint32_t max_anisotropy;
    nue_bool npot_full;                 /* non power of two sizes with mips and wrap: no restriction */
    nue_bool npot_with_mips;
    nue_bool block_size_must_align;     /* compressed level sizes must be multiples of the block (GL on some drivers, D3D12) */
    nue_bool astc_hdr;                  /* ASTC HDR profile samples (iOS A13+, few Android) */
    nue_bool astc_ldr_full_profile;     /* false: LDR sRGB decode may differ slightly ("ldr" only drivers) */
    nue_bool supports_texture_swizzle;  /* sampler component swizzle exists (planner can leave swizzles to the GPU) */
    uint64_t available_memory_bytes;    /* memory a texture budget can be a share of, as reported by the probe (0 = unknown) */
    uint32_t reserved[8];
} nue_tex_caps;

/* Fill with the typical capabilities of a platform class (the pure-logic fallback table; backends refine it from real queries). */
NUE_API nue_result NUE_CALL nue_tex_caps_for_class(nue_tex_device_class device_class, nue_tex_caps* out);
NUE_API const char* NUE_CALL nue_tex_device_class_name(nue_tex_device_class device_class);
NUE_API nue_tex_device_class NUE_CALL nue_tex_device_class_from_name(const char* name);   /* "desktop_bc", "android_astc"...; SOFTWARE if unknown */
NUE_API void NUE_CALL nue_tex_caps_clear(nue_tex_caps* caps);                             /* everything unsupported except RGBA8/BGRA8 sampling */
NUE_API void NUE_CALL nue_tex_caps_set_format(nue_tex_caps* caps, nue_tex_format format, uint32_t flags);
NUE_API uint32_t NUE_CALL nue_tex_caps_get_format(const nue_tex_caps* caps, nue_tex_format format);
/* True when the GPU keeps `format` compressed and filters it (not EMULATED). */
NUE_API nue_bool NUE_CALL nue_tex_caps_can_use(const nue_tex_caps* caps, nue_tex_format format);
/* Backends fill the table from what the graphics API says:
 *   - bgfx:   pass bgfx::Caps::formats (uint16_t[105], BGFX_CAPS_FORMAT_TEXTURE_* flags) and the maximum texture size;
 *   - Vulkan / Metal / GL probes (Android and iOS sessions): call nue_tex_caps_set_format for each supported format.
 * Formats bgfx cannot express (ASTC HDR, sRGB twins of block formats, ETC2 A1 sRGB...) are derived from their linear twin. */
NUE_API nue_result NUE_CALL nue_tex_caps_from_bgfx(nue_tex_caps* caps, const uint16_t* bgfx_format_caps, uint32_t count, uint32_t max_texture_size);
struct nue_gpu_caps;
/* Fill from the engine's GPU probe (gfx.h: nue_gpu_probe / nue_gfx_get_gpu_caps). Format families map to every texture format they contain
 * (sRGB twins when the family reports NUE_FMTSUP_SRGB); NUE_FMTSUP_EMULATED becomes NUE_TEX_FCAP_EMULATED. */
NUE_API nue_result NUE_CALL nue_tex_caps_from_gpu_caps(nue_tex_caps* caps, const struct nue_gpu_caps* gpu);
NUE_API nue_result NUE_CALL nue_tex_caps_to_string(const nue_tex_caps* caps, char* buffer, size_t capacity, size_t* out_size);   /* NUE_STRING_OUT: one format per line */

/* ============================================================================================== planner */

typedef enum nue_tex_priority {
    NUE_TEX_PRIORITY_BALANCED = 0,      /* keep memory about equal, keep the best quality that fits it */
    NUE_TEX_PRIORITY_QUALITY,           /* allow up to 2x the memory for quality (e.g. BC1 -> ASTC 4x4) */
    NUE_TEX_PRIORITY_SPEED,             /* cheapest encoder (ETC2 rather than ASTC), RGBA8 instead of re-encoding when the data is small */
    NUE_TEX_PRIORITY_MEMORY             /* smallest result: ASTC with bigger blocks, RGB565/RGBA4 fallbacks */
} nue_tex_priority;

enum {                                  /* nue_tex_policy.flags */
    NUE_TEX_POLICY_NO_LOSSY            = 1u << 0,   /* never choose a lossy re-encode: upload as is or decode to uncompressed (can be large) */
    NUE_TEX_POLICY_NO_SHADER_SWIZZLE   = 1u << 1,   /* every plan must sample with the original channel layout (no "rrrg" tricks) */
    NUE_TEX_POLICY_COMPRESS_RAW        = 1u << 2,   /* compress uncompressed sources (RGBA8 -> BC/ETC2/ASTC) when the data is big enough */
    NUE_TEX_POLICY_FORCE_UNCOMPRESSED  = 1u << 3,   /* always decode to RGBA8 (debugging; reference rendering) */
    NUE_TEX_POLICY_ALLOW_16BIT         = 1u << 4,   /* RGB565 / RGBA4444 fallbacks when memory is the priority */
    NUE_TEX_POLICY_KEEP_SOURCE_FORMAT  = 1u << 5    /* never change a format the GPU supports (default behaviour; kept for explicitness) */
};

typedef struct nue_tex_policy {
    uint32_t struct_size;
    nue_tex_priority priority;
    uint32_t flags;                     /* NUE_TEX_POLICY_* */
    float memory_tolerance;             /* target size / source size allowed; 0 = from priority (quality 2.0, balanced 1.1, speed 2.0, memory 0.6) */
    uint32_t max_dimension;             /* 0 = unlimited; larger textures drop their top mip levels */
    uint32_t min_recompress_dimension;  /* textures whose longest side is smaller are not re-encoded, they become RGBA8 (default 16) */
    uint32_t quality;                   /* nue_tex_quality + 1, 0 = automatic from the priority */
    uint32_t reserved[4];
} nue_tex_policy;
NUE_API void NUE_CALL nue_tex_policy_default(nue_tex_policy* out);

typedef enum nue_tex_usage {
    NUE_TEX_USAGE_COLOR = 0,            /* albedo / diffuse / UI: perceptual error */
    NUE_TEX_USAGE_NORMAL_MAP,           /* two-channel tangent space normals: XY precision matters, Z is rebuilt */
    NUE_TEX_USAGE_DATA,                 /* masks, roughness, lookup tables: linear error, no perceptual weighting */
    NUE_TEX_USAGE_HDR,                  /* lightmaps / environment: floats */
    NUE_TEX_USAGE_UI                    /* sharp edges, alpha: never use big ASTC blocks */
} nue_tex_usage;

typedef enum nue_tex_alpha_info {       /* what the content really uses (nue_tex_scan_level fills it) */
    NUE_TEX_ALPHA_INFO_UNKNOWN = 0, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_ALPHA_INFO_BINARY, NUE_TEX_ALPHA_INFO_SMOOTH
} nue_tex_alpha_info;

enum {                                  /* nue_tex_plan_request.flags */
    NUE_TEX_REQ_RENDER_TARGET = 1u << 0,    /* the GPU writes it: only renderable formats, never compressed */
    NUE_TEX_REQ_DYNAMIC       = 1u << 1,    /* the game rewrites it often (Lock every frame): no re-encode, uncompressed */
    NUE_TEX_REQ_NO_MIP_SKIP   = 1u << 2,
    NUE_TEX_REQ_SRGB          = 1u << 3,    /* content is sRGB (D3D9 SRGBTexture sampler state seen / known) */
    NUE_TEX_REQ_CUBE          = 1u << 4,
    NUE_TEX_REQ_VOLUME        = 1u << 5
};

typedef struct nue_tex_plan_request {
    uint32_t struct_size;
    nue_tex_format source_format;
    uint32_t width, height, depth;
    uint32_t mip_levels;                /* 0 = full chain */
    uint32_t layers, faces;             /* 0 -> 1 */
    nue_tex_usage usage;
    nue_tex_alpha_info alpha_info;
    uint32_t flags;                     /* NUE_TEX_REQ_* */
    const nue_tex_policy* policy;       /* NULL = defaults */
    uint32_t reserved[4];
} nue_tex_plan_request;

typedef enum nue_tex_action {
    NUE_TEX_ACTION_UPLOAD_AS_IS = 0,    /* the GPU samples the source format */
    NUE_TEX_ACTION_CONVERT_PIXELS,      /* uncompressed -> other uncompressed layout (channel order, depth) */
    NUE_TEX_ACTION_DECODE,              /* compressed -> uncompressed */
    NUE_TEX_ACTION_ENCODE,              /* uncompressed -> compressed */
    NUE_TEX_ACTION_TRANSCODE,           /* compressed -> other compressed (decode + encode) */
    NUE_TEX_ACTION_UNSUPPORTED          /* nothing usable (target_format UNKNOWN) */
} nue_tex_action;

enum {                                  /* nue_tex_plan.flags */
    NUE_TEX_PLAN_LOSSY           = 1u << 0,    /* the result differs from the source beyond rounding */
    NUE_TEX_PLAN_ALPHA_DROPPED   = 1u << 1,    /* the target has no alpha: content scanned opaque */
    NUE_TEX_PLAN_FALLBACK        = 1u << 2,    /* the preferred choice was not available */
    NUE_TEX_PLAN_PRECISION_LOSS  = 1u << 3,    /* fewer bits than the source (HDR -> LDR, float -> 8 bit) */
    NUE_TEX_PLAN_MIPS_SKIPPED    = 1u << 4,
    NUE_TEX_PLAN_NEEDS_SWIZZLE   = 1u << 5,    /* shaders must apply plan.shader_swizzle when sampling */
    NUE_TEX_PLAN_ENCODE_SLOW     = 1u << 6,    /* expensive encode: do it ahead of time (import) and cache it */
    NUE_TEX_PLAN_SRGB_IN_SHADER  = 1u << 7     /* target has no hardware sRGB decode for sRGB content: the shader must linearise */
};

typedef struct nue_tex_plan {
    uint32_t struct_size;
    nue_tex_format source_format, target_format;
    nue_tex_action action;
    uint32_t flags;                     /* NUE_TEX_PLAN_* */
    uint32_t quality;                   /* nue_tex_quality the encoder should use */
    uint32_t skip_mips;                 /* top levels dropped (max_dimension / memory) */
    char shader_swizzle[5];             /* "rgba" or e.g. "rrrg": components to read from the sampled value to get back the source channels
                                           (NUE_TEX_PLAN_NEEDS_SWIZZLE when it differs from "rgba") */
    uint8_t pad[3];
    uint64_t source_bytes, target_bytes;/* size of the whole chain (after skip_mips for the target) */
    float expected_psnr_db;             /* typical PSNR of the result versus the decoded source (0 = unknown, 99 = lossless) */
    float encode_ms_per_mpixel;         /* single thread estimate on a mid-range phone core, see docs/TEXTURE.md */
    nue_tex_format alternatives[6];     /* other acceptable targets in order (UNKNOWN terminated) */
    char explanation[512];              /* human readable log of the decision, one sentence per line */
    uint32_t reserved[4];
} nue_tex_plan;

/* [any] Decide (nue_tex_make_plan). Always succeeds with a usable plan unless nothing can be sampled (NUE_ERR_UNSUPPORTED, plan.action = UNSUPPORTED).
 * NUE_ERR_INVALID_ARGUMENT for unknown formats or zero sizes. The decision is deterministic for the same inputs. */
NUE_API nue_result NUE_CALL nue_tex_make_plan(const nue_tex_caps* caps, const nue_tex_plan_request* request, nue_tex_plan* out_plan);
NUE_API const char* NUE_CALL nue_tex_action_name(nue_tex_action action);

/* ============================================================================================== images */

NUE_DEFINE_HANDLE(nue_tex_image);

enum { NUE_TEX_IMAGE_CUBE = 1u << 0 };      /* faces == 6 */

typedef struct nue_tex_image_desc {
    uint32_t struct_size;
    nue_tex_format format;
    uint32_t width, height;
    uint32_t depth;                     /* 0 -> 1; > 1 = volume (no arrays of volumes) */
    uint32_t mip_levels;                /* 0 -> full chain */
    uint32_t layers;                    /* array layers, 0 -> 1 */
    uint32_t faces;                     /* 1 or 6 (cube), 0 -> 1 */
    uint32_t flags;
    uint32_t reserved[4];
} nue_tex_image_desc;

typedef struct nue_tex_level_view {
    uint32_t struct_size;
    void* data;
    uint64_t size;
    uint32_t width, height, depth;
    uint32_t blocks_x, blocks_y;
    uint32_t row_pitch;                 /* bytes of one row of blocks */
    uint64_t slice_pitch;               /* bytes of one depth slice */
} nue_tex_level_view;

/* Memory is zero-initialised. Layout: layer-major, then face, then mip level, all contiguous. */
NUE_API nue_result NUE_CALL nue_tex_image_create(const nue_tex_image_desc* desc, nue_tex_image* out_image);
NUE_API void NUE_CALL nue_tex_image_destroy(nue_tex_image image);
NUE_API nue_result NUE_CALL nue_tex_image_get_desc(nue_tex_image image, nue_tex_image_desc* out);   /* mip_levels etc. filled in (never 0) */
NUE_API nue_result NUE_CALL nue_tex_image_get_level(nue_tex_image image, uint32_t level, uint32_t layer, uint32_t face, nue_tex_level_view* out);
NUE_API uint64_t NUE_CALL nue_tex_image_total_size(nue_tex_image image);
NUE_API nue_result NUE_CALL nue_tex_image_clone(nue_tex_image image, nue_tex_image* out_image);

typedef enum nue_tex_file_kind { NUE_TEX_FILE_AUTO = 0, NUE_TEX_FILE_DDS, NUE_TEX_FILE_KTX2, NUE_TEX_FILE_PNG } nue_tex_file_kind;

enum {                                  /* load/save flags */
    NUE_TEX_FILE_DDS_FORCE_DX10   = 1u << 0,    /* save: always write the DX10 extension header */
    NUE_TEX_FILE_DDS_LEGACY_ONLY  = 1u << 1,    /* save: fail instead of writing a DX10 header when the format has a D3D9 FOURCC / bitmask form */
    NUE_TEX_FILE_ASSUME_SRGB      = 1u << 2,    /* load: formats with an sRGB twin and no colour space in the file (legacy DDS, PNG) become sRGB */
    NUE_TEX_FILE_FLIP_Y           = 1u << 3     /* load PNG: no-op placeholder (PNG rows are top-down, like D3D) */
};

/* Parse a file image held in memory. DDS: legacy FOURCC / bitmask headers (D3D9 era) and DX10 headers, mips, cube maps, volumes, arrays.
 * KTX2: any supported vkFormat, supercompression none only (others: NUE_ERR_UNSUPPORTED). PNG: 8/16-bit grey/GA/RGB/RGBA -> L8/LA8/RGB8/RGBA8 (16 bit -> L16/RGBA16); saving PNG writes level 0 as RGBA8. */
NUE_API nue_result NUE_CALL nue_tex_image_load_memory(const void* data, size_t size, nue_tex_file_kind kind, uint32_t flags, nue_tex_image* out_image);
NUE_API nue_result NUE_CALL nue_tex_image_load_file(const char* utf8_path, nue_tex_file_kind kind, uint32_t flags, nue_tex_image* out_image);
/* Serialise. The buffer is allocated with nue_alloc-compatible memory: free it with nue_tex_free. */
NUE_API nue_result NUE_CALL nue_tex_image_save_memory(nue_tex_image image, nue_tex_file_kind kind, uint32_t flags, void** out_data, size_t* out_size);
NUE_API nue_result NUE_CALL nue_tex_image_save_file(nue_tex_image image, const char* utf8_path, nue_tex_file_kind kind, uint32_t flags);
NUE_API void NUE_CALL nue_tex_free(void* ptr);
NUE_API nue_tex_file_kind NUE_CALL nue_tex_detect_file_kind(const void* data, size_t size);

/* ============================================================================================== codecs */

typedef enum nue_tex_quality { NUE_TEX_QUALITY_FASTEST = 0, NUE_TEX_QUALITY_FAST, NUE_TEX_QUALITY_NORMAL, NUE_TEX_QUALITY_HIGH, NUE_TEX_QUALITY_BEST } nue_tex_quality;

NUE_DEFINE_HANDLE(nue_tex_executor);

enum {                                  /* nue_tex_encode_params.flags */
    NUE_TEX_ENC_NORMAL_MAP   = 1u << 0,     /* content is a tangent-space normal map: angular error (ASTC: stores R,R,R,G) */
    NUE_TEX_ENC_PERCEPTUAL   = 1u << 1,     /* weigh the error by perceptual luma (colour data); default for COLOR usage */
    NUE_TEX_ENC_SRGB         = 1u << 2,     /* the source RGB is sRGB encoded; the target is an *_SRGB format or linearised by the sampler */
    NUE_TEX_ENC_PUNCHTHROUGH = 1u << 3,     /* quantise alpha to 0/255 (BC1 3-colour mode, ETC2 A1) with `alpha_threshold` */
    NUE_TEX_ENC_DITHER       = 1u << 4,     /* BC1 / ETC1 error diffusion dither: faster paths, cheaper-looking gradients */
    NUE_TEX_ENC_ALPHA_WEIGHT = 1u << 5      /* ASTC: weigh colour error by alpha */
};

typedef struct nue_tex_encode_params {
    uint32_t struct_size;
    nue_tex_quality quality;
    uint32_t flags;                     /* NUE_TEX_ENC_* */
    uint8_t alpha_threshold;            /* punch-through cut (default 128) */
    char swizzle[5];                    /* source channel -> encoded channel, from "rgba01": default "rgba" ("rrrg" for ASTC normal maps is applied
                                           automatically by NUE_TEX_ENC_NORMAL_MAP) */
    uint8_t pad[2];
    nue_tex_executor executor;          /* NULL = run on the calling thread */
    uint32_t max_threads;               /* 0 = all of the executor's */
    uint32_t reserved[4];
} nue_tex_encode_params;
NUE_API void NUE_CALL nue_tex_encode_params_default(nue_tex_encode_params* out);

/* Executor: a priority job queue on worker threads. The codecs split big levels into bands of block rows and run them as jobs.
 * Either NUE owns the threads (desc.threads) or the host supplies `submit` (engine job system): then no threads are created. */
typedef enum nue_tex_job_priority { NUE_TEX_JOB_LOW = 0, NUE_TEX_JOB_NORMAL, NUE_TEX_JOB_HIGH, NUE_TEX_JOB_CRITICAL } nue_tex_job_priority;
typedef void (NUE_CALL *nue_tex_job_fn)(void* userdata);
typedef struct nue_tex_executor_desc {
    uint32_t struct_size;
    uint32_t threads;                   /* 0 = hardware concurrency - 1 (at least 1) */
    const char* name;                   /* thread name prefix */
    int32_t thread_priority;            /* -2 (lowest) .. +2: mapped to the OS (background import uses -1) */
    /* Optional host job system. When set, jobs are handed to it (higher `priority` first) and must eventually be run by calling job(userdata). */
    void (NUE_CALL *submit)(void* host_userdata, nue_tex_job_fn job, void* job_userdata, nue_tex_job_priority priority);
    void* host_userdata;
    uint32_t reserved[4];
} nue_tex_executor_desc;
NUE_API nue_result NUE_CALL nue_tex_executor_create(const nue_tex_executor_desc* desc, nue_tex_executor* out);
NUE_API void NUE_CALL nue_tex_executor_destroy(nue_tex_executor executor);             /* waits for queued jobs */
NUE_API nue_result NUE_CALL nue_tex_executor_submit(nue_tex_executor executor, nue_tex_job_fn job, void* userdata, nue_tex_job_priority priority);
NUE_API void NUE_CALL nue_tex_executor_wait_idle(nue_tex_executor executor);
NUE_API uint32_t NUE_CALL nue_tex_executor_thread_count(nue_tex_executor executor);

/* Level API: one 2D level (or volume slice by slice) of tightly packed data, "raw blocks" in the Direct3D 9 sense.
 * dst/src sizes must be at least nue_tex_level_size(format, w, h, 1). Rows are padded to whole blocks on the right/bottom by replicating edge texels. */
/* Decode any decodable format to RGBA8 (sRGB formats stay sRGB encoded: the values are not linearised) or RGBA32F (linear floats; sRGB formats are
 * linearised; HDR formats keep their range). */
NUE_API nue_result NUE_CALL nue_tex_decode_level(nue_tex_format format, uint32_t width, uint32_t height, const void* src, size_t src_size,
                                                  nue_tex_format dst_format /* RGBA8_UNORM or RGBA32_FLOAT */, void* dst, size_t dst_size,
                                                  nue_tex_executor executor);
/* Encode RGBA8 or RGBA32F pixels (`src_pitch` bytes per row) into a block/packed/uncompressed format. */
NUE_API nue_result NUE_CALL nue_tex_encode_level(nue_tex_format format, uint32_t width, uint32_t height, nue_tex_format src_format,
                                                  const void* src, size_t src_pitch, void* dst, size_t dst_size, const nue_tex_encode_params* params);
/* Any format to any format for one level through an RGBA intermediate, with copy fast paths. dst_size >= nue_tex_level_size(dst_format,...). */
NUE_API nue_result NUE_CALL nue_tex_convert_level(nue_tex_format src_format, nue_tex_format dst_format, uint32_t width, uint32_t height,
                                                   const void* src, size_t src_size, void* dst, size_t dst_size, const nue_tex_encode_params* params);

/* Pixel row helpers for uncompressed formats (also used by the D3D9 layer for legacy formats): unpack to RGBA8 / RGBA32F and pack back. */
NUE_API nue_result NUE_CALL nue_tex_unpack_pixels(nue_tex_format format, const void* src, uint32_t count, nue_tex_format dst_format /* RGBA8 or RGBA32F */, void* dst);
NUE_API nue_result NUE_CALL nue_tex_pack_pixels(nue_tex_format format, const void* src, uint32_t count, nue_tex_format src_format /* RGBA8 or RGBA32F */, void* dst);

/* Whole images (all mips, layers, faces). If the source has fewer mip levels than `dst_desc.mip_levels` (or the full chain when 0) the missing levels are
 * generated from the previous level with the mip filter; present levels are converted one by one (source mips are preserved). */
typedef enum nue_tex_mip_filter {
    NUE_TEX_MIP_BOX = 0,                /* 2x2 average (exact halving), the fast default for power-of-two */
    NUE_TEX_MIP_TRIANGLE,
    NUE_TEX_MIP_KAISER,                 /* sharper, better for non-power-of-two reduction */
    NUE_TEX_MIP_LANCZOS
} nue_tex_mip_filter;
enum {                                  /* nue_tex_mip_params.flags */
    NUE_TEX_MIP_SRGB            = 1u << 0,  /* filter in linear light (RGB channels; alpha is always linear) */
    NUE_TEX_MIP_NORMAL_MAP      = 1u << 1,  /* renormalise RG(B) vectors after filtering */
    NUE_TEX_MIP_ALPHA_COVERAGE  = 1u << 2,  /* keep the proportion of alpha above 0.5 equal to level 0 (alpha tested foliage) */
    NUE_TEX_MIP_WRAP            = 1u << 3,  /* tile at the borders (default clamp) */
    NUE_TEX_MIP_PREMULTIPLY     = 1u << 4   /* filter colour * alpha */
};
typedef struct nue_tex_mip_params {
    uint32_t struct_size;
    nue_tex_mip_filter filter;
    uint32_t flags;
    nue_tex_executor executor;
    uint32_t reserved[4];
} nue_tex_mip_params;
NUE_API void NUE_CALL nue_tex_mip_params_default(nue_tex_mip_params* out);
/* Generate the full mip chain of an uncompressed image (levels beyond the existing ones). Compressed sources: convert to RGBA8 first. */
NUE_API nue_result NUE_CALL nue_tex_generate_mips(nue_tex_image src, const nue_tex_mip_params* params, nue_tex_image* out_image);
NUE_API nue_result NUE_CALL nue_tex_image_convert(nue_tex_image src, nue_tex_format dst_format, uint32_t dst_mip_levels /* 0 = same as source, or full chain if the source has one level */,
                                                   const nue_tex_encode_params* encode, const nue_tex_mip_params* mips, nue_tex_image* out_image);
/* Drop the top `count` mip levels (memory budget / max_dimension). */
NUE_API nue_result NUE_CALL nue_tex_image_drop_top_mips(nue_tex_image src, uint32_t count, nue_tex_image* out_image);

/* Content scan of one level (cheap, no full decode for block formats): what alpha is really used, whether BC1 blocks use the 3-colour mode. */
typedef struct nue_tex_scan_result {
    uint32_t struct_size;
    nue_tex_alpha_info alpha_info;
    uint8_t min_alpha, max_alpha;
    nue_bool uses_punchthrough;         /* BC1 3-colour transparent blocks, ETC2 A1 */
    nue_bool constant_color;            /* every texel decodes to one colour (uncompressed formats only) */
    uint32_t reserved[4];
} nue_tex_scan_result;
NUE_API nue_result NUE_CALL nue_tex_scan_level(nue_tex_format format, uint32_t width, uint32_t height, const void* src, size_t src_size, nue_tex_scan_result* out);

/* Quality metrics between two images of identical size, compared as RGBA8 (formats decoded as needed). */
typedef struct nue_tex_metrics {
    uint32_t struct_size;
    double psnr_db;                     /* over RGB (+A when present), 99 when identical */
    double psnr_rgb_db, psnr_alpha_db;
    double ssim;                        /* mean SSIM over luma, 8x8 windows, 0..1 */
    double delta_e_mean, delta_e_max;   /* CIEDE2000 on sRGB RGB */
    double max_abs_error;               /* largest channel difference, 0..255 */
    uint32_t reserved[4];
} nue_tex_metrics;
NUE_API nue_result NUE_CALL nue_tex_compare_level(nue_tex_format format_a, const void* a, nue_tex_format format_b, const void* b, uint32_t width, uint32_t height,
                                                   nue_bool srgb, nue_tex_metrics* out);
NUE_API nue_result NUE_CALL nue_tex_compare_images(nue_tex_image a, nue_tex_image b, nue_tex_metrics* out_level0);

/* Version string of the codec set, e.g. "astc-encoder 5.7.0, etcpak 3d716e1, bcdec 0.985, pvrtc-javelin; simd sse4.1" */
NUE_API const char* NUE_CALL nue_tex_codec_info(void);

/* ============================================================================================== context, cache, import */

typedef void (NUE_CALL *nue_tex_log_fn)(void* userdata, int level /* nue_log_level numbers: 3 info, 4 warn, 5 error, 2 debug */, const char* message);
/* Optional: route the module's log lines (default: nothing; NUE_TEX_LOG=1 in the environment prints them to stderr). */
NUE_API void NUE_CALL nue_tex_set_log_callback(nue_tex_log_fn fn, void* userdata);

/* Texture memory budget policy. There is NO built-in limit anywhere in the module: the host (game, platform layer, user settings) chooses one.
 * `mode` selects how the byte limit is derived; presets are only convenient defaults a platform layer may apply. */
typedef enum nue_tex_budget_mode {
    NUE_TEX_BUDGET_UNLIMITED = 0,       /* never evict */
    NUE_TEX_BUDGET_BYTES,               /* limit_bytes */
    NUE_TEX_BUDGET_FRACTION,            /* fraction x available_bytes (supplied by the host from the system: RAM, per-process limit, GPU memory) */
    NUE_TEX_BUDGET_PRESET               /* preset below applied to available_bytes */
} nue_tex_budget_mode;
typedef enum nue_tex_budget_preset {
    NUE_TEX_BUDGET_PRESET_UNLIMITED = 0,
    NUE_TEX_BUDGET_PRESET_LOW_MEMORY_MOBILE,   /* 12% of available, floor 64 MiB: small phones, textures shed aggressively */
    NUE_TEX_BUDGET_PRESET_MOBILE,              /* 20% of available */
    NUE_TEX_BUDGET_PRESET_DESKTOP,             /* 35% of available (graphics memory is usually separate) */
    NUE_TEX_BUDGET_PRESET_HIGH_END             /* 50% of available */
} nue_tex_budget_preset;
typedef struct nue_tex_budget_policy {
    uint32_t struct_size;
    nue_tex_budget_mode mode;
    uint64_t limit_bytes;               /* BYTES */
    double fraction;                    /* FRACTION: 0..1 */
    nue_tex_budget_preset preset;       /* PRESET */
    uint64_t available_bytes;           /* FRACTION / PRESET: memory the texture budget is a share of; the host queries it (system.h memory info, platform per-process limit) */
    uint64_t min_bytes, max_bytes;      /* clamp of the derived limit (0 = none) */
} nue_tex_budget_policy;
/* Resolve a policy to bytes (UINT64_MAX = unlimited). [any] */
NUE_API uint64_t NUE_CALL nue_tex_budget_resolve(const nue_tex_budget_policy* policy);
NUE_API void NUE_CALL nue_tex_budget_policy_preset(nue_tex_budget_preset preset, uint64_t available_bytes, nue_tex_budget_policy* out);

NUE_DEFINE_HANDLE(nue_tex_context);

/* Context flags. By default NOTHING is encoded at run time: the lossy codecs (ASTC, ETC2, BC7...) are far too slow for a frame and a hitch is worse than a placeholder.
 * Textures are converted COLD, once, by the import pipeline / nue_tex_cook_image / `nue_texconv cook` into the cache; at run time conversion is a cache lookup by content.
 * Set ALLOW_RUNTIME_ENCODE only for development (missing variants are then encoded on the spot). */
enum { NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE = 1u << 0 };

typedef struct nue_tex_context_desc {
    uint32_t struct_size;
    const nue_tex_caps* caps;           /* required (copied) */
    const nue_tex_policy* policy;       /* NULL = defaults (copied) */
    const char* cache_dir;              /* UTF-8 native path of the cache directory (created on demand); NULL = no cache */
    uint64_t cache_max_bytes;           /* disk cache size limit, trimmed least-recently-used first; 0 = unlimited (hosts normally set a limit) */
    uint32_t worker_threads;            /* executor threads; 0 = auto */
    nue_tex_budget_policy budget;       /* texture memory budget policy (a preset or explicit bytes / fraction), see above; NUE_STRUCT_INIT = unlimited */
    const nue_tex_executor_desc* executor; /* NULL = own threads */
    uint32_t flags;                     /* NUE_TEX_CONTEXT_* */
    uint32_t reserved[3];
} nue_tex_context_desc;
NUE_API nue_result NUE_CALL nue_tex_context_create(const nue_tex_context_desc* desc, nue_tex_context* out);
NUE_API void NUE_CALL nue_tex_context_destroy(nue_tex_context ctx);
NUE_API nue_result NUE_CALL nue_tex_context_get_caps(nue_tex_context ctx, nue_tex_caps* out);
NUE_API nue_result NUE_CALL nue_tex_context_set_caps(nue_tex_context ctx, const nue_tex_caps* caps);   /* device change (Reset): cache keys include the target, so entries stay valid */
NUE_API nue_tex_executor NUE_CALL nue_tex_context_executor(nue_tex_context ctx);

/* Cache key: SHA-256 over (source bytes, source format, size, mips, layers, faces, target format, quality, encode flags, swizzle, codec version).
 * Entries are KTX2 files "<dir>/<aa>/<bb>/<hash>.ktx2" with the key and a payload CRC in the key/value block. */
typedef struct nue_tex_cache_stats {
    uint32_t struct_size;
    uint64_t hits, misses, stores, corrupt_dropped, evicted;
    uint64_t entries, bytes;
    uint64_t hit_bytes, store_bytes;
    uint32_t reserved[4];
} nue_tex_cache_stats;
NUE_API nue_result NUE_CALL nue_tex_cache_get_stats(nue_tex_context ctx, nue_tex_cache_stats* out);
NUE_API nue_result NUE_CALL nue_tex_cache_clear(nue_tex_context ctx);                 /* deletes every entry; always safe */
NUE_API nue_result NUE_CALL nue_tex_cache_trim(nue_tex_context ctx, uint64_t max_bytes);   /* evict least recently used until under max_bytes */
NUE_API nue_result NUE_CALL nue_tex_cache_scan(nue_tex_context ctx);

/* ---- Cold conversion: all formats at once, ahead of time ------------------------------------------------------------------------------------------------
 * A cache entry is keyed by the source CONTENT and the target format, not by the device, so one cook produces the variants for every device class and the cache folder can be
 * built once (on a PC, or at first start) and shipped or copied. Each entry also records the hash of every source level, so at run time the D3D9 layer finds the converted level
 * from the bytes the game uploads (nue_tex_d3d9_convert_level) without knowing file names. */
#define NUE_TEX_CLASS_BIT(device_class) (1u << (device_class))
#define NUE_TEX_CLASS_MASK_ALL ((1u << NUE_TEX_DEVICE_COUNT) - 1u)
typedef struct nue_tex_cook_result {
    uint32_t struct_size;
    uint32_t variants;                  /* distinct lossy targets the chosen classes need (BC/as-is/uncompressed targets need no file) */
    uint32_t stored, already_cached, failed;
    uint64_t bytes_written;
    uint32_t reserved[4];
} nue_tex_cook_result;

typedef struct nue_tex_runtime_stats {
    uint32_t struct_size;
    uint64_t level_hits;                /* levels served from the cold cache by content */
    uint64_t level_misses;              /* levels the cache did not know: a placeholder (zero blocks) was returned, NUE_OK_PARTIAL */
    uint64_t runtime_encodes;           /* lossy encodes done at run time (only with NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE) */
    uint64_t indexed_levels;            /* source levels known to the index */
    uint32_t reserved[4];
} nue_tex_runtime_stats;
NUE_API nue_result NUE_CALL nue_tex_context_get_runtime_stats(nue_tex_context ctx, nue_tex_runtime_stats* out);

/* Where the time goes (cumulative nanoseconds spent inside each phase, summed over all threads that ran it; nested phases overlap: convert contains decode, encode and mips). For profiling the cook and the lookups. */
typedef struct nue_tex_profile {
    uint32_t struct_size;
    uint64_t alpha_scan_ns, plan_ns, source_hash_ns, cache_lookup_ns, drop_mips_ns, convert_ns, decode_ns, encode_ns, mips_ns, level_hash_ns, store_ns, lookup_ns, load_ns;
    uint64_t hashed_bytes;              /* bytes run through SHA-256 */
    uint32_t reserved[4];
} nue_tex_profile;
NUE_API void NUE_CALL nue_tex_profile_get(nue_tex_profile* out);
NUE_API void NUE_CALL nue_tex_profile_reset(void);
/* Cumulative encode time and encoded pixels per target format since the last reset (all threads). */
NUE_API void NUE_CALL nue_tex_profile_encode(nue_tex_format format, uint64_t* out_ns, uint64_t* out_pixels);                  /* rebuild the statistics from the directory (drops corrupt files) */

/* Convert an image for this device: plan + (cache lookup | convert + cache store). `source_key` may be NULL (the key is computed from the bytes).
 * out_plan may be NULL. The result is owned by the caller. */
typedef struct nue_tex_convert_request {
    uint32_t struct_size;
    nue_tex_image source;
    nue_tex_usage usage;
    uint32_t flags;                     /* NUE_TEX_REQ_* */
    const nue_tex_policy* policy;       /* NULL = the context's */
    nue_tex_alpha_info alpha_info;      /* UNKNOWN: scanned */
    uint32_t reserved[4];
} nue_tex_convert_request;
NUE_API nue_result NUE_CALL nue_tex_context_convert(nue_tex_context ctx, const nue_tex_convert_request* request, nue_tex_image* out_image, nue_tex_plan* out_plan,
                                                     nue_bool* out_cache_hit);
/* Convert one image for every device class in `class_mask` (NUE_TEX_CLASS_MASK_ALL = everything), skipping variants that already exist, and store the results. [any]
 * Colour space: variants for sRGB sampling (ASTC has a separate sRGB encode; ETC2/BC/PVRTC share their bytes with the linear one) are cooked only when request->flags has NUE_TEX_REQ_SRGB,
 * because they cost a second encode. The Direct3D 9 layer of NFSU2 never samples textures as sRGB. */
NUE_API nue_result NUE_CALL nue_tex_cook_image(nue_tex_context ctx, const nue_tex_convert_request* request, uint32_t class_mask, nue_tex_cook_result* out);

/* Asynchronous conversion for textures that show up at run time. The request is queued at `priority` (HIGH for something about to be drawn);
 * the callback gets the result on a worker thread and OWNS `image` (free it with nue_tex_image_destroy); `plan` is valid only during the call. The source
 * image is copied at request time, so the caller may destroy it right away. Returns a 32-bit request id (0 invalid). */
typedef uint32_t nue_tex_request_id;
typedef void (NUE_CALL *nue_tex_done_fn)(void* userdata, nue_tex_request_id id, nue_result result, nue_tex_image image, const nue_tex_plan* plan);
NUE_API nue_result NUE_CALL nue_tex_request_async(nue_tex_context ctx, const nue_tex_convert_request* request, nue_tex_job_priority priority,
                                                   nue_tex_done_fn done, void* userdata, nue_tex_request_id* out_id);
NUE_API nue_result NUE_CALL nue_tex_request_cancel(nue_tex_context ctx, nue_tex_request_id id);   /* NUE_OK_NOOP when already running/done; the callback gets CANCELLED if it had not started */
NUE_API void NUE_CALL nue_tex_wait_idle(nue_tex_context ctx);

/* First-run import: convert every texture of a game once, in the background, so no run-time transcoding hitches. Items are supplied by callbacks
 * (the game's archive reader) or paths of DDS/KTX2/PNG files. Resumable: items already in the cache are skipped, so a cancelled import continues
 * where it stopped on the next start. */
NUE_DEFINE_HANDLE(nue_tex_importer);
typedef struct nue_tex_import_item {
    uint32_t struct_size;
    const char* name;                   /* label for logs / progress (copied) */
    const char* path;                   /* DDS / KTX2 / PNG file to read, or NULL when `load` is set */
    /* Alternative to `path`: produce the image on demand (called on a worker thread; return the image with NUE_OK, the importer destroys it). */
    nue_result (NUE_CALL *load)(void* userdata, nue_tex_image* out_image);
    void* userdata;
    nue_tex_usage usage;
    uint32_t flags;                     /* NUE_TEX_REQ_* */
    uint32_t reserved[4];
} nue_tex_import_item;
typedef struct nue_tex_import_progress {
    uint32_t struct_size;
    uint32_t total, done, skipped_cached, failed;
    uint64_t source_bytes, produced_bytes;
    double elapsed_seconds, eta_seconds;
    nue_bool running, cancelled, finished;
    char current[96];
    uint32_t reserved[4];
} nue_tex_import_progress;
typedef void (NUE_CALL *nue_tex_import_cb)(void* userdata, const nue_tex_import_progress* progress);   /* worker thread, at most ~10 per second */
NUE_API nue_result NUE_CALL nue_tex_importer_create(nue_tex_context ctx, nue_tex_import_cb progress_cb, void* userdata, nue_tex_importer* out);
NUE_API void NUE_CALL nue_tex_importer_destroy(nue_tex_importer imp);                  /* cancels and waits */
NUE_API nue_result NUE_CALL nue_tex_importer_add(nue_tex_importer imp, const nue_tex_import_item* item);
/* Which device classes the import cooks for: a mask of NUE_TEX_CLASS_BIT (NUE_TEX_CLASS_MASK_ALL = all formats at once). 0 (default) = only the context's own device. */
NUE_API nue_result NUE_CALL nue_tex_importer_set_classes(nue_tex_importer imp, uint32_t class_mask);
NUE_API nue_result NUE_CALL nue_tex_importer_start(nue_tex_importer imp);               /* background; items added later are still processed */
NUE_API nue_result NUE_CALL nue_tex_importer_cancel(nue_tex_importer imp);
NUE_API nue_result NUE_CALL nue_tex_importer_pause(nue_tex_importer imp, nue_bool paused);
NUE_API nue_result NUE_CALL nue_tex_importer_get_progress(nue_tex_importer imp, nue_tex_import_progress* out);
NUE_API nue_result NUE_CALL nue_tex_importer_wait(nue_tex_importer imp, int64_t timeout_ms /* < 0 forever */);   /* NUE_ERR_TIMEOUT */

/* ============================================================================================== memory budget */

NUE_DEFINE_HANDLE(nue_tex_budget);
typedef uint32_t nue_tex_resource_id;       /* 0 invalid */
enum { NUE_TEX_RES_PINNED = 1u << 0 };      /* never offered for eviction (render targets, UI atlas) */
/* Called (on the thread that triggered the over-budget condition, with no budget lock held) with candidates, least recently used first. The owner
 * releases the resource (and calls nue_tex_budget_remove) or keeps it; it should free at least `bytes_to_free`. Return the bytes actually freed. */
typedef uint64_t (NUE_CALL *nue_tex_evict_fn)(void* userdata, const nue_tex_resource_id* candidates, uint32_t count, uint64_t bytes_to_free);
NUE_API nue_result NUE_CALL nue_tex_budget_create(const nue_tex_budget_policy* policy, nue_tex_evict_fn evict, void* userdata, nue_tex_budget* out);
NUE_API void NUE_CALL nue_tex_budget_destroy(nue_tex_budget budget);
NUE_API void NUE_CALL nue_tex_budget_set_policy(nue_tex_budget budget, const nue_tex_budget_policy* policy);   /* runtime change (low-memory warning, settings) */
NUE_API nue_result NUE_CALL nue_tex_budget_add(nue_tex_budget budget, uint64_t bytes, uint32_t flags, nue_tex_resource_id* out_id);   /* may call evict */
NUE_API nue_result NUE_CALL nue_tex_budget_resize(nue_tex_budget budget, nue_tex_resource_id id, uint64_t bytes);
NUE_API nue_result NUE_CALL nue_tex_budget_remove(nue_tex_budget budget, nue_tex_resource_id id);
NUE_API void NUE_CALL nue_tex_budget_touch(nue_tex_budget budget, nue_tex_resource_id id);                       /* most recently used */
NUE_API nue_result NUE_CALL nue_tex_budget_set_flags(nue_tex_budget budget, nue_tex_resource_id id, uint32_t flags);
typedef struct nue_tex_budget_stats {
    uint32_t struct_size;
    uint64_t limit, used, peak;
    uint32_t resources, pinned;
    uint64_t evictions_requested, bytes_evicted;
    uint32_t over_budget_events;
    uint32_t reserved[4];
} nue_tex_budget_stats;
NUE_API nue_result NUE_CALL nue_tex_budget_get_stats(nue_tex_budget budget, nue_tex_budget_stats* out);
/* Ask the owner (through the evict callback, LRU candidates, pinned excluded) to release at least `bytes_needed`, whatever the limit. Returns the bytes freed. */
NUE_API uint64_t NUE_CALL nue_tex_budget_evict(nue_tex_budget budget, uint64_t bytes_needed);
/* Engine bridge (memory.h; only in builds that contain the engine, NUE_ERR_UNSUPPORTED in the standalone library). Registers a NUE_BUDGET_TEXTURE budget called `name`
 * ("texture" if NULL): from then on the engine owns the LIMIT (policy / config keys budget.<name>.*, pressure events). Every nue_tex_budget_add/resize goes through
 * nue_budget_request (NUE_ERR_OVERFLOW = does not fit even after eviction, nothing is accounted), remove/shrink are reported; the engine's eviction hook calls this budget's evict
 * callback with LRU candidates (bytes_needed 0 = a share of the use chosen by the pressure level). The callback must release resources with nue_tex_budget_remove. */
NUE_API nue_result NUE_CALL nue_tex_budget_attach_engine(nue_tex_budget budget, const char* name, uint32_t* out_engine_budget_id);
NUE_API nue_result NUE_CALL nue_tex_budget_detach_engine(nue_tex_budget budget);

/* Memory model: bytes of a set of textures under each candidate target format (docs/TEXTURE.md has the NFSU2 estimates). */
typedef struct nue_tex_memory_item {
    nue_tex_format format;
    uint32_t width, height, mip_levels, count;      /* `count` identical textures */
} nue_tex_memory_item;
NUE_API uint64_t NUE_CALL nue_tex_memory_estimate(const nue_tex_memory_item* items, uint32_t item_count, nue_tex_format target /* UNKNOWN = as is */);

/* ============================================================================================== Direct3D 9 seam */

/* What the nue_d3d9 layer calls (see docs/TEXTURE.md "Integration"). All three calls are [any] and allocation free except the level conversion
 * output buffer which the caller owns.
 *   CreateTexture/CreateCubeTexture  -> nue_tex_d3d9_plan_texture: the GPU format to create the bgfx texture with, and the per-texture state;
 *   UnlockRect / UpdateTexture level -> nue_tex_d3d9_convert_level: D3D9-format bytes of the (sub)rectangle in, GPU-format bytes out;
 *   texture destroyed                -> nue_tex_d3d9_texture_destroy. */

/* Mapping tables as DATA (legacy ABI layers use these instead of owning format knowledge). Entries are static and sorted by `d3d`. A FOURCC entry has
 * d3d = the FOURCC value (e.g. 'DXT1' little endian), is_fourcc = 1. `exact` = byte-identical meaning. Several D3D9 ids may map to one format. */
typedef struct nue_tex_d3d9_format_entry {
    uint32_t d3d;                       /* D3DFMT value or FOURCC */
    nue_tex_format format;
    uint8_t is_fourcc, exact, premultiplied_alpha, reserved;
    const char* d3d_name;               /* "D3DFMT_A8R8G8B8", "DXT1" */
} nue_tex_d3d9_format_entry;
NUE_API const nue_tex_d3d9_format_entry* NUE_CALL nue_tex_d3d9_format_table(uint32_t* out_count);

NUE_DEFINE_HANDLE(nue_tex_d3d9_texture);
typedef struct nue_tex_d3d9_texture_desc {
    uint32_t struct_size;
    uint32_t d3d_format;                /* D3DFMT value or FOURCC */
    uint32_t width, height;
    uint32_t levels;                    /* 0 = full chain */
    uint32_t usage;                     /* D3DUSAGE_* bits (rendertarget 0x1, depthstencil 0x2, dynamic 0x200, autogenmipmap 0x400) */
    uint32_t pool;                      /* D3DPOOL (0 default, 1 managed, 2 systemmem, 3 scratch) */
    nue_bool cube;
    nue_bool srgb;                      /* sampler state D3DSAMP_SRGBTEXTURE seen for it, or known from content */
    nue_tex_usage content;
    nue_tex_alpha_info alpha_hint;      /* what the layer knows about the alpha (UNKNOWN is safe but costs memory for DXT1: alpha-capable targets) */
    uint32_t reserved[3];
} nue_tex_d3d9_texture_desc;
typedef struct nue_tex_d3d9_texture_info {
    uint32_t struct_size;
    nue_tex_format source_format;       /* the D3D9 format as a registry id */
    nue_tex_format gpu_format;          /* create the GPU texture with this (bgfx: nue_tex_format_to_bgfx) */
    nue_tex_action action;
    uint32_t gpu_levels;                /* levels to create on the GPU (after skip_mips) */
    uint32_t skip_mips;                 /* source levels 0..skip_mips-1 are ignored: GPU level 0 = source level skip_mips */
    uint32_t gpu_width, gpu_height;
    uint64_t gpu_bytes;                 /* whole chain, all faces */
    nue_bool needs_shader_swizzle;
    char shader_swizzle[5];
    uint8_t pad[2];
    nue_bool defer_to_worker;           /* encode is slow: convert on a worker (the layer shows a placeholder until the callback) */
    uint32_t reserved[4];
} nue_tex_d3d9_texture_info;
NUE_API nue_result NUE_CALL nue_tex_d3d9_texture_create(nue_tex_context ctx, const nue_tex_d3d9_texture_desc* desc, nue_tex_d3d9_texture* out_texture,
                                                         nue_tex_d3d9_texture_info* out_info, nue_tex_plan* out_plan);
NUE_API void NUE_CALL nue_tex_d3d9_texture_destroy(nue_tex_d3d9_texture texture);
/* Convert a rectangle (x,y,w,h in texels of the SOURCE level; must be block aligned for block formats unless it reaches the level edge) of source level
 * `level`/`face` from `src` (rows of blocks, `src_pitch` bytes) to the GPU format. Returns the number of bytes written in *out_size and the GPU row
 * pitch in *out_pitch; dst_capacity from nue_tex_d3d9_level_gpu_size. NUE_OK_NOOP when level < skip_mips (nothing to upload). When the plan needs a lossy encode this is a
 * CACHE LOOKUP by the content of the level (cold conversion): NUE_OK = served from the cache; NUE_OK_PARTIAL = not in the cache, `dst` holds zero blocks (a placeholder) and the level is
 * counted in nue_tex_runtime_stats.level_misses; with NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE the level is encoded on the spot instead. The GPU rectangle is
 * the source rectangle / 2^skip... (skip_mips > 0 only for whole-level uploads; the layer must pass the whole level then). */
NUE_API nue_result NUE_CALL nue_tex_d3d9_convert_level(nue_tex_d3d9_texture texture, uint32_t level, uint32_t face, const nue_rect_i* rect,
                                                        const void* src, size_t src_pitch, void* dst, size_t dst_capacity, uint32_t* out_pitch, size_t* out_size);
NUE_API size_t NUE_CALL nue_tex_d3d9_level_gpu_size(nue_tex_d3d9_texture texture, uint32_t level, const nue_rect_i* rect);   /* 0 if the level is skipped */
NUE_API nue_result NUE_CALL nue_tex_d3d9_texture_info_get(nue_tex_d3d9_texture texture, nue_tex_d3d9_texture_info* out);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_TEXTURE_H */
