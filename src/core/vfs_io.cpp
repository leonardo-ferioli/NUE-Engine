// Async reads (I/O worker pool), manifest verification, file hashes and game data import.
#include "internal/vfs_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <vector>

using namespace nuei;

namespace {

// ============================================================================================ async I/O

struct IoReq {
    nue_io_request id = 0;
    nue_io_desc desc;
    std::shared_ptr<FileObj> file;
    int state = 0;                 // 0 queued, 1 running, 2 done
    bool cancelled = false;
    bool released = false;
    nue_result result = NUE_OK_PENDING;
    size_t bytes = 0;
};

struct IoPool {
    std::mutex mutex;
    std::condition_variable work_cv, done_cv;
    std::deque<std::shared_ptr<IoReq>> queue;
    std::map<uint32_t, std::shared_ptr<IoReq>> requests;
    std::vector<std::thread> workers;
    uint32_t next_id = 1;
    bool stop = false;
};

IoPool& pool() { static IoPool* p = new IoPool(); return *p; }

void io_worker() {
    set_thread_name("nue-io");
    IoPool& p = pool();
    for (;;) {
        std::shared_ptr<IoReq> r;
        {
            std::unique_lock<std::mutex> lock(p.mutex);
            p.work_cv.wait(lock, [&] { return p.stop || !p.queue.empty(); });
            if (p.stop && p.queue.empty()) return;
            // highest priority first, FIFO among equals
            auto best = p.queue.begin();
            for (auto it = p.queue.begin(); it != p.queue.end(); ++it) if ((*it)->desc.priority > (*best)->desc.priority) best = it;
            r = *best;
            p.queue.erase(best);
            if (r->cancelled) { r->state = 2; r->result = NUE_ERR_CANCELLED; }
            else r->state = 1;
        }
        if (r->state == 1) {
            size_t got = 0;
            nue_result res = os_file_read_at(r->file->handle, r->desc.offset, r->desc.buffer, r->desc.size, &got);
            if (NUE_SUCCEEDED(res)) res = (got == 0 && r->desc.size > 0) ? NUE_ERR_END_OF_DATA : got < r->desc.size ? NUE_OK_PARTIAL : NUE_OK;
            std::lock_guard<std::mutex> lock(p.mutex);
            r->bytes = got; r->result = res; r->state = 2;
        }
        // completion notifications outside the lock
        if (r->desc.callback) r->desc.callback(r->desc.userdata, r->id, r->result, r->bytes);
        if (r->desc.post_event) {
            nue_event e;
            std::memset(&e, 0, sizeof(e));
            e.type = NUE_EVENT_IO_COMPLETE;
            e.payload.user.code = r->result;
            e.payload.user.data1 = r->id;
            e.payload.user.data2 = r->bytes;
            nue_internal_push_event(&e);
        }
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            if (r->released) p.requests.erase(r->id);
        }
        p.done_cv.notify_all();
    }
}

void ensure_workers() {
    IoPool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.workers.empty()) return;
    p.stop = false;
    for (int i = 0; i < 2; ++i) p.workers.emplace_back(io_worker);
}

// ============================================================================================ manifests

struct ManifestEntry { std::string hash; uint64_t size = 0; std::string path; };

bool parse_manifest(const char* text, size_t length, std::vector<ManifestEntry>* out, std::string* error) {
    std::string all(text, length);
    size_t pos = 0;
    int line_no = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#') continue;
        line = line.substr(s);
        size_t a = line.find_first_of(" \t");
        size_t b = a == std::string::npos ? a : line.find_first_not_of(" \t", a);
        size_t c = b == std::string::npos ? b : line.find_first_of(" \t", b);
        size_t d = c == std::string::npos ? c : line.find_first_not_of(" \t", c);
        if (d == std::string::npos) { *error = "manifest line " + std::to_string(line_no) + ": expected '<hash> <size> <path>'"; return false; }
        ManifestEntry e;
        e.hash = line.substr(0, a);
        for (char& ch : e.hash) if (ch >= 'A' && ch <= 'F') ch = char(ch + 32);
        e.size = std::strtoull(line.substr(b, c - b).c_str(), nullptr, 10);
        e.path = line.substr(d);
        if (e.hash.size() != 8 && e.hash.size() != 64) { *error = "manifest line " + std::to_string(line_no) + ": hash must be 8 (crc32) or 64 (sha256) hex digits"; return false; }
        out->push_back(std::move(e));
    }
    return true;
}

