#include "sync-stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SyncStats {

namespace {

constexpr int    Frames    = 28;
constexpr size_t TableSize = 1024;

struct Entry {
	std::array<uint64_t, Frames> stack {};
	uint32_t                     kind  = 0;
	uint64_t                     shader = 0;
	uint64_t                     calls = 0;
	int64_t                      ns    = 0;
	bool                         used  = false;
};

Entry    g_table[TableSize];
uint64_t g_base = 0;
std::chrono::steady_clock::time_point g_start {};

void Print() {
	std::vector<const Entry*> entries;
	for (const auto& entry: g_table)
		if (entry.used) entries.push_back(&entry);
	std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) { return a->ns > b->ns; });
	int64_t total[2] {};
	for (const auto* entry: entries) total[entry->kind & 1] += entry->ns;
	std::printf("[sync] 10 s: GPU waits %.1f ms, synchronous downloads %.1f ms (render thread)\n", total[0] / 1e6, total[1] / 1e6);
	for (size_t i = 0; i < std::min<size_t>(entries.size(), 16); ++i) {
		const auto& entry = *entries[i];
		std::printf("[sync]  %s %7.1f ms %6" PRIu64 " calls, CS 0x%" PRIx64 ":", entry.kind == 0 ? "gpu " : "down", entry.ns / 1e6,
		            entry.calls, entry.shader);
		// (The emulator's frames only: through an exception handler the faulting code's frames come after the
		// system's.)
		for (const auto frame: entry.stack)
			if (frame >= 0x140000000ull && frame < 0x144000000ull) std::printf(" %" PRIx64, frame);
		std::printf("\n");
	}
	std::fflush(stdout);
	for (auto& entry: g_table) entry = {};
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SYNC_STATS");
		return value != nullptr && *value != '\0' && *value != '0';
	}();
	return enabled;
}

void Note(uint32_t kind, int64_t ns) noexcept {
#if defined(_WIN32)
	if (!Enabled()) return;
	if (g_base == 0) g_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
	void*        frames[Frames + 2] {};
	const auto   count = RtlCaptureStackBackTrace(2, Frames, frames, nullptr);
	std::array<uint64_t, Frames> stack {};
	const uint64_t shader = g_shader;
	uint64_t     hash  = kind * 0x9e3779b97f4a7c15ull ^ shader * 0xc2b2ae3d27d4eb4full;
	for (int i = 0; i < count && i < Frames; ++i) {
		auto address = reinterpret_cast<uint64_t>(frames[i]);
		// Exe-relative (the map's addresses: 0x140000000 + rva); other modules as they are.
		if (address >= g_base && address < g_base + 0x4000000) address = address - g_base + 0x140000000ull;
		stack[i] = address;
		hash     = (hash ^ address) * 0x100000001b3ull;
	}
	for (size_t probe = 0; probe < TableSize; ++probe) {
		auto& entry = g_table[(hash + probe) & (TableSize - 1)];
		if (!entry.used) {
			entry.used  = true;
			entry.stack = stack;
			entry.kind  = kind;
			entry.shader = shader;
		} else if (entry.kind != kind || entry.shader != shader || entry.stack != stack) {
			continue;
		}
		entry.calls++;
		entry.ns += ns;
		break;
	}
	const auto now = std::chrono::steady_clock::now();
	if (g_start == std::chrono::steady_clock::time_point {}) g_start = now;
	if (now - g_start >= std::chrono::seconds(10)) {
		Print();
		g_start = now;
	}
#else
	(void)kind;
	(void)ns;
#endif
}

} // namespace SyncStats
