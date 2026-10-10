#ifndef EMULATOR_SRC_LOCAL_FSR3_FG_H_
#define EMULATOR_SRC_LOCAL_FSR3_FG_H_
// AMD FSR 3.1 frame generation (KYTY_FSR3_FG=1), through the FidelityFX API's signed DLL
// (amd_fidelityfx_vk.dll next to the emulator) on any Vulkan GPU. The frame generation swapchain
// replaces the window's swapchain: it paces one generated frame between two real ones on threads of
// its own, so the guest's clock (vblank, flips) and the present thread keep their rate. Generation is
// on only for the frames whose guest frame ran the game's TAA pass (its depth and motion vectors,
// FrameGen::FrameInputs); menus, loading screens and movies have none and are presented as they are
// (frame generation off: the swapchain passes them through and restarts generation after them).
#include "graphics/host_gpu/graphicContext.h"

#include <cstdint>

namespace Libs::Graphics {
namespace FrameGen {
struct FrameInputs;
}
namespace Fsr3 {

// Process start (before the device): KYTY_FSR3_FG=1 and the DLL loaded.
[[nodiscard]] bool Requested();

// Device creation: the extra queues the frame generation swapchain needs (a present queue of the
// graphics family, an async compute queue and an image acquire queue). `queue_family` is the
// graphics family; the counts are added to `graphics_count` (the graphics family's queue count) and
// written for the compute family. False: they cannot be had (frame generation stays off).
struct QueuePlan {
	uint32_t graphics_index = 0;            // index of the FI present queue in the graphics family
	uint32_t compute_family = UINT32_MAX;   // family of the async compute and image acquire queues
	uint32_t compute_count  = 0;            // queues to create in it
};
bool PlanQueues(vk::PhysicalDevice physical_device, uint32_t queue_family, uint32_t graphics_count, QueuePlan& plan);
// After vkCreateDevice: fetch them.
void OnDevice(GraphicContext& graphics, const QueuePlan& plan);

// The swapchain. Create: replaces vkCreateSwapchainKHR (false: frame generation off, create it the
// normal way). The others replace their Vulkan function while Active().
[[nodiscard]] bool Active();
bool               CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR* out);
void               DestroySwapchain(GraphicContext& graphics, vk::SwapchainKHR swapchain);
vk::Result         GetSwapchainImages(GraphicContext& graphics, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images);
vk::Result         AcquireNextImage(GraphicContext& graphics, vk::SwapchainKHR swapchain, vk::Semaphore semaphore,
                                    uint32_t* index);
vk::Result         QueuePresent(GraphicContext& graphics, const vk::PresentInfoKHR& info);

// Present thread, after the frame's image is in the swapchain's backbuffer and its commands were
// submitted (and finished), before QueuePresent: the frame's generation inputs (null or invalid:
// none, no generation for this frame).
void BeforePresent(GraphicContext& graphics, const FrameGen::FrameInputs* inputs, vk::SwapchainKHR swapchain,
                   vk::Extent2D backbuffer, vk::Format format);

} // namespace Fsr3
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_LOCAL_FSR3_FG_H_
