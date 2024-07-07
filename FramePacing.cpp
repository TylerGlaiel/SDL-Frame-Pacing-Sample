#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <cmath>
#include <cstring>
#include "OpenGLOnDXGI.h"

struct GameState {
    float blue_x, blue_y;
    float blue_previous_x, blue_previous_y;

    float red_x, red_y;
    float red_timer;

    float pacer_x, pacer_y;
    float pacer_timer;
};

void game_render(double delta_time, double frame_percent, void* data);
void game_fixed_update(double delta_time, void* data);
void game_variable_update(double delta_time, void* data);

typedef void(*SDL_FramePacing_RenderCallback)(double,double,void*);
typedef void(*SDL_FramePacing_FixedUpdateCallback)(double, void*);
typedef void(*SDL_FramePacing_VariableUpdateCallback)(double, void*);

struct SDL_FramePacingInfo {
    float update_rate;
    SDL_FramePacing_FixedUpdateCallback fixed_update_callback;
    SDL_FramePacing_VariableUpdateCallback variable_update_callback;
    SDL_FramePacing_RenderCallback render_callback;
    void* user_data;
};

void SDL_PaceFrame(Uint64 delta_time, SDL_FramePacingInfo* pacing_info);
Uint64 SDL_GetFrameTime(SDL_Window* window);


void SDL_Internal_FramePacing_PostPresent(OpenGLContext* ctx);
void SDL_Internal_FramePacing_Init(SDL_Window* window);

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_Window* window = SDL_CreateWindow("Frame Pacing Sample (vsync on)", 1280, 720, SDL_WINDOW_OPENGL);
    SDL_Internal_FramePacing_Init(window);
    //SDL_GLContext glcontext = SDL_GL_CreateContext(window);
    //SDL_GL_SetSwapInterval(1);
    OpenGLContext* ctx = CreateDXGIGLContext(window);
   
    bool running = true;
    bool vsync = true;

    GameState state = {0};

    SDL_FramePacingInfo pacing_info = {0};
    pacing_info.update_rate = 144;
    pacing_info.fixed_update_callback = game_fixed_update;
    pacing_info.variable_update_callback = game_variable_update;
    pacing_info.render_callback = game_render;
    pacing_info.user_data = &state;

    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
            if(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { //click to toggle vsync
                if(event.button.which == 0) {
                    vsync = !vsync;
                    //SDL_GL_SetSwapInterval(vsync);
                    if(vsync) {
                        SDL_SetWindowTitle(window, "Frame Pacing Sample (vsync on)");
                    } else {
                        SDL_SetWindowTitle(window, "Frame Pacing Sample (vsync off)");
                    }
                }
            }
            if(event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                DXGIGLResize(ctx, event.window.data1, event.window.data2);
            }
        };

        Uint64 frame_time = SDL_GetFrameTime(window);
        DXGIGLPrepareBuffers(ctx);
        SDL_PaceFrame(frame_time, &pacing_info);

        //SDL_GL_SwapWindow(window);
        DXGIGLSwapBuffers(ctx, vsync);
        SDL_Internal_FramePacing_PostPresent(ctx);
    }

    return 0;
}

//sample user code
float lerp(float a, float b, float t) {
    return a * (1-t) + b * t;
}
float clamp(float v, float min, float max) {
    if(v < min) v = min;
    if(v > max) v = max;
    return v;
}

void draw_gl_rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x+w, y);
    glVertex2f(x+w, y+h);
    glVertex2f(x, y+h);
    glEnd();
}

