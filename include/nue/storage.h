/*
 * NUE engine - storage: a virtual file system over named roots, with the path rules a Windows game expects
 * (case-insensitive lookup, '\' accepted as separator) on every platform, plus the per-platform game data import.
 *
 * Paths
 *   A VFS path is  "root:relative/path"  e.g.  "data:GLOBAL/GLOBALB.LUT",  "user:saves/profile1.dat",  "cache:shaders/x.bin".
 *   '\' is normalised to '/'; repeated separators and "." segments are removed; ".." may not climb above the root
 *   (NUE_ERR_ACCESS_DENIED). A path without "root:" is a NATIVE OS path and is passed through untouched (use NUE_VFS_RAW when you
 *   need to be explicit). Root names are case-insensitive. Windows drive letters ("C:\x") are recognised and treated as native.
 *   Case-insensitive resolution: when an exact match does not exist, each segment is matched against the directory
 *   listing ignoring case (ASCII + Unicode simple folding); the first listing order match wins and results are cached.
 *   Writes through a path whose parent exists with different case reuse that parent. New names keep the case the caller gave.
 *
 * Roots
 *   data      read-only game data (install folder, or the imported copy on mobile)
 *   user      writable per-user data: saves, profiles, logs
 *   cache     writable, may be deleted by the OS or the user at any time
 *   settings  writable configuration (nue.ini, key bindings, graphics settings)
 *   temp      scratch space, cleared at start
 *   Applications may add roots (nue_vfs_mount) and stack several directories under one root name (mods: searched first-added-last,
 *   i.e. the most recently mounted wins for reads; writes go to the first writable mount).
 *
 * Handles: file ids are 32-bit (index | generation<<16-ish), 0 invalid, so a recompiled game can hand them out as its own
 * HANDLE values. Thread: [any] for everything; a single file handle must not be used concurrently except nue_file_read_at.
 */
#ifndef NUE_STORAGE_H
#define NUE_STORAGE_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== roots and mounts */

typedef enum nue_root_kind {
    NUE_ROOT_DATA = 1, NUE_ROOT_USER, NUE_ROOT_CACHE, NUE_ROOT_SETTINGS, NUE_ROOT_TEMP, NUE_ROOT_CUSTOM = 100
} nue_root_kind;

typedef uint32_t nue_mount_flags;
enum {
    NUE_MOUNT_READ_ONLY        = 1u << 0,
    NUE_MOUNT_CASE_SENSITIVE   = 1u << 1,   /* disable case folding for this mount (default folds) */
    NUE_MOUNT_CREATE_DIRECTORY = 1u << 2,   /* create the directory (and parents) if missing */
    NUE_MOUNT_APPEND           = 1u << 3    /* add at lowest priority instead of highest */
};

typedef struct nue_mount_desc {
    uint32_t struct_size;
    const char* root;                       /* "data", "mods", ... (letters, digits, '_', '-') */
    const char* native_path;                /* directory in the OS file system */
    nue_mount_flags flags;
} nue_mount_desc;

/* The engine mounts data/user/cache/settings/temp at nue_init according to the platform's rules (docs/ARCHITECTURE.md):
 *   Windows  data=<exe dir> or init path; user=%LOCALAPPDATA%\<org>\<app>; cache=...\cache; settings=...\settings
 *   Linux    user=$XDG_DATA_HOME/<org>/<app>, cache=$XDG_CACHE_HOME, settings=$XDG_CONFIG_HOME
 *   Android  data=internal files dir (imported copy) ; user=files dir; cache=cache dir
 *   iOS      data=Documents/<app>/GameData (imported); user=Application Support; cache=Caches
 */
NUE_API nue_result NUE_CALL nue_vfs_mount(const nue_mount_desc* desc);
NUE_API nue_result NUE_CALL nue_vfs_unmount(const char* root, const char* native_path_or_null);   /* NULL path = all mounts of root */
NUE_API nue_result NUE_CALL nue_vfs_get_root_path(const char* root, char* buffer, size_t capacity, size_t* out_size);   /* highest priority mount's native path */
NUE_API nue_bool NUE_CALL nue_vfs_has_root(const char* root);
/* Normalise a VFS path (separators, dots) without touching the disk (NUE_STRING_OUT). Pure; works before init. */
NUE_API nue_result NUE_CALL nue_path_normalize(const char* path, char* buffer, size_t capacity, size_t* out_size);
/* Resolve to the real native path with case-insensitive matching, mount overlay and parent creation not required.
 * NUE_ERR_NOT_FOUND if no mount has it (for_write: resolves into the first writable mount instead). NUE_STRING_OUT. */
