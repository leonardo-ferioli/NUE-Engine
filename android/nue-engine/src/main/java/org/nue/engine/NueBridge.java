package org.nue.engine;

import android.app.Activity;
import android.view.Surface;

import org.libsdl.app.SDLActivity;

/**
 * The single JNI boundary of the Android backend.
 *
 * <p>Java -> native: static {@code native*} methods that push state snapshots and events (registered by
 * JNI_OnLoad of libnue_android.so, see jni/android_jni.cpp; the descriptors must match).
 * Native -> Java: the static methods below, looked up by name by native code (kept by consumer-proguard-rules.pro).
 */
public final class NueBridge {
    private NueBridge() {}

    static Activity activity;

    // ------------------------------------------------------------------ Java -> native
    static native void nativeSetDeviceInfo(int sdk, String abi, String manufacturer, String model, String soc, long totalRam,
                                           int memClass, int largeMemClass, boolean lowRam, int cores, int pageSize,
                                           boolean touch, boolean tv, boolean chromeos, boolean emulator);
    static native void nativeSetStoragePaths(String files, String external, String cache, String game, String saves,
                                             String config, String crash);
    static native void nativeSetStorageFree(long internalFree, long externalFree, boolean complete);
    static native void nativeSetDisplay(int w, int h, int rotation, float refresh, float dpi, float[] supported,
                                        boolean cutout, boolean hdr, boolean wide);
    static native void nativeSetSafeArea(int l, int t, int r, int b);
    static native void nativeOnLifecycle(int event);
    static native void nativeOnSurface(int event, int w, int h);
    static native void nativeOnFocus(boolean focus);
    static native void nativeSetTargetFps(int fps);
    static native void nativeSetMemory(long avail, long threshold, boolean low);
    static native void nativeOnTrim(int level);
    static native void nativeOnThermal(int status, float headroom);
    static native void nativeOnGameMode(int mode);
    static native void nativeOnPowerSave(boolean on);
    static native void nativeSetBattery(int percent, boolean charging);
    static native void nativeSetSustainedSupported(boolean on);
    static native void nativeSetAudio(int rate, int frames, boolean lowLatency, boolean pro, int deviceType);
    static native void nativeOnAudioRoute(int deviceType);
    static native void nativeOnAudioFocus(int change);
    static native void nativeOnInputDevice(int id, int added, int sources, boolean isVirtual, int keyboardType);
    static native void nativeOnInputSample(int sources, boolean isVirtual, int keyboardType, float magnitude);
    static native void nativeSetGamepadConnected(boolean connected);
    static native int nativeImportRun(String destRoot, String manifest, String[] paths, long[] sizes, boolean fsync);
    static native int nativeImportDir(String dir, String destRoot, String manifest, boolean fsync);
    static native void nativeImportCancel();
    static native void nativeOnTreePicked(boolean picked);
    static native void nativeInstallCrashHandler(String crashDir, String version);
    static native int nativeChooseDisplayMode(int[] ids, int[] widths, int[] heights, float[] refresh, int currentId, float target);
    static native void nativeShutdown();

    // Lifecycle constants, equal to NueAndroidLifecycle / NueAndroidSurface in nue_android.h
    static final int LIFE_CREATE = 1, LIFE_START = 2, LIFE_RESUME = 3, LIFE_PAUSE = 4, LIFE_STOP = 5, LIFE_DESTROY = 6;
    static final int SURFACE_CREATED = 1, SURFACE_CHANGED = 2, SURFACE_DESTROYED = 3;

    // ------------------------------------------------------------------ native -> Java (all static, any thread)
    public static Surface getSurface() {
        return SDLActivity.getNativeSurface();
    }

    public static void setFrameRate(float fps) {
        NueDisplay.setFrameRate(fps);
    }

    public static void setKeepScreenOn(boolean on) {
        NueDisplay.setKeepScreenOn(activity, on);
    }

    public static void setImmersive(boolean on) {
        NueDisplay.setImmersive(activity, on);
    }

    public static void setPreferredRefresh(float fps) {
        NueDisplay.setPreferredRefresh(activity, fps);
    }

    public static void setGameState(boolean loading, int mode) {
        NuePower.setGameState(activity, loading, mode);
    }

    public static void setSustainedPerformance(boolean on) {
        NuePower.setSustainedPerformance(activity, on);
    }

    public static void audioFocus(int request) {
        NueAudio.audioFocus(activity, request != 0);
    }

    public static void vibrate(int ms, int amplitude) {
        NueInput.vibrate(activity, ms, amplitude);
    }

    public static void pickGameFolder() {
        NueStorage.pickFolder(activity);
    }

    public static boolean importStart(String manifest, boolean external) {
        return NueStorage.importStart(activity, manifest, external);
    }

    public static void importCancelFromNative() {
        NueStorage.importCancel();
    }

    public static int openImportFd(int index) {
        return NueStorage.openImportFd(activity, index);
    }
}
