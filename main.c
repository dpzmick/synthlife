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
#define WINDOW_SIZE 1400
#define N_CELLS     (WINDOW_SIZE/2)

static_assert(WINDOW_SIZE % N_CELLS == 0, "Window Size must evenly divide number of cells");
#define CELL_SIZE (WINDOW_SIZE / N_CELLS)

#define MIN_LIFESPAN 501
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

#ifdef JACK
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
#endif // JACK

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
    if (alive_neighbors == 4) return 1;
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

  TTF_Font* font = TTF_OpenFont("/usr/share/fonts/TTF/Inconsolata-Regular.ttf", 18);
  SDL_Color white = {255, 255, 255};
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

  volatile float ayf  = 0.0; // what a mess!
  volatile float amaf = 0.0;
  volatile float aof  = 0.0;

#ifdef JACK
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
#endif // JACK

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

  char fps_buffer[1024];
  atomic_store(&running, true);
  double average_age = MAX_LIFESPAN/2; // sort of
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
        int   n      = arrs[which][x+y*N_CELLS];
        int   mmm    = 200;
        float factor = ((float)mmm/((float)(MAX_LIFESPAN)));
        int   c      = MIN(n, MAX_LIFESPAN)*factor;

        double end_bucket_young = average_age/5;
        double end_bucket_ma    = end_bucket_young*4;

        assert(c <= 255);
        if (n < end_bucket_young) {
          yf  += 1;
          SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.3*c, 0.3*c, 0.8*c));
        }
        else if (n < end_bucket_ma) {
          maf += 1;
          SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.35*c, 0.25*c, 0.7*c));
        }
        else {
          of  += 1;
          if (n < MIN_LIFESPAN) {
            SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.35*c, 0.25*c, 0.7*c));
          }
          else {
            SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0.8*c, 0.2*c, 0.4*c));
          }
        }

        arrs[next][x + y*N_CELLS] = alive_next_cycle(arrs[which], x, y);
        average_age = (0.1)*(arrs[next][x+y*N_CELLS]) + (1 - 0.1)*average_age;
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
      SDL_BlitSurface(surfaceMessage, NULL, s, &(struct SDL_Rect){.x = 0, .y = 0, .h = 10, .w = 20});
    }

    float bar_chart_scale = 100 /* px */;

    char b2[1024];
    sprintf(b2, "avg: %.3f", average_age);
    SDL_Surface* label = TTF_RenderText_Blended(font, b2, white);
    SDL_BlitSurface(label, NULL, s, &(struct SDL_Rect){.x = 0, .y = WINDOW_SIZE-100, .h = 10, .w = 20});
    SDL_FreeSurface(label);

    // FIXME don't allcocate and free in render loop dummy
    label = TTF_RenderText_Blended(font, "Young", white);
    SDL_BlitSurface(label, NULL, s, &(struct SDL_Rect){.x = 56, .y = WINDOW_SIZE-80, .h = 10, .w = 20});
    SDL_FreeSurface(label);

    SDL_FillRect(s, &(struct SDL_Rect){.x = 110, .y = WINDOW_SIZE-70, .h = 10, .w = bar_chart_scale*yf}, SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF));

    label = TTF_RenderText_Blended(font, "Middle Aged", white);
    SDL_BlitSurface(label, NULL, s, &(struct SDL_Rect){.x = 1, .y = WINDOW_SIZE-60, .h = 10, .w = 20});
    SDL_FreeSurface(label);

    SDL_FillRect(s, &(struct SDL_Rect){.x = 110, .y = WINDOW_SIZE-50, .h = 10, .w = bar_chart_scale*maf}, SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF));

    label = TTF_RenderText_Blended(font, "Old", white);
    SDL_BlitSurface(label, NULL, s, &(struct SDL_Rect){.x = 71, .y = WINDOW_SIZE-40, .h = 10, .w = 20});
    SDL_FreeSurface(label);

    SDL_FillRect(s, &(struct SDL_Rect){.x = 110, .y = WINDOW_SIZE-30, .h = 10, .w = bar_chart_scale*of}, SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF));

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
      fps2 = (0.7)*(1e9 / (stop_render - start)) - (1. - 0.7)*fps;
      sprintf(fps_buffer, "fps: %.3f, compute %.3f", fps, fps2);
      if (surfaceMessage) SDL_FreeSurface(surfaceMessage);
      surfaceMessage = TTF_RenderText_Blended(font, fps_buffer, white);
      last_update = stop;
    }
  }

#ifdef JACK
  jack_client_close(cl);
#endif // JACK
  SDL_DestroyWindow(w);
  SDL_Quit();

  return 0;
}
