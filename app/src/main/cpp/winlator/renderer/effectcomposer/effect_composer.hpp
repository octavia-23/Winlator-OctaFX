#pragma once

#include <vector>
#include <unordered_map>
#include <atomic>
#include <unordered_set>
#include <memory>

#include "drawable.hpp"
#include "renderer_jni.hpp"
#include "vulkan.hpp"
#include "npu_frame_interpolator.hpp"

class EffectComposer {
    private:
        struct PushConstants {
            int width;
            int height;
        };
        
        struct DescriptorPool {
            VkDescriptorPool handle;
            uint32_t maxSize;
            std::unordered_set<VkImage> bindedImages;
            
            bool isFull() {
                return bindedImages.size() >= maxSize;
            }
            
            void addBindedImage(VkImage image) {
                bindedImages.insert(image);
            }
            
            void removeBindedImage(VkImage image) {
                bindedImages.erase(image);
            }
        };
        
        class DescriptorPoolBuffer {
            private: 
                std::vector<std::shared_ptr<DescriptorPool>> pools;
                std::unordered_map<VkDescriptorSet, DescriptorPool *> bindedSets;
            
            public:
                void addNew(VkDevice device) {
                    auto descriptorPool = std::make_shared<DescriptorPool>();
                    descriptorPool->maxSize = 1024;
                
                    VkDescriptorPoolSize descSizes[2];
                    descSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    descSizes[0].descriptorCount = descriptorPool->maxSize;
                    descSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    descSizes[1].descriptorCount = descriptorPool->maxSize;
                
                    VkDescriptorPoolCreateInfo createInfo{};
                    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                    createInfo.pNext = nullptr;
                    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                    createInfo.maxSets = descriptorPool->maxSize;
                    createInfo.poolSizeCount = 2;
                    createInfo.pPoolSizes = descSizes;
                
                    vkCreateDescriptorPool(device, &createInfo, nullptr, &descriptorPool->handle);
                
                    pools.push_back(descriptorPool);
                }
            
                DescriptorPool *findFreePool(VkDevice device) {
                    for (const auto& pool : pools) {
                        if (!pool->isFull()) 
                            return pool.get();
                    }
               
                    addNew(device);
                    return pools.back().get();
                }
                
                DescriptorPool *getPoolForSet(VkDescriptorSet descriptorSet) {
                    auto it = bindedSets.find(descriptorSet);
                    if (it == bindedSets.end())
                        return nullptr;
                        
                    return it->second;
                }
                
                void addDescriptorBinding(VkDescriptorSet descriptorSet, DescriptorPool *pool) {
                    bindedSets[descriptorSet] = pool;
                }
                
                void removeDescriptorBinding(VkDescriptorSet descriptorSet) {
                    bindedSets.erase(descriptorSet);
                }
            
                void removeCurrent(VkDevice device) {
                    auto descriptorPool = pools.back();
                    pools.pop_back();
                
                    vkDestroyDescriptorPool(device, descriptorPool->handle, nullptr);
                }
        };

        struct VulkanAHBImage {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView imageView = VK_NULL_HANDLE;
            VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlagBits currentStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlagBits currentAccess = static_cast<VkAccessFlagBits>(0);
            uint32_t width = 0;
            uint32_t height = 0;
        };

        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        DescriptorPoolBuffer poolsBuffer;
        VkPipeline colorSwapPipeline = VK_NULL_HANDLE;

        // Dedicated Vulkan Compute Frame Gen (Adreno 735 ALUs)
        VkDescriptorSetLayout frameGenDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout frameGenPipelineLayout = VK_NULL_HANDLE;
        VkPipeline frameGenPipeline = VK_NULL_HANDLE;
        VkSampler frameGenSampler = VK_NULL_HANDLE;
        VkDescriptorPool frameGenDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet frameGenDescriptorSet = VK_NULL_HANDLE;

        std::unordered_map<AHardwareBuffer*, VulkanAHBImage> ahbImages;
        VulkanAHBImage *getOrCreateAHBImage(AHardwareBuffer *ahb, int width, int height, VkImageUsageFlags usage);
        void destroyAHBImage(VulkanAHBImage &entry);
        void cleanupAHBImages();

        VulkanTable dispatchTable;
        
        bool colorSwapEnabled = false;
        int frameGenMode = FRAME_GEN_OFF;
        std::unique_ptr<NPUFrameInterpolator> npuInterpolator;
        bool initialized = false;
        std::atomic_bool isOperationPending{false};
        
        VkResult createInstance();
        VkResult pickPhysicalDevice();
        VkResult createDevice();
        VkResult createPipelines();
        void swapColors(Drawable *drawable);
        void vulkanInterpolate(Drawable *prevDrawable, Drawable *currDrawable, AHardwareBuffer *outputBuffer);
        
  public:      
        EffectComposer();
        ~EffectComposer();
        void cleanup();
        VkResult createComposerTexture(Drawable *drawable);
        void destroyComposerTexture(Drawable *drawable);
        void init();
        void apply(Drawable *drawable);
        void setColorSwapEnabled(bool enabled);
        bool isColorSwapEnabled();
        void setFrameGenEnabled(bool enabled);
        bool isFrameGenEnabled();
        void setFrameGenMode(int mode);
        int getFrameGenMode();
        void generateFrame(Drawable *prevDrawable, Drawable *currDrawable, AHardwareBuffer *outputBuffer);
        bool isEnabled();
        bool isSuitableForColorSwap(Drawable *drawable);
        bool isPending();
};
