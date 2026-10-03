#include "buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { BUFFER_MIN_CAP = 256 };

void buffer_init(struct buffer *b)
{
    b->data = NULL;
    b->off = b->len = b->cap = 0;
}

void buffer_free(struct buffer *b)
{
    free(b->data);
    buffer_init(b);
}

size_t buffer_size(const struct buffer *b)
{
    return b->len - b->off;
}

const char *buffer_peek(const struct buffer *b)
{
    return b->data ? b->data + b->off : "";
}

void buffer_consume(struct buffer *b, size_t n)
{
    if (n >= buffer_size(b))
        b->off = b->len = 0;  /* empty: rewind instead of moving memory */
    else
        b->off += n;
}

int buffer_append(struct buffer *b, const void *src, size_t n)
{
    if (n == 0)
        return 0;

    if (b->cap - b->len < n && b->off > 0) {
        /* reclaim the consumed prefix before growing */
        memmove(b->data, b->data + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
    }
    if (b->cap - b->len < n) {
        if (n > SIZE_MAX - b->len)
            return -1;
        size_t need = b->len + n;
        size_t cap = b->cap ? b->cap : BUFFER_MIN_CAP;
        while (cap < need) {
            if (cap > SIZE_MAX / 2)
                return -1;
            cap *= 2;
        }
        char *p = realloc(b->data, cap);
        if (p == NULL)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}
