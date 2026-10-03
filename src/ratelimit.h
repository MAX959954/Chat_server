/*
 * Token bucket rate limiter.
 *
 * The bucket holds up to `burst` tokens and refills at `rate` tokens per
 * second. Each message costs one token; with no token left the message is
 * rejected. This allows short bursts (pasting a few lines) while capping the
 * sustained rate. Time is passed in explicitly so the logic is unit-testable.
 */
#ifndef CHAT_RATELIMIT_H
#define CHAT_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

struct token_bucket {
    double   tokens;
    double   rate;     /* tokens per second */
    double   burst;    /* capacity */
    uint64_t last_ms;
};

void tb_init(struct token_bucket *tb, double rate, double burst, uint64_t now_ms);

/* Take one token if available. */
bool tb_take(struct token_bucket *tb, uint64_t now_ms);

/* Milliseconds from a monotonic clock. */
uint64_t monotonic_ms(void);

#endif