// Hashes a native file; returns hex of the width of `want_len` (8 or 64).
nue_result hash_native(const std::string& native, size_t want_len, std::string* hex, const std::function<bool(uint64_t)>& progress) {
    OsHandle h = kBadOsHandle;
    nue_result r = os_file_open(native, NUE_OPEN_READ | NUE_OPEN_SEQUENTIAL, &h);
    if (NUE_FAILED(r)) return r;
    std::vector<uint8_t> buf(256 * 1024);
    nue_sha256_state st;
    nue_sha256_begin(&st);
    uint32_t crc = 0;
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        r = os_file_read_at(h, off, buf.data(), buf.size(), &got);
        if (NUE_FAILED(r)) { os_file_close(h); return r; }
        if (got == 0) break;
        if (want_len == 64) nue_sha256_update(&st, buf.data(), got); else crc = nue_crc32(crc, buf.data(), got);
        off += got;
        if (progress && !progress(got)) { os_file_close(h); return NUE_ERR_CANCELLED; }
    }
    os_file_close(h);
    char out[80];
    if (want_len == 64) { uint8_t dg[32]; nue_sha256_end(&st, dg); nue_hex_encode(dg, 32, out, sizeof(out)); }
    else { uint8_t b[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)}; nue_hex_encode(b, 4, out, sizeof(out)); }
    *hex = out;
    return NUE_OK;
}

// ============================================================================================ import

struct ImportJob {
    nue_import_id id = 0;
    nue_import_desc desc;
    std::string source, dest_root, manifest;
    std::thread worker;
    std::mutex mutex;
    nue_import_status status;
    std::atomic<bool> cancel{false};
    bool released = false;
};

struct ImportTable {
    std::mutex mutex;
    std::map<uint32_t, std::shared_ptr<ImportJob>> jobs;
    uint32_t next_id = 1;
};
ImportTable& imports() { static ImportTable* t = new ImportTable(); return *t; }

// Built-in directory source (any platform where the folder is readable with ordinary file access).
struct DirSource {
    std::vector<std::string> pending_dirs;      // relative dirs still to scan
    std::vector<std::pair<std::string, uint64_t>> files;   // relative path, size of the current directory's files
    size_t next_file = 0;
    std::string base;
    OsHandle current = kBadOsHandle;
    uint64_t offset = 0;
    ~DirSource() { os_file_close(current); }
};

nue_result dir_open(nue_import_source_kind, const char* source, void** out) {
    DirSource* s = new DirSource();
    s->base = source;
    std::replace(s->base.begin(), s->base.end(), '\\', '/');
    nue_stat st;
    if (!os_stat_path(s->base, &st) || st.kind != NUE_ENTRY_DIRECTORY) { delete s; return fail(NUE_ERR_NOT_FOUND, "import source '%s' is not a readable folder", source); }
    s->pending_dirs.push_back("");
    *out = s;
    return NUE_OK;
}

