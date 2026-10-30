// Copyright © 2026 Winlator-OctaFX Authors.
// SPDX-License-Identifier: MIT

#include "arm_asr_test_pipeline.hpp"
#include "shaders/fullscreen_vert.spv.h"
#include "shaders/scene_frag.spv.h"

#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

#define LOG_TAG "OctaFX_ArmASRTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace octafx {
namespace asr {

namespace {

struct ScenePushConstants {
    float renderSize[2];
    float jitter[2];
    float camZ;
    float camZPrev;
    float tanHalfFov;
    float aspect;
    float nearZ;
    float farZ;
    float planeY;
    float time;
};

VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

ArmASRTestPipeline::ArmASRTestPipeline() = default;

ArmASRTestPipeline::~ArmASRTestPipeline() {
    destroy();
}

uint32_t ArmASRTestPipeline::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < memProperties_.memoryTypeCount; ++i) {
        if ((typeBits & (1 << i)) && (memProperties_.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool ArmASRTestPipeline::createTarget(Target& target, VkFormat format, VkExtent2D extent,
                                      VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    target.format = format;
    target.extent = extent;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ci, nullptr, &target.image) != VK_SUCCESS) {
        LOGE("Failed to create image for target (fmt=%d, %ux%u)", (int)format, extent.width, extent.height);
        return false;
    }

    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(device_, target.image, &req);
    target.allocatedBytes = req.size;

    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, 0);
    }
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        LOGE("Failed to find suitable memory type for image target");
        return false;
    }

    if (vkAllocateMemory(device_, &alloc, nullptr, &target.memory) != VK_SUCCESS) {
        LOGE("Failed to allocate %llu bytes for image target", static_cast<unsigned long long>(req.size));
        return false;
    }
    if (vkBindImageMemory(device_, target.image, target.memory, 0) != VK_SUCCESS) {
        LOGE("Failed to bind image memory");
        return false;
    }

    VkImageViewCreateInfo viewCi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image = target.image;
    viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format = format;
    viewCi.subresourceRange.aspectMask = aspect;
    viewCi.subresourceRange.baseMipLevel = 0;
    viewCi.subresourceRange.levelCount = 1;
    viewCi.subresourceRange.baseArrayLayer = 0;
    viewCi.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewCi, nullptr, &target.view) != VK_SUCCESS) {
        LOGE("Failed to create image view");
        return false;
    }
    return true;
}

void ArmASRTestPipeline::destroyTarget(Target& target) {
    if (device_ != VK_NULL_HANDLE) {
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, target.view, nullptr);
            target.view = VK_NULL_HANDLE;
        }
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
        }
        if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, target.memory, nullptr);
            target.memory = VK_NULL_HANDLE;
        }
    }
    target.allocatedBytes = 0;
}

bool ArmASRTestPipeline::createScenePass() {
    VkAttachmentDescription attachments[3] = {};
    // 0: Color
    attachments[0].format = color_.format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 1: Motion vectors
    attachments[1] = attachments[0];
    attachments[1].format = motion_.format;

    // 2: Depth
    attachments[2].format = depth_.format;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRefs[2] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentReference depthRef = {2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 3;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &ci, nullptr, &scenePass_) != VK_SUCCESS) {
        LOGE("Failed to create scene render pass");
        return false;
    }

    VkImageView views[3] = {color_.view, motion_.view, depth_.view};
    VkFramebufferCreateInfo fbCi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbCi.renderPass = scenePass_;
    fbCi.attachmentCount = 3;
    fbCi.pAttachments = views;
    fbCi.width = renderExtent_.width;
    fbCi.height = renderExtent_.height;
    fbCi.layers = 1;

    if (vkCreateFramebuffer(device_, &fbCi, nullptr, &sceneFramebuffer_) != VK_SUCCESS) {
        LOGE("Failed to create scene framebuffer");
        return false;
    }
    return true;
}

