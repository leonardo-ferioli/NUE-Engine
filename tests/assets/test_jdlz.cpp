// JDLZ codec: hand written streams, round trips of generated data at every level, malformed input, determinism, and (opt-in) the real game files.
#include "asset_common.hpp"

using namespace asstest;

// Independent reference decoder written straight from the format description in docs/ASSET_FORMATS.md (it does not share code with the library).
static bool reference_decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 18 || std::memcmp(in.data(), "JDLZ", 4) != 0) return false;
    const size_t length = get32(&in[8]), end = get32(&in[12]);
    out.clear();
    size_t cur = 18;
    unsigned flags = in[16] | 256u, kind = in[17] | 256u;
    while (cur < end && out.size() < length) {
        if (!(flags & 1)) out.push_back(in[cur++]);
        else {
            unsigned f = in[cur], s = in[cur + 1];
            cur += 2;
            size_t count, dist;
            if (kind & 1) { count = (((f & 0xF0) << 4) | s) + 3; dist = (f & 15) + 1; }
            else { count = (f & 31) + 3; dist = (((f & 0xE0) << 3) | s) + 17; }
            kind >>= 1;
            if (dist > out.size()) return false;
            for (size_t i = 0; i < count; ++i) out.push_back(out[out.size() - dist]);
        }
        if (out.size() >= length) break;
        flags >>= 1;
        if (flags == 1) flags = in[cur++] | 256u;
        if (kind == 1) kind = in[cur++] | 256u;
    }
    out.resize(length);
    return true;
}

static std::vector<uint8_t> compress(const std::vector<uint8_t>& src, int level) {
    std::vector<uint8_t> out(nue_jdlz_compress_bound(src.size()));
    size_t n = 0;
    CHECK_EQ(nue_jdlz_compress(src.data(), src.size(), level, out.data(), out.size(), &n), NUE_OK);
    out.resize(n);
    return out;
}

static std::vector<uint8_t> decompress(const std::vector<uint8_t>& packed, size_t expect, nue_result* res = nullptr) {
    std::vector<uint8_t> out(expect);
    size_t n = 0;
    nue_result r = nue_jdlz_decompress(packed.data(), packed.size(), out.data(), out.size(), &n);
    if (res) *res = r;
    out.resize(n);
    return out;
}

static std::vector<uint8_t> make_data(int kind, size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t s = 12345 + uint32_t(kind);
    static const char words[6][6] = {"chunk", "textu", "globa", "pipel", {0, 0, 0, 0, 0, 0}, {char(0x80), 0x13, 0, 0, 0, 0}};
    for (size_t i = 0; i < n; ++i) {
        switch (kind) {
            case 0: v[i] = 0; break;                                           // one long run
            case 1: v[i] = uint8_t(i); break;                                  // ramp
            case 2: v[i] = uint8_t(rng_next(s)); break;                        // noise (incompressible)
            case 3: v[i] = uint8_t("the quick brown fox jumps over the lazy dog. "[i % 45]); break;   // short period
            case 4: v[i] = uint8_t((i / 7) ^ (i % 5)); break;                  // structured
            case 5: v[i] = uint8_t(words[(i / 9) % 6][i % 5]); break;          // word soup
            case 6: v[i] = (rng_next(s) & 7) ? 0x11 : uint8_t(rng_next(s)); break;   // mostly filler with noise
            default: v[i] = uint8_t(i % 3 == 0 ? rng_next(s) : i); break;
        }
    }
    return v;
}

