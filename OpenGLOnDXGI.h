#include <SDL3/SDL.h>

struct OpenGLContext;

OpenGLContext* CreateDXGIGLContext(SDL_Window* window);
void DXGIGLResize(OpenGLContext* context, int width, int height);
void DXGIGLPrepareBuffers(OpenGLContext* context);
void DXGIGLSwapBuffers(OpenGLContext* context, bool vsync);
int64_t DXGIGLGetPreviousSwapTimestamp(OpenGLContext* context);