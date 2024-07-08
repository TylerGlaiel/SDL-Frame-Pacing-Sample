#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <cmath>
#include <cstring>
#include "DXGISwapChainAdapter.h"
#include <iostream>
#include "Windows.h"

struct GameState {
    float blue_x, blue_y;
    float blue_previous_x, blue_previous_y;

    float red_x, red_y;
    float red_timer;

    float pacer_x, pacer_y;
    float pacer_timer;

    float view_w, view_h;
    bool yflip;
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
Uint64 SDL_GetFrameTime();


void SDL_Internal_FramePacing_ComputeDeltaTime(DXGISwapChainAdapter* swapchain);
void SDL_Internal_FramePacing_Init(SDL_Window* window);
void SDL_Internal_SwapBuffersAndMeasureTime_NonDXGI(SDL_Window* window);

int main(int argc, char* argv[]) {
    bool use_dxgi = true;

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_Window* window = SDL_CreateWindow("Frame Pacing Sample (vsync on)", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_Internal_FramePacing_Init(window);
    SDL_GLContext glcontext = SDL_GL_CreateContext(window);
    if(!use_dxgi) SDL_GL_SetSwapInterval(1);
    DXGISwapChainAdapter* swapchain = use_dxgi?CreateDXGISwapChainAdapter(window):NULL;
   
    bool running = true;
    bool vsync = true;

    GameState state = {0};
    state.yflip = use_dxgi;

    SDL_FramePacingInfo pacing_info = {0};
    pacing_info.update_rate = 144;//DXGISwapChainAdapterRefreshRate(swapchain);//60;
    pacing_info.fixed_update_callback = game_fixed_update;
    pacing_info.variable_update_callback = game_variable_update;
    pacing_info.render_callback = game_render;
    pacing_info.user_data = &state;


    int framestats_out_counter = 0;

    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
            if(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { //click to toggle vsync
                if(event.button.which == 0) {
                    vsync = !vsync;
                    if(!use_dxgi) SDL_GL_SetSwapInterval(vsync);
                    if(vsync) {
                        SDL_SetWindowTitle(window, "Frame Pacing Sample (vsync on)");
                    } else {
                        SDL_SetWindowTitle(window, "Frame Pacing Sample (vsync off)");
                    }
                }
            }
            if(event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                if(use_dxgi) DXGISwapChainAdapterResize(swapchain, event.window.data1, event.window.data2);
                state.view_w = event.window.data1;
                state.view_h = event.window.data2;
            }
        };

        if(use_dxgi) DXGISwapChainAdapterPrepareBuffers(swapchain);
        SDL_Internal_FramePacing_ComputeDeltaTime(swapchain);

        Uint64 frame_time = SDL_GetFrameTime();
        SDL_PaceFrame(frame_time, &pacing_info);

        if(use_dxgi) {
            DXGISwapChainAdapterSwapBuffers(swapchain, vsync);
        } else {
            SDL_Internal_SwapBuffersAndMeasureTime_NonDXGI(window);
        }
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
    glViewport(0, 0, state->view_w, state->view_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1280, state->yflip?0:720, state->yflip?720:0, -1, 1); //when using DXGI swapchain the Y axis is inverted... i dont know how to fix on the swapchain level
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

    //uncomment this to simulate rendering taking longer (change the number)
    //SDL_Delay(6);
}
void game_fixed_update(double delta_time, void* data) {
    GameState* state = (GameState*)data;

    //cache blue box position, for interpolation
    state->blue_previous_x = state->blue_x;
    state->blue_previous_y = state->blue_y;

    //move blue box towards mouse at constant speed
    float mx, my;
    SDL_GetMouseState(&mx, &my);
    mx *= 1280 / state->view_w;
    my *= 720 / state->view_h;

    float vx = mx - state->blue_x;
    float vy = my - state->blue_y;
    float invl = 1.0/sqrt(vx*vx+vy*vy);
    if(isinf(invl)) invl = 0;

    state->blue_x += vx*invl*delta_time * 500;
    state->blue_y += vy*invl*delta_time * 500;

    state->blue_x = clamp(state->blue_x, 0, 1280);
    state->blue_y = clamp(state->blue_y, 0, 720);
}
void game_variable_update(double delta_time, void* data) {
    //move red box in a sin wave
    GameState* state = (GameState*)data;
    state->red_timer += delta_time * 10;

    state->red_y = 360;
    state->red_x = sin(state->red_timer)*400 + 640;

    //pacer that doesnt respect delta time (should go super fast when vsync is off)
    state->pacer_timer += 1.0 / (2*3.141592);

    state->pacer_x = sin(state->pacer_timer)*100 + 640;
    state->pacer_y = cos(state->pacer_timer)*100 + 360;
}



//sample frame timing internal
struct FrameTimingInternal {
    int64_t delta_time;
    int64_t clocks_per_second;
    int64_t prev_frame_time;
    int64_t snap_error;
    int64_t non_vsync_smoother;
    int64_t non_vsync_error;

    int64_t drift; //the difference between the sum of reported times, and the measured real times
} frame_timing_info;

struct FrameTimingInternal_NonDXGI {
    int64_t swap_time;
    double window_refresh_rate;
    bool is_actually_vsynced;

    static const int drift_detection_window = 128;
    int64_t snapped_deltas[drift_detection_window];
    int64_t realtime_deltas[drift_detection_window];
    int64_t drift_detection_index;
    int64_t snapped_total;
    int64_t realtime_total;
    int64_t snap_error;
    int is_vsynced_estimator;

} frame_timing_info_ndxgi;

struct FramePacingInternal {
    int64_t accumulator;
} frame_pacing_info;

void SDL_Internal_SwapBuffersAndMeasureTime_NonDXGI(SDL_Window* window) {
    //commented out time and delta here was a futile attempt to "measure if SDL_GL_SwapWindow blocked", 
    //unfortunately even when it doesnt block it can still take ~0.5ms which is too much error to be useful I think
    //int64_t time = SDL_GetPerformanceCounter();
    if(window) SDL_GL_SwapWindow(window);
    //int64_t delta = SDL_GetPerformanceCounter() - time;

    //timestamp
    int64_t timestamp = SDL_GetPerformanceCounter();
    int64_t delta = timestamp - frame_timing_info_ndxgi.swap_time;
    frame_timing_info_ndxgi.swap_time = timestamp;

    //VSYNC DETECTION (this is the part that is especially annoying without DXGI)
    int64_t monitor_refresh_period = frame_timing_info.clocks_per_second / frame_timing_info_ndxgi.window_refresh_rate;

    //this is essentially copied from how we do snapping in SDL_Internal_FramePacing_ComputeDeltaTime, except we let it be 0 sometimes
    //if we are vsynced, this should "not drift over time". so thats how we detect vsync
    int est_vsyncs = round((double)(delta+frame_timing_info_ndxgi.snap_error) / monitor_refresh_period);
    int64_t snapped_delta = monitor_refresh_period * est_vsyncs;
    frame_timing_info_ndxgi.snap_error /= 2; //decay previous snap error
    frame_timing_info_ndxgi.snap_error += delta - snapped_delta;

    //track realtime / snapped time drift over last 128 refreshes using a circular buffer of recorded times
    int index = (frame_timing_info_ndxgi.drift_detection_index++)%frame_timing_info_ndxgi.drift_detection_window;
    frame_timing_info_ndxgi.snapped_total -= frame_timing_info_ndxgi.snapped_deltas[index];
    frame_timing_info_ndxgi.realtime_total -= frame_timing_info_ndxgi.realtime_deltas[index];
    frame_timing_info_ndxgi.snapped_deltas[index] = snapped_delta;
    frame_timing_info_ndxgi.realtime_deltas[index] = delta;
    frame_timing_info_ndxgi.snapped_total += frame_timing_info_ndxgi.snapped_deltas[index];
    frame_timing_info_ndxgi.realtime_total += frame_timing_info_ndxgi.realtime_deltas[index];

    //if we are vsynced, the drift should remain small
    int64_t drift = frame_timing_info_ndxgi.realtime_total - frame_timing_info_ndxgi.snapped_total;

    double error_range = .005; //5ms range for vsync detection (dont know if theres a better way to determine a threshold here, 5ms seems like enough to absorb one-frame measurement error)
    if(frame_timing_info_ndxgi.drift_detection_index < frame_timing_info_ndxgi.drift_detection_window) {
        error_range = .1; //if we havent recorded enough frames, 100ms error range instead (this should diverge fast if not actually vsynced, so we dont have to wait too long)
    }

    bool probably_vsynced = abs(drift) < error_range * frame_timing_info.clocks_per_second;
    bool prev_vsynced = frame_timing_info_ndxgi.is_actually_vsynced;

    //this bit is copied from my DXGI stuff, because of random spikes the drift can be "off" sometimes, but usually resolved on the next frame. 
    //we wanna make sure we detect "not vsynced" for a few frames in a row before deciding thats the case
    //note that if you change modes it can take some time for this detection to kick in (when going from non-vsynced to vsynced)
    //for that reason it might be useful to flush these buffers if the application changes vsync manually
    if(frame_timing_info_ndxgi.is_actually_vsynced) {
        frame_timing_info_ndxgi.is_vsynced_estimator += probably_vsynced?-1:1;
        if(frame_timing_info_ndxgi.is_vsynced_estimator < 0) frame_timing_info_ndxgi.is_vsynced_estimator = 0;
        if(frame_timing_info_ndxgi.is_vsynced_estimator >= 4) { //net +4 unsynced frames, we aren't vsynced
            frame_timing_info_ndxgi.is_actually_vsynced = false;

            frame_timing_info_ndxgi.is_vsynced_estimator = 0;
        }
    } else {
        frame_timing_info_ndxgi.is_vsynced_estimator += probably_vsynced?1:-1;
        if(frame_timing_info_ndxgi.is_vsynced_estimator < 0) frame_timing_info_ndxgi.is_vsynced_estimator = 0;
        if(frame_timing_info_ndxgi.is_vsynced_estimator >= 16) { //net +16 vsynced frames, we are probably vsynced
            frame_timing_info_ndxgi.is_actually_vsynced = true;

            frame_timing_info_ndxgi.is_vsynced_estimator = 0;
        }
    }

    //Note: with variable rate refresh, if rendering takes "about as much time as 1 frame" (test this by putting a SDL_Delay in game_render(), 
    //this can keep swapping between vsynced and non-vsynced timings and the frame pacing ends up kind of jittery,
    //any frames > 1 refresh period behave as non-vsynced, and any frames < 1 refresh period behave as vsynced
    //the correct thing to do here is to probably just increase the detection thresholds by a bunch if this case is detected (switching between vsynced / non-vsynced often), in favor of non-vsynced timing
}

void SDL_Internal_FramePacing_ComputeDeltaTime(DXGISwapChainAdapter* swapchain) {
    bool is_vsynced; int64_t current_frametime; int64_t monitor_refresh_period;
    if(swapchain) {
        is_vsynced = DXGISwapChainAdapterIsActuallyVsynced(swapchain);
        current_frametime = DXGISwapChainAdapterGetPresentTimestamp(swapchain);
        monitor_refresh_period = frame_timing_info.clocks_per_second / DXGISwapChainAdapterRefreshRate(swapchain);
    } else {
        is_vsynced = frame_timing_info_ndxgi.is_actually_vsynced;
        current_frametime = frame_timing_info_ndxgi.swap_time;
        monitor_refresh_period = frame_timing_info.clocks_per_second / frame_timing_info_ndxgi.window_refresh_rate;
    }

    int64_t delta_time = current_frametime - frame_timing_info.prev_frame_time;

    if(frame_timing_info.prev_frame_time == 0) { //first update, just report 1 vsync time
        delta_time = monitor_refresh_period;
    }
    frame_timing_info.prev_frame_time = current_frametime;
    frame_timing_info.drift -= delta_time;

    if(is_vsynced) {
        //if the display adapter thinks we're vsynced, then always snap to the nearest vsync
        //note: sometimes I get glitch measurements still, even with the accurate frame timing method. 
        //     this usually appears like a frame with a longer time than it should have, followed by a frame with a lower time than it should have. 
        //     these sum up to 2 frames worth of time usually, I think, so I think its just an OS scheduling thing messing up when the time is recorded internally
        //     snap_error is meant to smooth this out slightly, though is not meant to compensate over the long term, so it decays
        
        int est_vsyncs = round((double)(delta_time+frame_timing_info.snap_error) / monitor_refresh_period);
        if(est_vsyncs == 0) est_vsyncs = 1;

        int64_t snapped_time = monitor_refresh_period * est_vsyncs;
        frame_timing_info.snap_error /= 2; //decay previous snap error
        frame_timing_info.snap_error += delta_time - snapped_time;

        delta_time = snapped_time;
        frame_timing_info.non_vsync_error = 0;
    }

    //non vsynced, we smooth out the measurements over a few frames
    //we also keep track of the total time drift and slightly compensate for it in the smoother
    //this should keep the reported values in non-vsync mode in sync with real time over the long term
    const double smoothing = 4;
    frame_timing_info.non_vsync_smoother *= (smoothing-1)/smoothing;
    frame_timing_info.non_vsync_smoother += (delta_time-frame_timing_info.non_vsync_error) / smoothing;

    if(!is_vsynced){
        frame_timing_info.non_vsync_error -= delta_time;
        delta_time = frame_timing_info.non_vsync_smoother;
        if(delta_time < 0) delta_time = 0;
        frame_timing_info.non_vsync_error += delta_time;
    }

    frame_timing_info.delta_time = delta_time;
    frame_timing_info.drift += delta_time;
}

Uint64 SDL_GetFrameTime() {
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
    memset(&frame_timing_info_ndxgi, 0, sizeof(frame_timing_info_ndxgi));

    SDL_DisplayID display_index = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* desktop = SDL_GetCurrentDisplayMode(display_index);
    if(desktop) {
        frame_timing_info_ndxgi.window_refresh_rate = desktop->refresh_rate;
    } else {
        frame_timing_info_ndxgi.window_refresh_rate = 0;
    }
    frame_timing_info_ndxgi.is_actually_vsynced = true;

    frame_timing_info.clocks_per_second = SDL_GetPerformanceFrequency();
}
