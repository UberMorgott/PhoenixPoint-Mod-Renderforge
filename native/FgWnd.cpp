// FgWnd.cpp - an HWND of our own inside the game window. DXGI refuses a second CreateSwapChainForHwnd on the
// window Unity's flip-model chain already owns (E_ACCESSDENIED, measured), and every vendor FG SDK creates its
// swapchain on an HWND, so the host owns a WS_CHILD window that covers the parent's client area and creates the
// shadow chain there. Same thread as the parent (created from inside a subclassed WndProc), so WM_NCHITTEST ->
// HTTRANSPARENT hands every mouse message to Unity and keyboard focus never moves.
//
// The subclass stays installed for the window's lifetime (restoring GWLP_WNDPROC after someone else subclassed on
// top of us would cut their chain) except when the parent changes or the plugin unloads: then it is restored IF we
// are still the top of the chain. WM_NCDESTROY drops the state so a recreated Unity window is subclassed afresh.
// Destroying the child must happen on the window's thread, so teardown posts a message; other threads only hide it.
// g_parent / g_child / g_origProc are touched from the UI, render and main threads: one SRWLOCK guards them.
#include "Fg.h"

namespace {

const UINT WM_RF_CREATE  = WM_APP + 0x51;   // wParam = 0; lResult = child HWND
const UINT WM_RF_DESTROY = WM_APP + 0x52;
const UINT WM_RF_PROBE   = WM_APP + 0x53;   // samples hit-test + focus on the UI thread

const wchar_t kClass[] = L"RenderforgeFgWnd";

SRWLOCK g_lock = SRWLOCK_INIT;
HWND    g_parent = NULL;
HWND    g_child = NULL;
WNDPROC g_origProc = NULL;
FgWndProbe g_probe = {};

struct Locked
{
    Locked()  { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

HWND Parent() { Locked lk; return g_parent; }
HWND Child()  { Locked lk; return g_child; }

LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:     return HTTRANSPARENT;          // mouse goes to the parent (same thread)
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT:         ValidateRect(h, NULL); return 0;
    case WM_SETFOCUS:      { HWND p = Parent(); if (p) SetFocus(p); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY:       { Locked lk; if (h == g_child) g_child = NULL; return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

void FitChild()
{
    HWND c = Child(), p = Parent();
    if (!c || !p) return;
    RECT r; GetClientRect(p, &r);
    SetWindowPos(c, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig;
    { Locked lk; orig = g_origProc; }
    switch (m) {
    case WM_RF_CREATE: {
        if (HWND c = Child()) return (LRESULT)c;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);                              // duplicate registration is harmless
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_CLIPCHILDREN);
        RECT r; GetClientRect(h, &r);
        HWND c = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kClass, kClass, WS_CHILD | WS_VISIBLE,
                                 0, 0, r.right - r.left, r.bottom - r.top, h, NULL, wc.hInstance, NULL);
        g_probe.createErr = c ? 0 : GetLastError();
        { Locked lk; g_child = c; }
        FitChild();
        return (LRESULT)c;
    }
    case WM_RF_DESTROY:                                      // wParam = the exact child to destroy (a newer one may exist)
        if (w) DestroyWindow((HWND)w);
        return 0;
    case WM_RF_PROBE: {
        RECT r; GetClientRect(h, &r);
        POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
        ClientToScreen(h, &c);
        HWND child = Child();
        g_probe.hit = child ? (int)SendMessageW(child, WM_NCHITTEST, 0, MAKELPARAM(c.x, c.y)) : 0;
        g_probe.focus = GetFocus();
        g_probe.foreground = GetForegroundWindow();
        g_probe.child = child;
        return 0;
    }
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
        FitChild();
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        FgHostTearDown(m == WM_DISPLAYCHANGE ? "WM_DISPLAYCHANGE" : "WM_DPICHANGED");   // the driver rebuilds (Fg_Alive -> 0)
        break;
    case WM_NCDESTROY: {
        // Unity is recreating its HWND: the child died with it (DestroyWindow destroys children first), the
        // subclass dies here, and the chain built on this window goes with them. DETACH only - the main thread's
        // pump destroys the chain; never a vendor teardown or a wait for the render thread on the UI thread.
        FgLog("wnd: parent %p WM_NCDESTROY", (void*)h);
        { Locked lk; g_child = NULL; g_parent = NULL; g_origProc = NULL; }
        FgHostTearDown("parent WM_NCDESTROY");
        FgHookForgetApp();
        return CallWindowProcW(orig, h, m, w, l);
    }
    }
    return CallWindowProcW(orig, h, m, w, l);
}

// Bounded cross-thread SendMessage: direct call on the window's own thread, else waits at most `ms`.
bool Send(UINT m, DWORD ms, LRESULT* out)
{
    DWORD_PTR res = 0;
    HWND p = Parent();
    if (!p) return false;
    LRESULT ok = SendMessageTimeoutW(p, m, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &res);
    if (out) *out = (LRESULT)res;
    return ok != 0;
}

// Put the parent's own WndProc back if we are still the top of its chain (a subclass on top of ours keeps it).
void Unsubclass()
{
    HWND p; WNDPROC orig;
    { Locked lk; p = g_parent; orig = g_origProc; g_parent = NULL; g_origProc = NULL; }
    if (!p || !orig || !IsWindow(p)) return;
    if ((WNDPROC)GetWindowLongPtrW(p, GWLP_WNDPROC) == ParentProc) { SetWindowLongPtrW(p, GWLP_WNDPROC, (LONG_PTR)orig); FgLog("wnd: restored WndProc of %p", (void*)p); }
    else FgLog("wnd: %p subclassed on top of us - WndProc left alone", (void*)p);
}

} // namespace

HWND FgWndCreate(HWND parent)
{
    if (Parent() && Parent() != parent) { FgWndDestroy(); Unsubclass(); }
    { Locked lk; g_parent = parent; }
    WNDPROC orig; { Locked lk; orig = g_origProc; }
    if (!orig) {
        orig = (WNDPROC)SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)ParentProc);
        if (!orig) { g_probe.createErr = GetLastError(); FgLog("wnd: subclass failed (%lu)", g_probe.createErr); Locked lk; g_parent = NULL; return NULL; }
        { Locked lk; g_origProc = orig; }
        FgLog("wnd: subclassed %p (orig %p) from tid %u, window tid %u", (void*)parent, (void*)orig,
              (unsigned)GetCurrentThreadId(), (unsigned)GetWindowThreadProcessId(parent, NULL));
    }
    LRESULT h = 0;
    if (!Send(WM_RF_CREATE, 3000, &h)) { FgLog("wnd: create message timed out"); return NULL; }
    FgLog("wnd: child %p (err %lu)", (void*)h, g_probe.createErr);
    return (HWND)h;
}

