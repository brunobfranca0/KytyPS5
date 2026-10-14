#ifndef KYTY_GRAPHICS_GUEST_GPU_THREADSAMPLER_H_
#define KYTY_GRAPHICS_GUEST_GPU_THREADSAMPLER_H_

// Research: KYTY_PROF (live, default 0). Windows only: a wall-clock sampling profiler of the
// emulator's own threads. While on, a background thread suspends each registered thread about
// every 1 ms, reads its RIP and resumes it; every 5 s it prints the hottest exe+0x<offset> RIPs
// (symbolize with kyty_emulator.map next to the exe; offsets are image-base relative, so ASLR is
// fine) plus "outside:<module>" totals for samples in other modules (driver, ntdll waits, ...).
namespace Libs::Graphics::ThreadSampler {

// Registers the calling thread for sampling under the name (a string literal). No-op off Windows.
void RegisterCurrentThread(const char* name);

} // namespace Libs::Graphics::ThreadSampler

#endif
