#ifndef KYTY_GRAPHICS_GUEST_GPU_THREADSAMPLER_H_
#define KYTY_GRAPHICS_GUEST_GPU_THREADSAMPLER_H_

// Research: KYTY_PROF (live, default 0). Windows only: a wall-clock sampling profiler of the
// emulator's own threads. While on, a background thread suspends each registered thread about
// every 1 ms, reads its RIP and resumes it; every 5 s it prints the hottest exe+0x<offset> RIPs
// (symbolize with kyty_emulator.map next to the exe; offsets are image-base relative, so ASLR is
// fine) plus "outside:<module>" totals for samples in other modules (driver, ntdll waits, ...).
// It also prints "by first exe caller": for each sample the first stack frame inside the exe (the
// RIP itself for exe samples; otherwise found by unwinding a copy of the stack taken while the
// thread was suspended, RtlLookupFunctionEntry/RtlVirtualUnwind, up to 32 frames), keyed with the
// sample's module, so time in the driver / ntdll is attributed to the emulator code that called it.
namespace Libs::Graphics::ThreadSampler {

// Registers the calling thread for sampling under the name (a string literal). No-op off Windows.
void RegisterCurrentThread(const char* name);

} // namespace Libs::Graphics::ThreadSampler

#endif
