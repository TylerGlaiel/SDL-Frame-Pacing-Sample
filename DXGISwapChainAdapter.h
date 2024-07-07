#include <SDL3/SDL.h>

struct DXGISwapChainAdapter;
struct FrameStatistics {
    int64_t sync_time;
    unsigned int present_count;
    unsigned int present_refresh_count;
    unsigned int sync_refresh_count;
};

DXGISwapChainAdapter* CreateDXGISwapChainAdapter(SDL_Window* window);
void DXGISwapChainAdapterResize(DXGISwapChainAdapter* context, int width, int height);
void DXGISwapChainAdapterPrepareBuffers(DXGISwapChainAdapter* context);
void DXGISwapChainAdapterSwapBuffers(DXGISwapChainAdapter* context, bool vsync);
double DXGISwapChainAdapterRefreshRate(DXGISwapChainAdapter* context);
int64_t DXGISwapChainAdapterGetPreviousSwapTimestamp(DXGISwapChainAdapter* context);

FrameStatistics DXGISwapChainAdapterGetFrameStatistics(DXGISwapChainAdapter* context);
