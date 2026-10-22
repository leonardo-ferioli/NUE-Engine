package org.nue.engine;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.util.List;
import java.util.Locale;

/**
 * Crash and exit diagnostics: native signal handler (reports in the crash directory), Java uncaught exception writer,
 * and a log of why the previous process died (ApplicationExitInfo, API 30: ANR, low-memory kill, native crash ...).
 */
final class NueDiagnostics {
    private NueDiagnostics() {}

    private static final String TAG = "NUE";

    static void init(Activity a, File crashDir) {
        String version = "unknown";
        try {
            PackageInfo pi = a.getPackageManager().getPackageInfo(a.getPackageName(), 0);
            version = pi.versionName + " (" + pi.getLongVersionCode() + ")";
        } catch (Exception ignored) {
            // keep unknown
        }
        crashDir.mkdirs();
        NueBridge.nativeInstallCrashHandler(crashDir.getAbsolutePath(), version);
        installJavaHandler(crashDir, version);
        logPreviousExit(a);
    }

    private static void installJavaHandler(final File dir, final String version) {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                StringWriter sw = new StringWriter();
                e.printStackTrace(new PrintWriter(sw));
                File f = new File(dir, String.format(Locale.US, "java-crash-%d.txt", System.currentTimeMillis() / 1000));
                try (FileWriter w = new FileWriter(f)) {
                    w.write("NUE Java crash, version " + version + ", thread " + t.getName() + ", sdk " + Build.VERSION.SDK_INT
                            + ", " + Build.MANUFACTURER + " " + Build.MODEL + "\n" + sw);
                }
            } catch (IOException | RuntimeException ignored) {
                // nothing more we can do while dying
            }
            if (previous != null) previous.uncaughtException(t, e);
        });
    }

    /** Logs the last few process exit reasons: tells an ANR or a low-memory kill from a crash without a debugger. */
    private static void logPreviousExit(Activity a) {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = (ActivityManager) a.getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> list = am.getHistoricalProcessExitReasons(null, 0, 3);
            for (ApplicationExitInfo i : list) {
                Log.i(TAG, "previous exit: reason=" + reasonName(i.getReason()) + " status=" + i.getStatus()
                        + " importance=" + i.getImportance() + " pssKb=" + i.getPss() + " rssKb=" + i.getRss()
                        + " time=" + i.getTimestamp() + " desc=" + i.getDescription());
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "exit info unavailable: " + e);
        }
    }

    private static String reasonName(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH(java)";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY(killed)";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INIT_FAILURE";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "USER_STOPPED";
            case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case ApplicationExitInfo.REASON_OTHER: return "OTHER";
            default: return "REASON_" + r;
        }
    }
}
