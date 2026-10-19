package com.winlator.cmod.core;

import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Locale;

public final class SoCDetector {
    private static final String TAG = "SoCDetector";

    public enum Preset {
        SNAPDRAGON_8S_GEN3(
                "Snapdragon 8s Gen 3 (SM8635) - POCO F6 Optimal",
                "3,4,5,6,7",       // Prime Cortex-X4 (7) + Performance Cortex-A720 (3,4,5,6)
                "4,5,6,7",         // 32-bit WoW64: Upper Performance + Prime
                "1 Cortex-X4 @ 3.0GHz + 4 Cortex-A720 @ 2.8GHz (Mask: Cores 3-7, Bypasses Little Cores 0-2)"
        ),
        SNAPDRAGON_8_GEN3(
                "Snapdragon 8 Gen 3 (SM8650)",
                "2,3,4,5,6,7",     // 1x X4 + 5x A720 (Cores 2-7)
                "4,5,6,7",
                "1 Cortex-X4 + 5 Cortex-A720 (Mask: Cores 2-7, Bypasses Little Cores 0-1)"
        ),
        SNAPDRAGON_8_GEN2(
                "Snapdragon 8 Gen 2 (SM8550)",
                "3,4,5,6,7",       // 1x X3 + 2x A715 + 2x A710 (Cores 3-7)
                "4,5,6,7",
                "1 Cortex-X3 + 4 Performance Cores (Mask: Cores 3-7, Bypasses Little Cores 0-2)"
        ),
        SNAPDRAGON_8_GEN1_888(
                "Snapdragon 8 Gen 1 / 888 (1+3+4)",
                "4,5,6,7",         // 1x X1/X2 + 3x A710/A78 (Cores 4-7)
                "4,5,6,7",
                "1 Prime + 3 Performance Cores (Mask: Cores 4-7, Bypasses Little Cores 0-3)"
        ),
        SNAPDRAGON_870_865(
                "Snapdragon 870 / 865 (1+3+4)",
                "4,5,6,7",
                "4,5,6,7",
                "1 Prime Kryo 585 + 3 Gold Cores (Mask: Cores 4-7, Bypasses Silver Cores 0-3)"
        ),
        DIMENSITY_FLAGSHIP(
                "MediaTek Dimensity 9300 / 9400 (All-Big-Core)",
                "0,1,2,3,4,5,6,7", // All 8 cores are high performance Cortex-X and Cortex-A7
                "4,5,6,7",
                "All 8 Cores Active (High Throughput / All-Big Architecture)"
        ),
        DIMENSITY_STANDARD(
                "MediaTek Dimensity 8000 / 9000 (4+4)",
                "4,5,6,7",
                "4,5,6,7",
                "4 Performance Cores (Mask: Cores 4-7, Bypasses Efficiency Cores 0-3)"
        ),
        GENERIC_OCTA_CORE(
                "Standard Octa-Core (Balanced Performance)",
                "4,5,6,7",
                "4,5,6,7",
                "Upper 4 Performance Cores (Mask: Cores 4-7)"
        ),
        ALL_CORES(
                "All CPU Cores (No Affinity Masking)",
                "0,1,2,3,4,5,6,7",
                "4,5,6,7",
                "All Cores Active without exclusion"
        );

        public final String title;
        public final String cpuList64;
        public final String cpuList32;
        public final String description;

        Preset(String title, String cpuList64, String cpuList32, String description) {
            this.title = title;
            this.cpuList64 = cpuList64;
            this.cpuList32 = cpuList32;
            this.description = description;
        }

        @Override
        public String toString() {
            return title;
        }
    }

    private static Preset detectedPreset = null;
    private static String socModelName = "";

    public static synchronized Preset getDetectedPreset() {
        if (detectedPreset == null) {
            detectSoC();
        }
        return detectedPreset;
    }

    public static synchronized String getSoCModelName() {
        if (detectedPreset == null) {
            detectSoC();
        }
        return socModelName;
    }

