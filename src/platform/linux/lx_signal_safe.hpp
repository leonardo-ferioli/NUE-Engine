// Formatting helpers that are safe to use inside a signal handler: no allocation, no stdio,
// no locale, fixed buffers only. Used by the crash handler and unit-tested on any host.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nue::lx {

// Appends to a caller-provided buffer; silently truncates (always keeps room for no NUL).
class FixedWriter {
public:
    FixedWriter(char* buf, size_t cap) : b_(buf), cap_(cap) {}
    void str(const char* s) {
        while (s && *s && n_ < cap_) b_[n_++] = *s++;
    }
    void chr(char c) {
        if (n_ < cap_) b_[n_++] = c;
    }
    void dec(int64_t v) {
        if (v < 0) {
            chr('-');
            udec(static_cast<uint64_t>(0) - static_cast<uint64_t>(v));
        } else {
            udec(static_cast<uint64_t>(v));
        }
    }
    void udec(uint64_t v) {
        char tmp[20];
        int k = 0;
        do {
            tmp[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k) chr(tmp[--k]);
    }
    // "0x" + at least min_digits hex digits.
    void hex(uint64_t v, int min_digits = 1) {
        str("0x");
        char tmp[16];
        int k = 0;
        do {
            tmp[k++] = "0123456789abcdef"[v & 15];
            v >>= 4;
        } while (v);
        while (k < min_digits && k < 16) tmp[k++] = '0';
        while (k) chr(tmp[--k]);
    }
    size_t size() const { return n_; }
    const char* data() const { return b_; }
    void reset() { n_ = 0; }

private:
    char* b_;
    size_t cap_;
    size_t n_ = 0;
};

const char* signal_name(int sig);  // "SIGSEGV", ... or "SIG?" (static strings)

}  // namespace nue::lx
