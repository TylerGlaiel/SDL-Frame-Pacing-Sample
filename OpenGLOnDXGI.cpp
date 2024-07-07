#include "OpenGLOnDXGI.h"
#include <SDL3/SDL_opengl.h>
#include <cstring>
#include <cassert>
#include <comdef.h>
#include "wglext.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxguid.lib")
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

struct OpenGLContext {
	HGLRC hGLRC;
	HDC hdc;

    GLuint rtvNameGL;
    GLuint dsvNameGL;
    GLuint dcbNameGL;
    GLuint fbo;

    HANDLE gl_handleD3D;
    HANDLE dxColor;
    HANDLE dsvHandleGL;
    HANDLE rtvHandleGL;
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
    
    bool vsynced;
    int64_t present_timestamp;
};

static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV;
static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV;
static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV;
static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV;
static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV;
static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV;

static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
static PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
static PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;

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

OpenGLContext* CreateDXGIGLContext(SDL_Window* window) {
	OpenGLContext* res = (OpenGLContext*)malloc(sizeof(OpenGLContext));
    memset(res, 0, sizeof(OpenGLContext));

	HWND hWnd = (HWND)SDL_GetProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    HINSTANCE hInstance = (HINSTANCE)SDL_GetProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);

    // GL context on temporary window, no drawing will happen to this window
    {
        // Create window that will be used to create a GL context
        HWND gl_hWnd = CreateWindowA("STATIC", "temp", WS_OVERLAPPED,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, NULL, NULL, NULL);
        CheckWin32(gl_hWnd != NULL);

        HDC gl_hDC = GetDC(gl_hWnd);
        CheckWin32(gl_hDC != NULL);

        // set pixelformat for window that supports OpenGL
        PIXELFORMATDESCRIPTOR gl_pfd = {};
        gl_pfd.nSize = sizeof(gl_pfd);
        gl_pfd.nVersion = 1;
        gl_pfd.dwFlags = PFD_SUPPORT_OPENGL;

        int chosenPixelFormat = ChoosePixelFormat(gl_hDC, &gl_pfd);
        CheckWin32(SetPixelFormat(gl_hDC, chosenPixelFormat, &gl_pfd) != FALSE);

        // Create dummy GL context that will be used to create the real context
        HGLRC dummy_hGLRC = wglCreateContext(gl_hDC);
        CheckWin32(dummy_hGLRC != NULL);

        // Use the dummy context to get function to create a better context
        CheckWin32(wglMakeCurrent(gl_hDC, dummy_hGLRC) != FALSE);

        PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

        int contextFlagsGL = 0;
        #ifdef _DEBUG
                contextFlagsGL |= WGL_CONTEXT_DEBUG_BIT_ARB;
        #endif

        int contextAttribsGL[] = {
          //  WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
          //  WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_FLAGS_ARB, contextFlagsGL,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        res->hGLRC = wglCreateContextAttribsARB(gl_hDC, NULL, contextAttribsGL);
        CheckWin32(res->hGLRC != NULL);

        // Switch to the new context and ditch the old one
        CheckWin32(wglMakeCurrent(gl_hDC, res->hGLRC) != FALSE);
        CheckWin32(wglDeleteContext(dummy_hGLRC) != FALSE);
    }

    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
    glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");

    // Enable OpenGL debugging
    #ifdef _DEBUG
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glEnable(GL_DEBUG_OUTPUT);
        PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)wglGetProcAddress("glDebugMessageCallback");
        glDebugMessageCallback(DebugCallbackGL, 0);
    #endif

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 2; //double buffer
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

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
    CD3D11_TEXTURE2D_DESC dstdesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R32G8X24_TYPELESS, 1280, 720, 1, 1, D3D11_BIND_DEPTH_STENCIL);
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
    CD3D11_TEXTURE2D_DESC dgldesc = CD3D11_TEXTURE2D_DESC(DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720, 1, 1, D3D11_BIND_RENDER_TARGET);
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return res;
}

void DXGIGLResize(OpenGLContext* context, int width, int height) {

}

void DXGIGLPrepareBuffers(OpenGLContext* context) {
    // Wait until the previous frame is presented before drawing the next frame
    CheckWin32(WaitForSingleObject(context->hFrameLatencyWaitableObject, INFINITE) == WAIT_OBJECT_0);

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

void DXGIGLSwapBuffers(OpenGLContext* context, bool vsync) {
    context->vsynced = vsync;

    // unlock the dsv/rtv
    wglDXUnlockObjectsNV(context->gl_handleD3D, 1, &context->dcbHandleGL);
    wglDXUnlockObjectsNV(context->gl_handleD3D, 1, &context->dsvHandleGL);

    //copy opengl framebuffer to swapchain framebuffer
    context->devCtx->CopyResource(context->dxColorBuffer, context->dxGlColorBuffer);

    CheckHR(context->swapChain->Present(vsync, vsync?0:DXGI_PRESENT_ALLOW_TEARING));
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    context->present_timestamp = timestamp.QuadPart;

    //release current backbuffer back to the swap chain
    context->colorBufferView->Release();
    context->dxColorBuffer->Release();
}

int64_t DXGIGLGetPreviousSwapTimestamp(OpenGLContext* context) {
    if(!context->vsynced) { //if not vsynced, we want to just use the measured time instead of the present time
        return context->present_timestamp;
    }

    DXGI_FRAME_STATISTICS out;
    context->swapChain->GetFrameStatistics(&out);
    return out.SyncQPCTime.QuadPart;
}