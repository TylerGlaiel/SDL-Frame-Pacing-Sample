#include "DXGISwapChainAdapter.h"
#include <SDL3/SDL_opengl.h>
#include <cstring>
#include <cassert>
#include <comdef.h>
#include <vector>
#include "wglext.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxguid.lib")
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV;
static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV;
static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV;
static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV;
static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV;
static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV;

static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;

struct DXGISwapChainAdapter {
    GLuint dsvNameGL;
    GLuint dcbNameGL;
    GLuint fbo;

    HANDLE gl_handleD3D;
    HANDLE dsvHandleGL;
    HANDLE dcbHandleGL;

    ID3D11Device* device;
    ID3D11DeviceContext* devCtx;
    IDXGISwapChain* swapChain;
    HANDLE hFrameLatencyWaitableObject;

    ID3D11Texture2D* dxDepthBuffer;
    ID3D11Texture2D* dxColorBuffer;
    ID3D11Texture2D* dxGlColorBuffer;
    ID3D11RenderTargetView* colorBufferView;
    ID3D11DepthStencilView* depthBufferView;


    int64_t performance_frequency;
    int64_t swap_timestamp;
    int64_t prev_swap_timestamp;
    DXGI_FRAME_STATISTICS prev_frame_stats;
    DXGI_FRAME_STATISTICS frame_stats;
    double refresh_rate;

    bool is_actually_vsynced;
    int is_vsynced_estimator;
};

//opengl-on-dxgi partially copied from https://github.com/nlguillemot/OpenGL-on-DXGI/blob/master/main.cpp
//but modified to work with DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL (which is needed for accurate timing)
//WGL_NV_DX_interop2 doesnt seem to work with sharing the back buffer directly with FLIP swapchains,
//so instead we let openGL render to a texture, share that, then copy that to the DXGI back buffer in directx instead
//seems to work well as a proof of concept at least

