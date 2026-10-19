#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <android/hardware_buffer.h>
#include <android/log.h>

#define NPU_LOG_TAG "NPUFrameGen"
#define NPU_LOGI(...) __android_log_print(ANDROID_LOG_INFO, NPU_LOG_TAG, __VA_ARGS__)
#define NPU_LOGW(...) __android_log_print(ANDROID_LOG_WARN, NPU_LOG_TAG, __VA_ARGS__)
#define NPU_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, NPU_LOG_TAG, __VA_ARGS__)

enum FrameGenMode {
    FRAME_GEN_OFF = 0,
    FRAME_GEN_GPU = 1,     // Vulkan compute shader (Adreno 735)
    FRAME_GEN_NPU = 2      // Qualcomm Hexagon NPU (HTP Direct)
};

class NPUFrameInterpolator {
public:
    NPUFrameInterpolator();
    ~NPUFrameInterpolator();

    bool init();
    void release();
    bool isAvailable() const { return m_available; }
    const char *getBackendName() const { return m_backendName; }

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

    // Fast working buffers for downsampled motion estimation
    std::vector<uint8_t> m_lumA;
    std::vector<uint8_t> m_lumB;
    std::vector<int8_t>  m_flowX;
    std::vector<int8_t>  m_flowY;
    int m_cachedW = 0;
    int m_cachedH = 0;
    int m_dsW = 0;
    int m_dsH = 0;

    void ensureBuffers(int width, int height);
    void extractLumaDownsampled4x(const uint8_t *srcRGBA, int srcStride,
                                 uint8_t *dstLuma, int dstW, int dstH);
    void computeBlockMotion4x(const uint8_t *lumaA, const uint8_t *lumaB,
                              int8_t *flowX, int8_t *flowY,
                              int dsW, int dsH);
    void synthesizeInterpolatedFrame(const uint8_t *srcA, int strideA,
                                    const uint8_t *srcB, int strideB,
                                    uint8_t *dst, int strideDst,
                                    const int8_t *flowX, const int8_t *flowY,
                                    int width, int height,
                                    int dsW, int dsH);
};
