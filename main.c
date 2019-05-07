#include "clock.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <jack/jack.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))

uint64_t fmix64(uint64_t k)
{
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdul;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ul;
  k ^= k >> 33;

  return k;
}

// FIXME thread local?
static atomic_bool running;

// both square
#define WINDOW_SIZE 2048
#define N_CELLS     1024

static_assert(WINDOW_SIZE % N_CELLS == 0, "Window Size must evenly divide number of cells");
#define CELL_SIZE (WINDOW_SIZE / N_CELLS)

#define MIN_LIFESPAN 400
#define MAX_LIFESPAN 800

static struct SDL_Rect const* temp_cell_rect(int x, int y)
{
  static struct SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = CELL_SIZE;
  rect.h = CELL_SIZE;
  return &rect;
}

typedef struct {
  jack_port_t * young;
  jack_port_t * middle_aged;
  jack_port_t * old;

  // volatile is technically wrong but whatever
  volatile float * yf;  // percent
  volatile float * maf; // percent
  volatile float * of;  // percent
} args_t;

static int proc_audio(jack_nframes_t nframes, void* arg)
{
  args_t const* args = arg;
  float* young       = jack_port_get_buffer(args->young,       nframes);
  float* middle_aged = jack_port_get_buffer(args->middle_aged, nframes);
  float* old         = jack_port_get_buffer(args->old,         nframes);

  for (size_t i = 0; i < nframes; ++i) {
    young[i]       = *args->yf;
    middle_aged[i] = *args->maf;
    old[i]         = *args->of;
  }

  return 0;
}

static int alive_next_cycle(int* cells, size_t my_x, size_t my_y)
{
  static ssize_t xoffs[3] = {-1, 0, 1};
  static ssize_t yoffs[3] = {-1, 0, 1};

  size_t alive_neighbors = 0;
  for (size_t xx = 0; xx < 3; ++xx) {
    for (size_t yy = 0; yy < 3; ++yy) {
      if (!xoffs[xx] && !yoffs[yy])             continue;
      if ((ssize_t)my_x + xoffs[xx] < 0)        continue;
      if ((ssize_t)my_x + xoffs[xx] >= N_CELLS) continue;
      if ((ssize_t)my_y + yoffs[yy] < 0)        continue;
      if ((ssize_t)my_y + yoffs[yy] >= N_CELLS) continue;

      int n = cells[my_x + xoffs[xx] + (my_y + yoffs[yy])*N_CELLS];
      alive_neighbors += n ? 1 : 0;
    }
  }

  int curr = cells[my_x + my_y*N_CELLS];

  static uint64_t entropy = 0;
  uint64_t random_wiggle  = fmix64(++entropy ^ alive_neighbors ^ curr) % (uint64_t)(0.9*MAX_LIFESPAN);
  int      max_lifespan   = MAX_LIFESPAN + random_wiggle;

  random_wiggle  = fmix64(++entropy ^ alive_neighbors ^ curr) % (uint64_t)(0.3*MIN_LIFESPAN);
  int min_lifespan   = MIN_LIFESPAN + random_wiggle;

  if (curr) {
    if (curr < min_lifespan)  return ++curr;
    if (alive_neighbors <= 1) return 0;
    if (alive_neighbors >= 4) return 0;
    if (curr >= max_lifespan) return 0;
    /* otherwise */           return ++curr;
  }
  else {
    if (alive_neighbors == 3) return 1;
    else                      return 0;
  }

  __builtin_unreachable();
}

