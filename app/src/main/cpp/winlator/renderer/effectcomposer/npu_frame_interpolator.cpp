#include "npu_frame_interpolator.hpp"

#include <dlfcn.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

// QNN Type Definitions for Dynamic Loading
typedef enum {
    QNN_SUCCESS = 0,
    QNN_COMMON_ERROR_NOT_SUPPORTED = 1,
    QNN_COMMON_ERROR_INVALID_ARGUMENT = 2
} Qnn_ErrorHandle_t;

typedef void* Qnn_BackendHandle_t;
typedef void* Qnn_DeviceHandle_t;
typedef void* Qnn_ContextHandle_t;
typedef void* Qnn_GraphHandle_t;
typedef void* Qnn_LoggerHandle_t;
typedef void* Qnn_ProfileHandle_t;

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} Qnn_Version_t;

typedef struct {
    const char *providerName;
    Qnn_Version_t apiVersion;
} QnnInterface_t;

typedef Qnn_ErrorHandle_t (*pfnQnnInterface_getProviders)(const QnnInterface_t***, uint32_t*);

NPUFrameInterpolator::NPUFrameInterpolator() {
}

NPUFrameInterpolator::~NPUFrameInterpolator() {
    release();
}

bool NPUFrameInterpolator::init() {
    if (m_initialized) return m_available;

    NPU_LOGI("Initializing Qualcomm Hexagon / NEON Frame Gen Engine...");

    if (!m_threadPool) {
        m_threadPool = std::make_unique<FrameGenThreadPool>(4);
    }

    if (initQnnHtp()) {
        m_available = true;
        m_backendName = (m_graphHandle != nullptr) ? "Qualcomm Hexagon HTP" : "CPU NEON SIMD";
        NPU_LOGI("Hexagon HTP Backend initialized (graph bound: %s): %s",
                 (m_graphHandle != nullptr ? "yes" : "no"), m_backendName);
    } else {
        m_available = true;
        m_backendName = "CPU NEON SIMD";
        NPU_LOGI("CPU NEON SIMD engine active: %s", m_backendName);
    }

    m_initialized = true;
    return m_available;
}

void NPUFrameInterpolator::release() {
    releaseQnnHtp();
    m_threadPool.reset();
    m_lumA.clear();
    m_lumB.clear();
    m_flowX.clear();
    m_flowY.clear();
    m_filteredFlowX.clear();
    m_filteredFlowY.clear();
    m_cachedW = 0;
    m_cachedH = 0;
    m_initialized = false;
    m_available = false;
}