NUE_API nue_result NUE_CALL nue_vfs_resolve(const char* path, nue_bool for_write, char* buffer, size_t capacity, size_t* out_size);
NUE_API void NUE_CALL nue_vfs_clear_cache(void);                                                 /* drop directory listing caches */

/* ============================================================================================== files */

typedef uint32_t nue_file;               /* 0 = invalid */

typedef uint32_t nue_open_flags;
enum {
    NUE_OPEN_READ       = 1u << 0,
    NUE_OPEN_WRITE      = 1u << 1,
    NUE_OPEN_CREATE     = 1u << 2,       /* create if missing (WRITE) */
    NUE_OPEN_TRUNCATE   = 1u << 3,
    NUE_OPEN_APPEND     = 1u << 4,
    NUE_OPEN_EXCLUSIVE  = 1u << 5,       /* with CREATE: fail with NUE_ERR_ALREADY_EXISTS if present */
    NUE_OPEN_SEQUENTIAL = 1u << 6,       /* hint: read ahead */
    NUE_OPEN_RANDOM     = 1u << 7,       /* hint: no read ahead */
    NUE_OPEN_RAW        = 1u << 8        /* path is native; no VFS resolution */
};

typedef enum nue_seek_origin { NUE_SEEK_SET = 0, NUE_SEEK_CUR, NUE_SEEK_END } nue_seek_origin;

NUE_API nue_result NUE_CALL nue_file_open(const char* path, nue_open_flags flags, nue_file* out_file);
NUE_API nue_result NUE_CALL nue_file_close(nue_file file);
/* Read up to `size` bytes at the current position. *out_read = bytes read (0 and NUE_ERR_END_OF_DATA at EOF; NUE_OK_PARTIAL if fewer than requested). */
NUE_API nue_result NUE_CALL nue_file_read(nue_file file, void* buffer, size_t size, size_t* out_read);
NUE_API nue_result NUE_CALL nue_file_write(nue_file file, const void* data, size_t size, size_t* out_written);
/* Positioned read that does not move the file pointer and may be used from several threads at once. */
NUE_API nue_result NUE_CALL nue_file_read_at(nue_file file, uint64_t offset, void* buffer, size_t size, size_t* out_read);
NUE_API nue_result NUE_CALL nue_file_write_at(nue_file file, uint64_t offset, const void* data, size_t size, size_t* out_written);
NUE_API nue_result NUE_CALL nue_file_seek(nue_file file, int64_t offset, nue_seek_origin origin, uint64_t* out_position);
NUE_API nue_result NUE_CALL nue_file_tell(nue_file file, uint64_t* out_position);
NUE_API nue_result NUE_CALL nue_file_size(nue_file file, uint64_t* out_size);
NUE_API nue_result NUE_CALL nue_file_set_size(nue_file file, uint64_t size);
NUE_API nue_result NUE_CALL nue_file_flush(nue_file file);                      /* flush to the OS; NUE_FILE_FLUSH_SYNC semantics via nue_file_sync */
NUE_API nue_result NUE_CALL nue_file_sync(nue_file file);                       /* fsync / FlushFileBuffers */
NUE_API nue_result NUE_CALL nue_file_get_native_path(nue_file file, char* buffer, size_t capacity, size_t* out_size);

/* Whole-file helpers. nue_file_load allocates with nue_alloc (release with nue_free); *out_size excludes the extra NUL byte
 * that is always appended for convenience. nue_file_save is atomic (temp file + rename) unless append. */
NUE_API nue_result NUE_CALL nue_file_load(const char* path, void** out_data, size_t* out_size);
NUE_API nue_result NUE_CALL nue_file_save(const char* path, const void* data, size_t size);

