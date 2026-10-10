#include "fsr3-fg.h"

#include "frame-gen.h"
#include "graphics/presentation/videoOut.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>
#if defined(KYTY_LOCAL_VULKAN_RECORDING)
#include "local/vulkan-recording.h"
#endif
#endif

namespace Libs::Graphics::Fsr3 {

#if defined(_WIN32)
namespace {

bool         g_requested = false;
HMODULE      g_dll       = nullptr;
ffxFunctions g_ffx {};

// Queues (OnDevice).
GraphicContext* g_graphics       = nullptr;
VkQueue         g_present_queue  = VK_NULL_HANDLE; // FI present: graphics family
VkQueue         g_async_queue    = VK_NULL_HANDLE; // compute family, index 0
VkQueue         g_acquire_queue  = VK_NULL_HANDLE; // compute family, index 1
uint32_t        g_compute_family = UINT32_MAX;

// The driver's functions, not the recording layer's.
PFN_vkGetDeviceProcAddr g_get_device_proc = nullptr;
PFN_vkQueueSubmit       g_queue_submit    = nullptr;

// The swapchain context and its replacement functions.
ffxContext                                g_swapchain_context = nullptr;
ffxQueryDescSwapchainReplacementFunctionsVK g_functions {};

// The frame generation context, made for one display size and format.
ffxContext   g_fg_context = nullptr;
vk::Extent2D g_fg_display {};
vk::Extent2D g_fg_max_render {};
vk::Format   g_fg_format = vk::Format::eUndefined;
bool         g_fg_failed = false;

// The prepare pass's command buffers (a ring, each reused once its fence signaled).
struct Slot {
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence         fence   = VK_NULL_HANDLE;
	bool            pending = false;
};
VkCommandPool         g_pool = VK_NULL_HANDLE;
std::array<Slot, 4>   g_slots {};
uint32_t              g_slot = 0;

uint64_t g_frame_id      = 0;
bool     g_generating    = false; // the last frame had generation on
int      g_state_logged  = -1;
std::chrono::steady_clock::time_point g_last_real {};

// Per-second statistics (the run log).
struct Stats {
	std::chrono::steady_clock::time_point start {};
	uint32_t real = 0, generated_on = 0, no_inputs = 0, movie = 0;
	uint64_t presents_at_start = 0;
	double   prepare_ms = 0, present_ms = 0, max_present_ms = 0;
} g_stats;

std::mutex g_mutex; // the contexts are not thread safe (presents, prepare, destruction)

bool EnvOn(const char* name, bool fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || *value == '\0') return fallback;
	return *value != '0';
}
float EnvFloat(const char* name, float fallback) {
	const char* value = std::getenv(name);
	return value != nullptr && *value != '\0' ? static_cast<float>(std::atof(value)) : fallback;
}

void Message(uint32_t type, const wchar_t* message) {
	static std::atomic<uint32_t> count {0};
	if (count.fetch_add(1, std::memory_order_relaxed) > 200) return;
	std::printf("FSR3 FG: FidelityFX message (%u): %ls\n", type, message);
	std::fflush(stdout);
}

VkResult GameQueueSubmit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
	Common::LockGuard lock(g_graphics->queue_mutex);
	return g_queue_submit(static_cast<VkQueue>(g_graphics->queue), count, submits, fence);
}

FfxApiResource ImageResource(const VulkanImage& image, bool depth) {
	FfxApiResource resource {};
	resource.resource                = static_cast<void*>(static_cast<VkImage>(image.image));
	resource.state                   = FFX_API_RESOURCE_STATE_COMPUTE_READ;
	auto& desc                       = resource.description;
	desc.type                        = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	desc.format                      = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(image.format));
	desc.width                       = image.extent.width;
	desc.height                      = image.extent.height;
	desc.depth                       = 1;
	desc.mipCount                    = 1;
	desc.flags                       = FFX_API_RESOURCE_FLAGS_NONE;
	desc.usage                       = depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : FFX_API_RESOURCE_USAGE_READ_ONLY;
	return resource;
}