nue_result dir_next(void* src, nue_import_entry* e) {
    DirSource* s = static_cast<DirSource*>(src);
    os_file_close(s->current);
    s->current = kBadOsHandle;
    s->offset = 0;
    while (s->next_file >= s->files.size()) {
        if (s->pending_dirs.empty()) return NUE_ERR_END_OF_DATA;
        std::string rel = s->pending_dirs.back();
        s->pending_dirs.pop_back();
        std::vector<OsDirEntry> entries;
        if (!os_list_dir(native_join(s->base, rel), &entries)) continue;
        std::sort(entries.begin(), entries.end(), [](const OsDirEntry& a, const OsDirEntry& b) { return a.name < b.name; });
        s->files.clear();
        s->next_file = 0;
        for (const OsDirEntry& d : entries) {
            std::string r = rel.empty() ? d.name : rel + "/" + d.name;
            if (d.kind == NUE_ENTRY_DIRECTORY) s->pending_dirs.push_back(r);
            else if (d.kind == NUE_ENTRY_FILE) s->files.push_back({r, d.size});
        }
    }
    const auto& f = s->files[s->next_file++];
    NUE_STRUCT_INIT(e);
    std::snprintf(e->path, sizeof(e->path), "%s", f.first.c_str());
    e->size = f.second;
    nue_result r = os_file_open(native_join(s->base, f.first), NUE_OPEN_READ | NUE_OPEN_SEQUENTIAL, &s->current);
    return r;
}

nue_result dir_read(void* src, void* buffer, size_t cap, size_t* out_read) {
    DirSource* s = static_cast<DirSource*>(src);
    nue_result r = os_file_read_at(s->current, s->offset, buffer, cap, out_read);
    s->offset += *out_read;
    return r;
}

void dir_close(void* src) { delete static_cast<DirSource*>(src); }

const nue_import_source_ops g_dir_ops = {dir_open, dir_next, dir_read, dir_close, nullptr};

std::string fold_path(const std::string& p) { return fold_case_utf8(p); }

void set_status(ImportJob& j, const std::function<void(nue_import_status&)>& fn) {
    std::lock_guard<std::mutex> lock(j.mutex);
    fn(j.status);
}

