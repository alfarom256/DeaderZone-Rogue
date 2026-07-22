#include "hooks.h"
#include "stats_overlay.h"
#include "damage_tracker.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <imgui_impl_dx12.h>
#include <MinHook.h>
#include <vector>

extern void Log(const char* fmt, ...);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── Minimal DirectComposition COM interfaces (avoids dcomp.h dependency) ──
MIDL_INTERFACE("C37EA93A-E7AA-450D-B16F-9746CB0407F3")
IDCompositionDevice_ : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Commit() = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForCommitCompletion() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTargetForHwnd(HWND hwnd, BOOL topmost, IUnknown** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVisual(IUnknown** visual) = 0;
};

MIDL_INTERFACE("eacdd04c-117e-4e17-88f4-d1b12b0e3d89")
IDCompositionTarget_ : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetRoot(IUnknown* visual) = 0;
};

MIDL_INTERFACE("4d93059d-097b-4651-9a60-f0f25116e2f2")
IDCompositionVisual_ : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOffsetX_f(float) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetX_a(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetY_f(float) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetY_a(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform_m(const void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform_t(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransformParent(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEffect(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBitmapInterpolationMode(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBorderMode(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClip_r(const void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClip_c(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetContent(IUnknown* content) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddVisual(IUnknown*, BOOL, IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveVisual(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAllVisuals() = 0;
};

static const GUID IID_IDCompositionDevice_ =
    {0xC37EA93A, 0xE7AA, 0x450D, {0xB1,0x6F,0x97,0x46,0xCB,0x04,0x07,0xF3}};

namespace Hooks {

static ID3D11Device*           g_d3dDevice   = nullptr;
static ID3D11DeviceContext*    g_d3dContext  = nullptr;
static IDXGISwapChain1*        g_swapChain   = nullptr;
static ID3D11RenderTargetView* g_rtv         = nullptr;
static ID3D11BlendState*       g_blendState  = nullptr;

static HWND     g_gameWindow    = nullptr;
static HWND     g_overlayWindow = nullptr;
static WNDPROC  g_origWndProc   = nullptr;
static bool     g_running       = false;
static bool     g_useDComp      = false;

// DComp objects (kept alive for app lifetime)
static IDCompositionDevice_* g_dcompDevice = nullptr;
static IUnknown*             g_dcompTarget = nullptr;
static IUnknown*             g_dcompVisual = nullptr;

// ── WndProc hook on GAME window (for input interception) ────────────
static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_menuOpen && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 0;

    if (g_menuOpen) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:  case WM_MOUSEMOVE:
        case WM_KEYDOWN:     case WM_KEYUP:
        case WM_SYSKEYDOWN:  case WM_SYSKEYUP:
        case WM_CHAR:
            return 0;
        }
    }

    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// ── Overlay window proc ────────────────────────────────────────────
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        g_running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool CreateOverlayWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DZR_Overlay";
    RegisterClassExW(&wc);

    RECT gameRect;
    GetWindowRect(g_gameWindow, &gameRect);
    int w = gameRect.right - gameRect.left;
    int h = gameRect.bottom - gameRect.top;

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"DZR_Overlay", L"",
        WS_POPUP,
        gameRect.left, gameRect.top, w, h,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_overlayWindow) {
        Log("[OVERLAY] CreateWindowExW failed: %d\n", GetLastError());
        return false;
    }

    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    Log("[OVERLAY] Window created: %dx%d (NOREDIRECTIONBITMAP)\n", w, h);
    return true;
}

static bool InitD3D11() {
    RECT rect;
    GetClientRect(g_overlayWindow, &rect);
    if (rect.right == 0 || rect.bottom == 0) {
        GetWindowRect(g_overlayWindow, &rect);
        rect.right -= rect.left; rect.bottom -= rect.top;
        rect.left = 0; rect.top = 0;
    }

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &g_d3dDevice, &featureLevel, &g_d3dContext);

    if (FAILED(hr)) {
        Log("[OVERLAY] D3D11CreateDevice failed: 0x%08X\n", hr);
        return false;
    }

    // Get DXGI factory
    IDXGIDevice* dxgiDevice = nullptr;
    g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();

    // Create swap chain with premultiplied alpha (for DirectComposition)
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = rect.right;
    scd.Height = rect.bottom;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Flags = 0;

    hr = factory->CreateSwapChainForComposition(g_d3dDevice, &scd, nullptr, &g_swapChain);
    if (SUCCEEDED(hr)) {
        Log("[OVERLAY] SwapChain created (PREMULTIPLIED alpha, %dx%d)\n", scd.Width, scd.Height);
        g_useDComp = true;
    } else {
        Log("[OVERLAY] CreateSwapChainForComposition failed: 0x%08X, trying HWND fallback\n", hr);
        scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        hr = factory->CreateSwapChainForHwnd(g_d3dDevice, g_overlayWindow, &scd, nullptr, nullptr, &g_swapChain);
        if (FAILED(hr)) {
            Log("[OVERLAY] CreateSwapChainForHwnd failed: 0x%08X\n", hr);
            factory->Release();
            dxgiDevice->Release();
            return false;
        }
        g_useDComp = false;
        Log("[OVERLAY] SwapChain created via HWND fallback\n");
    }
    factory->Release();

    // Set up DirectComposition to bind swap chain to window
    if (g_useDComp) {
        typedef HRESULT(WINAPI* DCompositionCreateDevice_t)(IDXGIDevice*, REFIID, void**);
        HMODULE dcomp = LoadLibraryA("dcomp.dll");
        if (!dcomp) {
            Log("[OVERLAY] Failed to load dcomp.dll\n");
            dxgiDevice->Release();
            return false;
        }

        auto pCreateDevice = (DCompositionCreateDevice_t)GetProcAddress(dcomp, "DCompositionCreateDevice");
        if (!pCreateDevice) {
            Log("[OVERLAY] DCompositionCreateDevice not found\n");
            dxgiDevice->Release();
            return false;
        }

        hr = pCreateDevice(dxgiDevice, IID_IDCompositionDevice_, (void**)&g_dcompDevice);
        if (FAILED(hr) || !g_dcompDevice) {
            Log("[OVERLAY] DCompositionCreateDevice failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        hr = g_dcompDevice->CreateTargetForHwnd(g_overlayWindow, TRUE, &g_dcompTarget);
        if (FAILED(hr)) {
            Log("[OVERLAY] CreateTargetForHwnd failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
        if (FAILED(hr)) {
            Log("[OVERLAY] CreateVisual failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        auto* visual = (IDCompositionVisual_*)g_dcompVisual;
        hr = visual->SetContent((IUnknown*)g_swapChain);
        if (FAILED(hr)) {
            Log("[OVERLAY] SetContent failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        auto* target = (IDCompositionTarget_*)g_dcompTarget;
        hr = target->SetRoot(g_dcompVisual);
        if (FAILED(hr)) {
            Log("[OVERLAY] SetRoot failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        hr = g_dcompDevice->Commit();
        if (FAILED(hr)) {
            Log("[OVERLAY] DComp Commit failed: 0x%08X\n", hr);
            dxgiDevice->Release();
            return false;
        }

        Log("[OVERLAY] DirectComposition bound successfully\n");
    }
    dxgiDevice->Release();

    // Create render target view
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    g_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();

    // Create blend state for premultiplied alpha rendering
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_d3dDevice->CreateBlendState(&blendDesc, &g_blendState);

    Log("[OVERLAY] D3D11 initialized (feature level 0x%X)\n", featureLevel);
    return true;
}

// ═══════════════ Game-frame HUD via IDXGISwapChain::Present hook ═══════════════
// Renders the always-visible HUD (stats + damage) into the GAME'S OWN back buffer
// when the menu is closed — no separate window during gameplay, so it cannot touch
// the game's input. A second ImGui context is bound to the game's D3D device. When
// the menu is OPEN the HUD is drawn by the RenderLoop onto the overlay window
// instead, so the two contexts are never used concurrently (gated by g_menuOpen).
static ImGuiContext* g_menuCtx = nullptr;
static ImGuiContext* g_hudCtx  = nullptr;
static bool          g_hudReady = false;

// ── D3D12 state for the game-frame HUD ──
static ID3D12Device*              g_d12Device  = nullptr;
static ID3D12CommandQueue*        g_d12Queue   = nullptr;  // captured via ExecuteCommandLists (game's)
static ID3D12CommandQueue*        g_d12UploadQueue = nullptr;  // our own, for ImGui font upload only
static ID3D12DescriptorHeap*      g_d12SrvHeap = nullptr;
static ID3D12DescriptorHeap*      g_d12RtvHeap = nullptr;
static ID3D12GraphicsCommandList* g_d12CmdList = nullptr;
static UINT                       g_d12BufCount = 0;
struct D12Frame { ID3D12CommandAllocator* alloc; ID3D12Resource* res; D3D12_CPU_DESCRIPTOR_HANDLE rtv; UINT64 fenceVal; };
static std::vector<D12Frame>      g_d12Frames;
static ID3D12Fence*               g_d12Fence  = nullptr;
static HANDLE                     g_d12FenceEvent = nullptr;
static UINT64                     g_d12FenceVal = 0;

// Minimal SRV-descriptor allocator over g_d12SrvHeap (ImGui 1.92 backend needs one).
static UINT                       g_srvIncr = 0, g_srvNext = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_srvCpu0 = {};
static D3D12_GPU_DESCRIPTOR_HANDLE g_srvGpu0 = {};
static std::vector<UINT>          g_srvFreed;
static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    UINT i;
    if (!g_srvFreed.empty()) { i = g_srvFreed.back(); g_srvFreed.pop_back(); }
    else i = g_srvNext++;
    cpu->ptr = g_srvCpu0.ptr + (SIZE_T)i * g_srvIncr;
    gpu->ptr = g_srvGpu0.ptr + (UINT64)i * g_srvIncr;
}
static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    g_srvFreed.push_back((UINT)((cpu.ptr - g_srvCpu0.ptr) / g_srvIncr));
}

// ExecuteCommandLists hook — captures the game's DIRECT command queue (ImGui needs it).
typedef void(__stdcall* ExecFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static ExecFn g_origExec = nullptr;
static ID3D12CommandQueue* g_lastDirectQueue = nullptr;
static bool                g_queueLocked = false;
static void __stdcall HookedExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
    if (q) {
        D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            // Log each distinct DIRECT queue ONCE — two alternate every frame, so logging
            // on every change floods the log and thrashes disk I/O (a stutter source).
            static ID3D12CommandQueue* s_seen[8] = {}; static int s_nSeen = 0;
            bool known = false;
            for (int i = 0; i < s_nSeen; i++) if (s_seen[i] == q) { known = true; break; }
            if (!known && s_nSeen < 8) { s_seen[s_nSeen++] = q; Log("[HUD] DIRECT queue seen: %p\n", (void*)q); }
            g_lastDirectQueue = q;
            if (!g_d12Queue) { g_d12Queue = q; Log("[HUD] captured D3D12 direct queue %p\n", (void*)q); }
        }
    }
    g_origExec(q, n, l);
}

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(__stdcall* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn  g_origPresent  = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeFn   g_origResize   = nullptr;

static void InitGameHud(IDXGISwapChain* sc) {
    if (!g_d12Device && FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&g_d12Device))) { g_d12Device = nullptr; return; }
    if (!g_d12Device || !g_d12Queue) return;   // wait until the queue is captured
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || d.BufferCount == 0 || d.BufferCount > 8) return;
    g_d12BufCount = d.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 64;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_d12Device->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&g_d12SrvHeap))) return;

    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = g_d12BufCount;
    if (FAILED(g_d12Device->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_d12RtvHeap))) return;

    UINT rtvSz = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_d12Frames.assign(g_d12BufCount, {});
    for (UINT i = 0; i < g_d12BufCount; i++) {
        if (FAILED(g_d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), (void**)&g_d12Frames[i].alloc))) return;
        ID3D12Resource* bb = nullptr;
        if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&bb)) || !bb) return;
        g_d12Device->CreateRenderTargetView(bb, nullptr, rtvH);
        g_d12Frames[i].res = bb;
        g_d12Frames[i].rtv = rtvH;
        rtvH.ptr += rtvSz;
    }
    if (FAILED(g_d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d12Frames[0].alloc,
            nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_d12CmdList))) return;
    g_d12CmdList->Close();

    if (FAILED(g_d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_d12Fence))) return;
    g_d12FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_d12FenceEvent) return;
    g_d12FenceVal = 0;

    g_srvIncr = g_d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srvCpu0 = g_d12SrvHeap->GetCPUDescriptorHandleForHeapStart();
    g_srvGpu0 = g_d12SrvHeap->GetGPUDescriptorHandleForHeapStart();
    g_srvNext = 0; g_srvFreed.clear();

    // Dedicated queue for ImGui's one-time font-atlas upload. The backend submits
    // a command list to this queue and blocks on it with an INFINITE wait — doing
    // that on the GAME'S queue (from inside Present) causes a TDR / device-removal.
    D3D12_COMMAND_QUEUE_DESC uq = {};
    uq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_d12Device->CreateCommandQueue(&uq, __uuidof(ID3D12CommandQueue), (void**)&g_d12UploadQueue))) return;

    g_hudCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_hudCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr; io.FontGlobalScale = 1.5f;
    ImGui::StyleColorsDark();
    ImGui_ImplDX12_InitInfo ii = {};
    ii.Device = g_d12Device;
    // g_d12Queue is the present queue here (HookedPresent set it before init ran), so
    // the one-time font upload runs on the SAME queue as the draw — no cross-queue.
    ii.CommandQueue = g_d12Queue;
    ii.NumFramesInFlight = (int)g_d12BufCount;
    ii.RTVFormat = d.BufferDesc.Format;
    ii.SrvDescriptorHeap = g_d12SrvHeap;
    ii.SrvDescriptorAllocFn = SrvAlloc;
    ii.SrvDescriptorFreeFn  = SrvFree;
    if (!ImGui_ImplDX12_Init(&ii)) { Log("[HUD] ImGui_ImplDX12_Init failed\n"); return; }
    g_hudReady = true;
    Log("[HUD] D3D12 HUD initialized (%u buffers, fmt %d)\n", g_d12BufCount, (int)d.BufferDesc.Format);
}