void DestroyFrameGeneration() {
	if (g_fg_context != nullptr) {
		// Off in the swapchain first (it calls the context while presenting).
		ffxConfigureDescFrameGeneration off {};
		off.header.type            = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		off.frameGenerationEnabled = false;
		g_ffx.Configure(&g_fg_context, &off.header);
		g_ffx.DestroyContext(&g_fg_context, nullptr);
		g_fg_context = nullptr;
	}
}

bool CreateFrameGeneration(GraphicContext& graphics, vk::Extent2D display, vk::Extent2D render, vk::Format format) {
	DestroyFrameGeneration();
	ffxCreateBackendVKDesc backend {};
	backend.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.vkDevice         = static_cast<VkDevice>(graphics.device);
	backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	backend.vkDeviceProcAddr = g_get_device_proc;

	ffxCreateContextDescFrameGeneration create {};
	create.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	create.header.pNext = &backend.header;
	// The game's depth is reversed (1 near, 0 far) with an infinite far plane (KYTY_FSR3_DEPTH=normal: as is).
	const char* depth   = std::getenv("KYTY_FSR3_DEPTH");
	const bool  normal  = depth != nullptr && std::string(depth) == "normal";
	create.flags        = normal ? 0u : (FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE);
	if (EnvOn("KYTY_FSR3_DEBUG", false)) create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
	create.displaySize      = {display.width, display.height};
	create.maxRenderSize    = {render.width, render.height};
	create.backBufferFormat = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(format));
	const auto result       = g_ffx.CreateContext(&g_fg_context, &create.header, nullptr);
	if (result != FFX_API_RETURN_OK) {
		std::printf("FSR3 FG: frame generation context not created (%u): generation stays off\n", result);
		std::fflush(stdout);
		g_fg_context = nullptr;
		g_fg_failed  = true;
		return false;
	}
	g_fg_display    = display;
	g_fg_max_render = render;
	g_fg_format     = format;
	std::printf("FSR3 FG: frame generation context %ux%u (render up to %ux%u, depth %s)\n", display.width, display.height,
	            render.width, render.height, normal ? "as is" : "reversed, infinite");
	std::fflush(stdout);
	return true;
}

bool EnsureCommandBuffers(GraphicContext& graphics) {
	if (g_pool != VK_NULL_HANDLE) return true;
	const auto device = static_cast<VkDevice>(graphics.device);
	auto*      create_pool = reinterpret_cast<PFN_vkCreateCommandPool>(g_get_device_proc(device, "vkCreateCommandPool"));
	auto*      allocate    = reinterpret_cast<PFN_vkAllocateCommandBuffers>(g_get_device_proc(device, "vkAllocateCommandBuffers"));
	auto*      create_fence = reinterpret_cast<PFN_vkCreateFence>(g_get_device_proc(device, "vkCreateFence"));
	VkCommandPoolCreateInfo pool {};
	pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = graphics.queue_family;
	if (create_pool(device, &pool, nullptr, &g_pool) != VK_SUCCESS) return false;
	for (auto& slot: g_slots) {
		VkCommandBufferAllocateInfo info {};
		info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		info.commandPool        = g_pool;
		info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		info.commandBufferCount = 1;
		if (allocate(device, &info, &slot.command) != VK_SUCCESS) return false;
		VkFenceCreateInfo fence {};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (create_fence(device, &fence, nullptr, &slot.fence) != VK_SUCCESS) return false;
	}
	return true;
}

