// Copyright © 2026 Winlator-OctaFX Authors.
// SPDX-License-Identifier: MIT
#pragma once

#include <vulkan/vulkan.h>
#include <host/backends/vk/ffxm_vk.h>
#include <host/ffxm_fsr2.h>

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>

namespace octafx {
namespace asr {

struct ArmASRCapabilities {
    bool supported = false;
    bool spirv14 = false;
    bool shaderFloat16 = false;
    bool storage16bit = false;
    bool subgroupSizeCtrl = false;
    uint32_t minSubgroupSize = 0;
    uint32_t maxSubgroupSize = 0;
    size_t scratchMemoryBytes = 0;
    std::string deviceName;
    std::string driverVersion;
    std::string failureReason;
};

struct ArmASRConfig {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    float sharpness = 0.5f;
    bool enableAutoExposure = true;
    bool hdr = false;
    arm::FfxmFsr2ShaderQualityMode qualityMode = arm::FFXM_FSR2_SHADER_QUALITY_MODE_QUALITY;
};

struct ArmASRDispatchParams {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage colorImage = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkExtent2D colorExtent = {0, 0};

    VkImage depthImage = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkExtent2D depthExtent = {0, 0};

    VkImage motionImage = VK_NULL_HANDLE;
    VkFormat motionFormat = VK_FORMAT_R16G16_SFLOAT;
    VkExtent2D motionExtent = {0, 0};

    VkImage outputImage = VK_NULL_HANDLE;
    VkFormat outputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkExtent2D outputExtent = {0, 0};

    float frameTimeDeltaMs = 16.6667f;
    int32_t frameIndex = 0;
    bool reset = false;

    float cameraNear = 0.1f;
    float cameraFar = 200.0f;
    float cameraFovVerticalRad = 1.0471975512f; // 60 degrees

    float sharpness = 0.5f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
};

struct ArmASRMetrics {
    size_t scratchSizeBytes = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    int32_t jitterPhaseCount = 0;
};

class ArmASRContext {
public:
    ArmASRContext();
    ~ArmASRContext();

    // Non-copyable, movable
    ArmASRContext(const ArmASRContext&) = delete;
    ArmASRContext& operator=(const ArmASRContext&) = delete;
    ArmASRContext(ArmASRContext&&) noexcept;
    ArmASRContext& operator=(ArmASRContext&&) noexcept;

    static ArmASRCapabilities probeDevice(VkPhysicalDevice physicalDevice);

    bool init(const ArmASRConfig& config);
    void destroy();

    bool isInitialized() const { return contextCreated_; }

    int32_t getJitterPhaseCount() const { return jitterPhaseCount_; }
    void getJitterOffset(int32_t frameIndex, float* outX, float* outY) const;

    bool dispatch(const ArmASRDispatchParams& params);

    ArmASRMetrics getMetrics() const;

private:
    static arm::FfxmSurfaceFormat mapVulkanFormat(VkFormat format);

    VkDevice device_ = VK_NULL_HANDLE;
    arm::FfxmInterface interface_ = {};
    arm::FfxmFsr2Context context_ = {};
    std::vector<uint8_t> scratchBuffer_;
    bool contextCreated_ = false;
    int32_t jitterPhaseCount_ = 1;
    VkExtent2D renderExtent_ = {0, 0};
    VkExtent2D displayExtent_ = {0, 0};
    float defaultSharpness_ = 0.5f;
};

} // namespace asr
} // namespace octafx
