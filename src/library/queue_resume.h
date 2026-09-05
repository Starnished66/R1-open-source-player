#pragma once
#include <stdbool.h>

typedef struct {
    char ** paths;
    int * order;
    int * continuation;
    int count, current, pending, mode, shuffle_pos;
    double position;
} queue_resume_t;
void queue_resume_free(queue_resume_t * state);
bool queue_resume_write(const char * path, const queue_resume_t * state);
bool queue_resume_read(const char * path, queue_resume_t * state);