bool NPUFrameInterpolator::initQnnHtp() {
    const char *htpPaths[] = {
        "/vendor/lib64/libQnnHtp.so",
        "/vendor/lib64/libQnnHtpV75Stub.so",
        "/vendor/lib64/libQnnHtpV75.so",
        "/vendor/lib64/libQnnHtpV73.so",
        "/vendor/lib64/libQnnHtpV69.so",
        "/system/vendor/lib64/libQnnHtp.so",
        "libQnnHtp.so"
    };

    for (const char *path : htpPaths) {
        m_qnnHtpHandle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (m_qnnHtpHandle) {
            NPU_LOGI("Found Qualcomm HTP library at: %s", path);
            break;
        }
    }

    m_cdspHandle = dlopen("/vendor/lib64/libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!m_cdspHandle) {
        m_cdspHandle = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    }

    if (!m_qnnHtpHandle) {
        NPU_LOGI("Qualcomm HTP library not resident in user space; readying high-performance NEON SIMD engine");
        return false;
    }

    // Resolve QNN Backend, Device, Context, and Graph lifecycle functions
    m_pfnBackendCreate = (pfnQnnBackend_create)dlsym(m_qnnHtpHandle, "QnnBackend_create");
    m_pfnBackendInitialize = (pfnQnnBackend_initialize)dlsym(m_qnnHtpHandle, "QnnBackend_initialize");
    m_pfnDeviceCreate = (pfnQnnDevice_create)dlsym(m_qnnHtpHandle, "QnnDevice_create");
    m_pfnContextCreate = (pfnQnnContext_create)dlsym(m_qnnHtpHandle, "QnnContext_create");
    m_pfnContextFree = (pfnQnnContext_free)dlsym(m_qnnHtpHandle, "QnnContext_free");
    m_pfnDeviceFree = (pfnQnnDevice_free)dlsym(m_qnnHtpHandle, "QnnDevice_free");
    m_pfnBackendFree = (pfnQnnBackend_free)dlsym(m_qnnHtpHandle, "QnnBackend_free");
    m_pfnGraphExecute = (pfnQnnGraph_execute)dlsym(m_qnnHtpHandle, "QnnGraph_execute");

    // Initialize QNN Backend
    if (m_pfnBackendCreate) {
        uint32_t bRes = m_pfnBackendCreate(nullptr, nullptr, &m_backendHandle);
        if (bRes == QNN_SUCCESS && m_backendHandle) {
            NPU_LOGI("QNN HTP Backend created: %p", m_backendHandle);
            if (m_pfnBackendInitialize) {
                m_pfnBackendInitialize(m_backendHandle);
            }
        } else {
            NPU_LOGW("QNN HTP Backend creation returned %u", bRes);
        }
    }

    // Initialize QNN Device
    if (m_pfnDeviceCreate) {
        uint32_t dRes = m_pfnDeviceCreate(nullptr, nullptr, &m_deviceHandle);
        if (dRes == QNN_SUCCESS && m_deviceHandle) {
            NPU_LOGI("QNN HTP Device created: %p", m_deviceHandle);
        } else {
            NPU_LOGW("QNN HTP Device creation returned %u", dRes);
        }
    }

    // Initialize QNN Context
    if (m_pfnContextCreate && m_backendHandle) {
        uint32_t cRes = m_pfnContextCreate(m_backendHandle, m_deviceHandle, nullptr, &m_contextHandle);
        if (cRes == QNN_SUCCESS && m_contextHandle) {
            NPU_LOGI("QNN HTP Context created successfully: %p", m_contextHandle);
        } else {
            NPU_LOGW("QNN HTP Context creation returned %u", cRes);
        }
    }

    return (m_backendHandle != nullptr);
}

void NPUFrameInterpolator::releaseQnnHtp() {
    if (m_contextHandle && m_pfnContextFree) {
        m_pfnContextFree(m_contextHandle, nullptr);
        m_contextHandle = nullptr;
    }
    if (m_deviceHandle && m_pfnDeviceFree) {
        m_pfnDeviceFree(m_deviceHandle);
        m_deviceHandle = nullptr;
    }
    if (m_backendHandle && m_pfnBackendFree) {
        m_pfnBackendFree(m_backendHandle);
        m_backendHandle = nullptr;
    }
    m_graphHandle = nullptr;

    if (m_qnnHtpHandle) {
        dlclose(m_qnnHtpHandle);
        m_qnnHtpHandle = nullptr;
    }
    if (m_qnnSystemHandle) {
        dlclose(m_qnnSystemHandle);
        m_qnnSystemHandle = nullptr;
    }
    if (m_cdspHandle) {
        dlclose(m_cdspHandle);
        m_cdspHandle = nullptr;
    }
}

void NPUFrameInterpolator::ensureBuffers(int width, int height) {
    if (m_cachedW == width && m_cachedH == height) return;

    m_cachedW = width;
    m_cachedH = height;
    m_dsW = (width + 3) / 4;
    m_dsH = (height + 3) / 4;

    size_t lumaSize = static_cast<size_t>(m_dsW) * m_dsH;
    m_lumA.resize(lumaSize);
    m_lumB.resize(lumaSize);

    size_t blockSize = static_cast<size_t>((m_dsW + 7) / 8) * ((m_dsH + 7) / 8);
    m_flowX.resize(blockSize, 0);
    m_flowY.resize(blockSize, 0);
    m_filteredFlowX.resize(blockSize, 0);
    m_filteredFlowY.resize(blockSize, 0);
}

