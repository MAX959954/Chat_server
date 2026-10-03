/* Growable byte FIFO: append at the back, consume from the front. */
#ifndef CHAT_BUFFER_H
#define CHAT_BUFFER_H

#include <stddef.h>

struct buffer {
    char  *data;
    size_t off;  /* start of unconsumed bytes */
    size_t len;  /* end of valid bytes */
    size_t cap;
};

void        buffer_init(struct buffer *b);
void        buffer_free(struct buffer *b);

/* Returns 0 on success, -1 if memory could not be allocated. */
int         buffer_append(struct buffer *b, const void *src, size_t n);

size_t      buffer_size(const struct buffer *b);
const char *buffer_peek(const struct buffer *b);
void        buffer_consume(struct buffer *b, size_t n);

#endif
