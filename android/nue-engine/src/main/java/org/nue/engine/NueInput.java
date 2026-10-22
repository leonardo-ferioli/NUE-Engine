package org.nue.engine;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Input support that SDL3 does not provide: hot-plug classification of input devices and the "last used input device"
 * signal (to switch on-screen button glyphs between touch / keyboard / gamepad).  The events themselves reach the game
 * through SDL; this class only watches them (without consuming) and filters to keep JNI traffic negligible.
 */
final class NueInput {
    private NueInput() {}

    private static final float THRESHOLD = 0.35f;
    private static int lastSource = -1;
    private static boolean registered;

    private static final InputManager.InputDeviceListener listener = new InputManager.InputDeviceListener() {
        @Override
        public void onInputDeviceAdded(int id) {
            report(id, 1);
        }

        @Override
        public void onInputDeviceRemoved(int id) {
            NueBridge.nativeOnInputDevice(id, 0, lastKnownSources(id), false, 2);
            updateGamepadFlag();
        }

        @Override
        public void onInputDeviceChanged(int id) {
            report(id, 1);
        }
    };

    private static final java.util.HashMap<Integer, Integer> knownSources = new java.util.HashMap<>();

    private static int lastKnownSources(int id) {
        Integer s = knownSources.remove(id);
        return s == null ? 0 : s;
    }

    private static void report(int id, int added) {
        InputDevice d = InputDevice.getDevice(id);
        if (d == null) return;
        knownSources.put(id, d.getSources());
        NueBridge.nativeOnInputDevice(id, added, d.getSources(), d.isVirtual(), d.getKeyboardType());
        updateGamepadFlag();
    }

    private static void updateGamepadFlag() {
        boolean pad = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                pad = true;
                break;
            }
        }
        NueBridge.nativeSetGamepadConnected(pad);
    }

    static void start(Activity a) {
        if (registered) return;
        registered = true;
        InputManager im = (InputManager) a.getSystemService(Context.INPUT_SERVICE);
        im.registerInputDeviceListener(listener, new Handler(Looper.getMainLooper()));
        for (int id : InputDevice.getDeviceIds()) report(id, 1);
    }

    static void stop(Activity a) {
        if (!registered) return;
        registered = false;
        InputManager im = (InputManager) a.getSystemService(Context.INPUT_SERVICE);
        im.unregisterInputDeviceListener(listener);
    }

    /** Called from Activity.dispatch*Event before SDL sees the event. Never consumes. */
    static void onEvent(android.view.InputEvent e) {
        float magnitude = 0f;
        if (e instanceof KeyEvent) {
            if (((KeyEvent) e).getAction() == KeyEvent.ACTION_DOWN) magnitude = 1f;
        } else if (e instanceof MotionEvent) {
            MotionEvent m = (MotionEvent) e;
            int src = m.getSource();
            if ((src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                float x = Math.abs(m.getAxisValue(MotionEvent.AXIS_X));
                float y = Math.abs(m.getAxisValue(MotionEvent.AXIS_Y));
                float z = Math.abs(m.getAxisValue(MotionEvent.AXIS_Z));
                float rz = Math.abs(m.getAxisValue(MotionEvent.AXIS_RZ));
                float lt = m.getAxisValue(MotionEvent.AXIS_LTRIGGER);
                float rt = m.getAxisValue(MotionEvent.AXIS_RTRIGGER);
                magnitude = Math.max(Math.max(Math.max(x, y), Math.max(z, rz)), Math.max(lt, rt));
            } else {
                int action = m.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_BUTTON_PRESS
                        || action == MotionEvent.ACTION_SCROLL) {
                    magnitude = 1f;
                } else {
                    magnitude = 0.1f; // moves and hovers: too weak to switch the "last used" device
                }
            }
        }
        if (magnitude < THRESHOLD) return;
        int source = e.getSource();
        if (source == lastSource) return;
        lastSource = source;
        InputDevice d = e.getDevice();
        boolean virt = d != null && d.isVirtual();
        int kb = d != null ? d.getKeyboardType() : 0;
        NueBridge.nativeOnInputSample(source, virt, kb, magnitude);
    }

    static void vibrate(Activity a, int ms, int amplitude) {
        if (a == null || ms <= 0) return;
        Vibrator v;
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager vm = (VibratorManager) a.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            v = vm != null ? vm.getDefaultVibrator() : null;
        } else {
            v = (Vibrator) a.getSystemService(Context.VIBRATOR_SERVICE);
        }
        if (v == null || !v.hasVibrator()) return;
        int amp = amplitude <= 0 ? VibrationEffect.DEFAULT_AMPLITUDE : Math.min(255, amplitude);
        v.vibrate(VibrationEffect.createOneShot(ms, amp));
    }
}