/* Memory mapping of a read-only file range (offset need not be aligned). Emulated with a heap copy when the file cannot be mapped. */
typedef struct nue_file_map_info {
    uint32_t struct_size;
    const void* data;                       /* points at the requested offset */
    size_t size;
    nue_bool emulated;                      /* copy rather than a real mapping */
} nue_file_map_info;
NUE_API nue_result NUE_CALL nue_file_map(nue_file file, uint64_t offset, size_t size /* 0 = to the end */, nue_file_map_info* out);
NUE_API nue_result NUE_CALL nue_file_unmap(nue_file file, const nue_file_map_info* map);

/* ============================================================================================== paths and directories */

typedef enum nue_entry_kind { NUE_ENTRY_NONE = 0, NUE_ENTRY_FILE, NUE_ENTRY_DIRECTORY, NUE_ENTRY_OTHER } nue_entry_kind;

typedef struct nue_stat {
    uint32_t struct_size;
    nue_entry_kind kind;
    uint64_t size;
    int64_t modified_unix_ms;
    int64_t created_unix_ms;
    nue_bool read_only;
} nue_stat;

NUE_API nue_bool NUE_CALL nue_vfs_exists(const char* path);
NUE_API nue_result NUE_CALL nue_vfs_stat(const char* path, nue_stat* out);
NUE_API nue_result NUE_CALL nue_vfs_create_directory(const char* path);         /* creates parents; OK if it exists */
NUE_API nue_result NUE_CALL nue_vfs_remove(const char* path);                   /* file or empty directory */
NUE_API nue_result NUE_CALL nue_vfs_remove_tree(const char* path);              /* ONLY inside user/cache/temp/settings roots */
NUE_API nue_result NUE_CALL nue_vfs_rename(const char* from, const char* to);   /* same root; replaces an existing file atomically where the OS can */

typedef struct nue_dir_entry {
    uint32_t struct_size;
    const char* name;                       /* valid only during the callback */
    nue_entry_kind kind;
    uint64_t size;
    int64_t modified_unix_ms;
} nue_dir_entry;
/* Return NUE_FALSE from the callback to stop. Entries of all mounts of a root are merged (duplicates by folded name appear once). */
typedef nue_bool (NUE_CALL *nue_dir_callback)(void* userdata, const nue_dir_entry* entry);
NUE_API nue_result NUE_CALL nue_vfs_list(const char* path, nue_dir_callback cb, void* userdata);
/* Glob over one directory level: '*' '?' only, case-insensitive. */
NUE_API nue_result NUE_CALL nue_vfs_glob(const char* path, const char* pattern, nue_dir_callback cb, void* userdata);

/* ============================================================================================== async read */

typedef uint32_t nue_io_request;        /* 0 = invalid */
typedef void (NUE_CALL *nue_io_callback)(void* userdata, nue_io_request request, nue_result result, size_t bytes_read);   /* runs on an I/O worker thread, or via the event queue if `post_event` */

typedef struct nue_io_desc {
    uint32_t struct_size;
    nue_file file;
    uint64_t offset;
    void* buffer;                           /* must stay valid until completion */
    size_t size;
    nue_io_callback callback;               /* may be NULL */
    void* userdata;
    nue_bool post_event;                    /* also push NUE_EVENT_IO_COMPLETE (data1 = request id, data2 = bytes, window_id = 0) */
    int32_t priority;                       /* higher first; streaming audio uses > 0 */
} nue_io_desc;

NUE_API nue_result NUE_CALL nue_io_read_async(const nue_io_desc* desc, nue_io_request* out_request);
/* Poll: NUE_OK_PENDING while running, NUE_OK when done (then *out_bytes valid), negative on error/cancel. The request id stays valid until released. */
NUE_API nue_result NUE_CALL nue_io_poll(nue_io_request request, size_t* out_bytes);
NUE_API nue_result NUE_CALL nue_io_wait(nue_io_request request, int64_t timeout_ns, size_t* out_bytes);
NUE_API nue_result NUE_CALL nue_io_cancel(nue_io_request request);
NUE_API void NUE_CALL nue_io_release(nue_io_request request);

/* ============================================================================================== integrity */

