// FgLatch.h - "SDK results decide state" (docs\DESIGN.md) without an SDK in it, so retirement_probe can drive it.
#pragma once
#include <windows.h>

// A setting the SDK is handed only when it changes (Streamline: Reflex mode + frame cap, FgStreamline.cpp ReflexApply).
// `mode` / `us` are what the SDK last ACCEPTED - that is what it has now. A refusal records nothing as applied, backs
// that same request off for kRetry asks (another request is issued at once) and counts.
// INVARIANT: `bad` = the latest ask was for a NEEDED setting and it is not in effect. It is set by a refused or
// backed-off needed ask, and cleared by (a) any accepted call, (b) an ask for what is already in effect - no call is
// made, the accepted state IS the wanted one, (c) NewChain. Without (b) one refused cap change followed by a return
// to the working cap left `bad` up for good: nothing was fed and the starvation watchdog dropped a healthy provider.
// One thread asks (main); `bad` is read on another (render), hence the interlocked writes. Zero-initialised POD.
struct FgReflexLatch
{
    enum { kRetry = 30 };
    enum { kBackoff = 0, kInEffect = 1, kAccepted = 2, kRefused = -1 };   // > 0 = the setting asked for is in effect
    int           mode;                       // last accepted (-1 = none yet)
    unsigned      us;
    int           failMode;                   // the request last refused, asks to let pass before it is issued again,
    unsigned      failUs, skip, fails;        // refusals since the last accepted call
    volatile LONG bad;

    // `call` issues the SDK call for (m, u) and returns true when the SDK accepted it; it runs only when needed.
    template <class F> int Apply(int m, unsigned u, bool needed, F call)
    {
        if (mode == m && us == u) { InterlockedExchange(&bad, 0); return kInEffect; }
        if (skip && failMode == m && failUs == u) { --skip; if (needed) InterlockedExchange(&bad, 1); return kBackoff; }
        // Accepted: `skip` is left alone - it is 0 if this was the refused request, and if it was another one the
        // refused request keeps its back-off (eOff accepted between two low-latency asks must not retry it per frame).
        if (call()) { mode = m; us = u; fails = 0; InterlockedExchange(&bad, 0); return kAccepted; }
        failMode = m; failUs = u; skip = kRetry; ++fails;
        if (needed) InterlockedExchange(&bad, 1);
        return kRefused;
    }
    // A new chain asks afresh: no back-off and no refusal of the previous one outlives it. What the SDK accepted stays
    // (the SDK is up for the process).
    void NewChain() { skip = 0; fails = 0; InterlockedExchange(&bad, 0); }
};
