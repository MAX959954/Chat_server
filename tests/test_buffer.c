#include "buffer.h"
#include "check.h"

static void test_empty(void)
{
    struct buffer b;
    buffer_init(&b);
    CHECK(buffer_size(&b) == 0);
    CHECK_STR(buffer_peek(&b), "");
    buffer_consume(&b, 10);  /* consuming from empty is harmless */
    CHECK(buffer_size(&b) == 0);
    buffer_free(&b);
}

static void test_append_consume(void)
{
    struct buffer b;
    buffer_init(&b);
    CHECK(buffer_append(&b, "hello ", 6) == 0);
    CHECK(buffer_append(&b, "world", 5) == 0);
    CHECK(buffer_size(&b) == 11);
    CHECK(memcmp(buffer_peek(&b), "hello world", 11) == 0);

    buffer_consume(&b, 6);
    CHECK(buffer_size(&b) == 5);
    CHECK(memcmp(buffer_peek(&b), "world", 5) == 0);

    buffer_consume(&b, 5);
    CHECK(buffer_size(&b) == 0);
    buffer_free(&b);
}

static void test_growth_and_compaction(void)
{
    struct buffer b;
    buffer_init(&b);
    char chunk[100];
    for (int i = 0; i < 100; i++)
        chunk[i] = (char)('a' + i % 26);

    /* interleave appends and partial consumes so the consumed prefix gets
     * reclaimed and the buffer also has to grow */
    size_t expected = 0;
    for (int round = 0; round < 1000; round++) {
        CHECK(buffer_append(&b, chunk, sizeof chunk) == 0);
        expected += sizeof chunk;
        buffer_consume(&b, 60);
        expected -= 60;
    }
    CHECK(buffer_size(&b) == expected);
    /* after consuming 60 per 100-byte chunk the data must still be the
     * repeating pattern, starting at the right offset */
    size_t start = (1000u * 60u) % sizeof chunk;
    CHECK(buffer_peek(&b)[0] == chunk[start]);
    buffer_free(&b);
}

int main(void)
{
    test_empty();
    test_append_consume();
    test_growth_and_compaction();
    return CHECK_REPORT();
}