void FgWndDestroy(void)
{
    HWND c, p;
    { Locked lk; c = g_child; p = g_parent; g_child = NULL; }
    if (!c || !p) return;
    ShowWindowAsync(c, SW_HIDE);                            // safe from any thread; the destroy runs on the UI thread
    if (!PostMessageW(p, WM_RF_DESTROY, (WPARAM)c, 0)) {
        FgLog("wnd: WM_RF_DESTROY post failed (%lu) - child %p kept for the next destroy", GetLastError(), (void*)c);
        Locked lk; if (!g_child) g_child = c;               // still ours: retried by the next FgWndDestroy
    }
}

HWND FgWndChild(void) { return Child(); }

bool FgWndIsOurs(HWND h)
{
    wchar_t cls[64] = {};
    return h && GetClassNameW(h, cls, 64) && wcscmp(cls, kClass) == 0;
}

// Plugin unload, main thread. The window's own thread is the main thread in a Unity player, so the child can go
// synchronously; otherwise the bounded Send does the same from here.
void FgWndUnload(void)
{
    HWND c, p;
    { Locked lk; c = g_child; p = g_parent; g_child = NULL; }
    if (c && p && IsWindow(c)) {
        if (GetWindowThreadProcessId(p, NULL) == GetCurrentThreadId()) DestroyWindow(c);
        else { DWORD_PTR r = 0; SendMessageTimeoutW(p, WM_RF_DESTROY, (WPARAM)c, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r); }
    }
    Unsubclass();
    UnregisterClassW(kClass, GetModuleHandleW(NULL));       // fails harmlessly while a window of the class still exists
}

const FgWndProbe* FgWndProbeNow(void)
{
    bool live; { Locked lk; live = g_parent && g_origProc; }
    if (live) Send(WM_RF_PROBE, 200, NULL);
    g_probe.parent = Parent();
    return &g_probe;
}
