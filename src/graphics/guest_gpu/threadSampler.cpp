#include "graphics/guest_gpu/threadSampler.h"

#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS

namespace Libs::Graphics::ThreadSampler {

void RegisterCurrentThread(const char* /*name*/) {}

} // namespace Libs::Graphics::ThreadSampler

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/liveSwitches.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ThreadSampler {

namespace {

struct Target {
	const char*                                  name   = nullptr;
	HANDLE                                       handle = nullptr;
	DWORD                                        id     = 0;
	uint64_t                                     samples = 0;
	std::unordered_map<uint64_t, uint64_t>       rips;    // exe offset -> count
	std::unordered_map<std::string, uint64_t>    outside; // module name -> count
};

std::mutex          g_mutex; // NOLINT
std::vector<Target> g_targets; // NOLINT
std::once_flag      g_start; // NOLINT

uintptr_t g_image_base = 0;
uintptr_t g_image_end  = 0;

std::unordered_map<uintptr_t, std::string> g_page_modules; // NOLINT, sampler thread only

const std::string& ModuleName(uintptr_t rip) {
	const uintptr_t page = rip & ~static_cast<uintptr_t>(0xfff);
	auto            it   = g_page_modules.find(page);
	if (it != g_page_modules.end()) {
		return it->second;
	}
	std::string name = "outside:?";
	HMODULE     mod  = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(rip), &mod) != 0 &&
	    mod != nullptr) {
		char path[MAX_PATH] = {};
		if (GetModuleFileNameA(mod, path, MAX_PATH) != 0) {
			const char* base = path;
			for (const char* p = path; *p != '\0'; p++) {
				if (*p == '\\' || *p == '/') {
					base = p + 1;
				}
			}
			name = std::string("outside:") + base;
		}
	} else {
		name = "outside:<no module>";
	}
	return g_page_modules.emplace(page, std::move(name)).first->second;
}

template <class K>
std::vector<std::pair<K, uint64_t>> Sorted(const std::unordered_map<K, uint64_t>& map) {
	std::vector<std::pair<K, uint64_t>> v(map.begin(), map.end());
	std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	return v;
}

void Print(Target& t) {
	::printf("Profile (5 s) %s: %" PRIu64 " samples\n", t.name, t.samples);
	if (t.samples != 0) {
		const double total = static_cast<double>(t.samples);
		auto         rips  = Sorted(t.rips);
		for (size_t i = 0; i < rips.size() && i < 40; i++) {
			::printf("  %5.2f%%  exe+0x%" PRIx64 "\n", 100.0 * static_cast<double>(rips[i].second) / total, rips[i].first);
		}
		std::unordered_map<uint64_t, uint64_t> buckets;
		for (const auto& [off, n]: t.rips) {
			buckets[off >> 8u] += n;
		}
		auto sorted_buckets = Sorted(buckets);
		::printf(" %s by 256-byte bucket:\n", t.name);
		for (size_t i = 0; i < sorted_buckets.size() && i < 25; i++) {
			::printf("  %5.2f%%  exe+0x%" PRIx64 "..0x%" PRIx64 "\n",
			         100.0 * static_cast<double>(sorted_buckets[i].second) / total, sorted_buckets[i].first << 8u,
			         (sorted_buckets[i].first << 8u) + 0xffu);
		}
		auto outside = Sorted(t.outside);
		for (const auto& [mod, n]: outside) {
			::printf("  %5.2f%%  %s\n", 100.0 * static_cast<double>(n) / total, mod.c_str());
		}
	}
	t.samples = 0;
	t.rips.clear();
	t.outside.clear();
}

void SamplerThread() {
	static auto& prof = Common::LiveSwitches::Get("KYTY_PROF", 0);
	auto report_time = std::chrono::steady_clock::now();
	bool was_on      = false;
	for (;;) {
		const bool on = prof.load(std::memory_order_relaxed) != 0;
		if (!on) {
			was_on = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!was_on) {
			was_on      = true;
			report_time = now;
		}
		{
			std::lock_guard lock(g_mutex);
			for (auto& t: g_targets) {
				if (t.handle == nullptr) {
					continue;
				}
				if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
					continue;
				}
				CONTEXT ctx {};
				ctx.ContextFlags = CONTEXT_CONTROL;
				const BOOL ok    = GetThreadContext(t.handle, &ctx);
				ResumeThread(t.handle);
				if (ok == 0) {
					continue;
				}
				// Nothing below may take a lock the target could hold: it is already resumed.
				const auto rip = static_cast<uintptr_t>(ctx.Rip);
				t.samples++;
				if (rip >= g_image_base && rip < g_image_end) {
					t.rips[rip - g_image_base]++;
				} else {
					t.outside[ModuleName(rip)]++;
				}
			}
			if (now - report_time >= std::chrono::seconds(5)) {
				report_time = now;
				for (auto& t: g_targets) {
					Print(t);
				}
				::fflush(stdout);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void Start() {
	auto* base   = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
	g_image_base = reinterpret_cast<uintptr_t>(base);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	g_image_end     = g_image_base + nt->OptionalHeader.SizeOfImage;
	std::thread(SamplerThread).detach();
}

} // namespace

void RegisterCurrentThread(const char* name) {
	std::call_once(g_start, Start);
	HANDLE handle = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
	                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0) == 0) {
		return;
	}
	std::lock_guard lock(g_mutex);
	for (auto& t: g_targets) {
		if (std::strcmp(t.name, name) == 0) {
			// A restarted thread (the recording thread) replaces its earlier registration.
			if (t.handle != nullptr) {
				CloseHandle(t.handle);
			}
			t.handle = handle;
			t.id     = GetCurrentThreadId();
			return;
		}
	}
	Target t;
	t.name   = name;
	t.handle = handle;
	t.id     = GetCurrentThreadId();
	g_targets.push_back(std::move(t));
}

} // namespace Libs::Graphics::ThreadSampler

#endif
