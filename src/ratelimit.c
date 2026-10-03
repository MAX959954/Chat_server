#include "ratelimit.h"

#include <time.h>

void tb_init(struct token_bucket *tb, double rate, double burst, uint64_t now_ms)
{
    tb->rate = rate;
    tb->burst = burst;
    tb->tokens = burst;  /* start full */
    tb->last_ms = now_ms;
}

bool tb_take(struct token_bucket *tb, uint64_t now_ms)
{
    if (now_ms > tb->last_ms) {
        tb->tokens += (double)(now_ms - tb->last_ms) * tb->rate / 1000.0;
        if (tb->tokens > tb->burst)
            tb->tokens = tb->burst;
        tb->last_ms = now_ms;
    }
    if (tb->tokens < 1.0)
        return false;
    tb->tokens -= 1.0;
    return true;
}

uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