TEST_CASE(hand_written_stream) {
    // "ABCABCABC": three literals, then a near match (distance 3, count 6). Flag bits 0,0,0,1 -> 0x08; kind bit for the match -> 0x01.
    std::vector<uint8_t> s = {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0, 9, 0, 0, 0, 23, 0, 0, 0, 0x08, 0x01, 'A', 'B', 'C', 0x02, 0x03};
    nue_result r;
    auto out = decompress(s, 9, &r);
    CHECK_EQ(r, NUE_OK);
    CHECK(std::string(out.begin(), out.end()) == "ABCABCABC");
    uint32_t unc = 0, stream = 0;
    CHECK_EQ(nue_jdlz_identify(s.data(), s.size(), &unc, &stream), NUE_OK);
    CHECK_EQ(unc, 9u);
    CHECK_EQ(stream, 23u);

    // far match: 17 literals, then a match with distance 17 and count 5 (f = count-3 = 2, s = distance-17 = 0).
    // The second flags byte is fetched right after the 8th token, the third after the 16th.
    std::vector<uint8_t> t = {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0, 22, 0, 0, 0, 0, 0, 0, 0};
    t.push_back(0x00);                      // flags A for tokens 0..7
    t.push_back(0x00);                      // flags B: the far match is the first match -> bit 0 = 0
    for (int i = 0; i < 8; ++i) t.push_back(uint8_t('a' + i));
    t.push_back(0x00);                      // flags A for tokens 8..15
    for (int i = 8; i < 16; ++i) t.push_back(uint8_t('a' + i));
    t.push_back(0x02);                      // flags A for tokens 16..: token 16 literal (0), token 17 match (1)
    t.push_back('a' + 16);
    t.push_back(0x02);
    t.push_back(0x00);
    t[12] = uint8_t(t.size());
    auto out2 = decompress(t, 22, &r);
    CHECK_EQ(r, NUE_OK);
    std::string expect;
    for (int i = 0; i < 17; ++i) expect += char('a' + i);
    expect += expect.substr(0, 5);
    CHECK(std::string(out2.begin(), out2.end()) == expect);
    std::vector<uint8_t> ref;
    CHECK(reference_decode(t, ref));
    CHECK(std::string(ref.begin(), ref.end()) == expect);
}

TEST_CASE(round_trips) {
    const size_t sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 18, 33, 255, 256, 1000, 4097, 5000, 70000};
    const int levels[] = {0, 1, 3, 6, 7, 9};
    for (int kind = 0; kind < 8; ++kind)
        for (size_t n : sizes) {
            auto src = make_data(kind, n);
            for (int level : levels) {
                auto packed = compress(src, level);
                CHECK(packed.size() <= nue_jdlz_compress_bound(n));
                nue_result r;
                auto out = decompress(packed, n, &r);
                CHECK_EQ(r, NUE_OK);
                CHECK(out == src);
                std::vector<uint8_t> ref;
                CHECK(reference_decode(packed, ref));
                CHECK(ref == src);
                uint32_t unc = 0, stream = 0;
                CHECK_EQ(nue_jdlz_identify(packed.data(), packed.size(), &unc, &stream), NUE_OK);
                CHECK_EQ(unc, uint32_t(n));
                CHECK_EQ(stream, uint32_t(packed.size()));
            }
        }
}

TEST_CASE(match_length_and_distance_limits) {
    // runs (near matches up to 4098 long) and repeats exactly at the distance limits 16/17 and 2064/2065
    std::vector<uint8_t> a(10000, 'x');
    for (int level : {0, 6, 9}) { auto p = compress(a, level); CHECK(decompress(p, a.size()) == a); CHECK(p.size() < 200); }
    for (uint32_t gap : {15u, 16u, 17u, 18u, 2063u, 2064u, 2065u, 2066u}) {
        std::vector<uint8_t> v;
        uint32_t s = gap;
        for (uint32_t i = 0; i < gap; ++i) v.push_back(uint8_t(rng_next(s) | 1));
        std::vector<uint8_t> head(v.begin(), v.begin() + 6);
        v.insert(v.end(), head.begin(), head.end());            // a 6 byte repeat exactly `gap` bytes back
        v.insert(v.end(), head.begin(), head.end());
        for (int level : {0, 6, 9}) { auto p = compress(v, level); CHECK(decompress(p, v.size()) == v); }
    }
}

TEST_CASE(compression_actually_compresses) {
    auto z = make_data(0, 100000);
    CHECK(compress(z, 6).size() < 300);
    auto t = make_data(3, 100000);
    CHECK(compress(t, 6).size() < 12000);
    auto n = make_data(2, 20000);
    CHECK(compress(n, 6).size() <= nue_jdlz_compress_bound(n.size()));
    auto lo = compress(make_data(5, 60000), 1), hi = compress(make_data(5, 60000), 9);
    CHECK(hi.size() <= lo.size());          // more effort never loses
}

TEST_CASE(deterministic_output) {
    auto src = make_data(4, 30000);
    for (int level : {0, 4, 6, 9}) CHECK(compress(src, level) == compress(src, level));
    std::vector<uint8_t> in(40);
    for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t("abcabcabd"[i % 9]);
    auto p = compress(in, 6);
    CHECK(decompress(p, in.size()) == in);
    CHECK_EQ(get32(&p[8]), 40u);
    CHECK_EQ(get32(&p[12]), uint32_t(p.size()));
    CHECK(p.size() < in.size());
}

