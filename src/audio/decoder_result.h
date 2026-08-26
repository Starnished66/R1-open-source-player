#ifndef DECODER_RESULT_H
#define DECODER_RESULT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DECODER_READ_OK = 0,
    DECODER_READ_EOF,
    DECODER_READ_RECOVERABLE_ERROR,
    DECODER_READ_FATAL_ERROR
} decoder_read_status_t;

typedef struct {
    uint64_t frames;
    decoder_read_status_t status;
} decoder_read_result_t;

#endif /* DECODER_RESULT_H */