/* Manifest text, one entry per line:   <sha256 hex or crc32 hex> <size> <path>      ('#' comments).  Hash width tells the algorithm. */
typedef struct nue_verify_progress {
    uint32_t struct_size;
    uint32_t files_total, files_done;
    uint64_t bytes_total, bytes_done;
    const char* current_path;
} nue_verify_progress;
/* Return NUE_FALSE to cancel. */
typedef nue_bool (NUE_CALL *nue_progress_callback)(void* userdata, const nue_verify_progress* progress);

typedef struct nue_verify_result {
    uint32_t struct_size;
    uint32_t files_checked, files_missing, files_size_mismatch, files_hash_mismatch;
    char first_bad_path[260];
} nue_verify_result;

/* Check every file of `manifest_text` under `root_prefix` (e.g. "data:"). Returns NUE_OK if all match, NUE_ERR_CORRUPT otherwise
 * (details in *out), NUE_ERR_CANCELLED if the callback stopped it. [any]; run on a worker thread for big sets. */
NUE_API nue_result NUE_CALL nue_vfs_verify_manifest(const char* root_prefix, const char* manifest_text, size_t length,
                                                    nue_progress_callback progress, void* userdata, nue_verify_result* out);
NUE_API nue_result NUE_CALL nue_file_hash_sha256(const char* path, uint8_t out_digest[32]);
NUE_API nue_result NUE_CALL nue_file_hash_crc32(const char* path, uint32_t* out_crc);

/* ============================================================================================== game data import */

/* Mobile platforms cannot read the game from an arbitrary folder: the data must be copied into app storage first.
 *   Android  source = document tree (Storage Access Framework) picked by the user, or a path readable with file permission
 *   iOS      source = the app's Documents folder (user copied the files in via Files/iTunes), or a document picker folder
 *   Desktop  source = any folder; "import" is a plain verified copy, normally not needed (data root can point at the install)
 * One import runs at a time per destination. Progress arrives through the callback and NUE_EVENT_IMPORT_PROGRESS/FINISHED. */

typedef enum nue_import_source_kind {
    NUE_IMPORT_SOURCE_PATH = 1,             /* native directory path */
    NUE_IMPORT_SOURCE_PICKER,               /* ask the user: platform folder picker (Android SAF tree, iOS document picker) */
    NUE_IMPORT_SOURCE_URI                   /* platform URI: Android content://...tree..., iOS security-scoped bookmark URL string */
} nue_import_source_kind;

typedef uint32_t nue_import_flags;
enum {
    NUE_IMPORT_VERIFY      = 1u << 0,       /* verify against manifest after copying */
    NUE_IMPORT_SKIP_EXISTING = 1u << 1,     /* skip files whose size (and hash when verifying) already match: resumable */
    NUE_IMPORT_MOVE        = 1u << 2,       /* delete the source after a verified copy (iOS Documents) where allowed */
    NUE_IMPORT_FLATTEN_ROOT = 1u << 3       /* if the picked folder holds a single subfolder, use its content as the root */
};

typedef struct nue_import_desc {
    uint32_t struct_size;
    nue_import_source_kind source_kind;
    const char* source;                     /* path or URI (not for PICKER) */
    const char* dest_root;                  /* VFS root to import into; default "data" */
    nue_import_flags flags;
    const char* manifest_text;              /* optional, see nue_vfs_verify_manifest; also used to decide what to copy (only listed files) */
    size_t manifest_length;
    nue_progress_callback progress;         /* called on the import worker thread; return NUE_FALSE to cancel */
    void* userdata;
} nue_import_desc;

typedef uint32_t nue_import_id;         /* 0 = invalid */
typedef struct nue_import_status {
    uint32_t struct_size;
    nue_result state;                       /* NUE_OK_PENDING running, NUE_OK done, negative failed/cancelled */
    nue_verify_progress progress;
    char last_error[256];
} nue_import_status;

NUE_API nue_result NUE_CALL nue_import_start(const nue_import_desc* desc, nue_import_id* out_id);
NUE_API nue_result NUE_CALL nue_import_get_status(nue_import_id id, nue_import_status* out);
NUE_API nue_result NUE_CALL nue_import_cancel(nue_import_id id);
NUE_API void NUE_CALL nue_import_release(nue_import_id id);
/* True when the data root already holds a complete import (marker file written after a verified import). */
NUE_API nue_bool NUE_CALL nue_import_is_complete(const char* dest_root);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_STORAGE_H */