// The camera's vertical field of view and near plane from the world->clip matrix (row-major, clip = M * world),
// and its axes and position (FSR 3.1.4's camera info). False: no camera this frame.
struct CameraValues {
	float fov = 1.0f, z_near = 0.1f;
	float position[3] {}, up[3] {0, 1, 0}, right[3] {1, 0, 0}, forward[3] {0, 0, 1};
	bool  valid = false;
};
CameraValues FromCamera(const FrameGen::Camera& camera) {
	CameraValues out;
	if (!camera.valid) return out;
	const auto& m   = camera.view_proj;
	const auto  row = [&](uint32_t i) { return std::array<double, 4> {m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]}; };
	const auto  r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
	const double flen = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
	if (flen < 1e-6) return out;
	const double f[3] {r3[0] / flen, r3[1] / flen, r3[2] / flen};
	const double tf = r3[3] / flen;
	const auto split = [&](const std::array<double, 4>& r, double& s, double axis[3], double& t) {
		const double off = (r[0] * f[0] + r[1] * f[1] + r[2] * f[2]) / flen;
		double v[3] {r[0] / flen - off * f[0], r[1] / flen - off * f[1], r[2] / flen - off * f[2]};
		s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (s < 1e-9) return false;
		for (int i = 0; i < 3; ++i) axis[i] = v[i] / s;
		t = (r[3] / flen - off * tf) / s;
		return true;
	};
	double sx = 0, sy = 0, x[3] {}, y[3] {}, tx = 0, ty = 0;
	if (!split(r0, sx, x, tx) || !split(r1, sy, y, ty)) return out;
	const double up[3] {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
	if (up[0] * y[0] + up[1] * y[1] + up[2] * y[2] < 0) ty = -ty;
	for (int i = 0; i < 3; ++i) {
		out.right[i]    = static_cast<float>(x[i]);
		out.up[i]       = static_cast<float>(up[i]);
		out.forward[i]  = static_cast<float>(f[i]);
		out.position[i] = static_cast<float>(-(tx * x[i] + ty * up[i] + tf * f[i]));
	}
	out.fov    = static_cast<float>(2.0 * std::atan(1.0 / std::fabs(sy)));
	const double z_near = std::fabs(r2[3] / flen);
	out.z_near = z_near > 1e-4 && z_near < 100.0 ? static_cast<float>(z_near) : 0.1f;
	out.valid  = std::isfinite(out.fov) && out.fov > 0.1f && out.fov < 3.0f;
	return out;
}

void LogState(int state, const char* text) {
	if (state == g_state_logged) return;
	g_state_logged = state;
	std::printf("FSR3 FG: %s\n", text);
	std::fflush(stdout);
}

} // namespace