TEST_CASE(malformed_input) {
    auto src = make_data(3, 5000);
    auto good = compress(src, 6);
    nue_result r;
    auto bad = good;
    bad[0] = 'X';
    decompress(bad, src.size(), &r); CHECK_EQ(r, NUE_ERR_UNSUPPORTED);
    bad = good; bad[4] = 3;
    decompress(bad, src.size(), &r); CHECK_EQ(r, NUE_ERR_UNSUPPORTED);
    decompress(good, src.size() - 1, &r); CHECK_EQ(r, NUE_ERR_BUFFER_TOO_SMALL);
    bad.assign(good.begin(), good.begin() + good.size() / 2);                   // truncated stream
    decompress(bad, src.size(), &r); CHECK(r < 0);
    bad.assign(good.begin(), good.begin() + 10);                                // shorter than a header
    decompress(bad, src.size(), &r); CHECK(r < 0);
    bad = good; bad[8] = uint8_t(bad[8] + 100);                                 // header claims more output than the stream holds
    std::vector<uint8_t> out(src.size() + 200);
    size_t n = 0;
    r = nue_jdlz_decompress(bad.data(), bad.size(), out.data(), out.size(), &n);
    CHECK(r < 0);
    // a match that reaches back before the start of the output
    std::vector<uint8_t> s = {'J', 'D', 'L', 'Z', 2, 0x10, 0, 0, 9, 0, 0, 0, 22, 0, 0, 0, 0x01, 0x01, 0x02, 0x03, 0, 0};
    decompress(s, 9, &r); CHECK_EQ(r, NUE_ERR_CORRUPT);
    // flipped bits: never crashes, never writes outside the buffer (guard bytes behind it stay untouched)
    uint32_t seed = 99;
    for (int i = 0; i < 2000; ++i) {
        auto m = good;
        m[rng_next(seed) % m.size()] ^= uint8_t(1u << (rng_next(seed) & 7));
        std::vector<uint8_t> guarded(src.size() + 32, 0xAB);
        size_t w = 0;
        nue_jdlz_decompress(m.data(), m.size(), guarded.data(), src.size(), &w);
        for (size_t k = src.size(); k < guarded.size(); ++k) CHECK_EQ(guarded[k], 0xAB);
        if (nue_test::failures() > 50) break;
    }
}

TEST_CASE(real_game_files) {
    if (!game_dir()) { std::printf("  (skipped: set NUE_TEST_GAME_DIR to run on the game files)\n"); return; }
    std::vector<uint8_t> lzc, bun;
    if (!load_file(game_path("GLOBAL/InGameCommon.lzc"), lzc) || !load_file(game_path("GLOBAL/InGameCommon.bun"), bun)) { std::printf("  (skipped: files not found)\n"); return; }
    // the game's compressed file decodes to the 1,595,640 byte reference (the bytes the game's own decompressor produces)
    nue_result r;
    auto out = decompress(lzc, bun.size(), &r);
    CHECK_EQ(r, NUE_OK);
    CHECK_EQ(out.size(), size_t(1595640));
    CHECK(out == bun);
    std::vector<uint8_t> ref;
    CHECK(reference_decode(lzc, ref));
    CHECK(ref == bun);
    // our compressor on the same data: decodes back exactly with both decoders
    for (int level : {1, 6, 9}) {
        auto packed = compress(bun, level);
        auto back = decompress(packed, bun.size(), &r);
        CHECK_EQ(r, NUE_OK);
        CHECK(back == bun);
        std::vector<uint8_t> ref2;
        CHECK(reference_decode(packed, ref2));
        CHECK(ref2 == bun);
        std::printf("  level %d: %zu -> %zu bytes (%.1f%%; the game file: %zu = %.1f%%)\n", level, bun.size(), packed.size(), 100.0 * packed.size() / bun.size(), lzc.size(), 100.0 * lzc.size() / bun.size());
    }
}

TEST_MAIN_BEGIN
    RUN(hand_written_stream);
    RUN(round_trips);
    RUN(match_length_and_distance_limits);
    RUN(compression_actually_compresses);
    RUN(deterministic_output);
    RUN(malformed_input);
    RUN(real_game_files);
TEST_MAIN_END
