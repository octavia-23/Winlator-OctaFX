// Copyright © 2026 Winlator-OctaFX Authors.
// SPDX-License-Identifier: MIT

#include <jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <string>
#include <sstream>
#include <vector>

#include "arm_asr_context.hpp"
#include "arm_asr_test_pipeline.hpp"
#include "../vulkan/VulkanRendererContext.h"

#define LOG_TAG "OctaFX_ArmASRJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct HeadlessVk {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool valid = false;

    bool init() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OctaFX_ArmASR_Probe";
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instCi{};
        instCi.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instCi.pApplicationInfo = &appInfo;

        if (vkCreateInstance(&instCi, nullptr, &instance) != VK_SUCCESS) {
            // Try fallback to Vulkan 1.1 if 1.2 instance fails
            appInfo.apiVersion = VK_API_VERSION_1_1;
            if (vkCreateInstance(&instCi, nullptr, &instance) != VK_SUCCESS) {
                LOGE("HeadlessVk: Failed to create Vulkan instance");
                return false;
            }
        }

        uint32_t devCount = 0;
        vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
        if (devCount == 0) {
            LOGE("HeadlessVk: No physical devices found");
            cleanup();
            return false;
        }

        std::vector<VkPhysicalDevice> devices(devCount);
        vkEnumeratePhysicalDevices(instance, &devCount, devices.data());
        physicalDevice = devices[0];

        // Find compute/graphics queue
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, qProps.data());

        bool foundQueue = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (qProps[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                queueFamily = i;
                foundQueue = true;
                break;
            }
        }
        if (!foundQueue) {
            LOGE("HeadlessVk: No queue family with graphics/compute support");
            cleanup();
            return false;
        }

        // Query available extensions
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());

        auto hasExt = [&](const char* name) {
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, name) == 0) return true;
            }
            return false;
        };

        std::vector<const char*> enabledExts;
        if (hasExt(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
            enabledExts.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        }
        if (hasExt(VK_KHR_16BIT_STORAGE_EXTENSION_NAME)) {
            enabledExts.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
        }

        VkPhysicalDevice16BitStorageFeatures storage16{};
        storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        storage16.storageBuffer16BitAccess = VK_TRUE;
        storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;

        VkPhysicalDeviceShaderFloat16Int8Features f16{};
        f16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        f16.shaderFloat16 = VK_TRUE;
        f16.pNext = &storage16;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &f16;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qCi{};
        qCi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qCi.queueFamilyIndex = queueFamily;
        qCi.queueCount = 1;
        qCi.pQueuePriorities = &priority;

        VkDeviceCreateInfo devCi{};
        devCi.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devCi.pNext = &features2;
        devCi.queueCreateInfoCount = 1;
        devCi.pQueueCreateInfos = &qCi;
        devCi.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
        devCi.ppEnabledExtensionNames = enabledExts.data();

        if (vkCreateDevice(physicalDevice, &devCi, nullptr, &device) != VK_SUCCESS) {
            LOGE("HeadlessVk: Failed to create logical device with float16 features");
            // Retry with standard features2
            devCi.pNext = nullptr;
            devCi.enabledExtensionCount = 0;
            devCi.ppEnabledExtensionNames = nullptr;
            if (vkCreateDevice(physicalDevice, &devCi, nullptr, &device) != VK_SUCCESS) {
                LOGE("HeadlessVk: Fallback device creation also failed");
                cleanup();
                return false;
            }
        }

        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        valid = true;
        return true;
    }

    void cleanup() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        valid = false;
    }

    ~HeadlessVk() {
        cleanup();
    }
};

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_widget_VulkanXServerView_nativeProbeArmASR(
    JNIEnv* env, jclass /* clazz */, jlong handle)
{
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    HeadlessVk headless;

    if (handle != 0) {
        auto* ctx = reinterpret_cast<VulkanRendererContext*>(handle);
        physDev = ctx->physicalDevice;
    } else {
        if (headless.init()) {
            physDev = headless.physicalDevice;
        }
    }

    if (physDev == VK_NULL_HANDLE) {
        return env->NewStringUTF("{\"supported\": false, \"error\": \"No Vulkan physical device available\"}");
    }

    auto caps = octafx::asr::ArmASRContext::probeDevice(physDev);

    std::ostringstream ss;
    ss << "{\n"
       << "  \"supported\": " << (caps.supported ? "true" : "false") << ",\n"
       << "  \"deviceName\": \"" << caps.deviceName << "\",\n"
       << "  \"driverVersion\": \"" << caps.driverVersion << "\",\n"
       << "  \"spirv14\": " << (caps.spirv14 ? "true" : "false") << ",\n"
       << "  \"shaderFloat16\": " << (caps.shaderFloat16 ? "true" : "false") << ",\n"
       << "  \"storage16bit\": " << (caps.storage16bit ? "true" : "false") << ",\n"
       << "  \"subgroupSizeCtrl\": " << (caps.subgroupSizeCtrl ? "true" : "false") << ",\n"
       << "  \"minSubgroupSize\": " << caps.minSubgroupSize << ",\n"
       << "  \"maxSubgroupSize\": " << caps.maxSubgroupSize << ",\n"
       << "  \"scratchMemoryBytes\": " << caps.scratchMemoryBytes << ",\n"
       << "  \"failureReason\": \"" << caps.failureReason << "\"\n"
       << "}";

    return env->NewStringUTF(ss.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_widget_VulkanXServerView_nativeRunArmASRBenchmark(
    JNIEnv* env, jclass /* clazz */, jlong handle,
    jint renderWidth, jint renderHeight,
    jint displayWidth, jint displayHeight,
    jint frames)
{
    if (renderWidth <= 0) renderWidth = 960;
    if (renderHeight <= 0) renderHeight = 540;
    if (displayWidth <= 0) displayWidth = 1920;
    if (displayHeight <= 0) displayHeight = 1080;
    if (frames <= 0) frames = 30;

    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    uint32_t qFam = 0;

    HeadlessVk headless;
    if (handle != 0) {
        auto* ctx = reinterpret_cast<VulkanRendererContext*>(handle);
        dev = ctx->device;
        physDev = ctx->physicalDevice;
        q = ctx->graphicsQueue;
        qFam = ctx->graphicsQueueFamilyIndex;
    } else {
        if (!headless.init()) {
            return env->NewStringUTF("{\"success\": false, \"summary\": \"Failed to initialize headless Vulkan context\"}");
        }
        dev = headless.device;
        physDev = headless.physicalDevice;
        q = headless.queue;
        qFam = headless.queueFamily;
    }

    octafx::asr::ArmASRTestPipeline pipeline;
    VkExtent2D rExt = {static_cast<uint32_t>(renderWidth), static_cast<uint32_t>(renderHeight)};
    VkExtent2D dExt = {static_cast<uint32_t>(displayWidth), static_cast<uint32_t>(displayHeight)};

    if (!pipeline.init(dev, physDev, q, qFam, rExt, dExt)) {
        return env->NewStringUTF("{\"success\": false, \"summary\": \"Failed to initialize ArmASRTestPipeline\"}");
    }

    auto result = pipeline.runBenchmark(static_cast<uint32_t>(frames));
    pipeline.destroy();

    std::ostringstream ss;
    ss << "{\n"
       << "  \"success\": " << (result.success ? "true" : "false") << ",\n"
       << "  \"deviceName\": \"" << result.deviceName << "\",\n"
       << "  \"framesExecuted\": " << result.framesExecuted << ",\n"
       << "  \"avgGpuTimeMs\": " << result.avgGpuTimeMs << ",\n"
       << "  \"p50GpuTimeMs\": " << result.p50GpuTimeMs << ",\n"
       << "  \"p95GpuTimeMs\": " << result.p95GpuTimeMs << ",\n"
       << "  \"minGpuTimeMs\": " << result.minGpuTimeMs << ",\n"
       << "  \"maxGpuTimeMs\": " << result.maxGpuTimeMs << ",\n"
       << "  \"scratchMemoryBytes\": " << result.scratchMemoryBytes << ",\n"
       << "  \"targetMemoryBytes\": " << result.targetMemoryBytes << ",\n"
       << "  \"summary\": \"" << result.summary << "\"\n"
       << "}";

    return env->NewStringUTF(ss.str().c_str());
}