void game_render(double delta_time, double frame_percent, void* data) {
    GameState* state = (GameState*)data;
    glViewport(0, 0, 1280, 720);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1280, 0, 720, -1, 1); //when using DXGI swapchain the Y axis is inverted... i dont know how to fix on the swapchain level
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    //meter in the middle that shows frame percent
    glColor4f(.5, .5, .5, .5);
    draw_gl_rect(0, 0, 40, 720);

    glColor4f(1, 1, 1, 1);
    draw_gl_rect(0, 0, 40, 720*frame_percent);

    //pacer is updated in variable update, but doesnt respect delta time, draw its actual position
    glColor4f(1, 0, 0, 1);
    draw_gl_rect(
        state->pacer_x - 20,
        state->pacer_y - 20,
        40, 40
    );

    //red box is updated in variable update, draw its actual position
    glColor4f(1,0,0,1);
    draw_gl_rect(
        state->red_x - 20, 
        state->red_y - 20,
        40, 40
    );

    //blue box is updated in fixed update, draw its interpolated position
    glColor4f(0, 0, 1, 1);
    draw_gl_rect(
        lerp(state->blue_previous_x, state->blue_x, frame_percent) - 20, 
        lerp(state->blue_previous_y, state->blue_y, frame_percent) - 20,
        40, 40
    );

   // SDL_Delay(1);
  //  glFinish();
   
}
void game_fixed_update(double delta_time, void* data) {
    GameState* state = (GameState*)data;

    //cache blue box position, for interpolation
    state->blue_previous_x = state->blue_x;
    state->blue_previous_y = state->blue_y;

    //move blue box towards mouse at constant speed
    float mx, my;
    SDL_GetMouseState(&mx, &my);

    float vx = mx - state->blue_x;
    float vy = my - state->blue_y;
    float invl = 1.0/sqrt(vx*vx+vy*vy);
    if(isinf(invl)) invl = 0;

    state->blue_x += vx*invl*delta_time * 500;
    state->blue_y += vy*invl*delta_time * 500;

    state->blue_x = clamp(state->blue_x, 0, 1280);
    state->blue_y = clamp(state->blue_y, 0, 720);

    //SDL_Delay(1);
}
void game_variable_update(double delta_time, void* data) {
    //move red box in a sin wave
    GameState* state = (GameState*)data;
    state->red_timer += delta_time * 2;

    state->red_y = 360;
    state->red_x = sin(state->red_timer)*400 + 640;

    //pacer that doesnt respect delta time (should go super fast when vsync is off)
    state->pacer_timer += 1.0 / (2*3.141592);

    state->pacer_x = sin(state->pacer_timer)*100 + 640;
    state->pacer_y = cos(state->pacer_timer)*100 + 360;

   // SDL_Delay(1);
}



//sample frame timing internal
struct FrameTimingInternal {
    int64_t delta_time;

    //cached queries
    int64_t clocks_per_second;
    float window_framerate;

    //stuff needed for dumb method
    int64_t prev_frame_time;
    int64_t current_frame_time;

    //stuff needed for smart method
    bool is_vsynced;
    bool prev_was_vsynced;

    //ring buffer for vsync detection
    double vsync_detection_buffer[128];
    int64_t vsync_detection_buffer_index;

    //ring buffer for frame time smoothing (non-vsynced case)
    int64_t frametime_buffer[8];
    int64_t frametime_buffer_index;
    

    int64_t real_time_tracker;
    int64_t game_time_tracker;
} frame_timing_info;

struct FramePacingInternal {
    int64_t accumulator;
} frame_pacing_info;

Uint64 SDL_GetFrameTime(SDL_Window* window) {
    return frame_timing_info.delta_time;
}
void SDL_PaceFrame(Uint64 delta_time, SDL_FramePacingInfo* pacing_info) {
    Uint64 desired_frame_time = frame_timing_info.clocks_per_second / pacing_info->update_rate;
    if(delta_time > frame_timing_info.clocks_per_second * .25) { //more than 1/4th of a second, this is a hitch and we should just do one frame
        delta_time = desired_frame_time;
        frame_pacing_info.accumulator = delta_time;
    }

    frame_pacing_info.accumulator += delta_time;
    int64_t consumedDeltaTime = delta_time;

    while(frame_pacing_info.accumulator > desired_frame_time) {
        frame_pacing_info.accumulator -= desired_frame_time;
        pacing_info->fixed_update_callback(1.0/pacing_info->update_rate, pacing_info->user_data);
    }

    if(consumedDeltaTime > 0) pacing_info->variable_update_callback((double)consumedDeltaTime / frame_timing_info.clocks_per_second, pacing_info->user_data);
    pacing_info->render_callback((double)delta_time / frame_timing_info.clocks_per_second, (double)frame_pacing_info.accumulator / desired_frame_time, pacing_info->user_data);
}


void SDL_Internal_FramePacing_Init(SDL_Window* window) {
    memset(&frame_timing_info, 0, sizeof(frame_timing_info));
    memset(&frame_pacing_info, 0, sizeof(frame_pacing_info));

    SDL_DisplayID display_index = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* desktop = SDL_GetCurrentDisplayMode(display_index);
    if(desktop) {
        frame_timing_info.window_framerate = desktop->refresh_rate;
    } else {
        frame_timing_info.window_framerate = 0;
    }
    frame_timing_info.prev_frame_time = SDL_GetPerformanceCounter();
    frame_timing_info.current_frame_time = frame_timing_info.prev_frame_time;
    frame_timing_info.clocks_per_second = SDL_GetPerformanceFrequency();
    frame_timing_info.is_vsynced = true; //assume vsync at the start
}