void import_run(std::shared_ptr<ImportJob> job) {
    set_thread_name("nue-import");
    ImportJob& j = *job;
    const nue_platform_ops* plat = platform();
    const nue_import_source_ops* ops = j.desc.source_kind == NUE_IMPORT_SOURCE_PATH ? &g_dir_ops : (plat ? plat->import : nullptr);
    nue_result final_result = NUE_OK;
    std::string error;

    std::map<std::string, ManifestEntry> expected;   // folded path -> entry
    uint64_t bytes_total = 0;
    if (!j.manifest.empty()) {
        std::vector<ManifestEntry> v;
        if (!parse_manifest(j.manifest.data(), j.manifest.size(), &v, &error)) final_result = NUE_ERR_CORRUPT;
        for (auto& e : v) { bytes_total += e.size; expected[fold_path(e.path)] = e; }
    }
    void* source = nullptr;
    if (NUE_SUCCEEDED(final_result)) {
        if (!ops || !ops->open) { final_result = NUE_ERR_UNSUPPORTED; error = "this platform has no import source for that kind"; }
        else {
            std::string src = j.source;
            nue_result r = ops->open(j.desc.source_kind, src.c_str(), &source);
            if (NUE_FAILED(r)) { final_result = r; error = nue_get_last_error_message(); }
        }
    }
    set_status(j, [&](nue_import_status& st) { st.progress.bytes_total = bytes_total; st.progress.files_total = uint32_t(expected.size()); });

    int64_t last_progress_event = 0;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t bytes_done = 0;
    uint32_t files_done = 0;
    while (NUE_SUCCEEDED(final_result) && source) {
        if (j.cancel) { final_result = NUE_ERR_CANCELLED; break; }
        nue_import_entry entry;
        NUE_STRUCT_INIT(&entry);
        nue_result r = ops->next(source, &entry);
        if (r == NUE_ERR_END_OF_DATA) break;
        if (NUE_FAILED(r)) { final_result = r; error = nue_get_last_error_message(); break; }
        std::string rel = entry.path;
        const ManifestEntry* exp = nullptr;
        if (!expected.empty()) {
            auto it = expected.find(fold_path(rel));
            if (it == expected.end()) continue;   // a manifest lists exactly what is wanted
            exp = &it->second;
        }
        std::string dest_vfs = j.dest_root + ":" + rel;
        // resumable: skip files that are already complete
        if (j.desc.flags & NUE_IMPORT_SKIP_EXISTING) {
            nue_stat st;
            NUE_STRUCT_INIT(&st);
            if (NUE_SUCCEEDED(nue_vfs_stat(dest_vfs.c_str(), &st)) && st.kind == NUE_ENTRY_FILE && st.size == entry.size) {
                bool same = true;
                if (exp && (j.desc.flags & NUE_IMPORT_VERIFY)) {
                    std::string native, hex;
                    same = NUE_SUCCEEDED(resolve_path(dest_vfs, false, &native)) && NUE_SUCCEEDED(hash_native(native, exp->hash.size(), &hex, nullptr)) && hex == exp->hash;
                }
                if (same) { bytes_done += entry.size; ++files_done; continue; }
            }
        }
        std::string native_dest;
        r = resolve_path(dest_vfs, true, &native_dest);
        if (NUE_FAILED(r)) { final_result = r; error = nue_get_last_error_message(); break; }
        os_make_parent_dirs(native_dest);
        std::string part = native_dest + ".nue_part";
        OsHandle out = kBadOsHandle;
        r = os_file_open(part, NUE_OPEN_WRITE | NUE_OPEN_CREATE | NUE_OPEN_TRUNCATE, &out);
        if (NUE_FAILED(r)) { final_result = r; error = "cannot create '" + part + "'"; break; }
        nue_sha256_state sha;
        nue_sha256_begin(&sha);
        uint32_t crc = 0;
        uint64_t written = 0;
        for (;;) {
            if (j.cancel) { final_result = NUE_ERR_CANCELLED; break; }
            size_t got = 0;
            r = ops->read(source, buf.data(), buf.size(), &got);
            if (NUE_FAILED(r)) { final_result = r; error = "reading '" + rel + "' failed"; break; }
            if (got == 0) break;
            size_t put = 0;
            r = os_file_write_at(out, written, buf.data(), got, &put);
            if (NUE_FAILED(r) || put != got) { final_result = NUE_FAILED(r) ? r : NUE_ERR_IO; error = "writing '" + part + "' failed (disk full?)"; break; }
            written += got;
            bytes_done += got;
            if (exp && exp->hash.size() == 64) nue_sha256_update(&sha, buf.data(), got); else crc = nue_crc32(crc, buf.data(), got);
            int64_t now = nue_time_ns();
            if (now - last_progress_event > 100000000) {
                last_progress_event = now;
                nue_verify_progress prog;
                NUE_STRUCT_INIT(&prog);
                prog.files_total = uint32_t(expected.size()); prog.files_done = files_done; prog.bytes_total = bytes_total; prog.bytes_done = bytes_done;
                prog.current_path = rel.c_str();
                set_status(j, [&](nue_import_status& st) { st.progress = prog; st.progress.current_path = nullptr; });
                if (j.desc.progress && !j.desc.progress(j.desc.userdata, &prog)) { j.cancel = true; }
                nue_event ev;
                std::memset(&ev, 0, sizeof(ev));
                ev.type = NUE_EVENT_IMPORT_PROGRESS;
                ev.payload.user.data1 = j.id;
                ev.payload.user.data2 = bytes_done;
                nue_internal_push_event(&ev);
            }
        }
        if (NUE_SUCCEEDED(final_result)) os_file_sync(out);
        os_file_close(out);
        if (NUE_FAILED(final_result)) { os_remove_file(part); break; }
        if (written != entry.size && entry.size != 0) { os_remove_file(part); final_result = NUE_ERR_CORRUPT; error = "'" + rel + "': copied " + std::to_string(written) + " bytes, expected " + std::to_string(entry.size); break; }
        if (exp && (j.desc.flags & NUE_IMPORT_VERIFY)) {
            if (written != exp->size) { os_remove_file(part); final_result = NUE_ERR_CORRUPT; error = "'" + rel + "': size does not match the manifest"; break; }
            char hexbuf[80];
            if (exp->hash.size() == 64) { uint8_t dg[32]; nue_sha256_end(&sha, dg); nue_hex_encode(dg, 32, hexbuf, sizeof(hexbuf)); }
            else { uint8_t b[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)}; nue_hex_encode(b, 4, hexbuf, sizeof(hexbuf)); }
            if (exp->hash != hexbuf) { os_remove_file(part); final_result = NUE_ERR_CORRUPT; error = "'" + rel + "': hash does not match the manifest"; break; }
        }
        if (!os_rename_file(part, native_dest)) { os_remove_file(part); final_result = NUE_ERR_IO; error = "cannot move '" + part + "' into place"; break; }
        ++files_done;
    }
    if (source && ops && ops->close) ops->close(source);
    if (NUE_SUCCEEDED(final_result) && !expected.empty() && files_done < expected.size()) {
        final_result = NUE_ERR_NOT_FOUND;
        error = "the source is missing " + std::to_string(expected.size() - files_done) + " file(s) listed in the manifest";
    }
    if (NUE_SUCCEEDED(final_result)) {
        std::string marker = j.dest_root + ":.nue_import_complete";
        std::string text = "files=" + std::to_string(files_done) + "\nbytes=" + std::to_string(bytes_done) + "\n";
        nue_file_save(marker.c_str(), text.data(), text.size());
    }
    nue_vfs_clear_cache();
    set_status(j, [&](nue_import_status& st) {
        st.state = final_result;
        st.progress.files_done = files_done;
        st.progress.bytes_done = bytes_done;
        std::snprintf(st.last_error, sizeof(st.last_error), "%s", error.c_str());
    });
    if (NUE_FAILED(final_result)) NUE_LOGE("import", "import %u failed: %s (%s)", j.id, nue_result_name(final_result), error.c_str());
    else NUE_LOGI("import", "import %u finished: %u files, %llu bytes", j.id, files_done, (unsigned long long)bytes_done);
    nue_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = NUE_EVENT_IMPORT_FINISHED;
    ev.payload.user.code = final_result;
    ev.payload.user.data1 = j.id;
    ev.payload.user.data2 = bytes_done;
    nue_internal_push_event(&ev);
}

}  // namespace

