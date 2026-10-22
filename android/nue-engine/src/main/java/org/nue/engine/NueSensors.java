package org.nue.engine;

import android.app.Activity;
import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;

/**
 * Game rotation vector (gyro + accelerometer fusion without magnetometer): the stable "tilt the phone to steer" source.
 * SDL3 exposes raw accelerometer / gyroscope only, so this one is ours.  Off until the engine asks for it.
 */
final class NueSensors {
    private NueSensors() {}

    private static SensorManager sm;
    private static Sensor rotation;
    private static boolean on, wanted, suspended;
    private static final float[] q = new float[4];

    private static final SensorEventListener listener = new SensorEventListener() {
        @Override
        public void onSensorChanged(SensorEvent e) {
            // values = x*sin(a/2), y*sin(a/2), z*sin(a/2), cos(a/2) (API 18+ always delivers all four)
            SensorManager.getQuaternionFromVector(q, e.values);
            NueBridge.nativeSetRotation(q[1], q[2], q[3], q[0], e.timestamp);
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    };

    static void init(Activity a) {
        sm = (SensorManager) a.getSystemService(Context.SENSOR_SERVICE);
        rotation = sm != null ? sm.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR) : null;
    }

    static boolean available() {
        return rotation != null;
    }

    /** SENSOR_DELAY_GAME is about 50 Hz; enough for steering, cheap on battery. */
    static void enable(boolean enable) {
        wanted = enable;
        apply();
    }

    /** Sensors must not run while the activity is paused (battery, and Android stops delivering anyway). */
    static void setActive(boolean active) {
        suspended = !active;
        apply();
    }

    private static void apply() {
        if (rotation == null) return;
        boolean run = wanted && !suspended;
        if (run == on) return;
        on = run;
        if (run) sm.registerListener(listener, rotation, SensorManager.SENSOR_DELAY_GAME);
        else sm.unregisterListener(listener);
    }
}