bool Requested() {
	static const bool requested = [] {
		if (!EnvOn("KYTY_FSR3_FG", false)) return false;
		wchar_t exe[MAX_PATH] {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring dir(exe);
		dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
		for (const auto* name: {L"amd_fidelityfx_vk.dll", L"fsr3\\amd_fidelityfx_vk.dll"}) {
			g_dll = LoadLibraryW((dir + name).c_str());
			if (g_dll != nullptr) break;
		}
		if (g_dll == nullptr) {
			std::printf("FSR3 FG: amd_fidelityfx_vk.dll not found next to the emulator: frame generation off\n");
			std::fflush(stdout);
			return false;
		}
		ffxLoadFunctions(&g_ffx, g_dll);
		if (g_ffx.CreateContext == nullptr || g_ffx.Configure == nullptr || g_ffx.Dispatch == nullptr) {
			std::printf("FSR3 FG: amd_fidelityfx_vk.dll has no FidelityFX API: frame generation off\n");
			std::fflush(stdout);
			return false;
		}
		std::printf("FSR3 FG: requested (KYTY_FSR3_FG), FidelityFX API loaded\n");
		std::fflush(stdout);
		return true;
	}();
	g_requested = requested;
	return requested;
}

bool PlanQueues(vk::PhysicalDevice physical_device, uint32_t queue_family, uint32_t graphics_count, QueuePlan& plan) {
	if (!Requested()) return false;
	uint32_t family_count = 0;
	physical_device.getQueueFamilyProperties(&family_count, nullptr);
	std::vector<vk::QueueFamilyProperties> families(family_count);
	physical_device.getQueueFamilyProperties(&family_count, families.data());
	if (queue_family >= family_count || families[queue_family].queueCount < graphics_count + 1) {
		std::printf("FSR3 FG: no free graphics queue for its presents: frame generation off\n");
		std::fflush(stdout);
		g_requested = false;
		return false;
	}
	plan.graphics_index = graphics_count;
	for (uint32_t family = 0; family < family_count; ++family) {
		const auto flags = families[family].queueFlags;
		if (family != queue_family && (flags & vk::QueueFlagBits::eCompute) && !(flags & vk::QueueFlagBits::eGraphics) &&
		    families[family].queueCount >= 2) {
			plan.compute_family = family;
			plan.compute_count  = 2;
			break;
		}
	}
	if (plan.compute_family == UINT32_MAX) {
		std::printf("FSR3 FG: no async compute queue family: frame generation off\n");
		std::fflush(stdout);
		g_requested = false;
		return false;
	}
	return true;
}

void OnDevice(GraphicContext& graphics, const QueuePlan& plan) {
	if (!g_requested || plan.compute_family == UINT32_MAX) return;
	g_graphics = &graphics;
#if defined(KYTY_LOCAL_VULKAN_RECORDING)
	g_get_device_proc = LocalVulkanRecording::DirectDispatch().vkGetDeviceProcAddr;
#endif
	if (g_get_device_proc == nullptr) g_get_device_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
	const auto device = static_cast<VkDevice>(graphics.device);
	g_queue_submit    = reinterpret_cast<PFN_vkQueueSubmit>(g_get_device_proc(device, "vkQueueSubmit"));
	auto* get_queue   = reinterpret_cast<PFN_vkGetDeviceQueue>(g_get_device_proc(device, "vkGetDeviceQueue"));
	get_queue(device, graphics.queue_family, plan.graphics_index, &g_present_queue);
	get_queue(device, plan.compute_family, 0, &g_async_queue);
	get_queue(device, plan.compute_family, 1, &g_acquire_queue);
	g_compute_family = plan.compute_family;
	if (g_present_queue == VK_NULL_HANDLE || g_async_queue == VK_NULL_HANDLE || g_acquire_queue == VK_NULL_HANDLE ||
	    g_queue_submit == nullptr) {
		std::printf("FSR3 FG: its queues were not created: frame generation off\n");
		std::fflush(stdout);
		g_requested = false;
		return;
	}
	ffxConfigureDescGlobalDebug1 debug {};
	debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
	debug.debugLevel  = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
	debug.fpMessage   = &Message;
	g_ffx.Configure(nullptr, &debug.header);
	std::printf("FSR3 FG: queues: present (graphics family %u, queue %u), async compute and image acquire (family %u)\n",
	            graphics.queue_family, plan.graphics_index, plan.compute_family);
	std::fflush(stdout);
}

bool Active() {
	return g_swapchain_context != nullptr;
}

bool CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR* out) {
	if (!g_requested || g_graphics == nullptr) return false;
	std::lock_guard lock(g_mutex);
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	// (A new context each time: one re-created through the context's own replacement function presented nothing,
	// a black screen after the window went fullscreen.)
	ffxCreateContextDescFrameGenerationSwapChainVK create {};
	create.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
	create.physicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	create.device         = static_cast<VkDevice>(graphics.device);
	create.swapchain      = &swapchain;
	create.allocator      = nullptr;
	create.createInfo     = static_cast<VkSwapchainCreateInfoKHR>(info);
	create.gameQueue      = {static_cast<VkQueue>(graphics.queue), graphics.queue_family, &GameQueueSubmit};
	create.asyncComputeQueue = {g_async_queue, g_compute_family, nullptr};
	create.presentQueue      = {g_present_queue, graphics.queue_family, nullptr};
	create.imageAcquireQueue = {g_acquire_queue, g_compute_family, nullptr};
	const auto result        = g_ffx.CreateContext(&g_swapchain_context, &create.header, nullptr);
	if (result != FFX_API_RETURN_OK || swapchain == VK_NULL_HANDLE) {
		std::printf("FSR3 FG: frame generation swapchain not created (%u): frame generation off\n", result);
		std::fflush(stdout);
		g_swapchain_context = nullptr;
		g_requested         = false;
		return false;
	}
	g_functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
	g_ffx.Query(&g_swapchain_context, &g_functions.header);
	if (g_functions.pOutAcquireNextImageKHR == nullptr || g_functions.pOutQueuePresentKHR == nullptr ||
	    g_functions.pOutGetSwapchainImagesKHR == nullptr) {
		std::printf("FSR3 FG: no replacement swapchain functions: frame generation off\n");
		std::fflush(stdout);
		g_ffx.DestroyContext(&g_swapchain_context, nullptr);
		g_swapchain_context = nullptr;
		g_requested         = false;
		return false;
	}
	std::printf("FSR3 FG: frame generation swapchain %ux%u (present mode %d)\n", info.imageExtent.width,
	            info.imageExtent.height, static_cast<int>(info.presentMode));
	std::fflush(stdout);
	*out = swapchain;
	return true;
}

