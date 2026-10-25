package org.nue.engine;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

/** Device memory facts, periodic ActivityManager.MemoryInfo refresh and onTrimMemory forwarding. */
final class NueMemory {
    private NueMemory() {}

    private static final long POLL_MS = 3000;
    private static ActivityManager am;
    private static Handler handler;
    private static boolean running;

    private static final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            push();
            handler.postDelayed(this, POLL_MS);
        }
    };

    static void init(Activity a) {
        am = (ActivityManager) a.getSystemService(Context.ACTIVITY_SERVICE);
        handler = new Handler(Looper.getMainLooper());
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        PackageManager pm = a.getPackageManager();
        boolean tv = pm.hasSystemFeature(PackageManager.FEATURE_LEANBACK) || pm.hasSystemFeature("android.software.leanback");
        boolean chromeos = pm.hasSystemFeature("org.chromium.arc") || pm.hasSystemFeature("org.chromium.arc.device_management");
        boolean emulator = Build.FINGERPRINT.startsWith("generic") || Build.FINGERPRINT.contains("emulator")
                || Build.HARDWARE.contains("ranchu") || Build.HARDWARE.contains("goldfish") || Build.MODEL.contains("Emulator");
        String soc = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : Build.HARDWARE;
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "";
        NueBridge.nativeSetDeviceInfo(Build.VERSION.SDK_INT, abi, Build.MANUFACTURER, Build.MODEL, soc, mi.totalMem,
                am.getMemoryClass(), am.getLargeMemoryClass(), am.isLowRamDevice(), Runtime.getRuntime().availableProcessors(),
                pageSize(), pm.hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN), tv, chromeos, emulator);
    }

    private static int pageSize() {
        // 16 KB page devices (Android 15+ option) report 16384; the NDK has the authoritative value
        return android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE) > 0
                ? (int) android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE) : 4096;
    }

    static void start() {
        if (running) return;
        running = true;
        poll.run();
    }

    static void stop() {
        running = false;
        if (handler != null) handler.removeCallbacks(poll);
    }

    static void push() {
        if (am == null) return;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        NueBridge.nativeSetMemory(mi.availMem, mi.threshold, mi.lowMemory);
    }

    /** ComponentCallbacks2 level (5/10/15 while running, 20 UI hidden, 40/60/80 cached). */
    static void onTrim(int level) {
        push();
        NueBridge.nativeOnTrim(level);
    }

    static void onLowMemory() {
        push();
        NueBridge.nativeOnTrim(-1);
    }
}
