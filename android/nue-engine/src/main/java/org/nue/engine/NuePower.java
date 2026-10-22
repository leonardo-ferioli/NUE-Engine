package org.nue.engine;

import android.app.Activity;
import android.app.GameManager;
import android.app.GameState;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;

/**
 * Power state for the engine: thermal status + headroom, battery saver, battery level, Game Mode, sustained
 * performance, GameState.  Everything is pushed to native as snapshots / events.
 */
final class NuePower {
    private NuePower() {}

    private static final long POLL_MS = 5000;
    private static Handler handler;
    private static boolean started;
    private static PowerManager pm;
    private static GameManager gameManager;
    private static int lastGameMode = -1;
    private static float lastHeadroom = Float.NaN;
    private static int lastThermalStatus = -1;

    private static final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(action)) {
                NueBridge.nativeOnPowerSave(pm != null && pm.isPowerSaveMode());
            } else if (Intent.ACTION_BATTERY_CHANGED.equals(action)) {
                pushBattery(intent);
            }
        }
    };

    private static final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!started) return;
            pollGameMode();
            pollHeadroom();
            handler.postDelayed(this, POLL_MS);
        }
    };

    static void start(final Activity a) {
        if (started) return;
        started = true;
        pm = (PowerManager) a.getSystemService(Context.POWER_SERVICE);
        handler = new Handler(Looper.getMainLooper());
        NueBridge.nativeSetSustainedSupported(pm.isSustainedPerformanceModeSupported());
        NueBridge.nativeOnPowerSave(pm.isPowerSaveMode());

        // API 29: PowerManager.addThermalStatusListener
        pm.addThermalStatusListener(a.getMainExecutor(), new PowerManager.OnThermalStatusChangedListener() {
            @Override
            public void onThermalStatusChanged(int status) {
                lastThermalStatus = status;
                NueBridge.nativeOnThermal(status, lastHeadroom);
            }
        });
        lastThermalStatus = pm.getCurrentThermalStatus();
        NueBridge.nativeOnThermal(lastThermalStatus, Float.NaN);

        if (Build.VERSION.SDK_INT >= 31) {
            gameManager = (GameManager) a.getSystemService(Context.GAME_SERVICE);
        }

        IntentFilter f = new IntentFilter();
        f.addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
        f.addAction(Intent.ACTION_BATTERY_CHANGED);
        Intent sticky;
        if (Build.VERSION.SDK_INT >= 33) {
            sticky = a.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            sticky = a.registerReceiver(receiver, f);
        }
        if (sticky != null) pushBattery(sticky);
        poll.run();
    }

    static void stop(Activity a) {
        if (!started) return;
        started = false;
        try {
            a.unregisterReceiver(receiver);
        } catch (IllegalArgumentException ignored) {
            // not registered
        }
        if (handler != null) handler.removeCallbacks(poll);
    }

    private static void pushBattery(Intent i) {
        int level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
        int status = i.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
        boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL;
        NueBridge.nativeSetBattery(level >= 0 && scale > 0 ? level * 100 / scale : -1, charging);
    }

    /** There is no public listener for Game Mode changes: poll (cheap binder call) and when the activity resumes. */
    static void pollGameMode() {
        if (gameManager == null) return;
        int mode = gameManager.getGameMode(); // 0 unsupported, 1 standard, 2 performance, 3 battery, 4 custom
        if (mode != lastGameMode) {
            lastGameMode = mode;
            NueBridge.nativeOnGameMode(mode);
        }
    }

    /** getThermalHeadroom(10): predicted headroom in 10 s; 1.0 = severe throttling; NaN when unsupported (API 30). */
    private static void pollHeadroom() {
        if (Build.VERSION.SDK_INT < 30 || pm == null) return;
        float h = pm.getThermalHeadroom(10);
        if (Float.isNaN(h) && Float.isNaN(lastHeadroom)) return;
        if (!Float.isNaN(h) && !Float.isNaN(lastHeadroom) && Math.abs(h - lastHeadroom) < 0.03f) return;
        lastHeadroom = h;
        NueBridge.nativeOnThermal(lastThermalStatus, h);
    }

    static void onResume(Activity a) {
        pollGameMode();
    }

    /**
     * GameState API (API 33): tells the system when the game is loading (no frame rate / CPU boost penalty) and what
     * kind of gameplay is running.  mode: 0 none, 1 gameplay uninterruptible, 2 gameplay interruptible, 3 content.
     */
    static void setGameState(final Activity a, final boolean loading, final int mode) {
        if (Build.VERSION.SDK_INT < 33 || gameManager == null) return;
        try {
            gameManager.setGameState(new GameState(loading, mode));
        } catch (RuntimeException e) {
            android.util.Log.w("NUE", "setGameState failed: " + e);
        }
    }

    static void setSustainedPerformance(final Activity a, final boolean on) {
        if (a == null || pm == null || !pm.isSustainedPerformanceModeSupported()) return;
        a.runOnUiThread(() -> a.getWindow().setSustainedPerformanceMode(on));
    }
}