void NPUFrameInterpolator::extractLumaSlice(const uint8_t *srcRGBA, int srcStride,
                                           uint8_t *dstLuma, int dstW, int yStart, int yEnd) {
    for (int y = yStart; y < yEnd; y++) {
        const uint8_t *srcRow = srcRGBA + (y * 4) * (srcStride * 4);
        uint8_t *dstRow = dstLuma + y * dstW;

        int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        for (; x <= dstW - 8; x += 8) {
            uint8_t r_arr[8], g_arr[8], b_arr[8];
            for (int k = 0; k < 8; k++) {
                const uint8_t *p = srcRow + (x + k) * 16;
                r_arr[k] = p[0];
                g_arr[k] = p[1];
                b_arr[k] = p[2];
            }
            uint8x8_t r = vld1_u8(r_arr);
            uint8x8_t g = vld1_u8(g_arr);
            uint8x8_t b = vld1_u8(b_arr);
            uint16x8_t luma16 = vmull_u8(r, vdup_n_u8(2));
            luma16 = vmlal_u8(luma16, g, vdup_n_u8(5));
            luma16 = vaddw_u8(luma16, b);
            uint8x8_t luma8 = vshrn_n_u16(luma16, 3);
            vst1_u8(dstRow + x, luma8);
        }
#endif
        for (; x < dstW; x++) {
            const uint8_t *p = srcRow + x * 16;
            dstRow[x] = static_cast<uint8_t>((p[0] * 2 + p[1] * 5 + p[2]) >> 3);
        }
    }
}

void NPUFrameInterpolator::computeBlockMotionSlice(const uint8_t *lumaA, const uint8_t *lumaB,
                                                  int8_t *flowX, int8_t *flowY,
                                                  int dsW, int dsH, int byStart, int byEnd) {
    const int BLOCK_SIZE = 8;
    const int SEARCH_RADIUS = 6;
    int blocksX = (dsW + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (int by = byStart; by < byEnd; by++) {
        int py = by * BLOCK_SIZE;
        for (int bx = 0; bx < blocksX; bx++) {
            int px = bx * BLOCK_SIZE;
            int blockIdx = by * blocksX + bx;

            int bestSAD = 1000000;
            int bestDx = 0;
            int bestDy = 0;

            // Diamond / Spiral search pattern with early exit threshold
            for (int dy = -SEARCH_RADIUS; dy <= SEARCH_RADIUS; dy += 2) {
                int refY = py + dy;
                if (refY < 0 || refY + BLOCK_SIZE > dsH) continue;

                for (int dx = -SEARCH_RADIUS; dx <= SEARCH_RADIUS; dx += 2) {
                    int refX = px + dx;
                    if (refX < 0 || refX + BLOCK_SIZE > dsW) continue;

                    int sad = 0;
                    for (int r = 0; r < BLOCK_SIZE; r += 2) {
                        const uint8_t *rowCur = lumaB + (py + r) * dsW + px;
                        const uint8_t *rowRef = lumaA + (refY + r) * dsW + refX;

                        for (int c = 0; c < BLOCK_SIZE; c += 2) {
                            sad += std::abs(static_cast<int>(rowCur[c]) - static_cast<int>(rowRef[c]));
                        }
                    }

                    // Spatial coherence penalty for large jumps
                    sad += (std::abs(dx) + std::abs(dy)) * 2;

                    if (sad < bestSAD) {
                        bestSAD = sad;
                        bestDx = dx;
                        bestDy = dy;
                        if (bestSAD < 16) goto match_found; // Early exit on high-precision match
                    }
                }
            }

match_found:
            // Scale to full-resolution displacement
            flowX[blockIdx] = static_cast<int8_t>(std::clamp(bestDx * 4, -32, 32));
            flowY[blockIdx] = static_cast<int8_t>(std::clamp(bestDy * 4, -32, 32));
        }
    }
}

// 3x3 Spatial Vector Filter (FG 2.0: Removes isolated noise vectors & block-edge tearing)
void NPUFrameInterpolator::filterMotionField(int blocksX, int blocksY) {
    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            int idx = by * blocksX + bx;

            int sumX = 0;
            int sumY = 0;
            int count = 0;

            for (int dy = -1; dy <= 1; dy++) {
                int ny = by + dy;
                if (ny < 0 || ny >= blocksY) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = bx + dx;
                    if (nx < 0 || nx >= blocksX) continue;

                    int nIdx = ny * blocksX + nx;
                    sumX += m_flowX[nIdx];
                    sumY += m_flowY[nIdx];
                    count++;
                }
            }

            m_filteredFlowX[idx] = static_cast<int8_t>(sumX / count);
            m_filteredFlowY[idx] = static_cast<int8_t>(sumY / count);
        }
    }
}

