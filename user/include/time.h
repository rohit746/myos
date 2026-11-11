#pragma once

#include <sys/types.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t clock, struct timespec *ts);
