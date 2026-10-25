package org.nue.engine;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;

/**
 * Audio facts and policy hooks.  The stream itself is opened by SDL3 (AAudio, OpenSL ES fallback); this class supplies
 * the device's native rate / burst size, tracks the output route (headset plug, Bluetooth) and owns audio focus
 * (phone calls, other media apps), which SDL does not manage.
 */
final class NueAudio {
    private NueAudio() {}

    private static AudioManager am;
    private static AudioFocusRequest focusRequest;
    private static boolean registered;
    private static int lastRouteType = -1;

    private static final AudioManager.OnAudioFocusChangeListener focusListener = new AudioManager.OnAudioFocusChangeListener() {
        @Override
        public void onAudioFocusChange(int change) {
            NueBridge.nativeOnAudioFocus(change);
        }
    };

    private static final AudioDeviceCallback deviceCallback = new AudioDeviceCallback() {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
            pushRoute();
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            pushRoute();
        }
    };

    static void init(Activity a) {
        am = (AudioManager) a.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        PackageManager pm = a.getPackageManager();
        int rate = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE), 48000);
        int frames = parse(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER), 192);
        boolean low = pm.hasSystemFeature(PackageManager.FEATURE_AUDIO_LOW_LATENCY);
        boolean pro = pm.hasSystemFeature(PackageManager.FEATURE_AUDIO_PRO);
        NueBridge.nativeSetAudio(rate, frames, low, pro, currentRouteType());
        if (!registered) {
            registered = true;
            am.registerAudioDeviceCallback(deviceCallback, new Handler(Looper.getMainLooper()));
        }
        a.setVolumeControlStream(AudioManager.STREAM_MUSIC);
    }

    static void shutdown() {
        if (am != null && registered) {
            am.unregisterAudioDeviceCallback(deviceCallback);
            registered = false;
        }
        abandon();
    }

    private static int parse(String s, int def) {
        try {
            return s == null ? def : Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }

    /** The route Android is most likely using: Bluetooth > USB > wired > HDMI > speaker. */
    private static int currentRouteType() {
        if (am == null) return 0;
        int best = 0, bestScore = -1;
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int t = d.getType();
            int score;
            switch (t) {
                case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
                case AudioDeviceInfo.TYPE_BLUETOOTH_SCO:
                case AudioDeviceInfo.TYPE_BLE_HEADSET:
                case AudioDeviceInfo.TYPE_BLE_SPEAKER:
                    score = 6;
                    break;
                case AudioDeviceInfo.TYPE_USB_DEVICE:
                case AudioDeviceInfo.TYPE_USB_HEADSET:
                    score = 5;
                    break;
                case AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
                case AudioDeviceInfo.TYPE_WIRED_HEADSET:
                case AudioDeviceInfo.TYPE_LINE_ANALOG:
                    score = 4;
                    break;
                case AudioDeviceInfo.TYPE_HDMI:
                    score = 3;
                    break;
                case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER:
                    score = 1;
                    break;
                default:
                    score = 0;
            }
            if (score > bestScore) {
                bestScore = score;
                best = t;
            }
        }
        return best;
    }

    private static void pushRoute() {
        int t = currentRouteType();
        if (t == lastRouteType) return;
        lastRouteType = t;
        NueBridge.nativeOnAudioRoute(t);
    }

    /** USAGE_GAME focus request; losing it pauses output through the lifecycle model (transient) or for good. */
    static void audioFocus(Activity a, boolean request) {
        if (am == null) return;
        if (!request) {
            abandon();
            return;
        }
        if (focusRequest == null) {
            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attrs)
                    .setOnAudioFocusChangeListener(focusListener, new Handler(Looper.getMainLooper()))
                    .setWillPauseWhenDucked(false) // we duck ourselves: engine sound must keep running
                    .build();
        }
        am.requestAudioFocus(focusRequest);
    }

    private static void abandon() {
        if (am != null && focusRequest != null) am.abandonAudioFocusRequest(focusRequest);
    }
}