bool ArmASRTestPipeline::createScenePipeline() {
    VkShaderModule vs = createShaderModule(device_, g_fullscreen_vert, sizeof(g_fullscreen_vert));
    VkShaderModule fs = createShaderModule(device_, g_scene_frag, sizeof(g_scene_frag));
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        LOGE("Failed to create shader modules for scene pipeline");
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPushConstantRange pushRange = {
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants)
    };
    VkPipelineLayoutCreateInfo layoutCi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device_, &layoutCi, nullptr, &sceneLayout_) != VK_SUCCESS) {
        LOGE("Failed to create scene pipeline layout");
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
    for (auto& b : blendAttachments) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    blend.attachmentCount = 2;
    blend.pAttachments = blendAttachments;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.maxDepthBounds = 1.0f;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipeCi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeCi.stageCount = 2;
    pipeCi.pStages = stages;
    pipeCi.pVertexInputState = &vertexInput;
    pipeCi.pInputAssemblyState = &inputAssembly;
    pipeCi.pViewportState = &viewportState;
    pipeCi.pRasterizationState = &raster;
    pipeCi.pMultisampleState = &multisample;
    pipeCi.pDepthStencilState = &depthStencil;
    pipeCi.pColorBlendState = &blend;
    pipeCi.pDynamicState = &dynamic;
    pipeCi.layout = sceneLayout_;
    pipeCi.renderPass = scenePass_;

    VkResult res = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeCi, nullptr, &scenePipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);

    if (res != VK_SUCCESS) {
        LOGE("Failed to create graphics pipeline for scene pass");
        return false;
    }
    return true;
}

bool ArmASRTestPipeline::init(VkDevice device,
                              VkPhysicalDevice physicalDevice,
                              VkQueue queue,
                              uint32_t queueFamily,
                              VkExtent2D renderExtent,
                              VkExtent2D displayExtent) {
    destroy();

    device_ = device;
    physicalDevice_ = physicalDevice;
    queue_ = queue;
    queueFamily_ = queueFamily;
    renderExtent_ = renderExtent;
    displayExtent_ = displayExtent;

    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties_);

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    timestampPeriodNs_ = props.limits.timestampPeriod;

    // 1. Create G-Buffer targets
    const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!createTarget(color_, VK_FORMAT_R16G16B16A16_SFLOAT, renderExtent_, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!createTarget(motion_, VK_FORMAT_R16G16_SFLOAT, renderExtent_, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!createTarget(depth_, VK_FORMAT_D32_SFLOAT, renderExtent_,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT)) return false;

    // Output target needs STORAGE for Arm ASR compute writes
    const VkImageUsageFlags outUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!createTarget(output_, VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_, outUsage, VK_IMAGE_ASPECT_COLOR_BIT)) return false;

    // 2. Create RenderPass and Pipeline
    if (!createScenePass() || !createScenePipeline()) return false;

    // 3. Create Command Pool and Buffer
    VkCommandPoolCreateInfo poolCi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolCi, nullptr, &cmdPool_) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &cmdBuf_) != VK_SUCCESS) return false;

    VkFenceCreateInfo fenceCi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fenceCi, nullptr, &fence_) != VK_SUCCESS) return false;

    // 4. Try creating timestamp query pool
    VkQueryPoolCreateInfo qpCi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpCi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpCi.queryCount = 2;
    if (vkCreateQueryPool(device_, &qpCi, nullptr, &queryPool_) == VK_SUCCESS) {
        timestampsSupported_ = true;
    } else {
        timestampsSupported_ = false;
        queryPool_ = VK_NULL_HANDLE;
    }

    // 5. Initialize Arm ASR context
    ArmASRConfig asrCfg = {};
    asrCfg.device = device_;
    asrCfg.physicalDevice = physicalDevice_;
    asrCfg.renderWidth = renderExtent_.width;
    asrCfg.renderHeight = renderExtent_.height;
    asrCfg.displayWidth = displayExtent_.width;
    asrCfg.displayHeight = displayExtent_.height;
    asrCfg.sharpness = 0.5f;
    asrCfg.enableAutoExposure = true;
    asrCfg.qualityMode = arm::FFXM_FSR2_SHADER_QUALITY_MODE_QUALITY;

    if (!asrContext_.init(asrCfg)) {
        LOGE("ArmASRTestPipeline: Failed to initialize Arm ASR Context");
        destroy();
        return false;
    }

    initialized_ = true;
    LOGI("ArmASRTestPipeline initialized: %ux%u -> %ux%u, timestamps=%d",
         renderExtent_.width, renderExtent_.height, displayExtent_.width, displayExtent_.height,
         timestampsSupported_ ? 1 : 0);
    return true;
}

