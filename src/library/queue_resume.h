#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ** paths;
    int * order;
    int * continuation;
    int count, current, pending, mode, shuffle_pos;
    double position;
} queue_resume_t;
void queue_resume_free(queue_resume_t * state);
bool queue_resume_write(const char * path, const queue_resume_t * state);
/* Publish `name` inside an already-open directory.  The descriptor pins the
 * mount generation for the complete temporary-file/write/rename/cleanup
 * sequence; callers may run this from a worker without re-resolving a path. */
bool queue_resume_write_at(int dirfd, const char * name, const queue_resume_t * state);
bool queue_resume_read(const char * path, queue_resume_t * state);
