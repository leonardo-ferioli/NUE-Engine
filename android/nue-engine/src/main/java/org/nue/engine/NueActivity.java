package org.nue.engine;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * Activity of every NUE game on Android.
 *
 * <p>Why SDLActivity and not GameActivity: the portable core is built on SDL3 on every platform, and SDL3's Android
 * backend (surface, input, joysticks, audio devices, text input, storage helpers) needs SDL to own the activity.
 * GameActivity would force a second input / lifecycle path next to SDL.  The Android Game Development Kit pieces that
 * do not need the activity (ADPF, frame pacing hints, Game Mode) are used directly.
 *
 * <p>The game's native code lives in the library named by the meta-data {@code org.nue.engine.main_library}
 * (default {@code main}); it is started on SDL's own thread ("SDLThread"), which is the engine thread: engine-owned
 * loop mode runs there, host-owned loop mode (the recompiled game thread) pumps events from that same thread.
 */
public class NueActivity extends SDLActivity {
    private static final String TAG = "NUE";
    static final String META_MAIN_LIBRARY = "org.nue.engine.main_library";
    static final String META_TARGET_FPS = "org.nue.engine.target_fps";

    private int metaInt(String key, int def) {
        try {
            ActivityInfo ai = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA);
            return ai.metaData != null ? ai.metaData.getInt(key, def) : def;
        } catch (PackageManager.NameNotFoundException e) {
            return def;
        }
    }

    private String metaString(String key, String def) {
        try {
            ActivityInfo ai = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA);
            String v = ai.metaData != null ? ai.metaData.getString(key) : null;
            return v != null ? v : def;
        } catch (PackageManager.NameNotFoundException e) {
            return def;
        }
    }

    @Override
    protected String[] getLibraries() {
        // The last entry is the one SDL starts SDL_main from.
        return new String[] {"SDL3", "nue_android", metaString(META_MAIN_LIBRARY, "main")};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new NueSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState); // loads the libraries: nue_android's JNI_OnLoad registers the natives
        if (SDLActivity.mBrokenLibraries) {
            Log.e(TAG, "native libraries failed to load, the engine is not started");
            return;
        }
        NueBridge.activity = this;
        NueMemory.init(this);
        NueStorage.init(this);
        NueAudio.init(this);
        NueDiagnostics.init(this, NueStorage.crashDir(this));
        NueBridge.nativeSetTargetFps(metaInt(META_TARGET_FPS, 60));
        NueDisplay.refresh(this);
        NueDisplay.setImmersive(this, true);
        NuePower.start(this);
        NueInput.start(this);
        NueBridge.nativeOnLifecycle(NueBridge.LIFE_CREATE);
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (!SDLActivity.mBrokenLibraries) NueBridge.nativeOnLifecycle(NueBridge.LIFE_START);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (SDLActivity.mBrokenLibraries) return;
        NueMemory.start();
        NuePower.onResume(this);
        NueDisplay.refresh(this);
        NueBridge.nativeOnLifecycle(NueBridge.LIFE_RESUME);
    }

    @Override
    protected void onPause() {
        if (!SDLActivity.mBrokenLibraries) {
            NueBridge.nativeOnLifecycle(NueBridge.LIFE_PAUSE); // before SDL pauses its thread: the engine can save state
            NueMemory.stop();
        }
        super.onPause();
    }

    @Override
    protected void onStop() {
        if (!SDLActivity.mBrokenLibraries) NueBridge.nativeOnLifecycle(NueBridge.LIFE_STOP);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (!SDLActivity.mBrokenLibraries) {
            NueBridge.nativeOnLifecycle(NueBridge.LIFE_DESTROY);
            NuePower.stop(this);
            NueInput.stop(this);
            NueAudio.shutdown();
            NueBridge.nativeShutdown();
            NueBridge.activity = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (SDLActivity.mBrokenLibraries) return;
        if (hasFocus) NueDisplay.setImmersive(this, true); // bars come back after dialogs / the shade
        NueBridge.nativeOnFocus(hasFocus);
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (!SDLActivity.mBrokenLibraries) NueMemory.onTrim(level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        if (!SDLActivity.mBrokenLibraries) NueMemory.onLowMemory();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (!SDLActivity.mBrokenLibraries) NueDisplay.onConfigurationChanged(this, newConfig);
    }

    @Override
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) {
        super.onMultiWindowModeChanged(isInMultiWindowMode, newConfig);
        if (!SDLActivity.mBrokenLibraries) NueDisplay.onConfigurationChanged(this, newConfig);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (!SDLActivity.mBrokenLibraries && NueStorage.onActivityResult(this, requestCode, resultCode, data)) return;
        super.onActivityResult(requestCode, resultCode, data);
    }

    // The last-used-device signal watches events on their way to SDL; it never consumes them.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (!SDLActivity.mBrokenLibraries) NueInput.onEvent(event);
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (!SDLActivity.mBrokenLibraries) NueInput.onEvent(event);
        return super.dispatchTouchEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (!SDLActivity.mBrokenLibraries) NueInput.onEvent(event);
        return super.dispatchGenericMotionEvent(event);
    }
}
