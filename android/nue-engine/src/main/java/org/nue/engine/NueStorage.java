package org.nue.engine;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Game-data storage.
 *
 * <p>The game files live on the user's phone in a place only the Storage Access Framework can read (Downloads,
 * Documents, an SD card, a USB stick).  They are copied once into app-private storage (filesDir/gamedata) and read with
 * plain file access afterwards.  Tree enumeration and file descriptors are Java (ContentResolver); copying, hashing,
 * journal and resume are native (logic/import_engine.cpp) so they are fast and unit tested.
 */
final class NueStorage {
    private NueStorage() {}

    private static final String TAG = "NUE";
    private static final int REQ_PICK_TREE = 0x4E55;
    private static final String PREFS = "nue_storage";
    private static final String PREF_TREE = "game_tree_uri";

    private static Uri treeUri;
    private static volatile boolean cancel;
    private static Thread worker;
    private static List<Uri> importUris = new ArrayList<>();

    static File filesDir(Activity a) { return a.getFilesDir(); }

    static File gameDir(Activity a, boolean external) {
        File base = external ? a.getExternalFilesDir(null) : null;
        if (base == null) base = a.getFilesDir();
        return new File(base, "gamedata");
    }

    static void init(Activity a) {
        File files = a.getFilesDir();
        File ext = a.getExternalFilesDir(null);
        File saves = new File(files, "saves");
        File config = new File(files, "config");
        File crash = new File(files, "crash");
        saves.mkdirs();
        config.mkdirs();
        crash.mkdirs();
        NueBridge.nativeSetStoragePaths(files.getAbsolutePath(), ext != null ? ext.getAbsolutePath() : "",
                a.getCacheDir().getAbsolutePath(), new File(files, "gamedata").getAbsolutePath(),
                saves.getAbsolutePath(), config.getAbsolutePath(), crash.getAbsolutePath());
        refreshFree(a);
        SharedPreferences sp = a.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String saved = sp.getString(PREF_TREE, null);
        if (saved != null) treeUri = Uri.parse(saved);
    }

    static File crashDir(Activity a) {
        return new File(a.getFilesDir(), "crash");
    }

    /** Free bytes of both targets and whether the internal copy carries a valid completion marker. */
    static void refreshFree(Activity a) {
        long internal = new StatFs(a.getFilesDir().getPath()).getAvailableBytes();
        long external = 0;
        File ext = a.getExternalFilesDir(null);
        if (ext != null) {
            try {
                external = new StatFs(ext.getPath()).getAvailableBytes();
            } catch (IllegalArgumentException ignored) {
                // volume gone
            }
        }
        boolean complete = isComplete(gameDir(a, false)) || isComplete(gameDir(a, true));
        NueBridge.nativeSetStorageFree(internal, external, complete);
    }

    private static boolean isComplete(File gameDir) {
        return new File(gameDir, ".import/complete.txt").isFile();
    }

    // ------------------------------------------------------------------------------------------ picker

