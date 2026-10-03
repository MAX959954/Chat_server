#include "check.h"
#include "ratelimit.h"

static void test_burst_then_limit(void)
{
    struct token_bucket tb;
    tb_init(&tb, 2.0, 5.0, 1000);  /* 2 msg/s, burst 5 */

    for (int i = 0; i < 5; i++)
        CHECK(tb_take(&tb, 1000));  /* full bucket allows the burst */
    CHECK(!tb_take(&tb, 1000));     /* then empty */
    CHECK(!tb_take(&tb, 1400));     /* 0.8 tokens after 400 ms: not enough */
    CHECK(tb_take(&tb, 1500));      /* 1.0 token after 500 ms */
    CHECK(!tb_take(&tb, 1500));
}

static void test_refill_is_capped(void)
{
    struct token_bucket tb;
    tb_init(&tb, 10.0, 3.0, 0);
    for (int i = 0; i < 3; i++)
        CHECK(tb_take(&tb, 0));
    /* a long idle period refills only up to the burst size */
    int allowed = 0;
    for (int i = 0; i < 10; i++)
        allowed += tb_take(&tb, 60000);
    CHECK(allowed == 3);
}

static void test_sustained_rate(void)
{
    struct token_bucket tb;
    tb_init(&tb, 5.0, 1.0, 0);
    /* try every 10 ms for 10 s: about 5/s get through, plus the initial token */
    int allowed = 0;
    for (uint64_t t = 0; t <= 10000; t += 10)
        allowed += tb_take(&tb, t);
    CHECK(allowed >= 50 && allowed <= 52);
}

static void test_clock_going_backwards(void)
{
    struct token_bucket tb;
    tb_init(&tb, 1.0, 1.0, 5000);
    CHECK(tb_take(&tb, 5000));
    CHECK(!tb_take(&tb, 4000));  /* earlier timestamp must not add tokens */
}

int main(void)
{
    test_burst_then_limit();
    test_refill_is_capped();
    test_sustained_rate();
    test_clock_going_backwards();
    return CHECK_REPORT();
}