namespace nuei {
void io_shutdown() {
    IoPool& p = pool();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.stop = true;
        workers.swap(p.workers);
    }
    p.work_cv.notify_all();
    for (auto& t : workers) t.join();
    { std::lock_guard<std::mutex> lock(p.mutex); p.queue.clear(); p.requests.clear(); }
    ImportTable& t = imports();
    std::map<uint32_t, std::shared_ptr<ImportJob>> jobs;
    { std::lock_guard<std::mutex> lock(t.mutex); jobs.swap(t.jobs); }
    for (auto& kv : jobs) { kv.second->cancel = true; if (kv.second->worker.joinable()) kv.second->worker.join(); }
}
}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_io_read_async(const nue_io_desc* desc, nue_io_request* out) {
    if (!desc || !out || !desc->buffer) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_io_read_async: desc, out and buffer are required");
    *out = 0;
    std::shared_ptr<FileObj> f = file_from_id(desc->file);
    if (!f) return fail(NUE_ERR_INVALID_HANDLE, "invalid file handle");
    if (!(f->flags & NUE_OPEN_READ)) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for reading");
    ensure_workers();
    IoPool& p = pool();
    auto r = std::make_shared<IoReq>();
    r->desc = *desc;
    r->file = f;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        r->id = p.next_id++;
        if (p.next_id == 0) p.next_id = 1;
        p.requests[r->id] = r;
        p.queue.push_back(r);
    }
    p.work_cv.notify_one();
    *out = r->id;
    return NUE_OK_PENDING;
}

NUE_API nue_result NUE_CALL nue_io_poll(nue_io_request id, size_t* out_bytes) {
    IoPool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    auto it = p.requests.find(id);
    if (it == p.requests.end()) return NUE_ERR_INVALID_HANDLE;
    if (it->second->state != 2) return NUE_OK_PENDING;
    if (out_bytes) *out_bytes = it->second->bytes;
    return it->second->result;
}

