package org.nue.engine;

import android.content.Context;
import android.graphics.Rect;
import android.view.SurfaceHolder;
import android.view.WindowInsets;

import org.libsdl.app.SDLSurface;

/**
 * SDL's surface view plus surface events and insets for the engine.  The engine must release its swapchain
 * (bgfx / EGL) before surfaceDestroyed returns, so the native side blocks this callback (bounded) until the
 * engine acknowledges, see nue_android_surface_released().
 */
public class NueSurface extends SDLSurface {
    public NueSurface(Context context) {
        super(context);
    }

    private static Rect frame(SurfaceHolder holder) {
        Rect r = holder.getSurfaceFrame();
        return r != null ? r : new Rect();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        super.surfaceCreated(holder);
        Rect r = frame(holder);
        NueBridge.nativeOnSurface(NueBridge.SURFACE_CREATED, r.width(), r.height());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        super.surfaceChanged(holder, format, width, height);
        NueBridge.nativeOnSurface(NueBridge.SURFACE_CHANGED, width, height);
        NueDisplay.refresh(NueBridge.activity);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Engine first (blocks up to ~750 ms while the engine tears its swapchain down), then SDL's own handling.
        NueBridge.nativeOnSurface(NueBridge.SURFACE_DESTROYED, 0, 0);
        super.surfaceDestroyed(holder);
    }

    @Override
    public WindowInsets onApplyWindowInsets(android.view.View v, WindowInsets insets) {
        WindowInsets result = super.onApplyWindowInsets(v, insets);
        NueDisplay.onInsets(insets);
        return result;
    }
}