// Structural D3D12 path confirmed working (green clear rendered without crashing on
// the present queue). Now render the real ImGui HUD instead of the diagnostic clear.
static bool g_hudClearOnly = false;

static void RenderHud(IDXGISwapChain* sc) {
    if (!g_hudReady) { InitGameHud(sc); return; }

    // Report the actual device-removed reason once (HUNG=sync/TDR vs REMOVED=bad access).
    HRESULT rr = g_d12Device->GetDeviceRemovedReason();
    if (rr != S_OK) {
        static bool s_l = false;
        if (!s_l) { s_l = true; Log("[HUD] DeviceRemovedReason = 0x%08X\n", rr); }
        return;
    }
    if (g_menuOpen) return;

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return;
    UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    if (idx >= g_d12Frames.size() || !g_d12Frames[idx].alloc) return;
    D12Frame& f = g_d12Frames[idx];

    if (!g_hudClearOnly) {
        ImGui::SetCurrentContext(g_hudCtx);
        ImGuiIO& io = ImGui::GetIO();
        DXGI_SWAP_CHAIN_DESC d = {};
        if (SUCCEEDED(sc->GetDesc(&d)) && d.BufferDesc.Width)
            io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        StatsOverlay::Render();
        DamageTracker::Render();
        ImGui::Render();
    }

    if (g_d12Fence->GetCompletedValue() < f.fenceVal) {
        g_d12Fence->SetEventOnCompletion(f.fenceVal, g_d12FenceEvent);
        WaitForSingleObject(g_d12FenceEvent, INFINITE);
    }
    f.alloc->Reset();
    g_d12CmdList->Reset(f.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = f.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_d12CmdList->ResourceBarrier(1, &b);
    g_d12CmdList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    if (g_hudClearOnly) {
        const float col[4] = { 0.0f, 1.0f, 0.0f, 0.25f };   // green tint = structure OK
        g_d12CmdList->ClearRenderTargetView(f.rtv, col, 0, nullptr);
    } else {
        g_d12CmdList->SetDescriptorHeaps(1, &g_d12SrvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_d12CmdList);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_d12CmdList->ResourceBarrier(1, &b);
    g_d12CmdList->Close();
    ID3D12CommandList* lists[] = { g_d12CmdList };
    g_d12Queue->ExecuteCommandLists(1, lists);
    g_d12Queue->Signal(g_d12Fence, ++g_d12FenceVal);
    f.fenceVal = g_d12FenceVal;
}

static void ReleaseHudD12() {
    // Wait for the GPU to finish any in-flight HUD work before tearing down.
    if (g_d12Fence && g_d12Queue) {
        g_d12Queue->Signal(g_d12Fence, ++g_d12FenceVal);
        if (g_d12Fence->GetCompletedValue() < g_d12FenceVal) {
            g_d12Fence->SetEventOnCompletion(g_d12FenceVal, g_d12FenceEvent);
            WaitForSingleObject(g_d12FenceEvent, INFINITE);
        }
    }
    if (g_hudCtx) { ImGui::SetCurrentContext(g_hudCtx); ImGui_ImplDX12_Shutdown(); }
    for (auto& f : g_d12Frames) { if (f.res) f.res->Release(); if (f.alloc) f.alloc->Release(); }
    g_d12Frames.clear();
    if (g_d12UploadQueue) { g_d12UploadQueue->Release(); g_d12UploadQueue = nullptr; }
    if (g_d12Fence) { g_d12Fence->Release(); g_d12Fence = nullptr; }
    if (g_d12FenceEvent) { CloseHandle(g_d12FenceEvent); g_d12FenceEvent = nullptr; }
    if (g_d12CmdList) { g_d12CmdList->Release(); g_d12CmdList = nullptr; }
    if (g_d12RtvHeap) { g_d12RtvHeap->Release(); g_d12RtvHeap = nullptr; }
    if (g_d12SrvHeap) { g_d12SrvHeap->Release(); g_d12SrvHeap = nullptr; }
    if (g_hudCtx) { ImGui::DestroyContext(g_hudCtx); g_hudCtx = nullptr; }
    g_hudReady = false;
}

static HRESULT __stdcall HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    static bool s_l = false; if (!s_l) { s_l = true; Log("[HUD] HookedPresent FIRED\n"); }
    // Lock the present queue ONCE. There are two DIRECT queues; switching per-frame
    // occasionally submits the back-buffer draw on the wrong one -> unsynchronized
    // hazard -> eventual GPU device removal. Lock the first stable one.
    if (!g_queueLocked && g_lastDirectQueue) { g_d12Queue = g_lastDirectQueue; g_queueLocked = true; }
    RenderHud(sc);
    return g_origPresent(sc, sync, flags);
}

static HRESULT __stdcall HookedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* pp) {
    static bool s_l = false; if (!s_l) { s_l = true; Log("[HUD] HookedPresent1 FIRED\n"); }
    // Lock the present queue ONCE. There are two DIRECT queues; switching per-frame
    // occasionally submits the back-buffer draw on the wrong one -> unsynchronized
    // hazard -> eventual GPU device removal. Lock the first stable one.
    if (!g_queueLocked && g_lastDirectQueue) { g_d12Queue = g_lastDirectQueue; g_queueLocked = true; }
    RenderHud((IDXGISwapChain*)sc);
    return g_origPresent1(sc, sync, flags, pp);
}

