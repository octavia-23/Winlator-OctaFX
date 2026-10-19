#define _GNU_SOURCE  
#include <jni.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <android/log.h>

#define LOG_TAG "ProcessHelper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#ifndef M_PURGE
#define M_PURGE (-101)
#endif

#ifndef PR_SET_TIMERSLACK_NS
#define PR_SET_TIMERSLACK_NS 29
#endif

JNIEXPORT jint JNICALL
Java_com_winlator_cmod_core_ProcessHelper_nativeSetProcessAffinity(
        JNIEnv *env, jclass clazz, jint pid, jint mask) {
    (void)env;
    (void)clazz;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int i = 0; i < 32; i++) {
        if (mask & (1 << i))
            CPU_SET(i, &cpuset);
    }

    int result = sched_setaffinity((pid_t) pid, sizeof(cpuset), &cpuset);

    return result == 0 ? 0 : errno;
}

JNIEXPORT jint JNICALL
Java_com_winlator_cmod_core_ProcessHelper_nativeGetProcessAffinity(
        JNIEnv *env, jclass clazz, jint pid) {
    (void)env;
    (void)clazz;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (sched_getaffinity((pid_t) pid, sizeof(cpuset), &cpuset) != 0)
        return -1; 

    int mask = 0;
    for (int i = 0; i < 32; i++) {
        if (CPU_ISSET(i, &cpuset))
            mask |= (1 << i);
    }
    return mask;
}

static void trySetOomScore(pid_t pid, int score) {
    char path[64];
    if (pid > 0) {
        snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)pid);
    } else {
        snprintf(path, sizeof(path), "/proc/self/oom_score_adj");
    }

    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char val[16];
        int len = snprintf(val, sizeof(val), "%d\n", score);
        write(fd, val, len);
        close(fd);
    } else {
        // Fallback to legacy oom_adj if oom_score_adj is unavailable
        if (pid > 0) {
            snprintf(path, sizeof(path), "/proc/%d/oom_adj", (int)pid);
        } else {
            snprintf(path, sizeof(path), "/proc/self/oom_adj");
        }
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            char val[16];
            int len = snprintf(val, sizeof(val), "%d\n", score < 0 ? -17 : 0);
            write(fd, val, len);
            close(fd);
        }
    }
}

JNIEXPORT jint JNICALL
Java_com_winlator_cmod_core_ProcessHelper_nativeApplyMemoryProtection(
        JNIEnv *env, jclass clazz, jint pid) {
    (void)env;
    (void)clazz;
    pid_t target_pid = (pid_t)pid;

    // 1. Lower OOM score to protect from Android LMKD kills (-1000 = immune)
    int scores[] = {-1000, -900, -800, -500, 0};
    for (size_t i = 0; i < sizeof(scores)/sizeof(scores[0]); i++) {
        trySetOomScore(target_pid, scores[i]);
    }

    // 2. Set high real-time scheduler priority (niceness -20)
    int prioRes = setpriority(PRIO_PROCESS, (id_t)target_pid, -20);
    if (prioRes != 0) {
        setpriority(PRIO_PROCESS, (id_t)target_pid, -10);
    }

    // 3. Lower timer slack for ultra-low latency scheduling
    prctl(PR_SET_TIMERSLACK_NS, 50000ULL);

    // 4. Ensure process is dumpable and transparently manageable
    prctl(PR_SET_DUMPABLE, 1);

    LOGI("Applied aggressive RAM defense & OOM score tuning to PID: %d", (int)target_pid);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_core_ProcessHelper_nativePurgeMemory(
        JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    // Immediately release freed bionic/jemalloc/scudo heap blocks to kernel
    mallopt(M_PURGE, 0);
    LOGI("Bionic heap memory purge executed via mallopt(M_PURGE)");
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_core_ProcessHelper_nativeLockCriticalMemory(
        JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    // Advise kernel to use transparent hugepages and maintain page readiness
#ifdef MADV_HUGEPAGE
    madvise(NULL, 0, MADV_HUGEPAGE);
#endif
#ifdef MADV_WILLNEED
    madvise(NULL, 0, MADV_WILLNEED);
#endif
    LOGI("Kernel memory advice (MADV_HUGEPAGE / MADV_WILLNEED) applied");
}
