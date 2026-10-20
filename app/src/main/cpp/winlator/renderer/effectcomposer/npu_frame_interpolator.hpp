#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <android/hardware_buffer.h>
#include <android/log.h>

#define NPU_LOG_TAG "NPUFrameGen"
#define NPU_LOGI(...) __android_log_print(ANDROID_LOG_INFO, NPU_LOG_TAG, __VA_ARGS__)
#define NPU_LOGW(...) __android_log_print(ANDROID_LOG_WARN, NPU_LOG_TAG, __VA_ARGS__)
#define NPU_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, NPU_LOG_TAG, __VA_ARGS__)

enum FrameGenMode {
    FRAME_GEN_OFF = 0,
    FRAME_GEN_GPU = 1,     // Vulkan compute / FG 2.0 (Adreno 735)
    FRAME_GEN_NPU = 2      // Qualcomm Hexagon NPU / FG 2.0 (HTP Direct)
};

// Lightweight persistent thread pool for sub-2.5ms parallel frame interpolation
class FrameGenThreadPool {
public:
    explicit FrameGenThreadPool(size_t threads = 4) : stop(false), inFlight(0) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->cv.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                    if (inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::unique_lock<std::mutex> lk(queue_mutex);
                        finished_cv.notify_all();
                    }
                }
            });
        }
    }

    void enqueue(std::function<void()> task) {
        inFlight.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        finished_cv.wait(lock, [this] {
            return inFlight.load(std::memory_order_acquire) == 0;
        });
    }

    ~FrameGenThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        cv.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::condition_variable finished_cv;
    bool stop;
    std::atomic<int> inFlight;
};

class NPUFrameInterpolator {
public:
    NPUFrameInterpolator();
    ~NPUFrameInterpolator();

    bool init();
    void release();
    bool isAvailable() const { return m_available; }
    const char *getBackendName() const { return m_backendName; }

    // Frame Gen 2.0: Multi-threaded, HUD-preserving, sub-pixel motion compensated interpolation
    bool interpolate(AHardwareBuffer *prevBuffer,
                     AHardwareBuffer *currBuffer,
                     AHardwareBuffer *outputBuffer,
                     int width, int height);

private:
    bool initQnnHtp();
    void releaseQnnHtp();

    bool m_initialized = false;
    bool m_available = false;
    const char *m_backendName = "None";

    void *m_qnnHtpHandle = nullptr;
    void *m_qnnSystemHandle = nullptr;
    void *m_cdspHandle = nullptr;

    std::unique_ptr<FrameGenThreadPool> m_threadPool;

    // Fast working buffers for downsampled motion estimation & spatial filtering
    std::vector<uint8_t> m_lumA;
    std::vector<uint8_t> m_lumB;
    std::vector<int8_t>  m_flowX;
    std::vector<int8_t>  m_flowY;
    std::vector<int8_t>  m_filteredFlowX;
    std::vector<int8_t>  m_filteredFlowY;
    int m_cachedW = 0;
    int m_cachedH = 0;
    int m_dsW = 0;
    int m_dsH = 0;

    void ensureBuffers(int width, int height);

    // Parallel slice operations
    void extractLumaSlice(const uint8_t *srcRGBA, int srcStride,
                          uint8_t *dstLuma, int dstW, int yStart, int yEnd);

    void computeBlockMotionSlice(const uint8_t *lumaA, const uint8_t *lumaB,
                                int8_t *flowX, int8_t *flowY,
                                int dsW, int dsH, int byStart, int byEnd);

    void filterMotionField(int blocksX, int blocksY);

    void synthesizeFrameSlice(const uint8_t *srcA, int strideA,
                             const uint8_t *srcB, int strideB,
                             uint8_t *dst, int strideDst,
                             const int8_t *flowX, const int8_t *flowY,
                             int width, int height,
                             int dsW, int dsH,
                             int yStart, int yEnd);
};
