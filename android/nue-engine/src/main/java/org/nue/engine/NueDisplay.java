package org.nue.engine;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Surface;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.Arrays;
import java.util.TreeSet;

/** Display mode / refresh rate, frame-rate hint, cutouts and safe area, immersive mode, keep-screen-on. */
final class NueDisplay {
    private NueDisplay() {}

    private static Display display(Activity a) {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                Display d = a.getDisplay();
                if (d != null) return d;
            } catch (UnsupportedOperationException ignored) {
                // not associated with a display yet
            }
        }
        return ((WindowManager) a.getSystemService(Activity.WINDOW_SERVICE)).getDefaultDisplay();
    }

    /** Pushes size, rotation, refresh rate and the list of refresh rates available at this resolution. */
    static void refresh(Activity a) {
        if (a == null) return;
        Display d = display(a);
        Display.Mode mode = d.getMode();
        DisplayMetrics dm = a.getResources().getDisplayMetrics();
        TreeSet<Float> rates = new TreeSet<>();
        for (Display.Mode m : d.getSupportedModes()) {
            if (m.getPhysicalWidth() == mode.getPhysicalWidth() && m.getPhysicalHeight() == mode.getPhysicalHeight())
                rates.add(Math.round(m.getRefreshRate() * 100f) / 100f);
        }
        float[] list = new float[rates.size()];
        int i = 0;
        for (Float f : rates) list[i++] = f;
        boolean hdr = Build.VERSION.SDK_INT >= 26 && d.isHdr();
        boolean wide = Build.VERSION.SDK_INT >= 26 && d.isWideColorGamut();
        boolean cutout = false;
        if (Build.VERSION.SDK_INT >= 29 && a.getWindow().getDecorView().getRootWindowInsets() != null) {
            cutout = a.getWindow().getDecorView().getRootWindowInsets().getDisplayCutout() != null;
        }
        NueBridge.nativeSetDisplay(mode.getPhysicalWidth(), mode.getPhysicalHeight(), d.getRotation(),
                d.getRefreshRate(), dm.densityDpi, list, cutout, hdr, wide);
    }

    /** Safe area = union of system bars and the display cutout, in pixels. */
    static void onInsets(WindowInsets insets) {
        int l, t, r, b;
        if (Build.VERSION.SDK_INT >= 30) {
            Insets in = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            l = in.left;
            t = in.top;
            r = in.right;
            b = in.bottom;
        } else {
            l = insets.getSystemWindowInsetLeft();
            t = insets.getSystemWindowInsetTop();
            r = insets.getSystemWindowInsetRight();
            b = insets.getSystemWindowInsetBottom();
            if (Build.VERSION.SDK_INT >= 28) {
                DisplayCutout c = insets.getDisplayCutout();
                if (c != null) {
                    l = Math.max(l, c.getSafeInsetLeft());
                    t = Math.max(t, c.getSafeInsetTop());
                    r = Math.max(r, c.getSafeInsetRight());
                    b = Math.max(b, c.getSafeInsetBottom());
                }
            }
        }
        NueBridge.nativeSetSafeArea(Math.max(0, l), Math.max(0, t), Math.max(0, r), Math.max(0, b));
    }

    /**
     * Surface.setFrameRate (API 30): tells SurfaceFlinger the content rate so it can pick a refresh rate that is a
     * multiple of it (60 on a 120 Hz panel) and, from Android 13, throttle the app's vsync callbacks accordingly.
     */
    static void setFrameRate(float fps) {
        if (Build.VERSION.SDK_INT < 30) return;
        Surface s = SDLActivity.getNativeSurface();
        if (s == null || !s.isValid()) return;
        try {
            if (fps <= 0f) {
                s.setFrameRate(0f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            } else if (Build.VERSION.SDK_INT >= 31) {
                s.setFrameRate(fps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
            } else {
                s.setFrameRate(fps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            }
        } catch (RuntimeException e) {
            android.util.Log.w("NUE", "setFrameRate failed: " + e);
        }
    }

    /** Picks the display mode that fits fps best at the current resolution and requests it. */
    static void setPreferredRefresh(final Activity a, final float fps) {
        if (a == null) return;
        a.runOnUiThread(() -> {
            Display d = display(a);
            Display.Mode[] modes = d.getSupportedModes();
            int n = modes.length;
            int[] ids = new int[n], w = new int[n], h = new int[n];
            float[] r = new float[n];
            for (int i = 0; i < n; i++) {
                ids[i] = modes[i].getModeId();
                w[i] = modes[i].getPhysicalWidth();
                h[i] = modes[i].getPhysicalHeight();
                r[i] = modes[i].getRefreshRate();
            }
            int id = NueBridge.nativeChooseDisplayMode(ids, w, h, r, d.getMode().getModeId(), fps);
            if (id < 0) return;
            WindowManager.LayoutParams lp = a.getWindow().getAttributes();
            if (lp.preferredDisplayModeId != id) {
                lp.preferredDisplayModeId = id;
                a.getWindow().setAttributes(lp);
            }
        });
    }

    static void setKeepScreenOn(final Activity a, final boolean on) {
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (on) a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        });
    }

    /** Draw into the cutout area and hide the system bars (swipe from the edge shows them transiently). */
    static void setImmersive(final Activity a, final boolean on) {
        if (a == null) return;
        a.runOnUiThread(() -> {
            Window w = a.getWindow();
            WindowManager.LayoutParams lp = w.getAttributes();
            if (Build.VERSION.SDK_INT >= 30) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            } else if (Build.VERSION.SDK_INT >= 28) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
            w.setAttributes(lp);
            if (Build.VERSION.SDK_INT >= 30) {
                w.setDecorFitsSystemWindows(!on);
                WindowInsetsController c = w.getInsetsController();
                if (c != null) {
                    if (on) {
                        c.hide(WindowInsets.Type.systemBars());
                        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    } else {
                        c.show(WindowInsets.Type.systemBars());
                    }
                }
            } else {
                int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
                if (on) {
                    flags |= View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
                }
                w.getDecorView().setSystemUiVisibility(flags);
            }
        });
    }

    /** Re-reads size / rate after rotation, fold posture or multi-window changes. */
    static void onConfigurationChanged(Activity a, Configuration cfg) {
        refresh(a);
    }

    static String describeRates(float[] rates) {
        return Arrays.toString(rates);
    }
}
