// FgWnd.cpp - an HWND of our own inside the game window. DXGI refuses a second CreateSwapChainForHwnd on the
// window Unity's flip-model chain already owns (E_ACCESSDENIED, measured), and every vendor FG SDK creates its
// swapchain on an HWND, so the host owns a WS_CHILD window that covers the parent's client area and creates the
// shadow chain there. Same thread as the parent (created from inside a subclassed WndProc), so WM_NCHITTEST ->
// HTTRANSPARENT hands every mouse message to Unity and keyboard focus never moves.
//
// The subclass stays installed for the process lifetime, like the vtable patch (RenderforgeNative.cpp:329):
// restoring GWLP_WNDPROC after someone else subclassed on top of us would cut their chain. Destroying the child
// must happen on the window's thread, so teardown posts a message; the render thread only hides it.
#include "Fg.h"

namespace {

const UINT WM_RF_CREATE  = WM_APP + 0x51;   // wParam = 0; lResult = child HWND
const UINT WM_RF_DESTROY = WM_APP + 0x52;
const UINT WM_RF_PROBE   = WM_APP + 0x53;   // samples hit-test + focus on the UI thread

const wchar_t kClass[] = L"RenderforgeFgWnd";

HWND    g_parent = NULL;
HWND    g_child = NULL;
WNDPROC g_origProc = NULL;
FgWndProbe g_probe = {};

LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:     return HTTRANSPARENT;          // mouse goes to the parent (same thread)
    case WM_ERASEBKGND:    return 1;
    case WM_PAINT:         ValidateRect(h, NULL); return 0;
    case WM_SETFOCUS:      if (g_parent) SetFocus(g_parent); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY:       if (h == g_child) g_child = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void FitChild()
{
    if (!g_child || !g_parent) return;
    RECT r; GetClientRect(g_parent, &r);
    SetWindowPos(g_child, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_RF_CREATE: {
        if (g_child) return (LRESULT)g_child;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);                              // duplicate registration is harmless
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_CLIPCHILDREN);
        RECT r; GetClientRect(h, &r);
        g_child = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kClass, kClass, WS_CHILD | WS_VISIBLE,
                                  0, 0, r.right - r.left, r.bottom - r.top, h, NULL, wc.hInstance, NULL);
        g_probe.createErr = g_child ? 0 : GetLastError();
        FitChild();
        return (LRESULT)g_child;
    }
    case WM_RF_DESTROY:                                      // wParam = the exact child to destroy (a newer one may exist)
        if (w) DestroyWindow((HWND)w);
        return 0;
    case WM_RF_PROBE: {
        RECT r; GetClientRect(h, &r);
        POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
        ClientToScreen(h, &c);
        g_probe.hit = g_child ? (int)SendMessageW(g_child, WM_NCHITTEST, 0, MAKELPARAM(c.x, c.y)) : 0;
        g_probe.focus = GetFocus();
        g_probe.foreground = GetForegroundWindow();
        g_probe.child = g_child;
        return 0;
    }
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
        FitChild();
        break;
    }
    return CallWindowProcW(g_origProc, h, m, w, l);
}

// Bounded cross-thread SendMessage: direct call on the window's own thread, else waits at most `ms`.
bool Send(UINT m, DWORD ms, LRESULT* out)
{
    DWORD_PTR res = 0;
    if (!g_parent) return false;
    LRESULT ok = SendMessageTimeoutW(g_parent, m, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &res);
    if (out) *out = (LRESULT)res;
    return ok != 0;
}

} // namespace

HWND FgWndCreate(HWND parent)
{
    if (g_parent && g_parent != parent) { FgWndDestroy(); g_origProc = NULL; }
    g_parent = parent;
    if (!g_origProc) {
        g_origProc = (WNDPROC)SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)ParentProc);
        if (!g_origProc) { g_probe.createErr = GetLastError(); FgLog("wnd: subclass failed (%lu)", g_probe.createErr); g_parent = NULL; return NULL; }
        FgLog("wnd: subclassed %p (orig %p) from tid %u, window tid %u", (void*)parent, (void*)g_origProc,
              (unsigned)GetCurrentThreadId(), (unsigned)GetWindowThreadProcessId(parent, NULL));
    }
    LRESULT h = 0;
    if (!Send(WM_RF_CREATE, 3000, &h)) { FgLog("wnd: create message timed out"); return NULL; }
    FgLog("wnd: child %p (err %lu)", (void*)h, g_probe.createErr);
    return (HWND)h;
}

void FgWndDestroy(void)
{
    HWND c = g_child;
    if (!c || !g_parent) return;
    g_child = NULL;
    ShowWindowAsync(c, SW_HIDE);                            // safe from any thread; the destroy runs on the UI thread
    PostMessageW(g_parent, WM_RF_DESTROY, (WPARAM)c, 0);
}

const FgWndProbe* FgWndProbeNow(void)
{
    if (g_parent && g_origProc) Send(WM_RF_PROBE, 200, NULL);
    g_probe.parent = g_parent;
    return &g_probe;
}
