#pragma once

#include <stdint.h>
#include <time.h>

static inline uint64_t wallclock()
{
  struct timespec time[1];
  clock_gettime(CLOCK_REALTIME, time);

  return time->tv_nsec + 1000000000*time->tv_sec;
}