    static void pickFolder(final Activity a) {
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            // Start in Downloads: Android 11+ refuses the root of the storage and Download itself, so the player must
            // choose a sub folder (the game folder), which also keeps the enumeration small.
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                    DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
            try {
                a.startActivityForResult(i, REQ_PICK_TREE);
            } catch (RuntimeException e) {
                Log.e(TAG, "no document picker available: " + e);
                NueBridge.nativeOnTreePicked(false);
            }
        });
    }

    /** @return true when the result was ours. */
    static boolean onActivityResult(Activity a, int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_PICK_TREE) return false;
        Uri uri = (resultCode == Activity.RESULT_OK && data != null) ? data.getData() : null;
        if (uri == null) {
            NueBridge.nativeOnTreePicked(false);
            return true;
        }
        try {
            a.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "could not persist the folder permission: " + e); // still usable for this run
        }
        treeUri = uri;
        a.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(PREF_TREE, uri.toString()).apply();
        NueBridge.nativeOnTreePicked(true);
        return true;
    }

    // ------------------------------------------------------------------------------------------ import

    static boolean importStart(final Activity a, final String manifest, final boolean external) {
        if (a == null || treeUri == null) return false;
        synchronized (NueStorage.class) {
            if (worker != null && worker.isAlive()) return false;
            cancel = false;
            final Uri tree = treeUri;
            worker = new Thread(() -> runImport(a, tree, manifest, external), "nue-import");
            worker.start();
        }
        return true;
    }

    static void importCancel() {
        cancel = true;
        NueBridge.nativeImportCancel();
    }

    private static void runImport(Activity a, Uri tree, String manifest, boolean external) {
        PowerManager.WakeLock lock = null;
        try {
            PowerManager pm = (PowerManager) a.getSystemService(Context.POWER_SERVICE);
            lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nue:import");
            lock.acquire(3 * 60 * 60 * 1000L);
        } catch (RuntimeException e) {
            Log.w(TAG, "no wake lock: " + e);
        }
        try {
            List<String> paths = new ArrayList<>();
            List<Uri> uris = new ArrayList<>();
            List<Long> sizes = new ArrayList<>();
            long t0 = System.nanoTime();
            enumerate(a.getContentResolver(), tree, paths, uris, sizes);
            Log.i(TAG, "import: " + paths.size() + " files found in " + (System.nanoTime() - t0) / 1000000 + " ms");
            if (cancel) return;
            stripToManifestRoot(paths, manifest);
            long[] sz = new long[paths.size()];
            for (int i = 0; i < sz.length; i++) sz[i] = sizes.get(i);
            importUris = uris;
            String dest = gameDir(a, external).getAbsolutePath();
            int rc = NueBridge.nativeImportRun(dest, manifest, paths.toArray(new String[0]), sz, true);
            Log.i(TAG, "import: native result " + rc);
        } catch (RuntimeException e) {
            Log.e(TAG, "import failed", e);
        } finally {
            importUris = new ArrayList<>();
            if (lock != null && lock.isHeld()) lock.release();
            refreshFree(a);
        }
    }

    /** Depth first walk through DocumentsContract queries (about 10x faster than DocumentFile). */
    private static void enumerate(ContentResolver cr, Uri tree, List<String> paths, List<Uri> uris, List<Long> sizes) {
        ArrayDeque<String[]> stack = new ArrayDeque<>();
        stack.push(new String[] {DocumentsContract.getTreeDocumentId(tree), ""});
        final String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        while (!stack.isEmpty() && !cancel) {
            String[] cur = stack.pop();
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, cur[0]);
            try (Cursor c = cr.query(children, cols, null, null, null)) {
                if (c == null) continue;
                while (c.moveToNext()) {
                    String id = c.getString(0);
                    String name = c.getString(1);
                    String mime = c.getString(2);
                    long size = c.isNull(3) ? 0 : c.getLong(3);
                    if (id == null || name == null) continue;
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                        stack.push(new String[] {id, cur[1] + name + "/"});
                    } else {
                        paths.add(cur[1] + name);
                        uris.add(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                        sizes.add(size);
                    }
                }
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot list " + cur[0] + ": " + e);
            }
        }
    }

    /**
     * Players often pick the folder that contains the game folder.  When the first required file of the manifest is
     * not at the root but exactly one prefix leads to it, drop that prefix (entries outside of it are not game files).
     * The path list is edited in place; uris stay index aligned because removed entries are replaced, not deleted.
     */
    static void stripToManifestRoot(List<String> paths, String manifest) {
        if (manifest == null) return;
        String marker = null;
        for (String line : manifest.split("\n")) {
            String t = line.trim();
            if (t.isEmpty() || t.startsWith("#") || t.startsWith("?")) continue;
            String[] parts = t.split("\\s+", 3);
            if (parts.length == 3) {
                marker = parts[2].replace('\\', '/').toLowerCase(Locale.ROOT);
                break;
            }
        }
        if (marker == null) return;
        String prefix = null;
        for (String p : paths) {
            String lp = p.replace('\\', '/').toLowerCase(Locale.ROOT);
            if (lp.equals(marker)) return; // already at the right root
            if (lp.endsWith("/" + marker)) {
                String cand = p.substring(0, p.length() - marker.length());
                if (prefix != null && !prefix.equalsIgnoreCase(cand)) return; // ambiguous
                prefix = cand;
            }
        }
        if (prefix == null) return;
        for (int i = 0; i < paths.size(); i++) {
            String p = paths.get(i);
            // files outside of the prefix keep an unusable name so the manifest cannot match them
            paths.set(i, p.regionMatches(true, 0, prefix, 0, prefix.length()) ? p.substring(prefix.length()) : "../" + p);
        }
    }

    /** Called by native code on the importing thread; the returned descriptor is owned by the caller. */
    static int openImportFd(Activity a, int index) {
        if (index < 0 || index >= importUris.size()) return -1;
        try {
            ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(importUris.get(index), "r");
            return pfd != null ? pfd.detachFd() : -1;
        } catch (Exception e) {
            Log.w(TAG, "cannot open " + importUris.get(index) + ": " + e);
            return -1;
        }
    }
}