NUE_API nue_result NUE_CALL nue_io_wait(nue_io_request id, int64_t timeout_ns, size_t* out_bytes) {
    IoPool& p = pool();
    std::unique_lock<std::mutex> lock(p.mutex);
    auto it = p.requests.find(id);
    if (it == p.requests.end()) return NUE_ERR_INVALID_HANDLE;
    std::shared_ptr<IoReq> r = it->second;
    auto done = [&] { return r->state == 2; };
    if (timeout_ns < 0) p.done_cv.wait(lock, done);
    else if (!p.done_cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns), done)) return NUE_ERR_TIMEOUT;
    if (out_bytes) *out_bytes = r->bytes;
    return r->result;
}

NUE_API nue_result NUE_CALL nue_io_cancel(nue_io_request id) {
    IoPool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    auto it = p.requests.find(id);
    if (it == p.requests.end()) return NUE_ERR_INVALID_HANDLE;
    if (it->second->state != 0) return NUE_OK_NOOP;   // already running or done: cannot be taken back
    it->second->cancelled = true;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_io_release(nue_io_request id) {
    IoPool& p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    auto it = p.requests.find(id);
    if (it == p.requests.end()) return;
    if (it->second->state == 2) p.requests.erase(it); else it->second->released = true;
}

// ---- integrity ----------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_vfs_verify_manifest(const char* root_prefix, const char* text, size_t length, nue_progress_callback progress, void* ud, nue_verify_result* out) {
    if (!root_prefix || !text) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<ManifestEntry> entries;
    std::string error;
    if (!parse_manifest(text, length, &entries, &error)) return fail(NUE_ERR_CORRUPT, "%s", error.c_str());
    nue_verify_result res;
    NUE_STRUCT_INIT(&res);
    nue_verify_progress prog;
    NUE_STRUCT_INIT(&prog);
    prog.files_total = uint32_t(entries.size());
    for (const auto& e : entries) prog.bytes_total += e.size;
    auto note_bad = [&](const std::string& p) { if (!res.first_bad_path[0]) std::snprintf(res.first_bad_path, sizeof(res.first_bad_path), "%s", p.c_str()); };
    bool cancelled = false;
    for (const ManifestEntry& e : entries) {
        std::string vpath = std::string(root_prefix) + e.path;
        prog.current_path = e.path.c_str();
        std::string native;
        nue_stat st;
        if (NUE_FAILED(resolve_path(vpath, false, &native)) || !os_stat_path(native, &st)) { ++res.files_missing; note_bad(e.path); }
        else if (st.size != e.size) { ++res.files_size_mismatch; note_bad(e.path); }
        else {
            std::string hex;
            nue_result r = hash_native(native, e.hash.size(), &hex, [&](uint64_t n) {
                prog.bytes_done += n;
                return !progress || progress(ud, &prog) != 0;
            });
            if (r == NUE_ERR_CANCELLED) { cancelled = true; break; }
            if (NUE_FAILED(r)) { ++res.files_missing; note_bad(e.path); }
            else if (hex != e.hash) { ++res.files_hash_mismatch; note_bad(e.path); }
        }
        ++res.files_checked;
        ++prog.files_done;
        if (progress && !progress(ud, &prog)) { cancelled = true; break; }
    }
    if (out) { uint32_t keep = out->struct_size ? out->struct_size : uint32_t(sizeof(res)); std::memcpy(out, &res, keep < sizeof(res) ? keep : sizeof(res)); out->struct_size = keep; }
    if (cancelled) return NUE_ERR_CANCELLED;
    return (res.files_missing || res.files_size_mismatch || res.files_hash_mismatch) ? fail(NUE_ERR_CORRUPT, "%u missing, %u wrong size, %u wrong hash (first: %s)", res.files_missing, res.files_size_mismatch, res.files_hash_mismatch, res.first_bad_path) : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_hash_sha256(const char* path, uint8_t out_digest[32]) {
    std::string native, hex;
    nue_result r = resolve_path(path ? path : "", false, &native);
    if (NUE_FAILED(r)) return r;
    r = hash_native(native, 64, &hex, nullptr);
    if (NUE_FAILED(r)) return r;
    for (int i = 0; i < 32; ++i) out_digest[i] = uint8_t(std::stoi(hex.substr(size_t(i) * 2, 2), nullptr, 16));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_hash_crc32(const char* path, uint32_t* out_crc) {
    std::string native, hex;
    nue_result r = resolve_path(path ? path : "", false, &native);
    if (NUE_FAILED(r)) return r;
    r = hash_native(native, 8, &hex, nullptr);
    if (NUE_FAILED(r)) return r;
    *out_crc = uint32_t(std::stoul(hex, nullptr, 16));
    return NUE_OK;
}

// ---- import -------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_import_start(const nue_import_desc* desc, nue_import_id* out_id) {
    if (!desc || !out_id) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_import_start: desc and out_id are required");
    *out_id = 0;
    if (require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    auto job = std::make_shared<ImportJob>();
    job->desc = *desc;
    job->source = desc->source ? desc->source : "";
    job->dest_root = desc->dest_root && *desc->dest_root ? desc->dest_root : "data";
    if (desc->manifest_text && desc->manifest_length) job->manifest.assign(desc->manifest_text, desc->manifest_length);
    job->desc.source = nullptr; job->desc.dest_root = nullptr; job->desc.manifest_text = nullptr;
    NUE_STRUCT_INIT(&job->status);
    job->status.state = NUE_OK_PENDING;
    NUE_STRUCT_INIT(&job->status.progress);
    if (desc->source_kind != NUE_IMPORT_SOURCE_PATH) {
        const nue_platform_ops* p = platform();
        if (!p || !p->import) return fail(NUE_ERR_UNSUPPORTED, "this platform has no import source for URIs or pickers");
        if (desc->source_kind == NUE_IMPORT_SOURCE_PICKER) return fail(NUE_ERR_UNSUPPORTED, "use the platform's folder picker (pick_folder) and pass the URI with NUE_IMPORT_SOURCE_URI");
    } else if (job->source.empty()) return fail(NUE_ERR_INVALID_ARGUMENT, "NUE_IMPORT_SOURCE_PATH needs a source folder");
    if (!nue_vfs_has_root(job->dest_root.c_str())) return fail(NUE_ERR_NOT_FOUND, "destination root '%s' does not exist", job->dest_root.c_str());
    ImportTable& t = imports();
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        job->id = t.next_id++;
        t.jobs[job->id] = job;
    }
    job->worker = std::thread(import_run, job);
    *out_id = job->id;
    return NUE_OK_PENDING;
}

NUE_API nue_result NUE_CALL nue_import_get_status(nue_import_id id, nue_import_status* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    ImportTable& t = imports();
    std::shared_ptr<ImportJob> j;
    { std::lock_guard<std::mutex> lock(t.mutex); auto it = t.jobs.find(id); if (it == t.jobs.end()) return NUE_ERR_INVALID_HANDLE; j = it->second; }
    std::lock_guard<std::mutex> lock(j->mutex);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &j->status, keep < sizeof(j->status) ? keep : sizeof(j->status));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_import_cancel(nue_import_id id) {
    ImportTable& t = imports();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto it = t.jobs.find(id);
    if (it == t.jobs.end()) return NUE_ERR_INVALID_HANDLE;
    it->second->cancel = true;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_import_release(nue_import_id id) {
    ImportTable& t = imports();
    std::shared_ptr<ImportJob> j;
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        auto it = t.jobs.find(id);
        if (it == t.jobs.end()) return;
        j = it->second;
        t.jobs.erase(it);
    }
    j->cancel = true;
    if (j->worker.joinable()) j->worker.join();
}

NUE_API nue_bool NUE_CALL nue_import_is_complete(const char* dest_root) {
    std::string marker = std::string(dest_root && *dest_root ? dest_root : "data") + ":.nue_import_complete";
    return nue_vfs_exists(marker.c_str());
}

}  // extern "C"