void NPUFrameInterpolator::synthesizeFrameSlice(const uint8_t *srcA, int strideA,
                                               const uint8_t *srcB, int strideB,
                                               uint8_t *dst, int strideDst,
                                               const int8_t *flowX, const int8_t *flowY,
                                               int width, int height,
                                               int dsW, int dsH,
                                               int yStart, int yEnd) {
    const int BLOCK_SIZE_FULL = 32;
    int blocksX = (dsW + 7) / 8;
    int blocksY = (dsH + 7) / 8;

    for (int y = yStart; y < yEnd; y++) {
        int by = std::clamp(y / BLOCK_SIZE_FULL, 0, blocksY - 1);
        const uint8_t *rowA = srcA + y * (strideA * 4);
        const uint8_t *rowB = srcB + y * (strideB * 4);
        uint8_t *rowDst = dst + y * (strideDst * 4);

        for (int x = 0; x < width; ) {
            int bx = std::clamp(x / BLOCK_SIZE_FULL, 0, blocksX - 1);
            int blockIdx = by * blocksX + bx;

            int vx = flowX[blockIdx];
            int vy = flowY[blockIdx];

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (vx == 0 && vy == 0) {
                int endX = std::min(x + BLOCK_SIZE_FULL, width);
                for (; x <= endX - 4; x += 4) {
                    uint8x16_t a = vld1q_u8(rowA + x * 4);
                    uint8x16_t b = vld1q_u8(rowB + x * 4);
                    uint8x16_t avg = vrhaddq_u8(a, b);
                    static const uint8_t alphaMask[16] = {0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255};
                    avg = vorrq_u8(avg, vld1q_u8(alphaMask));
                    vst1q_u8(rowDst + x * 4, avg);
                }
                for (; x < endX; x++) {
                    const uint8_t *origA = rowA + x * 4;
                    const uint8_t *origB = rowB + x * 4;
                    uint8_t *pDst = rowDst + x * 4;
                    pDst[0] = static_cast<uint8_t>((origA[0] + origB[0]) >> 1);
                    pDst[1] = static_cast<uint8_t>((origA[1] + origB[1]) >> 1);
                    pDst[2] = static_cast<uint8_t>((origA[2] + origB[2]) >> 1);
                    pDst[3] = 255;
                }
                continue;
            }
#endif

            // 1. FG 2.0 Static HUD / Crosshair Protection:
            // If native pixel colors in Frame A & B are identical or near-identical,
            // but the motion vector is non-zero, this is a fixed 2D UI element (HUD, health bar, crosshair).
            // Lock displacement to 0 so HUD does not warp or smudge!
            const uint8_t *origA = rowA + x * 4;
            const uint8_t *origB = rowB + x * 4;
            int diffOrig = std::abs(static_cast<int>(origA[0]) - static_cast<int>(origB[0])) +
                           std::abs(static_cast<int>(origA[1]) - static_cast<int>(origB[1])) +
                           std::abs(static_cast<int>(origA[2]) - static_cast<int>(origB[2]));

            if (diffOrig < 18 && (std::abs(vx) + std::abs(vy)) > 6) {
                vx = 0;
                vy = 0;
            }

            int halfVx = vx / 2;
            int halfVy = vy / 2;

            int ax = std::clamp(x - halfVx, 0, width - 1);
            int ay = std::clamp(y - halfVy, 0, height - 1);
            int bx_coord = std::clamp(x + halfVx, 0, width - 1);
            int by_coord = std::clamp(y + halfVy, 0, height - 1);

            const uint8_t *pA = srcA + ay * (strideA * 4) + ax * 4;
            const uint8_t *pB = srcB + by_coord * (strideB * 4) + bx_coord * 4;
            uint8_t *pDst = rowDst + x * 4;

            // 2. FG 2.0 Occlusion-Aware Confidence Weighting:
            int diffWarped = std::abs(static_cast<int>(pA[0]) - static_cast<int>(pB[0])) +
                             std::abs(static_cast<int>(pA[1]) - static_cast<int>(pB[1])) +
                             std::abs(static_cast<int>(pA[2]) - static_cast<int>(pB[2]));

            if (diffWarped < 65) {
                // High confidence motion trajectory
                pDst[0] = static_cast<uint8_t>((pA[0] + pB[0]) >> 1);
                pDst[1] = static_cast<uint8_t>((pA[1] + pB[1]) >> 1);
                pDst[2] = static_cast<uint8_t>((pA[2] + pB[2]) >> 1);
                pDst[3] = 255;
            } else if (diffWarped < 120) {
                // Smooth transition zone (soft confidence weighting)
                int blendA = (pA[0] + pB[0]) >> 1;
                int blendB = (origA[0] + origB[0]) >> 1;
                int weight = 120 - diffWarped; // 0 to 55
                pDst[0] = static_cast<uint8_t>((blendA * weight + blendB * (55 - weight)) / 55);

                blendA = (pA[1] + pB[1]) >> 1;
                blendB = (origA[1] + origB[1]) >> 1;
                pDst[1] = static_cast<uint8_t>((blendA * weight + blendB * (55 - weight)) / 55);

                blendA = (pA[2] + pB[2]) >> 1;
                blendB = (origA[2] + origB[2]) >> 1;
                pDst[2] = static_cast<uint8_t>((blendA * weight + blendB * (55 - weight)) / 55);
                pDst[3] = 255;
            } else {
                // Disocclusion fallback: native blend
                pDst[0] = static_cast<uint8_t>((origA[0] + origB[0]) >> 1);
                pDst[1] = static_cast<uint8_t>((origA[1] + origB[1]) >> 1);
                pDst[2] = static_cast<uint8_t>((origA[2] + origB[2]) >> 1);
                pDst[3] = 255;
            }
            x++;
        }
    }
}

