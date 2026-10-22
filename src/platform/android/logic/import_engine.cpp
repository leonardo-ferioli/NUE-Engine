#include "import_engine.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "crc32.hpp"

namespace nue::android {

const char* to_string(ImportResult r) {
    switch (r) {
    case ImportResult::Ok: return "ok";
    case ImportResult::Cancelled: return "cancelled";
    case ImportResult::NoSpace: return "not enough free space";
    case ImportResult::ReadError: return "read error";
    case ImportResult::WriteError: return "write error";
    case ImportResult::CrcMismatch: return "checksum mismatch";
    case ImportResult::SizeMismatch: return "size mismatch";
    case ImportResult::BadPath: return "unsafe path in source";
    case ImportResult::NameCollision: return "two files map to the same name";
    case ImportResult::MissingRequired: return "required files missing";
    }
    return "?";
}

ImportEngine::ImportEngine(FileSystem& fs, std::string dest_root, ImportOptions options)
    : fs_(fs), root_(std::move(dest_root)), opt_(options), layout_(ImportLayout::for_root(root_)) {}

namespace {

struct Planned {
    size_t source_index = 0;
    std::string rel;          // sanitised destination, relative to the root
    uint64_t size = 0;
    const ManifestEntry* manifest = nullptr;
};

} // namespace

ImportReport ImportEngine::run(ImportSource& source, const Manifest* manifest, const ProgressFn& progress,
                               const std::atomic<bool>& cancel) {
    ImportReport rep;
    ImportProgress prog;
    auto emit = [&](ImportPhase phase, const std::string& current) {
        prog.phase = phase;
        prog.current = current.c_str();
        if (progress) progress(prog);
    };
    auto finish = [&](ImportResult r, ImportPhase phase) {
        rep.result = r;
        emit(phase, rep.failed_file);
        return rep;
    };

    emit(ImportPhase::Scanning, "");

    // ---- plan: sanitise names, apply the manifest, detect collisions and missing files ----------------
    std::vector<Planned> plan;
    std::unordered_map<std::string, size_t> by_dest;
    const std::vector<SourceFile>& listed = source.files();
    for (size_t i = 0; i < listed.size(); ++i) {
        SafePath sp = sanitize_relative_path(listed[i].path, opt_.name_case);
        if (sp.status != PathStatus::Ok) {
            rep.bad_paths.push_back(listed[i].path);
            continue;
        }
        const ManifestEntry* me = nullptr;
        if (manifest) {
            me = manifest->find(sp.path);
            if (!me) continue; // not needed by the game
        }
        auto ins = by_dest.emplace(sp.path, plan.size());
        if (!ins.second) {
            rep.collisions.push_back(listed[i].path);
            continue;
        }
        plan.push_back({i, std::move(sp.path), listed[i].size, me});
    }
    if (!rep.bad_paths.empty() && !manifest) return finish(ImportResult::BadPath, ImportPhase::Failed);
    if (!rep.collisions.empty()) return finish(ImportResult::NameCollision, ImportPhase::Failed);
    if (manifest) {
        for (const ManifestEntry& e : manifest->entries) {
            if (!e.optional && !by_dest.count(e.path)) rep.missing_required.push_back(e.path);
        }
        if (!rep.missing_required.empty()) return finish(ImportResult::MissingRequired, ImportPhase::Failed);
        for (const Planned& p : plan) {
            if (p.manifest->size != p.size) {
                rep.failed_file = p.rel;
                return finish(ImportResult::SizeMismatch, ImportPhase::Failed);
            }
        }
    }
    std::sort(plan.begin(), plan.end(), [](const Planned& a, const Planned& b) { return a.rel < b.rel; });

    // ---- resume: what an earlier, interrupted run already finished ------------------------------------
    std::unordered_map<std::string, JournalEntry> done;
    {
        std::string jtext;
        if (fs_.read_text(layout_.journal, &jtext)) {
            for (JournalEntry& e : parse_journal(jtext)) done[e.path] = std::move(e);
        }
    }
    auto already_done = [&](const Planned& p) {
        auto it = done.find(p.rel);
        if (it == done.end() || it->second.size != p.size) return false;
        if (p.manifest && p.manifest->has_crc && opt_.verify_crc && p.manifest->crc != it->second.crc) return false;
        uint64_t on_disk = 0;
        return fs_.file_size(join_path(layout_.root, p.rel), &on_disk) && on_disk == p.size;
    };

    uint64_t to_copy = 0;
    prog.files_total = static_cast<uint32_t>(plan.size());
    for (const Planned& p : plan) {
        prog.bytes_total += p.size;
        if (!already_done(p)) to_copy += p.size;
    }
    rep.files_total = prog.files_total;
    rep.bytes_total = prog.bytes_total;

    if (!fs_.make_dirs(layout_.root) || !fs_.make_dirs(layout_.staging)) return finish(ImportResult::WriteError, ImportPhase::Failed);
    fs_.remove(layout_.complete); // a half-updated tree must not look complete

    uint64_t free_bytes = 0;
    if (fs_.free_space(layout_.root, &free_bytes)) {
        rep.space = check_free_space(to_copy, free_bytes);
        if (rep.space.result == SpaceCheck::NotEnough) return finish(ImportResult::NoSpace, ImportPhase::Failed);
    }

    // ---- copy ----------------------------------------------------------------------------------------
    std::vector<unsigned char> buf(opt_.buffer_size);
    uint64_t next_report = 0;
    for (const Planned& p : plan) {
        if (cancel.load(std::memory_order_relaxed)) return finish(ImportResult::Cancelled, ImportPhase::Cancelled);
        const std::string final_path = join_path(layout_.root, p.rel);
        if (already_done(p)) {
            ++rep.files_resumed;
            ++prog.files_done;
            prog.bytes_done += p.size;
            continue;
        }
        rep.failed_file = p.rel;
        emit(ImportPhase::Copying, p.rel);

        std::string dir = parent_path(final_path);
        if (!dir.empty() && !fs_.make_dirs(dir)) return finish(ImportResult::WriteError, ImportPhase::Failed);
        std::unique_ptr<ByteSource> in = source.open(p.source_index);
        if (!in) return finish(ImportResult::ReadError, ImportPhase::Failed);
        const std::string part = layout_.part_path(final_path);
        std::unique_ptr<WritableFile> out = fs_.create(part);
        if (!out) return finish(ImportResult::WriteError, ImportPhase::Failed);

        Crc32 crc;
        uint64_t written = 0;
        ImportResult err = ImportResult::Ok;
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) {
                err = ImportResult::Cancelled;
                break;
            }
            int64_t n = in->read(buf.data(), buf.size());
            if (n < 0) {
                err = ImportResult::ReadError;
                break;
            }
            if (n == 0) break;
            WriteStatus ws = out->write(buf.data(), static_cast<size_t>(n));
            if (ws != WriteStatus::Ok) {
                err = ws == WriteStatus::NoSpace ? ImportResult::NoSpace : ImportResult::WriteError;
                break;
            }
            crc.update(buf.data(), static_cast<size_t>(n));
            written += static_cast<uint64_t>(n);
            prog.bytes_done += static_cast<uint64_t>(n);
            if (prog.bytes_done >= next_report) {
                next_report = prog.bytes_done + opt_.progress_step_bytes;
                emit(ImportPhase::Copying, p.rel);
            }
        }
        if (err == ImportResult::Ok && written != p.size) err = ImportResult::SizeMismatch;
        if (err == ImportResult::Ok && p.manifest && p.manifest->has_crc && opt_.verify_crc &&
            crc.value() != p.manifest->crc)
            err = ImportResult::CrcMismatch;
        bool closed = out->close(err == ImportResult::Ok && opt_.fsync_files);
        out.reset();
        if (err == ImportResult::Ok && !closed) err = ImportResult::WriteError;
        if (err != ImportResult::Ok) {
            fs_.remove(part);
            if (err == ImportResult::Cancelled) return finish(err, ImportPhase::Cancelled);
            return finish(err, ImportPhase::Failed);
        }
        if (!fs_.rename(part, final_path)) {
            fs_.remove(part);
            return finish(ImportResult::WriteError, ImportPhase::Failed);
        }
        fs_.append_text(layout_.journal, format_journal_line({p.rel, p.size, crc.value()}), opt_.fsync_files);
        ++rep.files_copied;
        rep.bytes_copied += p.size;
        ++prog.files_done;
        rep.failed_file.clear();
    }

    // ---- seal ----------------------------------------------------------------------------------------
    emit(ImportPhase::Verifying, "");
    if (!fs_.write_text_atomic(layout_.complete,
                               make_complete_marker(opt_.marker_version, prog.files_total, prog.bytes_total)))
        return finish(ImportResult::WriteError, ImportPhase::Failed);
    fs_.remove(layout_.journal);
    return finish(ImportResult::Ok, ImportPhase::Done);
}

bool ImportEngine::is_complete(FileSystem& fs, const std::string& dest_root, uint32_t* files, uint64_t* bytes) {
    ImportLayout l = ImportLayout::for_root(dest_root);
    std::string text;
    if (!fs.read_text(l.complete, &text)) return false;
    uint32_t v = 0, f = 0;
    uint64_t b = 0;
    if (!parse_complete_marker(text, &v, &f, &b)) return false;
    if (files) *files = f;
    if (bytes) *bytes = b;
    return true;
}

bool ImportEngine::verify_installed(FileSystem& fs, const std::string& dest_root, const Manifest& manifest,
                                    std::vector<std::string>* problems) {
    ImportLayout l = ImportLayout::for_root(dest_root);
    bool ok = true;
    for (const ManifestEntry& e : manifest.entries) {
        uint64_t sz = 0;
        if (!fs.file_size(join_path(l.root, e.path), &sz)) {
            if (e.optional) continue;
            ok = false;
            if (problems) problems->push_back("missing: " + e.path);
        } else if (sz != e.size) {
            ok = false;
            if (problems) problems->push_back("size: " + e.path);
        }
    }
    return ok;
}

} // namespace nue::android