static HRESULT __stdcall HookedResize(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    // Must free all references to the back buffers before ResizeBuffers; the HUD
    // re-initializes lazily on the next present.
    if (g_hudReady) ReleaseHudD12();
    return g_origResize(sc, bc, w, h, fmt, flags);
}

static void HookGamePresent() {
    // Dummy swapchain to read the shared IDXGISwapChain vtable (Present=8, ResizeBuffers=13).
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DZR_Dummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, L"DZR_Dummy", L"", WS_OVERLAPPED, 0, 0, 8, 8,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummy;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        Log("[HUD] dummy swapchain failed 0x%08X\n", hr);
        if (dummy) DestroyWindow(dummy);
        return;
    }
    void** vtbl = *(void***)sc;
    void* presentAddr = vtbl[8];
    void* resizeAddr  = vtbl[13];
    // Flip-model games (UE5) present via IDXGISwapChain1::Present1 (vtbl index 22).
    void* present1Addr = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
        present1Addr = (*(void***)sc1)[22];
        sc1->Release();
    }
    sc->Release(); if (ctx) ctx->Release(); if (dev) dev->Release();
    if (dummy) DestroyWindow(dummy);

    MH_Initialize();  // idempotent if already initialized

    // Capture the game's D3D12 command queue by hooking ExecuteCommandLists (vtbl 10).
    HMODULE d12mod = GetModuleHandleW(L"d3d12.dll");
    if (d12mod) {
        typedef HRESULT(WINAPI* CreateDevFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        auto pCreate = (CreateDevFn)GetProcAddress(d12mod, "D3D12CreateDevice");
        ID3D12Device* dd = nullptr;
        if (pCreate && SUCCEEDED(pCreate(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dd)) && dd) {
            D3D12_COMMAND_QUEUE_DESC qd = {};
            ID3D12CommandQueue* dq = nullptr;
            if (SUCCEEDED(dd->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&dq)) && dq) {
                void* execAddr = (*(void***)dq)[10];
                dq->Release();
                if (MH_CreateHook(execAddr, (LPVOID)&HookedExec, (LPVOID*)&g_origExec) == MH_OK &&
                    MH_EnableHook(execAddr) == MH_OK)
                    Log("[HUD] ExecuteCommandLists hooked at %p\n", execAddr);
                else
                    Log("[HUD] ExecuteCommandLists hook FAILED\n");
            }
            dd->Release();
        }
    }

    if (MH_CreateHook(presentAddr, (LPVOID)&HookedPresent, (LPVOID*)&g_origPresent) == MH_OK &&
        MH_EnableHook(presentAddr) == MH_OK)
        Log("[HUD] Present hooked at %p\n", presentAddr);
    else
        Log("[HUD] Present hook FAILED\n");
    if (present1Addr &&
        MH_CreateHook(present1Addr, (LPVOID)&HookedPresent1, (LPVOID*)&g_origPresent1) == MH_OK &&
        MH_EnableHook(present1Addr) == MH_OK)
        Log("[HUD] Present1 hooked at %p\n", present1Addr);
    else
        Log("[HUD] Present1 hook not installed\n");
    if (MH_CreateHook(resizeAddr, (LPVOID)&HookedResize, (LPVOID*)&g_origResize) == MH_OK &&
        MH_EnableHook(resizeAddr) == MH_OK)
        Log("[HUD] ResizeBuffers hooked\n");
}