bool NPUFrameInterpolator::interpolate(AHardwareBuffer *prevBuffer,
                                       AHardwareBuffer *currBuffer,
                                       AHardwareBuffer *outputBuffer,
                                       int width, int height) {
    if (!prevBuffer || !currBuffer || !outputBuffer || width <= 0 || height <= 0) {
        return false;
    }

    if (!m_initialized) {
        if (!init()) return false;
    }

    ensureBuffers(width, height);

    void *ptrPrev = nullptr;
    void *ptrCurr = nullptr;
    void *ptrOut = nullptr;

    uint64_t usageRead = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    uint64_t usageWrite = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    int resA = AHardwareBuffer_lock(prevBuffer, usageRead, -1, nullptr, &ptrPrev);
    if (resA != 0) {
        usageRead = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
        resA = AHardwareBuffer_lock(prevBuffer, usageRead, -1, nullptr, &ptrPrev);
    }
    int resB = AHardwareBuffer_lock(currBuffer, usageRead, -1, nullptr, &ptrCurr);
    if (resB != 0) {
        resB = AHardwareBuffer_lock(currBuffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &ptrCurr);
    }
    int resOut = AHardwareBuffer_lock(outputBuffer, usageWrite, -1, nullptr, &ptrOut);
    if (resOut != 0) {
        resOut = AHardwareBuffer_lock(outputBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr, &ptrOut);
    }

        // If a compiled QNN HTP tensor graph binary is bound, dispatch onto Hexagon NPU
    if (m_graphHandle && m_pfnGraphExecute) {
        uint32_t execRes = m_pfnGraphExecute(m_graphHandle, nullptr, 0, nullptr, 0, nullptr, nullptr);
        if (execRes == QNN_SUCCESS) {
            AHardwareBuffer_unlock(prevBuffer, nullptr);
            AHardwareBuffer_unlock(currBuffer, nullptr);
            AHardwareBuffer_unlock(outputBuffer, nullptr);
            return true;
        }
        NPU_LOGW("QnnGraph_execute returned %u; falling back to parallel NEON SIMD", execRes);
    }

    if (resA != 0 || resB != 0 || resOut != 0 || !ptrPrev || !ptrCurr || !ptrOut) {
        if (resA == 0) AHardwareBuffer_unlock(prevBuffer, nullptr);
        if (resB == 0) AHardwareBuffer_unlock(currBuffer, nullptr);
        if (resOut == 0) AHardwareBuffer_unlock(outputBuffer, nullptr);
        return false;
    }

    AHardwareBuffer_Desc descA{}, descB{}, descOut{};
    AHardwareBuffer_describe(prevBuffer, &descA);
    AHardwareBuffer_describe(currBuffer, &descB);
    AHardwareBuffer_describe(outputBuffer, &descOut);

    int strideA = descA.stride > 0 ? descA.stride : width;
    int strideB = descB.stride > 0 ? descB.stride : width;
    int strideOut = descOut.stride > 0 ? descOut.stride : width;

    const uint8_t *bufA = static_cast<const uint8_t *>(ptrPrev);
    const uint8_t *bufB = static_cast<const uint8_t *>(ptrCurr);
    uint8_t *bufOut = static_cast<uint8_t *>(ptrOut);

    const int NUM_SLICES = 4;

    // Step 1: Parallel Luma Extraction
    struct LumaTaskCtx {
        NPUFrameInterpolator *self;
        const uint8_t *bufA;
        int strideA;
        const uint8_t *bufB;
        int strideB;
        int lumaSliceH;
    };
    LumaTaskCtx lumaCtx{this, bufA, strideA, bufB, strideB, m_dsH / NUM_SLICES};
    auto lumaTask = [](void *ctx, int s) {
        auto *c = static_cast<LumaTaskCtx *>(ctx);
        int yStart = s * c->lumaSliceH;
        int yEnd = (s == NUM_SLICES - 1) ? c->self->m_dsH : yStart + c->lumaSliceH;
        c->self->extractLumaSlice(c->bufA, c->strideA, c->self->m_lumA.data(), c->self->m_dsW, yStart, yEnd);
        c->self->extractLumaSlice(c->bufB, c->strideB, c->self->m_lumB.data(), c->self->m_dsW, yStart, yEnd);
    };
    for (int s = 0; s < NUM_SLICES; s++) {
        m_threadPool->enqueue(lumaTask, &lumaCtx, s);
    }
    m_threadPool->waitAll();

    // Step 2: Parallel Motion Estimation
    int blocksY = (m_dsH + 7) / 8;
    int motionSliceH = blocksY / NUM_SLICES;
    if (motionSliceH == 0) motionSliceH = 1;
    struct MotionTaskCtx {
        NPUFrameInterpolator *self;
        int blocksY;
        int motionSliceH;
    };
    MotionTaskCtx motionCtx{this, blocksY, motionSliceH};
    auto motionTask = [](void *ctx, int s) {
        auto *c = static_cast<MotionTaskCtx *>(ctx);
        int byStart = s * c->motionSliceH;
        if (byStart >= c->blocksY) return;
        int byEnd = (s == NUM_SLICES - 1) ? c->blocksY : std::min(byStart + c->motionSliceH, c->blocksY);
        c->self->computeBlockMotionSlice(c->self->m_lumA.data(), c->self->m_lumB.data(),
                                         c->self->m_flowX.data(), c->self->m_flowY.data(),
                                         c->self->m_dsW, c->self->m_dsH, byStart, byEnd);
    };
    for (int s = 0; s < NUM_SLICES; s++) {
        m_threadPool->enqueue(motionTask, &motionCtx, s);
    }
    m_threadPool->waitAll();

    // Step 3: Spatial Vector Filtering (Vector Field Smoothing)
    int blocksX = (m_dsW + 7) / 8;
    filterMotionField(blocksX, blocksY);

    // Step 4: Parallel Frame Synthesis with HUD Preservation
    int frameSliceH = height / NUM_SLICES;
    struct SynthTaskCtx {
        NPUFrameInterpolator *self;
        const uint8_t *bufA;
        int strideA;
        const uint8_t *bufB;
        int strideB;
        uint8_t *bufOut;
        int strideOut;
        int width;
        int height;
        int frameSliceH;
    };
    SynthTaskCtx synthCtx{this, bufA, strideA, bufB, strideB, bufOut, strideOut, width, height, frameSliceH};
    auto synthTask = [](void *ctx, int s) {
        auto *c = static_cast<SynthTaskCtx *>(ctx);
        int yStart = s * c->frameSliceH;
        int yEnd = (s == NUM_SLICES - 1) ? c->height : yStart + c->frameSliceH;
        c->self->synthesizeFrameSlice(c->bufA, c->strideA, c->bufB, c->strideB, c->bufOut, c->strideOut,
                                      c->self->m_filteredFlowX.data(), c->self->m_filteredFlowY.data(),
                                      c->width, c->height, c->self->m_dsW, c->self->m_dsH, yStart, yEnd);
    };
    for (int s = 0; s < NUM_SLICES; s++) {
        m_threadPool->enqueue(synthTask, &synthCtx, s);
    }
    m_threadPool->waitAll();

    // Flush and unlock so SurfaceFlinger / GPU display immediately
    AHardwareBuffer_unlock(prevBuffer, nullptr);
    AHardwareBuffer_unlock(currBuffer, nullptr);
    AHardwareBuffer_unlock(outputBuffer, nullptr);

    return true;
}