//this is done in the *dumb* way just so theres a baseline to compare a good implementation to
void SDL_Internal_FramePacing_PostPresent(OpenGLContext* ctx) {
    frame_timing_info.prev_frame_time = frame_timing_info.current_frame_time;
    frame_timing_info.current_frame_time = DXGIGLGetPreviousSwapTimestamp(ctx);

    frame_timing_info.delta_time = frame_timing_info.current_frame_time - frame_timing_info.prev_frame_time;
}

//this was some nonsense I was trying out to smooth out measured times, with the *accurate* DXGI timing instead this is not necessary
//with accurate DXGI timing, theres still a *small* amount of inaccuracy in measurements, but it seems *extremely small* (a few cycles here and there)
//so an actual solution would still need to pace this out

/*void SDL_Internal_FramePacing_PostPresent() {
    //initial timing measurement, track real time
    frame_timing_info.prev_frame_time = frame_timing_info.current_frame_time;
    frame_timing_info.current_frame_time = SDL_GetPerformanceCounter();
    const int64_t measured_delta_time = frame_timing_info.current_frame_time - frame_timing_info.prev_frame_time;
    frame_timing_info.frametime_buffer[(frame_timing_info.frametime_buffer_index++)%8] = measured_delta_time;


    //vsync detection
    double measured_frametime_seconds = (double)measured_delta_time / frame_timing_info.clocks_per_second;
    double window_frametime = 1.0 / frame_timing_info.window_framerate;

    int est_vsyncs = round(measured_frametime_seconds / window_frametime);
    if(est_vsyncs < 1) est_vsyncs = 1;
    for(int i = 0; i<est_vsyncs && i<128; i++) {
        frame_timing_info.vsync_detection_buffer[(frame_timing_info.vsync_detection_buffer_index++)%128] = measured_frametime_seconds / est_vsyncs;
    }

    if(!frame_timing_info.is_vsynced && frame_timing_info.vsync_detection_buffer_index > 128) {
        double vsync_detection_sum = 0;
        for(int i = 0; i<128; i++) {
            vsync_detection_sum += frame_timing_info.vsync_detection_buffer[i];
        }

        if(abs(vsync_detection_sum - window_frametime*128) < .0001*128) { //if we are "within 0.1ms / frame" of being vsynced, we're probably vsynced
            frame_timing_info.is_vsynced = true;
            frame_timing_info.real_time_tracker = 0;
            frame_timing_info.game_time_tracker = 0;
        }
    }

    //if we are vsynced, we should snap measured_delta_time to the nearest vsync multiple
    if(frame_timing_info.is_vsynced) {
        int64_t window_frame_time = frame_timing_info.clocks_per_second / frame_timing_info.window_framerate;
        int64_t snapped_time = round((double)measured_delta_time / window_frame_time) * window_frame_time;

        frame_timing_info.delta_time = snapped_time;

        if(snapped_time == 0) {
           // frame_timing_info.is_vsynced = false; //if we get a snapped time of 0, we probably arent actually vsynced and should immediately fall back to the non-vsynced version
            frame_timing_info.delta_time = snapped_time;
        }
    }

    //if we are not vsynced, then we should do an average of the last few frames of measurements, + drift compensation
    if(!frame_timing_info.is_vsynced) {
        int64_t average = frame_timing_info.real_time_tracker - frame_timing_info.game_time_tracker; //drift compensation
        //sum frame times
        for(int i = 0; i<frame_timing_info.frametime_buffer_index && i<8; i++) {
            average += frame_timing_info.frametime_buffer[i];
        }
        int64_t ct = 8;
        if(frame_timing_info.frametime_buffer_index < 8) ct = frame_timing_info.frametime_buffer_index;

        frame_timing_info.delta_time = average / ct;

        if(frame_timing_info.delta_time < 0) frame_timing_info.delta_time = 0;
    }

    //track drift
    frame_timing_info.real_time_tracker += measured_delta_time;
    frame_timing_info.game_time_tracker += frame_timing_info.delta_time;
    frame_timing_info.prev_was_vsynced = frame_timing_info.is_vsynced;

    if(frame_timing_info.is_vsynced) {
        //reset drift detection once per second, if vsynced and no notable drift occured over the last second (we do this before drift detection, so we can ignore anamoulous frame times here)
        if(frame_timing_info.real_time_tracker > frame_timing_info.clocks_per_second) {
            frame_timing_info.real_time_tracker = 0;
            frame_timing_info.game_time_tracker = 0;
        }

        //if real and game time drift by more than 10ms, we probably arent vsynced
        if(abs(frame_timing_info.real_time_tracker - frame_timing_info.game_time_tracker) > .01 * frame_timing_info.clocks_per_second) {
            frame_timing_info.is_vsynced = false;
        }
    }
}*/