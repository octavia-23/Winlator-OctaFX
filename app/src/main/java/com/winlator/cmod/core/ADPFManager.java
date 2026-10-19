package com.winlator.cmod.core;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.os.PowerManager;
import android.os.Process;
import android.util.Log;

public final class ADPFManager {
    private static final String TAG = "ADPFManager";
    private PerformanceHintManager.Session hintSession = null;
    private long targetDurationNanos = 16_666_666L; // Default 60 FPS
    private long lastFrameNanos = 0;
    private boolean isInitialized = false;

    public static void setSustainedPerformanceMode(Activity activity, boolean enabled) {
        if (activity == null) return;
        try {
            PowerManager pm = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
            if (pm != null && pm.isSustainedPerformanceModeSupported()) {
                activity.getWindow().setSustainedPerformanceMode(enabled);
                Log.i(TAG, "Sustained performance mode set to: " + enabled);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed setting sustained performance mode: " + t.getMessage());
        }
    }

    public void init(Context context, float refreshRate) {
        if (isInitialized) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            try {
                PerformanceHintManager phm = context.getSystemService(PerformanceHintManager.class);
                if (phm != null) {
                    float rate = refreshRate > 20.0f ? refreshRate : 60.0f;
                    targetDurationNanos = (long) (1_000_000_000.0 / rate);
                    int[] tids = new int[]{ Process.myTid() };
                    hintSession = phm.createHintSession(tids, targetDurationNanos);
                    isInitialized = true;
                    Log.i(TAG, "ADPF HintSession established: target=" + (targetDurationNanos / 1_000_000) + "ms (" + rate + "Hz)");
                }
            } catch (Throwable t) {
                Log.w(TAG, "Failed initializing ADPF hint session: " + t.getMessage());
            }
        }
    }

    public void onFrame() {
        long now = System.nanoTime();
        if (lastFrameNanos > 0 && hintSession != null) {
            long duration = now - lastFrameNanos;
            // Filter extreme outliers (e.g. paused / app switch)
            if (duration >= 1_000_000L && duration <= 100_000_000L) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    try {
                        hintSession.reportActualWorkDuration(duration);
                    } catch (Throwable ignored) {}
                }
            }
        }
        lastFrameNanos = now;
    }

    public void close() {
        if (hintSession != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                try {
                    hintSession.close();
                } catch (Throwable ignored) {}
            }
            hintSession = null;
        }
        isInitialized = false;
        lastFrameNanos = 0;
        Log.i(TAG, "ADPF HintSession closed.");
    }
}