void Init(HWND gameWindow) {
    g_gameWindow = gameWindow;

    if (!CreateOverlayWindow()) {
        Log("[OVERLAY] Failed to create overlay window\n");
        return;
    }

    if (!InitD3D11()) {
        Log("[OVERLAY] Failed to init D3D11\n");
        return;
    }

    IMGUI_CHECKVERSION();
    g_menuCtx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.5f);
    io.FontGlobalScale = 1.5f;

    ImGui_ImplWin32_Init(g_gameWindow);
    ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);

    StatsOverlay::Init();

    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

    // Hook the game's swapchain Present to draw the always-visible HUD into the
    // game's own frame (no separate window during gameplay).
    HookGamePresent();

    g_running = true;
    Log("[OVERLAY] Fully initialized, F10 to toggle menu\n");
}

void RenderLoop() {
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, g_overlayWindow, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!IsWindow(g_gameWindow)) break;

        // Read-only, always-on HUD: stats + damage are drawn into the GAME's own frame
        // by the Present hook (RenderHud). There is no menu anymore, so no F10 toggle and
        // no separate DX11 overlay-window render path. That path only existed for the
        // removed cheat menu, and its DX11<->D3D12 handoff hung the GPU on toggle
        // (DXGI_ERROR_HUNG 0x887A002B). g_menuOpen stays false so RenderHud always draws;
        // the overlay window stays hidden.
        ShowWindow(g_overlayWindow, SW_HIDE);

        // Heavy data polling (ProcessEvent + object scans) runs HERE on the mod thread,
        // NOT inside RenderHud on the game's render thread — doing it there hitched the
        // game. Render() (in the Present hook) only draws the cached values.
        StatsOverlay::Poll();

        Sleep(1);
    }
}

void Shutdown() {
    g_running = false;

    if (g_gameWindow && g_origWndProc) {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_blendState) g_blendState->Release();
    if (g_rtv) g_rtv->Release();
    if (g_swapChain) g_swapChain->Release();
    if (g_d3dContext) g_d3dContext->Release();
    if (g_d3dDevice) g_d3dDevice->Release();

    if (g_dcompVisual) g_dcompVisual->Release();
    if (g_dcompTarget) g_dcompTarget->Release();
    if (g_dcompDevice) g_dcompDevice->Release();

    if (g_overlayWindow) DestroyWindow(g_overlayWindow);
}

} // namespace Hooks
