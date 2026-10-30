// Copyright © 2026 Winlator-OctaFX Authors.
// SPDX-License-Identifier: MIT
#pragma once

#include "arm_asr_context.hpp"

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace octafx {
namespace asr {

struct ArmASRBenchmarkResult {
    bool success = false;
    uint32_t framesExecuted = 0;
    float avgGpuTimeMs = 0.0f;
    float minGpuTimeMs = 0.0f;
    float maxGpuTimeMs = 0.0f;
    float p50GpuTimeMs = 0.0f;
    float p95GpuTimeMs = 0.0f;
    size_t scratchMemoryBytes = 0;
    size_t targetMemoryBytes = 0;
    std::string deviceName;
    std::string summary;
};

class ArmASRTestPipeline {
public:
    ArmASRTestPipeline();
    ~ArmASRTestPipeline();

    // Non-copyable
    ArmASRTestPipeline(const ArmASRTestPipeline&) = delete;
    ArmASRTestPipeline& operator=(const ArmASRTestPipeline&) = delete;

    bool init(VkDevice device,
              VkPhysicalDevice physicalDevice,
              VkQueue queue,
              uint32_t queueFamily,
              VkExtent2D renderExtent,
              VkExtent2D displayExtent);

    void destroy();

    ArmASRBenchmarkResult runBenchmark(uint32_t numFrames = 30);

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {0, 0};
        VkDeviceSize allocatedBytes = 0;
    };

    bool createTarget(Target& target, VkFormat format, VkExtent2D extent,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroyTarget(Target& target);

    bool createScenePass();
    bool createScenePipeline();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProperties_ = {};
    float timestampPeriodNs_ = 1.0f;

    VkExtent2D renderExtent_ = {0, 0};
    VkExtent2D displayExtent_ = {0, 0};

    Target color_;
    Target motion_;
    Target depth_;
    Target output_;

    VkRenderPass scenePass_ = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer_ = VK_NULL_HANDLE;
    VkPipelineLayout sceneLayout_ = VK_NULL_HANDLE;
    VkPipeline scenePipeline_ = VK_NULL_HANDLE;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    bool timestampsSupported_ = false;

    ArmASRContext asrContext_;
    bool initialized_ = false;
};

} // namespace asr
} // namespace octafx
