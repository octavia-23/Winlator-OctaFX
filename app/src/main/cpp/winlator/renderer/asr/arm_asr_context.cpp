// Copyright © 2026 Winlator-OctaFX Authors.
// SPDX-License-Identifier: MIT

#include "arm_asr_context.hpp"

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

#define LOG_TAG "OctaFX_ArmASR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace octafx {
namespace asr {

namespace {

void messageCallback(arm::FfxmMsgType type, const wchar_t* message) {
    char buffer[512];
    size_t i = 0;
    for (; message && message[i] != L'\0' && i < sizeof(buffer) - 1; ++i) {
        buffer[i] = (message[i] > 0 && message[i] < 128) ? static_cast<char>(message[i]) : '?';
    }
    buffer[i] = '\0';

    if (type == arm::FFXM_MESSAGE_TYPE_ERROR) {
        LOGE("Arm ASR Core Error: %s", buffer);
    } else {
        LOGW("Arm ASR Core Warning: %s", buffer);
    }
}

arm::FfxmResourceDescription describeTexture(uint32_t width, uint32_t height,
                                             arm::FfxmSurfaceFormat format,
                                             arm::FfxmResourceUsage usage) {
    arm::FfxmResourceDescription desc = {};
    desc.type     = arm::FFXM_RESOURCE_TYPE_TEXTURE2D;
    desc.format   = format;
    desc.width    = width;
    desc.height   = height;
    desc.depth    = 1;
    desc.mipCount = 1;
    desc.flags    = arm::FFXM_RESOURCE_FLAGS_NONE;
    desc.usage    = usage;
    return desc;
}

} // namespace

ArmASRContext::ArmASRContext() = default;

ArmASRContext::~ArmASRContext() {
    destroy();
}

ArmASRContext::ArmASRContext(ArmASRContext&& other) noexcept
    : device_(other.device_),
      interface_(other.interface_),
      context_(other.context_),
      scratchBuffer_(std::move(other.scratchBuffer_)),
      contextCreated_(other.contextCreated_),
      jitterPhaseCount_(other.jitterPhaseCount_),
      renderExtent_(other.renderExtent_),
      displayExtent_(other.displayExtent_),
      defaultSharpness_(other.defaultSharpness_) {
    other.device_ = VK_NULL_HANDLE;
    other.interface_ = {};
    other.context_ = {};
    other.contextCreated_ = false;
}

ArmASRContext& ArmASRContext::operator=(ArmASRContext&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        interface_ = other.interface_;
        context_ = other.context_;
        scratchBuffer_ = std::move(other.scratchBuffer_);
        contextCreated_ = other.contextCreated_;
        jitterPhaseCount_ = other.jitterPhaseCount_;
        renderExtent_ = other.renderExtent_;
        displayExtent_ = other.displayExtent_;
        defaultSharpness_ = other.defaultSharpness_;

        other.device_ = VK_NULL_HANDLE;
        other.interface_ = {};
        other.context_ = {};
        other.contextCreated_ = false;
    }
    return *this;
}

arm::FfxmSurfaceFormat ArmASRContext::mapVulkanFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return arm::FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
            return arm::FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return arm::FFXM_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R16G16_SFLOAT:
            return arm::FFXM_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_R32_SFLOAT:
            return arm::FFXM_SURFACE_FORMAT_R32_FLOAT;
        default:
            LOGW("Unrecognized VkFormat %d, defaulting to UNKNOWN", (int)format);
            return arm::FFXM_SURFACE_FORMAT_UNKNOWN;
    }
}

