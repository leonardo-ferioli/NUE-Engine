// Sandbox paths, free space, backup exclusion, game-data validation, document-picker import with integrity check.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cstdio>
#include <cstring>

#include "nue_ios_internal.hpp"
#include "storage.hpp"

using namespace nue::ios;

namespace {

NSString* dir_for(NSSearchPathDirectory d) {
    NSArray<NSString*>* a = NSSearchPathForDirectoriesInDomains(d, NSUserDomainMask, YES);
    return a.count ? a[0] : nil;
}

bool ensure_dir(NSString* path) {
    NSError* err = nil;
    return [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:&err];
}

int copy_out(NSString* s, char* buf, size_t size) {
    if (!s || !buf || size == 0) return -1;
    const char* u = [s UTF8String];
    size_t n = std::strlen(u);
    if (n + 1 > size) return -1;
    std::memcpy(buf, u, n + 1);
    return static_cast<int>(n);
}

void append(char* report, size_t size, NSString* line) {
    if (!report || size == 0) return;
    size_t used = std::strlen(report);
    if (used + 1 >= size) return;
    std::snprintf(report + used, size - used, "%s\n", [line UTF8String]);
}

// Lists every regular file under root (relative paths, sizes).
std::vector<DirectoryEntry> list_tree(NSString* root) {
    std::vector<DirectoryEntry> out;
    NSFileManager* fm = [NSFileManager defaultManager];
    NSDirectoryEnumerator<NSString*>* en = [fm enumeratorAtPath:root];
    for (NSString* rel in en) {
        NSDictionary* attrs = [en fileAttributes];
        if ([attrs[NSFileType] isEqualToString:NSFileTypeRegular])
            out.push_back({[rel UTF8String], static_cast<uint64_t>([attrs fileSize])});
    }
    return out;
}

uint32_t crc_of_file(NSString* path, bool* ok) {
    FILE* f = std::fopen([path fileSystemRepresentation], "rb");
    if (!f) { *ok = false; return 0; }
    Crc32 c;
    std::vector<unsigned char> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) c.update(buf.data(), n);
    *ok = !std::ferror(f);
    std::fclose(f);
    return c.value();
}

// Copies one file in 1 MiB chunks, computing the CRC of what was read from the source; returns false on I/O error.
bool copy_file_crc(NSString* src, NSString* dst, uint32_t* crc, uint64_t* done, uint64_t total, int* last_percent) {
    FILE* in = std::fopen([src fileSystemRepresentation], "rb");
    if (!in) return false;
    FILE* out = std::fopen([dst fileSystemRepresentation], "wb");
    if (!out) { std::fclose(in); return false; }
    Crc32 c;
    std::vector<unsigned char> buf(1 << 20);
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        c.update(buf.data(), n);
        if (std::fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
        *done += n;
        int p = total ? static_cast<int>(*done * 100 / total) : 100;
        if (p != *last_percent) { *last_percent = p; nue::ios::detail::emit(NUE_IOS_EVENT_STORAGE_PROGRESS, p, static_cast<int64_t>(*done)); }
    }
    if (std::ferror(in)) ok = false;
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    *crc = c.value();
    return ok;
}

NueIosPickFolderFn g_pick_cb = nullptr;
void* g_pick_user = nullptr;
NSURL* g_scoped_url = nil;

}  // namespace

@interface NUEFolderPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end
@implementation NUEFolderPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url = urls.firstObject;
    if (g_scoped_url) { [g_scoped_url stopAccessingSecurityScopedResource]; g_scoped_url = nil; }
    if (url && [url startAccessingSecurityScopedResource]) {
        g_scoped_url = url;   // released by the next pick or by nue_ios_shutdown; the copy runs while it is held
        if (g_pick_cb) g_pick_cb([[url path] UTF8String], g_pick_user);
    } else if (g_pick_cb) {
        g_pick_cb(nullptr, g_pick_user);
    }
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    if (g_pick_cb) g_pick_cb(nullptr, g_pick_user);
}
@end

