// Native side of the game-data import: runs ImportEngine over file descriptors handed out by the Java side
// (Storage Access Framework documents) or over a plain directory (drop folder).
#include <errno.h>
#include <unistd.h>

#include <cstring>
#include <memory>

#include "android_state.hpp"

namespace nue::android::native {
namespace {

class FdByteSource final : public ByteSource {
public:
    explicit FdByteSource(int fd) : fd_(fd) {}
    ~FdByteSource() override {
        if (fd_ >= 0) close(fd_);
    }
    int64_t read(void* dst, size_t size) override {
        for (;;) {
            ssize_t n = ::read(fd_, dst, size);
            if (n >= 0) return n;
            if (errno != EINTR) return -1;
        }
    }

private:
    int fd_;
};

// Files are listed by Java (DocumentsContract queries); the bytes are fetched one file at a time through
// NueBridge.openImportFd(index), which returns a detached file descriptor owned by us.
class JavaSource final : public ImportSource {
public:
    JavaSource(JNIEnv* env, std::vector<SourceFile> files) : env_(env), files_(std::move(files)) {}
    const std::vector<SourceFile>& files() const override { return files_; }
    std::unique_ptr<ByteSource> open(size_t index) override {
        State& st = state();
        if (!st.m.openImportFd) return nullptr;
        jint fd = env_->CallStaticIntMethod(st.bridge, st.m.openImportFd, static_cast<jint>(index));
        if (env_->ExceptionCheck()) {
            env_->ExceptionClear();
            return nullptr;
        }
        if (fd < 0) return nullptr;
        return std::make_unique<FdByteSource>(fd);
    }

private:
    JNIEnv* env_; // valid on the importing thread only, which is the only thread that calls open()
    std::vector<SourceFile> files_;
};

int32_t state_code(ImportPhase p) {
    switch (p) {
    case ImportPhase::Idle: return NUE_ANDROID_IMPORT_IDLE;
    case ImportPhase::Scanning: return NUE_ANDROID_IMPORT_SCANNING;
    case ImportPhase::Copying: return NUE_ANDROID_IMPORT_COPYING;
    case ImportPhase::Verifying: return NUE_ANDROID_IMPORT_VERIFYING;
    case ImportPhase::Done: return NUE_ANDROID_IMPORT_DONE;
    case ImportPhase::Cancelled: return NUE_ANDROID_IMPORT_CANCELLED;
    case ImportPhase::Failed: return NUE_ANDROID_IMPORT_FAILED;
    }
    return NUE_ANDROID_IMPORT_IDLE;
}

int run_import(ImportSource& source, const std::string& dest_root, const std::string& manifest_text, bool fsync_files) {
    State& st = state();
    st.import_running = true;

    Manifest manifest;
    const Manifest* mp = nullptr;
    if (!manifest_text.empty()) {
        manifest = parse_manifest(manifest_text);
        for (const std::string& e : manifest.errors) NLOGW("import manifest: %s", e.c_str());
        if (manifest.entries.empty()) {
            std::snprintf(st.import_last_text, sizeof st.import_last_text, "manifest has no usable entries");
            st.import_last_code = static_cast<int>(ImportResult::MissingRequired);
            st.events.push(make_event(NUE_ANDROID_EVENT_IMPORT, NUE_ANDROID_IMPORT_FAILED));
            st.import_running = false;
            return st.import_last_code;
        }
        mp = &manifest;
    }

    auto fs = make_std_file_system();
    ImportOptions opt;
    opt.fsync_files = fsync_files;
    ImportEngine engine(*fs, dest_root, opt);
    NLOGI("import: %zu source files -> %s (manifest: %zu entries)", source.files().size(), dest_root.c_str(),
          mp ? mp->entries.size() : size_t(0));

    int64_t last_log = 0;
    ImportReport rep = engine.run(
        source, mp,
        [&](const ImportProgress& p) {
            NueAndroidEvent e = make_event(NUE_ANDROID_EVENT_IMPORT, state_code(p.phase),
                                           static_cast<int32_t>(p.files_done), static_cast<int32_t>(p.files_total));
            e.l[0] = static_cast<int64_t>(p.bytes_done);
            e.l[1] = static_cast<int64_t>(p.bytes_total);
            state().events.push(e);
            int64_t now = now_ms();
            if (now - last_log > 2000 || p.phase == ImportPhase::Done) {
                last_log = now;
                NLOGI("import: %u/%u files, %llu/%llu MiB %s", p.files_done, p.files_total,
                      (unsigned long long)(p.bytes_done >> 20), (unsigned long long)(p.bytes_total >> 20), p.current);
            }
        },
        st.import_cancel);

    std::snprintf(st.import_last_text, sizeof st.import_last_text, "%s%s%s", to_string(rep.result),
                  rep.failed_file.empty() ? "" : ": ", rep.failed_file.c_str());
    if (rep.result == ImportResult::NoSpace && rep.space.shortfall)
        std::snprintf(st.import_last_text, sizeof st.import_last_text, "not enough free space: %llu MiB more needed",
                      (unsigned long long)((rep.space.shortfall + (1u << 20) - 1) >> 20));
    if (rep.result == ImportResult::MissingRequired && !rep.missing_required.empty())
        std::snprintf(st.import_last_text, sizeof st.import_last_text, "required file missing: %s (+%zu more)",
                      rep.missing_required[0].c_str(), rep.missing_required.size() - 1);
    st.import_last_code = static_cast<int>(rep.result);
    NLOGI("import finished: %s (copied %u, resumed %u, %llu MiB)", st.import_last_text, rep.files_copied, rep.files_resumed,
          (unsigned long long)(rep.bytes_copied >> 20));
    st.import_running = false;
    return st.import_last_code;
}

} // namespace

int import_run_from_java(JNIEnv* env, jstring dest_root, jstring manifest_text, jobjectArray paths, jlongArray sizes,
                         jboolean fsync_files) {
    auto to_std = [&](jstring s) {
        std::string out;
        if (!s) return out;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) out = c;
        if (c) env->ReleaseStringUTFChars(s, c);
        return out;
    };
    jsize n = paths ? env->GetArrayLength(paths) : 0;
    std::vector<jlong> sz(n);
    if (n) env->GetLongArrayRegion(sizes, 0, n, sz.data());
    std::vector<SourceFile> files;
    files.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        jstring p = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        files.push_back({to_std(p), static_cast<uint64_t>(sz[i])});
        env->DeleteLocalRef(p);
    }
    JavaSource source(env, std::move(files));
    return run_import(source, to_std(dest_root), to_std(manifest_text), fsync_files);
}

int import_run_from_directory(const std::string& dir, const std::string& dest_root, const std::string& manifest_text,
                              bool fsync_files) {
    auto source = make_directory_source(dir);
    return run_import(*source, dest_root, manifest_text, fsync_files);
}

} // namespace nue::android::native