int main(void)
{
  SDL_Window*  w = NULL;
  SDL_Surface* s = NULL;

  if (0 != SDL_Init(SDL_INIT_VIDEO)) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  if (0 != TTF_Init()) {
    printf("TTF could not initialize! SDL_Error: %s\n", TTF_GetError());
    return 1;
  }

  w = SDL_CreateWindow("SDL Tutorial",
      /* x, y */  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      /* w, h */  WINDOW_SIZE, WINDOW_SIZE,
      /* flags */ SDL_WINDOW_SHOWN);

  if (!w) {
    printf("Could not create SDL window! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  TTF_Font* font = TTF_OpenFont("/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", 20);
  SDL_Color white = {0, 255, 0};
  if (!font) {
    printf("Could not open font! TTF_Error: %s\n", TTF_GetError());
    return 1;
  }

  int* arr1 = malloc(sizeof(int) * N_CELLS * N_CELLS);
  if (!arr1) {
    printf("Allocation failed\n");
    return 1;
  }

  int* arr2 = malloc(sizeof(int) * N_CELLS * N_CELLS);
  if (!arr2) {
    printf("Allocation failed\n");
    return 1;
  }

  jack_client_t * cl = jack_client_open("life", JackNoStartServer, NULL);
  if (!cl) {
    printf("Failed to open jack client\n");
    return 1;
  }

  jack_port_t * young = jack_port_register(cl, "young", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (!young) {
    printf("Failed to create young port\n");
    return 1;
  }

  jack_port_t * middle_aged = jack_port_register(cl, "middle_aged", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (!middle_aged) {
    printf("Failed to create young port\n");
    return 1;
  }

  jack_port_t * old = jack_port_register(cl, "old", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (!old) {
    printf("Failed to create young port\n");
    return 1;
  }

  volatile float ayf  = 0.0; // what a mess!
  volatile float amaf = 0.0;
  volatile float aof  = 0.0;

  int ret = jack_set_process_callback(cl, proc_audio, &(args_t){
      .young       = young,
      .middle_aged = middle_aged,
      .old         = old,
      .yf          = &ayf,
      .maf         = &amaf,
      .of          = &aof,
      });

  ret = jack_activate(cl);
  if (ret != 0) {
    printf("Failed to acitvate\n");
    return 1;
  }

  ret = jack_connect(cl, "life:young", "system:playback_5");
  if (ret != 0) {
    printf("Failed to connect\n");
    return 1;
  }
  ret = jack_connect(cl, "life:middle_aged", "system:playback_6");
  if (ret != 0) {
    printf("Failed to connect\n");
    return 1;
  }
  ret = jack_connect(cl, "life:old", "system:playback_7");
  if (ret != 0) {
    printf("Failed to connect\n");
    return 1;
  }

  if (ret != 0) {
    printf("Failed to set process callback\n");
    return 1;
  }

  // double buffer
  int*   arrs[2] = {arr1, arr2};
  size_t which   = 0;

  // initialize with random values
  uint64_t r = 0xcafebabe;
  for (size_t i = 0; i < N_CELLS*N_CELLS; ++i) {
    r = fmix64(r ^ i);
    arrs[which][i] = r > INT64_MAX/4;
  }

  float    fps         = 60.;
  float    fps2        = 60.;
  uint64_t last_update = 0;
  SDL_Surface* surfaceMessage = NULL;
  SDL_Surface* surfaceMessage2 = NULL;

  uint64_t end_bucket_young = MAX_LIFESPAN/3;
  uint64_t end_bucket_ma    = MAX_LIFESPAN/3 + MAX_LIFESPAN/3;

  char fps_buffer[1024];
  atomic_store(&running, true);
  while (atomic_load(&running)) {
    uint64_t start = wallclock();
    s = SDL_GetWindowSurface(w);
    SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0)); // make entire thing black

    float yf  = 0.0;
    float maf = 0.0;
    float of  = 0.0;

    size_t next = which == 1 ? 1 : 0;
    for (size_t x = 0; x < N_CELLS; ++x) {
      for (size_t y = 0; y < N_CELLS; ++y) {
        int n = arrs[which][x+y*N_CELLS];
        if (n < end_bucket_young)   yf  += 1;
        else if (n < end_bucket_ma) maf += 1;
        else                        of  += 1;

        arrs[next][x + y*N_CELLS] = alive_next_cycle(arrs[which], x, y);

        // draw
        int   mmm    = 200;
        float factor = ((float)mmm/((float)(MAX_LIFESPAN)));
        int   c      = MIN(arrs[which][x+y*N_CELLS], MAX_LIFESPAN)*factor;
        assert(c <= 255);
        if (c > 3*(mmm/4)) {
          SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.9*c, 0.2*c, 0.2*c));
        }
        else {
          SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.2*c, 0.3*c, 0.8*c));
        }
      }
    }
    yf  /= N_CELLS*N_CELLS;
    maf /= N_CELLS*N_CELLS;
    of  /= N_CELLS*N_CELLS;

    // update volatiles
    ayf  = yf;
    amaf = maf;
    aof  = of;
    uint64_t stop_render = wallclock();

    for (size_t x = 0; x < N_CELLS; ++x) {
      for (size_t y = 0; y < N_CELLS; ++y) {
      }
    }
    which = next;

    if (surfaceMessage) {
      SDL_BlitSurface(surfaceMessage, NULL, s, &(struct SDL_Rect){.x = 0, .y = 0, .h = 20, .w = 20});
    }

    if (surfaceMessage2) {
      SDL_BlitSurface(surfaceMessage2, NULL, s, &(struct SDL_Rect){.x = 0, .y = 20, .h = 20, .w = 20});
    }

    SDL_UpdateWindowSurface(w);
    // SDL_Delay(50);

    SDL_Event e[1];
    while (SDL_PollEvent(e)) {
      if (e->type == SDL_QUIT) atomic_store(&running, false);
    }

    uint64_t stop = wallclock();
    if (stop - last_update > 1e9) {
      float this_frame = 1e9 / (stop-start);
      fps = (0.7)*this_frame - (1. - 0.7)*fps;
      sprintf(fps_buffer, "fps: %.3f", fps);
      surfaceMessage = TTF_RenderText_Solid(font, fps_buffer, white);

      fps2 = (0.7)*(1e9 / (stop_render - start)) - (1. - 0.7)*fps;
      sprintf(fps_buffer, "fps: %.3f", fps2);
      surfaceMessage2 = TTF_RenderText_Solid(font, fps_buffer, white);
      last_update = stop;
    }
  }

  jack_client_close(cl);
  SDL_DestroyWindow(w);
  SDL_Quit();

  return 0;
}
