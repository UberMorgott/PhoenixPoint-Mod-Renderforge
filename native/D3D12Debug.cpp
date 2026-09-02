// D3D12Debug.cpp - see D3D12Debug.h. No allocation on the hot path, no exceptions, best effort throughout.
#include "D3D12Debug.h"

#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include <stdlib.h>

#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"

namespace RfDbg {

static int g_on = -1;
static FILE* g_f = NULL;
static ID3D12InfoQueue* g_iq = NULL;
static int g_attached = 0;
static HRESULT g_lastRemoved = S_OK;

bool On()
{
    if (g_on < 0) {
        char v[8] = {};
        DWORD n = GetEnvironmentVariableA("RENDERFORGE_D3D12_DEBUG", v, sizeof(v));
        g_on = (n > 0 && v[0] == '1') ? 1 : 0;
        if (g_on) {
            wchar_t dir[MAX_PATH] = {}, path[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, dir);
            _snwprintf_s(path, _TRUNCATE, L"%srenderforge-d3d12.log", dir);
            g_f = _wfsopen(path, L"w", _SH_DENYNO);   // readable while the game holds it open
        }
    }
    return g_on == 1;
}

bool NoEvents()
{
    static int n = -1;
    if (n < 0) {
        char v[8] = {};
        DWORD c = GetEnvironmentVariableA("RENDERFORGE_D3D12_NOEVENTS", v, sizeof(v));
        n = (c > 0 && v[0] == '1') ? 1 : 0;
    }
    return n == 1;
}

void Log(const char* fmt, ...)
{
    if (!On() || !g_f) return;
    va_list ap; va_start(ap, fmt);
    fprintf(g_f, "[%8u] ", GetTickCount());
    vfprintf(g_f, fmt, ap);
    fputc('\n', g_f);
    va_end(ap);
    fflush(g_f);
}

void EarlyEnable()
{
    if (!On()) return;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    typedef HRESULT(WINAPI * PFN_GET)(REFIID, void**);
    PFN_GET get = d3d12 ? (PFN_GET)GetProcAddress(d3d12, "D3D12GetDebugInterface") : NULL;
    if (!get) { Log("EarlyEnable: D3D12GetDebugInterface not found"); return; }

    ID3D12Debug* dbg = NULL;
    HRESULT hr = get(__uuidof(ID3D12Debug), (void**)&dbg);
    if (SUCCEEDED(hr) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); Log("EarlyEnable: EnableDebugLayer() called"); }
    else Log("EarlyEnable: ID3D12Debug hr=0x%08X", (unsigned)hr);

#ifdef __ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__
    ID3D12DeviceRemovedExtendedDataSettings* dred = NULL;
    hr = get(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void**)&dred);
    if (SUCCEEDED(hr) && dred) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
        Log("EarlyEnable: DRED breadcrumbs + page fault FORCED_ON");
    } else Log("EarlyEnable: DRED settings hr=0x%08X", (unsigned)hr);
#endif
}

void Attach(ID3D12Device* dev)
{
    if (!On() || g_attached || !dev) return;
    g_attached = 1;
    HRESULT hr = dev->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_iq);
    Log("Attach: device=%p InfoQueue hr=0x%08X (%s; without Unity's -force-d3d12-debug the debug layer is off)",
        (void*)dev, (unsigned)hr, g_iq ? "available" : "unavailable");
    if (g_iq) g_iq->SetMuteDebugOutput(FALSE);
}

void Drain()
{
    if (!On() || !g_iq) return;
    UINT64 n = g_iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_iq->GetMessage(i, NULL, &len)) || !len) continue;
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (!m) continue;
        if (SUCCEEDED(g_iq->GetMessage(i, m, &len)))
            Log("D3D12 %s cat=%d id=%d: %s",
                m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" :
                m->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARNING" :
                m->Severity == D3D12_MESSAGE_SEVERITY_INFO ? "INFO" : "MESSAGE",
                (int)m->Category, (int)m->ID, m->pDescription ? m->pDescription : "");
        free(m);
    }
    if (n) g_iq->ClearStoredMessages();
}

void Removed(ID3D12Device* dev, const char* where)
{
    if (!On() || !dev) return;
    HRESULT hr = dev->GetDeviceRemovedReason();
    if (hr == S_OK || hr == g_lastRemoved) return;
    g_lastRemoved = hr;
    Log("DEVICE REMOVED at %s: reason 0x%08X", where, (unsigned)hr);
    Drain();
#ifdef __ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__
    ID3D12DeviceRemovedExtendedData* dred = NULL;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), (void**)&dred)) || !dred) {
        Log("DRED: unavailable (not armed before device creation)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* nd = bc.pHeadAutoBreadcrumbNode; nd; nd = nd->pNext) {
            UINT done = nd->pLastBreadcrumbValue ? *nd->pLastBreadcrumbValue : 0;
            Log("DRED node cmdlist=%S queue=%S ops=%u lastCompleted=%u",
                nd->pCommandListDebugNameW ? nd->pCommandListDebugNameW : L"?",
                nd->pCommandQueueDebugNameW ? nd->pCommandQueueDebugNameW : L"?",
                nd->BreadcrumbCount, done);
            for (UINT i = done; i < nd->BreadcrumbCount && i < done + 8; ++i)
                Log("DRED   op[%u] = %d", i, (int)nd->pCommandHistory[i]);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
        Log("DRED page fault VA = 0x%llX", (unsigned long long)pf.PageFaultVA);
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
            Log("DRED   existing alloc '%S' type=%d", a->ObjectNameW ? a->ObjectNameW : L"?", (int)a->AllocationType);
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            Log("DRED   freed alloc '%S' type=%d", a->ObjectNameW ? a->ObjectNameW : L"?", (int)a->AllocationType);
    }
    dred->Release();
#endif
}

void Resource(const char* tag, ID3D12Resource* r)
{
    if (!On()) return;
    if (!r) { Log("res %-6s = NULL", tag); return; }
    D3D12_RESOURCE_DESC d = r->GetDesc();
    D3D12_HEAP_PROPERTIES hp = {}; D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    r->GetHeapProperties(&hp, &hf);
    Log("res %-6s %p %llux%u mips=%u fmt=%d flags=0x%X dim=%d samples=%u heap=%d",
        tag, (void*)r, (unsigned long long)d.Width, d.Height, d.MipLevels,
        (int)d.Format, (unsigned)d.Flags, (int)d.Dimension, d.SampleDesc.Count, (int)hp.Type);
}

void States(int n, const UnityGraphicsD3D12ResourceState* st)
{
    if (!On() || !st) return;
    for (int i = 0; i < n; ++i)
        Log("state[%d] res=%p expected=0x%X current=0x%X", i, (void*)st[i].resource,
            (unsigned)st[i].expected, (unsigned)st[i].current);
}

} // namespace RfDbg