void DestroySwapchain(GraphicContext& /*graphics*/, vk::SwapchainKHR /*swapchain*/) {
	std::lock_guard lock(g_mutex);
	if (g_swapchain_context == nullptr) return;
	// The frame generation context first (the swapchain calls it while presenting); destroying the swapchain's
	// context destroys its swapchain. The next CreateSwapchain makes both again.
	DestroyFrameGeneration();
	g_ffx.DestroyContext(&g_swapchain_context, nullptr);
	g_swapchain_context = nullptr;
	g_functions         = {};
	g_generating        = false;
	g_state_logged      = -1;
	g_stats             = {};
	std::printf("FSR3 FG: frame generation swapchain released (resize, fullscreen or exit)\n");
	std::fflush(stdout);
}

vk::Result GetSwapchainImages(GraphicContext& graphics, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images) {
	return static_cast<vk::Result>(g_functions.pOutGetSwapchainImagesKHR(
	    static_cast<VkDevice>(graphics.device), static_cast<VkSwapchainKHR>(swapchain), count, reinterpret_cast<VkImage*>(images)));
}

vk::Result AcquireNextImage(GraphicContext& graphics, vk::SwapchainKHR swapchain, vk::Semaphore semaphore, uint32_t* index) {
	return static_cast<vk::Result>(g_functions.pOutAcquireNextImageKHR(static_cast<VkDevice>(graphics.device),
	                                                                   static_cast<VkSwapchainKHR>(swapchain), UINT64_MAX,
	                                                                   static_cast<VkSemaphore>(semaphore), VK_NULL_HANDLE, index));
}

vk::Result QueuePresent(GraphicContext& graphics, const vk::PresentInfoKHR& info) {
	std::lock_guard lock(g_mutex);
	const auto      begin   = std::chrono::steady_clock::now();
	VkPresentInfoKHR present = info;
	present.pNext            = nullptr;
	const auto result = g_functions.pOutQueuePresentKHR(static_cast<VkQueue>(graphics.queue), &present);
	const double ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	g_stats.present_ms += ms;
	g_stats.max_present_ms = std::max(g_stats.max_present_ms, ms);
	return static_cast<vk::Result>(result);
}

