package com.winlator.cmod.core;

import android.content.Context;
import android.util.Log;

import com.winlator.cmod.xenvironment.ImageFs;

import org.apache.commons.compress.compressors.zstandard.ZstdCompressorInputStream;
import org.apache.commons.compress.compressors.zstandard.ZstdCompressorOutputStream;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;

public final class DXVKPipelineCacheManager {
    private static final String TAG = "DXVKCacheManager";

    public static File getCacheDirectory(Context context) {
        File dir = new File(context.getFilesDir(), "imagefs/" + ImageFs.CACHE_PATH);
        if (!dir.exists()) {
            dir.mkdirs();
        }
        return dir;
    }

    public static long getCacheSize(Context context) {
        File dir = getCacheDirectory(context);
        long total = 0;
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                if (f.isFile()) total += f.length();
            }
        }
        return total;
    }

    public static void clearCache(Context context) {
        File dir = getCacheDirectory(context);
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                if (f.isFile() && (f.getName().endsWith(".dxvk-cache") || f.getName().endsWith(".zst") || f.getName().endsWith(".dxvk-cache.zst"))) {
                    f.delete();
                }
            }
        }
        Log.i(TAG, "Cleared DXVK state and pipeline caches.");
    }

    public static void restoreCompressedCaches(Context context) {
        File dir = getCacheDirectory(context);
        File[] files = dir.listFiles();
        if (files == null) return;

        for (File zstFile : files) {
            if (zstFile.isFile() && zstFile.getName().endsWith(".dxvk-cache.zst")) {
                String targetName = zstFile.getName().substring(0, zstFile.getName().length() - 4);
                File targetFile = new File(dir, targetName);
                if (!targetFile.exists() || targetFile.length() == 0 || targetFile.lastModified() < zstFile.lastModified()) {
                    decompressZstd(zstFile, targetFile);
                }
            }
        }
    }

    public static void compressAllCaches(Context context) {
        File dir = getCacheDirectory(context);
        File[] files = dir.listFiles();
        if (files == null) return;

        for (File rawFile : files) {
            if (rawFile.isFile() && rawFile.getName().endsWith(".dxvk-cache") && !rawFile.getName().endsWith(".zst")) {
                if (rawFile.length() > 0) {
                    File zstFile = new File(dir, rawFile.getName() + ".zst");
                    compressZstd(rawFile, zstFile);
                }
            }
        }
    }

    public static boolean compressZstd(File src, File dst) {
        try (BufferedInputStream in = new BufferedInputStream(new FileInputStream(src), 65536);
             BufferedOutputStream out = new BufferedOutputStream(new FileOutputStream(dst), 65536);
             ZstdCompressorOutputStream zstdOut = new ZstdCompressorOutputStream(out, 3)) {
            byte[] buf = new byte[65536];
            int read;
            while ((read = in.read(buf)) != -1) {
                zstdOut.write(buf, 0, read);
            }
            zstdOut.flush();
            Log.i(TAG, "Compressed DXVK cache " + src.getName() + " (" + src.length() + "B -> " + dst.length() + "B)");
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "Failed to compress DXVK cache: " + src.getName(), t);
            return false;
        }
    }

    public static boolean decompressZstd(File src, File dst) {
        try (BufferedInputStream in = new BufferedInputStream(new FileInputStream(src), 65536);
             ZstdCompressorInputStream zstdIn = new ZstdCompressorInputStream(in);
             BufferedOutputStream out = new BufferedOutputStream(new FileOutputStream(dst), 65536)) {
            byte[] buf = new byte[65536];
            int read;
            while ((read = zstdIn.read(buf)) != -1) {
                out.write(buf, 0, read);
            }
            out.flush();
            Log.i(TAG, "Decompressed DXVK cache " + src.getName() + " -> " + dst.getName() + " (" + dst.length() + "B)");
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "Failed to decompress DXVK cache: " + src.getName(), t);
            return false;
        }
    }
}
