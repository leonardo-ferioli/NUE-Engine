// Direct3D 9's published numeric constants (D3DRS_*, D3DFMT_*, ...), written out so that the layer needs no SDK header.
#pragma once
#include <cstdint>

namespace nue::d3d9::k {

// D3DRENDERSTATETYPE
enum RenderState : uint32_t {
    rs_zenable = 7, rs_fillmode = 8, rs_shademode = 9, rs_zwriteenable = 14, rs_alphatestenable = 15, rs_lastpixel = 16, rs_srcblend = 19, rs_destblend = 20,
    rs_cullmode = 22, rs_zfunc = 23, rs_alpharef = 24, rs_alphafunc = 25, rs_ditherenable = 26, rs_alphablendenable = 27, rs_fogenable = 28,
    rs_specularenable = 29, rs_fogcolor = 34, rs_fogtablemode = 35, rs_fogstart = 36, rs_fogend = 37, rs_fogdensity = 38, rs_rangefogenable = 48,
    rs_stencilenable = 52, rs_stencilfail = 53, rs_stencilzfail = 54, rs_stencilpass = 55, rs_stencilfunc = 56, rs_stencilref = 57, rs_stencilmask = 58,
    rs_stencilwritemask = 59, rs_texturefactor = 60, rs_wrap0 = 128, rs_clipping = 136, rs_lighting = 137, rs_ambient = 139, rs_fogvertexmode = 140,
    rs_colorvertex = 141, rs_localviewer = 142, rs_normalizenormals = 143, rs_diffusematerialsource = 145, rs_specularmaterialsource = 146,
    rs_ambientmaterialsource = 147, rs_emissivematerialsource = 148, rs_vertexblend = 151, rs_clipplaneenable = 152, rs_pointsize = 154,
    rs_pointsize_min = 155, rs_pointspriteenable = 156, rs_pointscaleenable = 157, rs_pointscale_a = 158, rs_pointscale_b = 159, rs_pointscale_c = 160,
    rs_multisampleantialias = 161, rs_multisamplemask = 162, rs_patchedgestyle = 163, rs_pointsize_max = 166, rs_indexedvertexblendenable = 167,
    rs_colorwriteenable = 168, rs_tweenfactor = 170, rs_blendop = 171, rs_positiondegree = 172, rs_normaldegree = 173, rs_scissortestenable = 174,
    rs_slopescaledepthbias = 175, rs_antialiasedlineenable = 176, rs_twosidedstencilmode = 185, rs_ccw_stencilfail = 186, rs_ccw_stencilzfail = 187,
    rs_ccw_stencilpass = 188, rs_ccw_stencilfunc = 189, rs_colorwriteenable1 = 190, rs_colorwriteenable2 = 191, rs_colorwriteenable3 = 192,
    rs_blendfactor = 193, rs_srgbwriteenable = 194, rs_depthbias = 195, rs_wrap8 = 198, rs_separatealphablendenable = 206, rs_srcblendalpha = 207,
    rs_destblendalpha = 208, rs_blendopalpha = 209, rs_count = 256
};

// D3DSAMPLERSTATETYPE
enum SamplerState : uint32_t {
    ss_addressu = 1, ss_addressv = 2, ss_addressw = 3, ss_bordercolor = 4, ss_magfilter = 5, ss_minfilter = 6, ss_mipfilter = 7, ss_mipmaplodbias = 8,
    ss_maxmiplevel = 9, ss_maxanisotropy = 10, ss_srgbtexture = 11, ss_elementindex = 12, ss_dmapoffset = 13, ss_count = 14
};

// D3DTEXTURESTAGESTATETYPE
enum TextureStageState : uint32_t {
    tss_colorop = 1, tss_colorarg1 = 2, tss_colorarg2 = 3, tss_alphaop = 4, tss_alphaarg1 = 5, tss_alphaarg2 = 6, tss_bumpenvmat00 = 7, tss_bumpenvmat01 = 8,
    tss_bumpenvmat10 = 9, tss_bumpenvmat11 = 10, tss_texcoordindex = 11, tss_bumpenvlscale = 22, tss_bumpenvloffset = 23, tss_texturetransformflags = 24,
    tss_colorarg0 = 26, tss_alphaarg0 = 27, tss_resultarg = 28, tss_constant = 32, tss_count = 33
};

enum Blend : uint32_t { blend_zero = 1, blend_one, blend_srccolor, blend_invsrccolor, blend_srcalpha, blend_invsrcalpha, blend_destalpha, blend_invdestalpha, blend_destcolor,
                        blend_invdestcolor, blend_srcalphasat, blend_bothsrcalpha, blend_bothinvsrcalpha, blend_blendfactor, blend_invblendfactor };
enum BlendOp : uint32_t { blendop_add = 1, blendop_subtract, blendop_revsubtract, blendop_min, blendop_max };
enum Cmp : uint32_t { cmp_never = 1, cmp_less, cmp_equal, cmp_lessequal, cmp_greater, cmp_notequal, cmp_greaterequal, cmp_always };
enum Cull : uint32_t { cull_none = 1, cull_cw, cull_ccw };
enum StencilOp : uint32_t { stencil_keep = 1, stencil_zero, stencil_replace, stencil_incrsat, stencil_decrsat, stencil_invert, stencil_incr, stencil_decr };
enum Fill : uint32_t { fill_point = 1, fill_wireframe, fill_solid };
enum TexFilter : uint32_t { texf_none = 0, texf_point, texf_linear, texf_anisotropic, texf_pyramidalquad, texf_gaussianquad };
enum TexAddress : uint32_t { taddress_wrap = 1, taddress_mirror, taddress_clamp, taddress_border, taddress_mirroronce };
enum ClearFlags : uint32_t { clear_target = 1, clear_zbuffer = 2, clear_stencil = 4 };
enum Primitive : uint32_t { pt_pointlist = 1, pt_linelist, pt_linestrip, pt_trianglelist, pt_trianglestrip, pt_trianglefan };

enum Usage : uint32_t { usage_rendertarget = 1, usage_depthstencil = 2, usage_writeonly = 8, usage_softwareprocessing = 0x10, usage_dynamic = 0x200, usage_autogenmipmap = 0x400 };
enum Pool : uint32_t { pool_default = 0, pool_managed = 1, pool_systemmem = 2, pool_scratch = 3 };

constexpr uint32_t fourcc(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24; }
enum Format : uint32_t {
    fmt_unknown = 0, fmt_r8g8b8 = 20, fmt_a8r8g8b8 = 21, fmt_x8r8g8b8 = 22, fmt_r5g6b5 = 23, fmt_x1r5g5b5 = 24, fmt_a1r5g5b5 = 25, fmt_a4r4g4b4 = 26, fmt_r3g3b2 = 27,
    fmt_a8 = 28, fmt_a8r3g3b2 = 29, fmt_x4r4g4b4 = 30, fmt_a2b10g10r10 = 31, fmt_a8b8g8r8 = 32, fmt_x8b8g8r8 = 33, fmt_g16r16 = 34, fmt_a2r10g10b10 = 35,
    fmt_a16b16g16r16 = 36, fmt_a8p8 = 40, fmt_p8 = 41, fmt_l8 = 50, fmt_a8l8 = 51, fmt_a4l4 = 52, fmt_v8u8 = 60, fmt_l6v5u5 = 61, fmt_x8l8v8u8 = 62, fmt_q8w8v8u8 = 63,
    fmt_v16u16 = 64, fmt_a2w10v10u10 = 67, fmt_d16_lockable = 70, fmt_d32 = 71, fmt_d15s1 = 73, fmt_d24s8 = 75, fmt_d24x8 = 77, fmt_d24x4s4 = 79, fmt_d16 = 80,
    fmt_l16 = 81, fmt_d32f_lockable = 82, fmt_d24fs8 = 83, fmt_index16 = 101, fmt_index32 = 102, fmt_q16w16v16u16 = 110, fmt_r16f = 111, fmt_g16r16f = 112,
    fmt_a16b16g16r16f = 113, fmt_r32f = 114, fmt_g32r32f = 115, fmt_a32b32g32r32f = 116,
    fmt_dxt1 = fourcc('D', 'X', 'T', '1'), fmt_dxt2 = fourcc('D', 'X', 'T', '2'), fmt_dxt3 = fourcc('D', 'X', 'T', '3'), fmt_dxt4 = fourcc('D', 'X', 'T', '4'),
    fmt_dxt5 = fourcc('D', 'X', 'T', '5'), fmt_uyvy = fourcc('U', 'Y', 'V', 'Y'), fmt_yuy2 = fourcc('Y', 'U', 'Y', '2')
};

// D3DDECLTYPE and D3DDECLUSAGE
enum DeclType : uint8_t { dt_float1, dt_float2, dt_float3, dt_float4, dt_d3dcolor, dt_ubyte4, dt_short2, dt_short4, dt_ubyte4n, dt_short2n, dt_short4n, dt_ushort2n, dt_ushort4n,
                          dt_udec3, dt_dec3n, dt_float16_2, dt_float16_4, dt_unused };
enum DeclUsage : uint8_t { du_position, du_blendweight, du_blendindices, du_normal, du_psize, du_texcoord, du_tangent, du_binormal, du_tessfactor, du_positiont, du_color,
                           du_fog, du_depth, du_sample };

// D3DFVF_*
enum Fvf : uint32_t { fvf_xyz = 0x2, fvf_xyzrhw = 0x4, fvf_xyzb1 = 0x6, fvf_xyzb2 = 0x8, fvf_xyzb3 = 0xa, fvf_xyzb4 = 0xc, fvf_xyzb5 = 0xe, fvf_xyzw = 0x4002, fvf_posmask = 0x400e,
                      fvf_normal = 0x10, fvf_psize = 0x20, fvf_diffuse = 0x40, fvf_specular = 0x80, fvf_texcountmask = 0xf00, fvf_texcountshift = 8, fvf_lastbeta_ubyte4 = 0x1000 };

// Transform state types (D3DTS_*)
enum Transform : uint32_t { ts_view = 2, ts_projection = 3, ts_texture0 = 16, ts_world = 256 };

// Lock flags
enum LockFlag : uint32_t { lock_readonly = 0x10, lock_nooverwrite = 0x1000, lock_discard = 0x2000, lock_nodirtyupdate = 0x8000, lock_nosyslock = 0x800 };

}  // namespace nue::d3d9::k