void BeforePresent(GraphicContext& graphics, const FrameGen::FrameInputs* inputs, vk::SwapchainKHR swapchain,
                   vk::Extent2D backbuffer, vk::Format format) {
	if (!Active()) return;
	std::lock_guard lock(g_mutex);
	const auto now    = std::chrono::steady_clock::now();
	const bool movie  = VideoOut::MoviePlaying();
	const bool usable = inputs != nullptr && inputs->valid && !movie && !g_fg_failed;

	// Per-second line: real frames, the frames shown (real and generated), how many had generation.
	if (g_stats.start == std::chrono::steady_clock::time_point {}) g_stats.start = now;
	const double window = std::chrono::duration<double>(now - g_stats.start).count();
	if (window >= 1.0) {
		const uint64_t presents =
		    g_functions.pOutGetLastPresentCountFFXAPI != nullptr
		        ? g_functions.pOutGetLastPresentCountFFXAPI(static_cast<VkSwapchainKHR>(swapchain))
		        : 0;
		const uint64_t shown = presents >= g_stats.presents_at_start ? presents - g_stats.presents_at_start : 0;
		std::printf("[fsr3] real %.1f fps, shown %.1f fps; generation on %u of %u frames (no TAA %u, movie %u); "
		            "prepare %.2f ms, present call %.2f ms (max %.1f)\n",
		            g_stats.real / window, static_cast<double>(shown) / window, g_stats.generated_on, g_stats.real,
		            g_stats.no_inputs, g_stats.movie, g_stats.real != 0 ? g_stats.prepare_ms / g_stats.real : 0.0,
		            g_stats.real != 0 ? g_stats.present_ms / g_stats.real : 0.0, g_stats.max_present_ms);
		std::fflush(stdout);
		g_stats                   = {};
		g_stats.start             = now;
		g_stats.presents_at_start = presents;
	}
	g_stats.real++;
	if (movie) g_stats.movie++;
	else if (!usable) g_stats.no_inputs++;

	const double delta_ms = g_last_real == std::chrono::steady_clock::time_point {}
	                            ? 16.7
	                            : std::chrono::duration<double, std::milli>(now - g_last_real).count();
	g_last_real = now;

	if (usable && !g_fg_failed) {
		const vk::Extent2D render = inputs->render_extent;
		if (g_fg_context == nullptr || g_fg_display != backbuffer || g_fg_format != format ||
		    render.width > g_fg_max_render.width || render.height > g_fg_max_render.height) {
			CreateFrameGeneration(graphics, backbuffer,
			                      {std::max(render.width, g_fg_max_render.width), std::max(render.height, g_fg_max_render.height)},
			                      format);
		}
	}
	if (g_fg_context == nullptr) {
		LogState(0, movie ? "off (movie)" : "off (no TAA pass yet: menu, loading or movie)");
		return;
	}

	const bool generate = usable && g_fg_context != nullptr;
	++g_frame_id;
	ffxConfigureDescFrameGeneration config {};
	config.header.type             = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain               = static_cast<void*>(static_cast<VkSwapchainKHR>(swapchain));
	config.presentCallback         = nullptr;
	config.presentCallbackUserContext = nullptr;
	config.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* context) -> ffxReturnCode_t {
		return g_ffx.Dispatch(reinterpret_cast<ffxContext*>(context), &params->header);
	};
	config.frameGenerationCallbackUserContext = &g_fg_context;
	config.frameGenerationEnabled             = generate;
	config.allowAsyncWorkloads                = EnvOn("KYTY_FSR3_ASYNC", false);
	config.HUDLessColor                       = FfxApiResource {};
	config.flags = EnvOn("KYTY_FSR3_DEBUG_VIEW", false) ? static_cast<uint32_t>(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW) : 0u;
	config.onlyPresentGenerated = false;
	config.generationRect       = {0, 0, static_cast<int32_t>(backbuffer.width), static_cast<int32_t>(backbuffer.height)};
	config.frameID              = g_frame_id;
	if (const auto result = g_ffx.Configure(&g_fg_context, &config.header); result != FFX_API_RETURN_OK) {
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1) < 8) {
			std::printf("FSR3 FG: configure failed (%u)\n", result);
			std::fflush(stdout);
		}
		return;
	}
	if (!generate) {
		g_generating = false;
		LogState(1, movie ? "off (movie)" : "off (no TAA pass: menu, loading or movie)");
		return;
	}

	// The prepare pass: the game's depth and motion vectors (copied at the guest flip) into the context.
	if (!EnsureCommandBuffers(graphics)) return;
	const auto device       = static_cast<VkDevice>(graphics.device);
	static auto* wait_fences  = reinterpret_cast<PFN_vkWaitForFences>(g_get_device_proc(device, "vkWaitForFences"));
	static auto* reset_fences = reinterpret_cast<PFN_vkResetFences>(g_get_device_proc(device, "vkResetFences"));
	static auto* begin_cb     = reinterpret_cast<PFN_vkBeginCommandBuffer>(g_get_device_proc(device, "vkBeginCommandBuffer"));
	static auto* end_cb       = reinterpret_cast<PFN_vkEndCommandBuffer>(g_get_device_proc(device, "vkEndCommandBuffer"));
	static auto* reset_cb     = reinterpret_cast<PFN_vkResetCommandBuffer>(g_get_device_proc(device, "vkResetCommandBuffer"));
	auto&        slot         = g_slots[g_slot];
	g_slot                    = (g_slot + 1) % static_cast<uint32_t>(g_slots.size());
	if (slot.pending) {
		wait_fences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
		reset_fences(device, 1, &slot.fence);
		slot.pending = false;
	}
	reset_cb(slot.command, 0);
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin_cb(slot.command, &begin);

	const auto& camera = FromCamera(inputs->camera);
	static const float mv_sign = EnvFloat("KYTY_FSR3_MV_SIGN", -1.0f) >= 0 ? 1.0f : -1.0f;
	ffxDispatchDescFrameGenerationPrepare prepare {};
	prepare.header.type    = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare.frameID        = g_frame_id;
	prepare.flags          = config.flags;
	prepare.commandList    = slot.command;
	prepare.renderSize     = {inputs->render_extent.width, inputs->render_extent.height};
	prepare.jitterOffset   = {0.0f, 0.0f};
	// The game's vectors are in UV units along the motion; FSR wants pixels towards the previous position.
	prepare.motionVectorScale = {mv_sign * static_cast<float>(inputs->render_extent.width),
	                             mv_sign * static_cast<float>(inputs->render_extent.height)};
	prepare.frameTimeDelta = static_cast<float>(std::clamp(delta_ms, 1.0, 100.0));
	prepare.unused_reset   = false;
	// Reversed infinite depth: far is the near plane, near is infinite (the SDK's convention).
	const float z_near     = EnvFloat("KYTY_FSR3_NEAR", camera.valid ? camera.z_near : 0.1f);
	prepare.cameraNear     = FLT_MAX;
	prepare.cameraFar      = z_near;
	prepare.cameraFovAngleVertical  = EnvFloat("KYTY_FSR3_FOV", camera.valid ? camera.fov : 1.0f);
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth                   = ImageResource(inputs->depth, true);
	prepare.motionVectors           = ImageResource(inputs->motion, false);
	ffxDispatchDescFrameGenerationPrepareCameraInfo camera_info {};
	camera_info.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	if (camera.valid) {
		std::copy_n(camera.position, 3, camera_info.cameraPosition);
		std::copy_n(camera.up, 3, camera_info.cameraUp);
		std::copy_n(camera.right, 3, camera_info.cameraRight);
		std::copy_n(camera.forward, 3, camera_info.cameraForward);
		prepare.header.pNext = &camera_info.header;
	}
	const auto prepare_begin = std::chrono::steady_clock::now();
	const auto result        = g_ffx.Dispatch(&g_fg_context, &prepare.header);
	end_cb(slot.command);
	if (result != FFX_API_RETURN_OK) {
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1) < 8) {
			std::printf("FSR3 FG: prepare failed (%u)\n", result);
			std::fflush(stdout);
		}
		return;
	}
	VkSubmitInfo submit {};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &slot.command;
	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		g_queue_submit(static_cast<VkQueue>(graphics.queue), 1, &submit, slot.fence);
	}
	slot.pending = true;
	g_stats.prepare_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_begin).count();
	g_stats.generated_on++;
	if (!g_generating) {
		std::printf("FSR3 FG: on (TAA pass: %ux%u depth and motion vectors, camera %s)\n", inputs->render_extent.width,
		            inputs->render_extent.height, camera.valid ? "found" : "default");
		std::fflush(stdout);
		g_state_logged = 2;
	}
	g_generating = true;
}

#else // !_WIN32

bool Requested() {
	return false;
}
bool PlanQueues(vk::PhysicalDevice, uint32_t, uint32_t, QueuePlan&) {
	return false;
}
void OnDevice(GraphicContext&, const QueuePlan&) {}
bool Active() {
	return false;
}
bool CreateSwapchain(GraphicContext&, const vk::SwapchainCreateInfoKHR&, vk::SwapchainKHR*) {
	return false;
}
void       DestroySwapchain(GraphicContext&, vk::SwapchainKHR) {}
vk::Result GetSwapchainImages(GraphicContext&, vk::SwapchainKHR, uint32_t*, vk::Image*) {
	return vk::Result::eErrorFeatureNotPresent;
}
vk::Result AcquireNextImage(GraphicContext&, vk::SwapchainKHR, vk::Semaphore, uint32_t*) {
	return vk::Result::eErrorFeatureNotPresent;
}
vk::Result QueuePresent(GraphicContext&, const vk::PresentInfoKHR&) {
	return vk::Result::eErrorFeatureNotPresent;
}
void BeforePresent(GraphicContext&, const FrameGen::FrameInputs*, vk::SwapchainKHR, vk::Extent2D, vk::Format) {}

#endif

} // namespace Libs::Graphics::Fsr3