extern "C" {

int nue_ios_path(NueIosPathKind kind, char* buf, size_t size) {
    NSString* path = nil;
    switch (kind) {
    case NUE_IOS_PATH_GAME_ROOT: {
        NSString* docs = dir_for(NSDocumentDirectory);
        if (docs) path = [docs stringByAppendingPathComponent:[NSString stringWithUTF8String:nue::ios::detail::game_dir_name().c_str()]];
        break;
    }
    case NUE_IOS_PATH_SAVES: {
        NSString* as = dir_for(NSApplicationSupportDirectory);
        if (as) path = [[as stringByAppendingPathComponent:@"NUE"] stringByAppendingPathComponent:@"saves"];
        break;
    }
    case NUE_IOS_PATH_SHADER_CACHE: {
        NSString* c = dir_for(NSCachesDirectory);
        if (c) path = [[c stringByAppendingPathComponent:@"NUE"] stringByAppendingPathComponent:@"shaders"];
        break;
    }
    case NUE_IOS_PATH_LOGS: {
        NSString* c = dir_for(NSCachesDirectory);
        if (c) path = [[c stringByAppendingPathComponent:@"NUE"] stringByAppendingPathComponent:@"logs"];
        break;
    }
    case NUE_IOS_PATH_TEMP:
        path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"NUE"];
        break;
    }
    if (!path || !ensure_dir(path)) return -1;
    // Caches content is reproducible: never back it up. Saves and the game folder: the game folder is large (1.2 GB) and recoverable by
    // the player, so it is excluded too; only saves/settings go to iCloud backup.
    if (kind != NUE_IOS_PATH_SAVES) nue_ios_exclude_from_backup([path UTF8String], 1);
    return copy_out(path, buf, size);
}

int64_t nue_ios_free_space(void) {
    NSString* docs = dir_for(NSDocumentDirectory);
    if (!docs) return -1;
    NSError* err = nil;
    NSDictionary* v = [[NSURL fileURLWithPath:docs] resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:&err];
    NSNumber* n = v[NSURLVolumeAvailableCapacityForImportantUsageKey];
    return n ? [n longLongValue] : -1;
}

int nue_ios_exclude_from_backup(const char* path, int excluded) {
    if (!path) return -1;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError* err = nil;
    return [url setResourceValue:excluded ? @YES : @NO forKey:NSURLIsExcludedFromBackupKey error:&err] ? 0 : -1;
}

int nue_ios_validate_game_data(const char* required_csv, char* report, size_t size) {
    if (report && size) report[0] = 0;
    char root[1024];
    if (nue_ios_path(NUE_IOS_PATH_GAME_ROOT, root, sizeof root) < 0) return 1;
    std::vector<RequiredFile> req;
    NSString* csv = [NSString stringWithUTF8String:required_csv ? required_csv : ""];
    for (NSString* item in [csv componentsSeparatedByString:@","]) {
        if (item.length == 0) continue;
        NSRange colon = [item rangeOfString:@":" options:NSBackwardsSearch];
        RequiredFile r;
        if (colon.location != NSNotFound) {
            r.path = [[item substringToIndex:colon.location] UTF8String];
            r.min_size = strtoull([[item substringFromIndex:colon.location + 1] UTF8String], nullptr, 10);
        } else {
            r.path = [item UTF8String];
        }
        req.push_back(r);
    }
    auto listing = list_tree([NSString stringWithUTF8String:root]);
    ValidationReport v = validate_game_data(listing, req);
    for (auto& m : v.missing) append(report, size, [NSString stringWithFormat:@"missing: %s", m.c_str()]);
    for (auto& t : v.truncated) append(report, size, [NSString stringWithFormat:@"truncated: %s", t.c_str()]);
    return static_cast<int>(v.missing.size() + v.truncated.size());
}