ArmASRCapabilities ArmASRContext::probeDevice(VkPhysicalDevice physicalDevice) {
    ArmASRCapabilities caps = {};
    if (physicalDevice == VK_NULL_HANDLE) {
        caps.failureReason = "Physical device is VK_NULL_HANDLE";
        return caps;
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    caps.deviceName = props.deviceName;

    char drvVer[64];
    snprintf(drvVer, sizeof(drvVer), "%u.%u.%u",
             VK_API_VERSION_MAJOR(props.driverVersion),
             VK_API_VERSION_MINOR(props.driverVersion),
             VK_API_VERSION_PATCH(props.driverVersion));
    caps.driverVersion = drvVer;

    uint32_t extCount = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) != VK_SUCCESS) {
        caps.failureReason = "Failed to enumerate device extensions";
        return caps;
    }

    std::vector<VkExtensionProperties> exts(extCount);
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data()) != VK_SUCCESS) {
        caps.failureReason = "Failed to read device extensions";
        return caps;
    }

    auto hasExt = [&](const char* name) {
        for (const auto& e : exts) {
            if (strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    caps.spirv14 = (props.apiVersion >= VK_API_VERSION_1_2) || hasExt(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    caps.storage16bit = (props.apiVersion >= VK_API_VERSION_1_2) || hasExt(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    caps.subgroupSizeCtrl = hasExt(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    // Query 16-bit float capability
    if (props.apiVersion >= VK_API_VERSION_1_2 || hasExt(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
        VkPhysicalDeviceShaderFloat16Int8Features f16 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES
        };
        VkPhysicalDeviceFeatures2 features2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f16
        };
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        caps.shaderFloat16 = (f16.shaderFloat16 == VK_TRUE);
    }

    if (caps.subgroupSizeCtrl) {
        VkPhysicalDeviceSubgroupSizeControlProperties subgroupProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES
        };
        VkPhysicalDeviceProperties2 props2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroupProps
        };
        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
        caps.minSubgroupSize = subgroupProps.minSubgroupSize;
        caps.maxSubgroupSize = subgroupProps.maxSubgroupSize;
    }

    // Query scratch memory size required by Arm ASR backend
    caps.scratchMemoryBytes = arm::ffxmGetScratchMemorySizeVK(physicalDevice, 1);

    // Arm ASR hardware requirement: SPIR-V 1.4, shaderFloat16, and 16-bit storage
    if (!caps.spirv14) {
        caps.failureReason = "Missing SPIR-V 1.4 support";
    } else if (!caps.shaderFloat16) {
        caps.failureReason = "Missing shaderFloat16 support";
    } else if (!caps.storage16bit) {
        caps.failureReason = "Missing 16-bit storage support";
    } else {
        caps.supported = true;
    }

    LOGI("Arm ASR Probe [%s]: Supported=%d, spirv14=%d, fp16=%d, storage16=%d, subgroupCtrl=%d [%u-%u], scratch=%zu B",
         caps.deviceName.c_str(), caps.supported ? 1 : 0, caps.spirv14 ? 1 : 0,
         caps.shaderFloat16 ? 1 : 0, caps.storage16bit ? 1 : 0,
         caps.subgroupSizeCtrl ? 1 : 0, caps.minSubgroupSize, caps.maxSubgroupSize,
         caps.scratchMemoryBytes);

    return caps;
}

bool ArmASRContext::init(const ArmASRConfig& config) {
    destroy();

    if (config.device == VK_NULL_HANDLE || config.physicalDevice == VK_NULL_HANDLE) {
        LOGE("ArmASRContext::init: Invalid Vulkan device/physicalDevice handles");
        return false;
    }

    if (config.renderWidth == 0 || config.renderHeight == 0 ||
        config.displayWidth == 0 || config.displayHeight == 0) {
        LOGE("ArmASRContext::init: Invalid extents: %ux%u -> %ux%u",
             config.renderWidth, config.renderHeight, config.displayWidth, config.displayHeight);
        return false;
    }

    device_ = config.device;
    renderExtent_ = {config.renderWidth, config.renderHeight};
    displayExtent_ = {config.displayWidth, config.displayHeight};
    defaultSharpness_ = std::clamp(config.sharpness, 0.0f, 1.0f);

    // 1. Calculate scratch buffer size and allocate
    const size_t maxContexts = 1;
    const size_t scratchSize = arm::ffxmGetScratchMemorySizeVK(config.physicalDevice, maxContexts);
    if (scratchSize == 0) {
        LOGE("ArmASRContext::init: ffxmGetScratchMemorySizeVK returned 0");
        return false;
    }

    try {
        scratchBuffer_.resize(scratchSize);
    } catch (const std::bad_alloc&) {
        LOGE("ArmASRContext::init: Failed to allocate %zu bytes for scratch buffer", scratchSize);
        return false;
    }
    LOGI("ArmASRContext: Scratch memory allocated: %zu bytes", scratchSize);

    // 2. Setup backend device and interface
    arm::VkDeviceContext deviceContext = {};
    deviceContext.vkDevice = config.device;
    deviceContext.vkPhysicalDevice = config.physicalDevice;
    deviceContext.vkDeviceProcAddr = config.getDeviceProcAddr ? config.getDeviceProcAddr : vkGetDeviceProcAddr;

    arm::FfxmDevice ffxmDevice = arm::ffxmGetDeviceVK(&deviceContext);

    arm::FfxmErrorCode err = arm::ffxmGetInterfaceVK(&interface_, ffxmDevice,
                                                     scratchBuffer_.data(),
                                                     scratchBuffer_.size(),
                                                     maxContexts);
    if (err != arm::FFXM_OK) {
        LOGE("ArmASRContext::init: ffxmGetInterfaceVK failed: 0x%08X", (unsigned)err);
        destroy();
        return false;
    }

    // 3. Create FSR2 context
    uint32_t flags = 0;
    if (config.enableAutoExposure) flags |= arm::FFXM_FSR2_ENABLE_AUTO_EXPOSURE;
    if (config.hdr) flags |= arm::FFXM_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    flags |= arm::FFXM_FSR2_ENABLE_DEBUG_CHECKING;

    arm::FfxmFsr2ContextDescription contextDesc = {};
    contextDesc.qualityMode = config.qualityMode;
    contextDesc.flags = flags;
    contextDesc.maxRenderSize = {config.renderWidth, config.renderHeight};
    contextDesc.displaySize = {config.displayWidth, config.displayHeight};
    contextDesc.backendInterface = interface_;
    contextDesc.fpMessage = messageCallback;

    err = arm::ffxmFsr2ContextCreate(&context_, &contextDesc);
    if (err != arm::FFXM_OK) {
        LOGE("ArmASRContext::init: ffxmFsr2ContextCreate failed: 0x%08X", (unsigned)err);
        destroy();
        return false;
    }
    contextCreated_ = true;

    jitterPhaseCount_ = arm::ffxmFsr2GetJitterPhaseCount(
        static_cast<int32_t>(config.renderWidth),
        static_cast<int32_t>(config.displayWidth)
    );

    LOGI("ArmASRContext::init SUCCESS: %ux%u -> %ux%u, Jitter phases: %d",
         config.renderWidth, config.renderHeight, config.displayWidth, config.displayHeight,
         jitterPhaseCount_);
    return true;
}

void ArmASRContext::destroy() {
    if (contextCreated_) {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        arm::ffxmFsr2ContextDestroy(&context_);
        contextCreated_ = false;
        context_ = {};
        interface_ = {};
    }
    scratchBuffer_.clear();
    scratchBuffer_.shrink_to_fit();
    device_ = VK_NULL_HANDLE;
    renderExtent_ = {0, 0};
    displayExtent_ = {0, 0};
    jitterPhaseCount_ = 1;
}

void ArmASRContext::getJitterOffset(int32_t frameIndex, float* outX, float* outY) const {
    if (!outX || !outY) return;
    *outX = 0.0f;
    *outY = 0.0f;
    if (jitterPhaseCount_ > 0) {
        arm::ffxmFsr2GetJitterOffset(outX, outY, frameIndex % jitterPhaseCount_, jitterPhaseCount_);
    }
}

bool ArmASRContext::dispatch(const ArmASRDispatchParams& params) {
    if (!contextCreated_) {
        LOGE("ArmASRContext::dispatch called on uninitialized context");
        return false;
    }
    if (params.cmd == VK_NULL_HANDLE) {
        LOGE("ArmASRContext::dispatch called with VK_NULL_HANDLE command buffer");
        return false;
    }

    static wchar_t kColorName[]  = L"ArmASR_Color";
    static wchar_t kDepthName[]  = L"ArmASR_Depth";
    static wchar_t kMotionName[] = L"ArmASR_MotionVectors";
    static wchar_t kOutputName[] = L"ArmASR_Output";

    arm::FfxmSurfaceFormat colorFmt  = mapVulkanFormat(params.colorFormat);
    arm::FfxmSurfaceFormat depthFmt  = mapVulkanFormat(params.depthFormat);
    arm::FfxmSurfaceFormat motionFmt = mapVulkanFormat(params.motionFormat);
    arm::FfxmSurfaceFormat outputFmt = mapVulkanFormat(params.outputFormat);

    arm::FfxmFsr2DispatchDescription desc = {};
    desc.commandList = arm::ffxmGetCommandListVK(params.cmd);

    desc.color = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(params.colorImage),
        describeTexture(params.colorExtent.width, params.colorExtent.height,
                        colorFmt, arm::FFXM_RESOURCE_USAGE_RENDERTARGET),
        kColorName,
        arm::FFXM_RESOURCE_STATE_COMPUTE_READ
    );

    desc.depth = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(params.depthImage),
        describeTexture(params.depthExtent.width, params.depthExtent.height,
                        depthFmt, arm::FFXM_RESOURCE_USAGE_DEPTHTARGET),
        kDepthName,
        arm::FFXM_RESOURCE_STATE_COMPUTE_READ
    );

    desc.motionVectors = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(params.motionImage),
        describeTexture(params.motionExtent.width, params.motionExtent.height,
                        motionFmt, arm::FFXM_RESOURCE_USAGE_RENDERTARGET),
        kMotionName,
        arm::FFXM_RESOURCE_STATE_COMPUTE_READ
    );

    desc.output = arm::ffxmGetResourceVK(
        reinterpret_cast<void*>(params.outputImage),
        describeTexture(params.outputExtent.width, params.outputExtent.height,
                        outputFmt, arm::FFXM_RESOURCE_USAGE_UAV),
        kOutputName,
        arm::FFXM_RESOURCE_STATE_UNORDERED_ACCESS
    );

    desc.jitterOffset = {params.jitterX, params.jitterY};
    desc.motionVectorScale = {params.motionVectorScaleX, params.motionVectorScaleY};
    desc.renderSize = {params.colorExtent.width, params.colorExtent.height};
    desc.enableSharpening = (params.sharpness > 0.0f);
    desc.sharpness = std::clamp(params.sharpness, 0.0f, 1.0f);
    desc.frameTimeDelta = params.frameTimeDeltaMs > 0.0f ? params.frameTimeDeltaMs : 16.6667f;
    desc.preExposure = 1.0f;
    desc.reset = params.reset;
    desc.cameraNear = params.cameraNear;
    desc.cameraFar = params.cameraFar;
    desc.cameraFovAngleVertical = params.cameraFovVerticalRad;
    desc.viewSpaceToMetersFactor = 1.0f;

    arm::FfxmErrorCode err = arm::ffxmFsr2ContextDispatch(&context_, &desc);
    if (err != arm::FFXM_OK) {
        LOGE("ArmASRContext::dispatch failed with code: 0x%08X", (unsigned)err);
        return false;
    }
    return true;
}

ArmASRMetrics ArmASRContext::getMetrics() const {
    ArmASRMetrics m = {};
    m.scratchSizeBytes = scratchBuffer_.size();
    m.renderWidth = renderExtent_.width;
    m.renderHeight = renderExtent_.height;
    m.displayWidth = displayExtent_.width;
    m.displayHeight = displayExtent_.height;
    m.jitterPhaseCount = jitterPhaseCount_;
    return m;
}

} // namespace asr
} // namespace octafx