void APIENTRY DebugCallbackGL(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
    OutputDebugStringA("DebugCallbackGL: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

bool CheckHR(HRESULT hr) {
    if(SUCCEEDED(hr)) {
        return true;
    }

    _com_error err(hr);

    int result = MessageBoxW(NULL, err.ErrorMessage(), L"Error", MB_ABORTRETRYIGNORE);
    if(result == IDABORT) {
        ExitProcess(-1);
    } else if(result == IDRETRY) {
        DebugBreak();
    }

    return false;
}

bool CheckWin32(BOOL okay) {
    if(okay) {
        return true;
    }

    return CheckHR(HRESULT_FROM_WIN32(GetLastError()));
}

HRESULT GetRefreshRate(IUnknown* device, IDXGISwapChain* swapChain, double* outRefreshRate) {
    IDXGIOutput* dxgiOutput;
    HRESULT hr = swapChain->GetContainingOutput(&dxgiOutput);
    if(FAILED(hr))
        return hr;

    DXGI_MODE_DESC emptyMode = {};
    DXGI_MODE_DESC modeDescription;
    hr = dxgiOutput->FindClosestMatchingMode(&emptyMode, &modeDescription, device);

    if(SUCCEEDED(hr))
        *outRefreshRate = (double)modeDescription.RefreshRate.Numerator / (double)modeDescription.RefreshRate.Denominator;

    return hr;
}

DXGISwapChainAdapter* CreateDXGISwapChainAdapter(SDL_Window* window) {
    //from experiments, we don't actually need to create the GL context ourselves, we can use the one SDL made, which simplifies this a bit (can float on top of an existing context)

    DXGISwapChainAdapter* res = (DXGISwapChainAdapter*)malloc(sizeof(DXGISwapChainAdapter));
    memset(res, 0, sizeof(DXGISwapChainAdapter));

    HWND hWnd = (HWND)SDL_GetProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    HINSTANCE hInstance = (HINSTANCE)SDL_GetProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);

    RECT rect;
    int width = 1280;
    int height = 720;
    if(GetClientRect(hWnd, &rect)) {
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }

    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
    glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 2; //double buffer
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    UINT flags = 0;
#if _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    CheckHR(D3D11CreateDeviceAndSwapChain(NULL, // pAdapter
        D3D_DRIVER_TYPE_HARDWARE,               // DriverType
        NULL,                                   // Software
        flags,                                  // Flags (Do not set D3D11_CREATE_DEVICE_SINGLETHREADED)
        NULL,                                   // pFeatureLevels
        0,                                      // FeatureLevels
        D3D11_SDK_VERSION,                      // SDKVersion
        &scd,                                   // pSwapChainDesc
        &res->swapChain,                             // ppSwapChain
        &res->device,                                // ppDevice
        NULL,                                   // pFeatureLevel
        &res->devCtx));                              // ppImmediateContext

    // Register D3D11 device with GL
    res->gl_handleD3D = wglDXOpenDeviceNV(res->device);
    CheckWin32(res->gl_handleD3D != NULL);

    // get frame latency waitable object
    IDXGISwapChain2* swapChain2;
    CheckHR(res->swapChain->QueryInterface(&swapChain2));
    res->hFrameLatencyWaitableObject = swapChain2->GetFrameLatencyWaitableObject();

    // Create depth stencil texture
    CD3D11_TEXTURE2D_DESC dstdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32G8X24_TYPELESS, width, height, 1, 1, D3D11_BIND_DEPTH_STENCIL);
    CheckHR(res->device->CreateTexture2D(
        &dstdesc,
        NULL,
        &res->dxDepthBuffer));

    // Create depth stencil view
    CD3D11_DEPTH_STENCIL_VIEW_DESC dsvdesc = CD3D11_DEPTH_STENCIL_VIEW_DESC(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CheckHR(res->device->CreateDepthStencilView(
        res->dxDepthBuffer,
        &dsvdesc,
        &res->depthBufferView));

    // Create color buffer texture
    CD3D11_TEXTURE2D_DESC dgldesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, D3D11_BIND_RENDER_TARGET);
    CheckHR(res->device->CreateTexture2D(
        &dgldesc,
        NULL,
        &res->dxGlColorBuffer));

    // register the Direct3D depth/stencil buffer as texture2d in opengl
    glGenTextures(1, &res->dsvNameGL);
    res->dsvHandleGL = wglDXRegisterObjectNV(res->gl_handleD3D, res->dxDepthBuffer, res->dsvNameGL, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    CheckWin32(res->dsvHandleGL != NULL);

    //register color buffer as a texture2D in opengl
    glGenTextures(1, &res->dcbNameGL);
    res->dcbHandleGL = wglDXRegisterObjectNV(res->gl_handleD3D, res->dxGlColorBuffer, res->dcbNameGL, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    CheckWin32(res->dcbHandleGL != NULL);

    // attach the Direct3D depth buffer to FBO
    glGenFramebuffers(1, &res->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, res->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, res->dsvNameGL, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, res->dcbNameGL, 0);

    CheckHR(GetRefreshRate(res->device, res->swapChain, &res->refresh_rate));

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    res->performance_frequency = freq.QuadPart;

    res->is_actually_vsynced = true; //initial guess should be to assume we are vsynced

    return res;
}

double DXGISwapChainAdapterRefreshRate(DXGISwapChainAdapter* context) {
    return context->refresh_rate;
}

void DXGISwapChainAdapterResize(DXGISwapChainAdapter* context, int width, int height) {
    //release linked opengl resources
    wglDXUnregisterObjectNV(context->gl_handleD3D, context->dsvHandleGL);
    wglDXUnregisterObjectNV(context->gl_handleD3D, context->dcbHandleGL);

    context->devCtx->OMSetRenderTargets(0, NULL, NULL);

    //release directx resourcers
    context->depthBufferView->Release();
    context->dxDepthBuffer->Release();
    context->dxGlColorBuffer->Release();

    //resize the swap chain
    CheckHR(context->swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    //recreate directx resources
    // Create depth stencil texture
    CD3D11_TEXTURE2D_DESC dstdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32G8X24_TYPELESS, width, height, 1, 1, D3D11_BIND_DEPTH_STENCIL);
    CheckHR(context->device->CreateTexture2D(
        &dstdesc,
        NULL,
        &context->dxDepthBuffer));

    // Create depth stencil view
    CD3D11_DEPTH_STENCIL_VIEW_DESC dsvdesc = CD3D11_DEPTH_STENCIL_VIEW_DESC(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CheckHR(context->device->CreateDepthStencilView(
        context->dxDepthBuffer,
        &dsvdesc,
        &context->depthBufferView));

    // Create color buffer texture
    CD3D11_TEXTURE2D_DESC dgldesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, D3D11_BIND_RENDER_TARGET);
    CheckHR(context->device->CreateTexture2D(
        &dgldesc,
        NULL,
        &context->dxGlColorBuffer));

    //reregister the buffers with opengl
    context->dsvHandleGL = wglDXRegisterObjectNV(context->gl_handleD3D, context->dxDepthBuffer, context->dsvNameGL, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    CheckWin32(context->dsvHandleGL != NULL);

    //register color buffer as a texture2D in opengl
    context->dcbHandleGL = wglDXRegisterObjectNV(context->gl_handleD3D, context->dxGlColorBuffer, context->dcbNameGL, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    CheckWin32(context->dcbHandleGL != NULL);

    // reattach the Direct3D depth buffer to FBO
    glBindFramebuffer(GL_FRAMEBUFFER, context->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, context->dsvNameGL, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, context->dcbNameGL, 0);
}

static void update_timing_information(DXGISwapChainAdapter* context) {
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);

    context->prev_swap_timestamp = context->swap_timestamp;
    context->prev_frame_stats = context->frame_stats;

    context->swapChain->GetFrameStatistics(&context->frame_stats);
    context->swap_timestamp = timestamp.QuadPart;

    //before any frames are pushed, this reports 0, we wanna wait till we got real information before doing vsync detection
    if(context->frame_stats.SyncQPCTime.QuadPart == 0 || context->prev_frame_stats.SyncQPCTime.QuadPart == 0) return;

    bool was_previous_frame_synced = abs(context->swap_timestamp-context->frame_stats.SyncQPCTime.QuadPart) < .0001 * context->performance_frequency;
    bool definitely_not_vsynced = context->prev_frame_stats.PresentCount == context->frame_stats.PresentCount;

    if(context->is_actually_vsynced) {
        context->is_vsynced_estimator += was_previous_frame_synced?-1:1;
        if(context->is_vsynced_estimator < 0) context->is_vsynced_estimator = 0;
        if(context->is_vsynced_estimator >= 4) { //net +4 unsynced frames, we aren't vsynced
            context->is_vsynced_estimator = 0;
            context->is_actually_vsynced = false;
        }
    } else {
        context->is_vsynced_estimator += was_previous_frame_synced?1:-1;
        if(context->is_vsynced_estimator < 0 || definitely_not_vsynced) context->is_vsynced_estimator = 0;
        if(context->is_vsynced_estimator >= 8) { //net +8 vsynced frames, we are probably vsynced
            context->is_vsynced_estimator = 0;
            context->is_actually_vsynced = true;
        }
    }
}

void DXGISwapChainAdapterPrepareBuffers(DXGISwapChainAdapter* context) {
    // Wait for swap chain to signal that it is ready to render
    CheckWin32(WaitForSingleObject(context->hFrameLatencyWaitableObject, INFINITE) == WAIT_OBJECT_0);

    update_timing_information(context);
    

    // Fetch the current swapchain backbuffer from the FLIP swap chain
    CheckHR(context->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&context->dxColorBuffer));

    // Create RTV for swapchain backbuffer
    CD3D11_RENDER_TARGET_VIEW_DESC rtvdesc = CD3D11_RENDER_TARGET_VIEW_DESC(D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
    CheckHR(context->device->CreateRenderTargetView(
        context->dxColorBuffer,
        &rtvdesc,
        &context->colorBufferView));

    // Attach back buffer and depth texture to redertarget for the device.
    context->devCtx->OMSetRenderTargets(1, &context->colorBufferView, context->depthBufferView);

    // lock the dsv/rtv for GL access
    wglDXLockObjectsNV(context->gl_handleD3D, 1, &context->dsvHandleGL);
    wglDXLockObjectsNV(context->gl_handleD3D, 1, &context->dcbHandleGL);

    //bind opengl frame buffer
    glBindFramebuffer(GL_FRAMEBUFFER, context->fbo);
}

void DXGISwapChainAdapterSwapBuffers(DXGISwapChainAdapter* context, bool vsync) {
    // unlock the dsv/rtv
    wglDXUnlockObjectsNV(context->gl_handleD3D, 1, &context->dcbHandleGL);
    wglDXUnlockObjectsNV(context->gl_handleD3D, 1, &context->dsvHandleGL);

    //copy opengl framebuffer to swapchain framebuffer
    context->devCtx->CopyResource(context->dxColorBuffer, context->dxGlColorBuffer);

    CheckHR(context->swapChain->Present(vsync, vsync?0:DXGI_PRESENT_ALLOW_TEARING));

    //release current backbuffer back to the swap chain
    context->colorBufferView->Release();
    context->dxColorBuffer->Release();
}

int64_t DXGISwapChainAdapterGetPresentTimestamp(DXGISwapChainAdapter* context) {
    if(context->is_actually_vsynced) { //if not vsynced, we want to just use the measured time instead of the present time
        return context->frame_stats.SyncQPCTime.QuadPart;
    } else {
        return context->swap_timestamp;
    }
}
bool DXGISwapChainAdapterIsActuallyVsynced(DXGISwapChainAdapter* context) {
    return context->is_actually_vsynced;
}

int64_t DXGISwapChainAdapterGetTimingMethodDelta(DXGISwapChainAdapter* context) {
    return context->swap_timestamp - context->frame_stats.SyncQPCTime.QuadPart;
}

FrameStatistics DXGISwapChainAdapterGetFrameStatistics(DXGISwapChainAdapter* context) {
    FrameStatistics res;
    res.sync_time = context->frame_stats.SyncQPCTime.QuadPart;
    res.present_count = context->frame_stats.PresentCount;
    res.present_refresh_count = context->frame_stats.PresentRefreshCount;
    res.sync_refresh_count = context->frame_stats.SyncRefreshCount;
    return res;
}
