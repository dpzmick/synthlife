#include "clock.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

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
#define WINDOW_SIZE 1024
#define N_CELLS     512

static_assert(WINDOW_SIZE % N_CELLS == 0, "Window Size must evenly divide number of cells");
#define CELL_SIZE (WINDOW_SIZE / N_CELLS)

static struct SDL_Rect const* temp_cell_rect(int x, int y)
{
  static struct SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = CELL_SIZE;
  rect.h = CELL_SIZE;
  return &rect;
}

int alive_next_cycle(int* cells, size_t my_x, size_t my_y)
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

      alive_neighbors += cells[my_x + xoffs[xx] + (my_y + yoffs[yy])*N_CELLS];
    }
  }

  // alive
  if (cells[my_x + my_y*N_CELLS]) {
    if (alive_neighbors <= 1) return false;
    if (alive_neighbors >= 4) return false;
    else                      return true;
  }
  else {
    if (alive_neighbors == 3) return true;
    else                      return false;
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

  bool* arr1 = malloc(sizeof(bool) * N_CELLS * N_CELLS);
  if (!arr1) {
    printf("Allocation failed\n");
    return 1;
  }

  bool* arr2 = malloc(sizeof(bool) * N_CELLS * N_CELLS);
  if (!arr2) {
    printf("Allocation failed\n");
    return 1;
  }

  // double buffer
  bool*  arrs[2] = {arr1, arr2};
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

  char fps_buffer[1024];
  atomic_store(&running, true);
  while (atomic_load(&running)) {
    uint64_t start = wallclock();
    s = SDL_GetWindowSurface(w);
    SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0)); // make entire thing black

    for (size_t x = 0; x < N_CELLS; ++x) {
      for (size_t y = 0; y < N_CELLS; ++y) {
        if (arrs[which][x + y*N_CELLS]) {
          // each cell is CELL_SIZExCELL_SIZE
          SDL_FillRect(s, temp_cell_rect(x*CELL_SIZE, y*CELL_SIZE), SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF));
        }
      }
    }
    uint64_t stop_render = wallclock();

    size_t next = which == 1 ? 1 : 0;
    for (size_t x = 0; x < N_CELLS; ++x) {
      for (size_t y = 0; y < N_CELLS; ++y) {
        arrs[next][x + y*N_CELLS] = alive_next_cycle(arrs[which], x, y);
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
    // SDL_Delay(5);

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

    SDL_Delay(500);
  }

  SDL_DestroyWindow(w);
  SDL_Quit();

  return 0;
}