int nue_ios_import_folder(const char* source, char* report, size_t size) {
    if (report && size) report[0] = 0;
    if (!source) return -1;
    NSString* src = [NSString stringWithUTF8String:source];
    char rootc[1024];
    if (nue_ios_path(NUE_IOS_PATH_GAME_ROOT, rootc, sizeof rootc) < 0) { append(report, size, @"cannot create game folder"); return -2; }
    NSString* dst_root = [NSString stringWithUTF8String:rootc];

    auto listing = list_tree(src);
    if (listing.empty()) { append(report, size, @"source folder is empty or unreadable"); return -3; }
    // If the player picked a parent of the game folder, import from the folder that holds the marker file.
    std::string prefix = find_game_root(listing, "SPEED2.EXE").value_or("");
    std::vector<DirectoryEntry> rooted;
    for (auto& e : listing) {
        auto n = normalize_relative_path(e.path);
        if (n && n->size() >= prefix.size() && n->compare(0, prefix.size(), prefix) == 0) rooted.push_back({n->substr(prefix.size()), e.size});
    }
    const int64_t free_bytes = nue_ios_free_space();
    ImportPlan plan = plan_import(rooted, free_bytes < 0 ? 0 : static_cast<uint64_t>(free_bytes));
    if (!plan.enough_space) {
        append(report, size, [NSString stringWithFormat:@"not enough free space: need %llu MB, have %lld MB",
                              plan.required_free_bytes >> 20, free_bytes >> 20]);
        return -4;
    }
    NSFileManager* fm = [NSFileManager defaultManager];
    uint64_t done = 0;
    int last_percent = -1;
    int failures = 0;
    for (const ImportItem& it : plan.items) {
        NSString* from = [[src stringByAppendingPathComponent:[NSString stringWithUTF8String:prefix.c_str()]]
                          stringByAppendingPathComponent:[NSString stringWithUTF8String:it.dest_relative.c_str()]];
        NSString* to = [dst_root stringByAppendingPathComponent:[NSString stringWithUTF8String:it.dest_relative.c_str()]];
        [fm createDirectoryAtPath:[to stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
        // Every file is copied again (no resume yet) and verified: CRC of what was read from the source against a re-read of the destination.
        // Files that live only in iCloud (not downloaded) would need NSFileCoordinator; not handled, the read fails and is reported.
        uint32_t src_crc = 0;
        if (!copy_file_crc(from, to, &src_crc, &done, plan.total_bytes, &last_percent)) {
            append(report, size, [NSString stringWithFormat:@"copy failed: %s", it.dest_relative.c_str()]);
            ++failures;
            continue;
        }
        bool ok = false;
        uint32_t dst_crc = crc_of_file(to, &ok);
        if (!ok || dst_crc != src_crc) {
            append(report, size, [NSString stringWithFormat:@"checksum mismatch: %s", it.dest_relative.c_str()]);
            [fm removeItemAtPath:to error:nil];
            ++failures;
        }
    }
    for (auto& r : plan.rejected) append(report, size, [NSString stringWithFormat:@"skipped unsafe path: %s", r.c_str()]);
    nue::ios::detail::emit(NUE_IOS_EVENT_STORAGE_PROGRESS, failures ? -1 : 100, static_cast<int64_t>(done));
    return failures ? -5 : 0;
}

void nue_ios_pick_folder(NueIosPickFolderFn callback, void* user) {
    g_pick_cb = callback;
    g_pick_user = user;
    static NUEFolderPickerDelegate* delegate = [NUEFolderPickerDelegate new];
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    UIWindow* window = nil;
    for (UIScene* s in [UIApplication sharedApplication].connectedScenes)
        if ([s isKindOfClass:[UIWindowScene class]]) { window = ((UIWindowScene*)s).windows.firstObject; if (window) break; }
    UIViewController* top = window.rootViewController;
    while (top.presentedViewController) top = top.presentedViewController;
    if (!top) { if (callback) callback(nullptr, user); return; }
    [top presentViewController:picker animated:YES completion:nil];
}

}  // extern "C"