    private static void detectSoC() {
        String hardware = Build.HARDWARE != null ? Build.HARDWARE.toLowerCase(Locale.ROOT) : "";
        String board = Build.BOARD != null ? Build.BOARD.toLowerCase(Locale.ROOT) : "";
        String socModel = "";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            socModel = Build.SOC_MODEL != null ? Build.SOC_MODEL.toLowerCase(Locale.ROOT) : "";
        }
        String cpuInfo = readCpuInfo();

        Log.i(TAG, "Hardware: " + hardware + ", Board: " + board + ", SocModel: " + socModel);

        String device = Build.DEVICE != null ? Build.DEVICE.toLowerCase(Locale.ROOT) : "";
        String product = Build.PRODUCT != null ? Build.PRODUCT.toLowerCase(Locale.ROOT) : "";

        if (socModel.contains("sm8635") || hardware.contains("sm8635") || board.contains("sm8635")
                || device.contains("peridot") || product.contains("peridot")
                || cpuInfo.contains("sm8635")) {
            detectedPreset = Preset.SNAPDRAGON_8S_GEN3;
            socModelName = "Qualcomm Snapdragon 8s Gen 3 (SM8635 - POCO F6)";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (socModel.contains("sm8650") || hardware.contains("sm8650") || board.contains("sm8650")
                || board.contains("pineapple") || cpuInfo.contains("sm8650")) {
            detectedPreset = Preset.SNAPDRAGON_8_GEN3;
            socModelName = "Qualcomm Snapdragon 8 Gen 3 (SM8650)";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (socModel.contains("sm8550") || hardware.contains("sm8550") || board.contains("sm8550")
                || board.contains("kalama") || cpuInfo.contains("sm8550")) {
            detectedPreset = Preset.SNAPDRAGON_8_GEN2;
            socModelName = "Qualcomm Snapdragon 8 Gen 2 (SM8550)";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (socModel.contains("sm8450") || socModel.contains("sm8475") || socModel.contains("sm8350")
                || board.contains("taro") || board.contains("cape") || board.contains("lahaina")
                || hardware.contains("qcom") && (cpuInfo.contains("0xd46") || cpuInfo.contains("0xd48"))) {
            detectedPreset = Preset.SNAPDRAGON_8_GEN1_888;
            socModelName = "Qualcomm Snapdragon 8 Gen 1 / 888";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (socModel.contains("sm8250") || board.contains("kona") || cpuInfo.contains("sm8250")) {
            detectedPreset = Preset.SNAPDRAGON_870_865;
            socModelName = "Qualcomm Snapdragon 870 / 865";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (hardware.contains("mt6989") || hardware.contains("mt6991") || socModel.contains("dimensity 9300") || socModel.contains("dimensity 9400")) {
            detectedPreset = Preset.DIMENSITY_FLAGSHIP;
            socModelName = "MediaTek Dimensity 9300 / 9400 (All-Big-Core)";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        if (hardware.startsWith("mt") || board.startsWith("mt")) {
            detectedPreset = Preset.DIMENSITY_STANDARD;
            socModelName = "MediaTek Dimensity (" + (socModel.isEmpty() ? hardware : socModel) + ")";
            Log.i(TAG, "Matched: " + socModelName);
            return;
        }

        int numCores = Runtime.getRuntime().availableProcessors();
        if (numCores >= 8) {
            detectedPreset = Preset.GENERIC_OCTA_CORE;
            socModelName = "Octa-Core Processor (" + (socModel.isEmpty() ? hardware : socModel) + ")";
        } else {
            detectedPreset = Preset.ALL_CORES;
            socModelName = "Standard Processor (" + numCores + " Cores)";
        }
        Log.i(TAG, "Matched fallback: " + socModelName);
    }

    private static String readCpuInfo() {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line.toLowerCase(Locale.ROOT)).append("\n");
            }
        } catch (Throwable ignored) {}
        return sb.toString();
    }
}