void ArmASRTestPipeline::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);

        asrContext_.destroy();

        if (queryPool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, queryPool_, nullptr);
            queryPool_ = VK_NULL_HANDLE;
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
            fence_ = VK_NULL_HANDLE;
        }
        if (cmdPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, cmdPool_, nullptr);
            cmdPool_ = VK_NULL_HANDLE;
            cmdBuf_ = VK_NULL_HANDLE;
        }
        if (scenePipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, scenePipeline_, nullptr);
            scenePipeline_ = VK_NULL_HANDLE;
        }
        if (sceneLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, sceneLayout_, nullptr);
            sceneLayout_ = VK_NULL_HANDLE;
        }
        if (sceneFramebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_, sceneFramebuffer_, nullptr);
            sceneFramebuffer_ = VK_NULL_HANDLE;
        }
        if (scenePass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, scenePass_, nullptr);
            scenePass_ = VK_NULL_HANDLE;
        }

        destroyTarget(color_);
        destroyTarget(motion_);
        destroyTarget(depth_);
        destroyTarget(output_);
    }
    initialized_ = false;
}

ArmASRBenchmarkResult ArmASRTestPipeline::runBenchmark(uint32_t numFrames) {
    ArmASRBenchmarkResult res = {};
    if (!initialized_) {
        res.summary = "Pipeline not initialized";
        return res;
    }

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    res.deviceName = props.deviceName;

    res.scratchMemoryBytes = asrContext_.getMetrics().scratchSizeBytes;
    res.targetMemoryBytes = color_.allocatedBytes + motion_.allocatedBytes +
                            depth_.allocatedBytes + output_.allocatedBytes;

    std::vector<float> gpuTimes;
    gpuTimes.reserve(numFrames);

    float camZ = 0.0f;
    const float speed = 0.05f;

    LOGI("Starting Arm ASR benchmark for %u frames...", numFrames);

    for (uint32_t frame = 0; frame < numFrames; ++frame) {
        float camZPrev = camZ;
        camZ += speed;

        float jitterX = 0.0f, jitterY = 0.0f;
        asrContext_.getJitterOffset(static_cast<int32_t>(frame), &jitterX, &jitterY);

        vkResetCommandBuffer(cmdBuf_, 0);
        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmdBuf_, &beginInfo);

        if (timestampsSupported_) {
            vkCmdResetQueryPool(cmdBuf_, queryPool_, 0, 2);
        }

        // 1. Record G-Buffer generation pass
        VkClearValue clears[3] = {};
        clears[2].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpBegin.renderPass = scenePass_;
        rpBegin.framebuffer = sceneFramebuffer_;
        rpBegin.renderArea = {{0, 0}, renderExtent_};
        rpBegin.clearValueCount = 3;
        rpBegin.pClearValues = clears;
        vkCmdBeginRenderPass(cmdBuf_, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {0.0f, 0.0f, (float)renderExtent_.width, (float)renderExtent_.height, 0.0f, 1.0f};
        VkRect2D scissor = {{0, 0}, renderExtent_};
        vkCmdSetViewport(cmdBuf_, 0, 1, &viewport);
        vkCmdSetScissor(cmdBuf_, 0, 1, &scissor);

        vkCmdBindPipeline(cmdBuf_, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);

        ScenePushConstants pc = {};
        pc.renderSize[0] = static_cast<float>(renderExtent_.width);
        pc.renderSize[1] = static_cast<float>(renderExtent_.height);
        pc.jitter[0] = jitterX;
        pc.jitter[1] = jitterY;
        pc.camZ = camZ;
        pc.camZPrev = camZPrev;
        pc.tanHalfFov = std::tan(0.5f * 1.0471975512f);
        pc.aspect = static_cast<float>(renderExtent_.width) / static_cast<float>(renderExtent_.height);
        pc.nearZ = 0.1f;
        pc.farZ = 200.0f;
        pc.planeY = -1.0f;
        pc.time = static_cast<float>(frame) * 0.0166f;

        vkCmdPushConstants(cmdBuf_, sceneLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ScenePushConstants), &pc);
        vkCmdDraw(cmdBuf_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmdBuf_);

        // 2. Transition output image to GENERAL for compute writes
        imageBarrier(cmdBuf_, output_.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // 3. Dispatch Arm ASR
        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmdBuf_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, 0);
        }

        ArmASRDispatchParams dispatchParams = {};
        dispatchParams.cmd = cmdBuf_;
        dispatchParams.colorImage = color_.image;
        dispatchParams.colorFormat = color_.format;
        dispatchParams.colorExtent = color_.extent;

        dispatchParams.depthImage = depth_.image;
        dispatchParams.depthFormat = depth_.format;
        dispatchParams.depthExtent = depth_.extent;

        dispatchParams.motionImage = motion_.image;
        dispatchParams.motionFormat = motion_.format;
        dispatchParams.motionExtent = motion_.extent;

        dispatchParams.outputImage = output_.image;
        dispatchParams.outputFormat = output_.format;
        dispatchParams.outputExtent = output_.extent;

        dispatchParams.frameTimeDeltaMs = 16.6667f;
        dispatchParams.frameIndex = static_cast<int32_t>(frame);
        dispatchParams.reset = (frame == 0);
        dispatchParams.sharpness = 0.5f;
        dispatchParams.jitterX = jitterX;
        dispatchParams.jitterY = jitterY;
        dispatchParams.cameraNear = pc.nearZ;
        dispatchParams.cameraFar = pc.farZ;
        dispatchParams.cameraFovVerticalRad = 1.0471975512f;

        if (!asrContext_.dispatch(dispatchParams)) {
            LOGE("Benchmark dispatch failed on frame %u", frame);
            res.summary = "Dispatch failed on frame " + std::to_string(frame);
            return res;
        }

        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmdBuf_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, 1);
        }

        vkEndCommandBuffer(cmdBuf_);

        // 4. Submit and wait
        vkResetFences(device_, 1, &fence_);
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmdBuf_;

        auto cpuStart = std::chrono::high_resolution_clock::now();
        vkQueueSubmit(queue_, 1, &submit, fence_);
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        auto cpuEnd = std::chrono::high_resolution_clock::now();

        float frameElapsedMs = 0.0f;
        if (timestampsSupported_) {
            uint64_t timestamps[2] = {0, 0};
            VkResult qRes = vkGetQueryPoolResults(device_, queryPool_, 0, 2, sizeof(timestamps),
                                                  timestamps, sizeof(uint64_t),
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (qRes == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
                uint64_t delta = timestamps[1] - timestamps[0];
                frameElapsedMs = static_cast<float>(delta) * timestampPeriodNs_ / 1000000.0f;
            }
        }
        if (frameElapsedMs <= 0.0f) {
            frameElapsedMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
        }

        // Skip frame 0 warmup from statistical metrics
        if (frame > 0) {
            gpuTimes.push_back(frameElapsedMs);
        }
    }

    if (!gpuTimes.empty()) {
        std::sort(gpuTimes.begin(), gpuTimes.end());
        float sum = 0.0f;
        for (float t : gpuTimes) sum += t;

        res.avgGpuTimeMs = sum / static_cast<float>(gpuTimes.size());
        res.minGpuTimeMs = gpuTimes.front();
        res.maxGpuTimeMs = gpuTimes.back();

        size_t p50Idx = gpuTimes.size() / 2;
        size_t p95Idx = static_cast<size_t>(static_cast<float>(gpuTimes.size()) * 0.95f);
        if (p95Idx >= gpuTimes.size()) p95Idx = gpuTimes.size() - 1;

        res.p50GpuTimeMs = gpuTimes[p50Idx];
        res.p95GpuTimeMs = gpuTimes[p95Idx];
    }

    res.framesExecuted = numFrames;
    res.success = true;

    std::ostringstream ss;
    ss << "Arm ASR Benchmark (" << renderExtent_.width << "x" << renderExtent_.height
       << " -> " << displayExtent_.width << "x" << displayExtent_.height << "): "
       << numFrames << " frames on " << res.deviceName << "\n"
       << "GPU Time: avg=" << std::fixed << std::setprecision(2) << res.avgGpuTimeMs << " ms, "
       << "p50=" << res.p50GpuTimeMs << " ms, "
       << "p95=" << res.p95GpuTimeMs << " ms, "
       << "min=" << res.minGpuTimeMs << " ms, "
       << "max=" << res.maxGpuTimeMs << " ms\n"
       << "Memory: Scratch=" << (res.scratchMemoryBytes / 1024) << " KiB, "
       << "Targets=" << (res.targetMemoryBytes / 1024) << " KiB";
    res.summary = ss.str();

    LOGI("%s", res.summary.c_str());
    return res;
}

} // namespace asr
} // namespace octafx
