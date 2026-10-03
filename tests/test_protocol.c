#include "check.h"
#include "protocol.h"

/* Simulate recv(): copy `data` into the reader's free space. */
static void feed(struct line_reader *lr, const char *data)
{
    char  *dst;
    size_t n = strlen(data);
    size_t space = line_reader_space(lr, &dst);
    CHECK(n <= space);
    memcpy(dst, data, n);
    line_reader_commit(lr, n);
}

static void test_single_line(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line;
    size_t len;

    feed(&lr, "hello\n");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "hello");
    CHECK(len == 5);
    CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
}

static void test_split_across_reads(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line;
    size_t len;

    feed(&lr, "hel");
    CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
    feed(&lr, "lo wor");
    CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
    feed(&lr, "ld\n");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "hello world");
}

static void test_coalesced_lines(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line;
    size_t len;

    feed(&lr, "one\ntwo\r\nthree\npart");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "one");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "two");  /* CRLF stripped */
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "three");
    CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
    feed(&lr, "ial\n");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK_STR(line, "partial");
}

static void test_empty_lines(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line;
    size_t len;

    feed(&lr, "\n\r\n");
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK(len == 0);
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK(len == 0);
    CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
}

static void test_too_long(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line, *dst;
    size_t len;

    /* exactly the maximum: PROTO_MAX_LINE - 1 bytes plus '\n' is accepted */
    size_t space = line_reader_space(&lr, &dst);
    CHECK(space == PROTO_MAX_LINE);
    memset(dst, 'x', space - 1);
    dst[space - 1] = '\n';
    line_reader_commit(&lr, space);
    CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
    CHECK(len == PROTO_MAX_LINE - 1);

    /* one byte more without a newline is a violation */
    space = line_reader_space(&lr, &dst);
    CHECK(space == PROTO_MAX_LINE);
    memset(dst, 'y', space);
    line_reader_commit(&lr, space);
    CHECK(line_reader_next(&lr, &line, &len) == LR_TOO_LONG);
}

static void test_space_reclaimed_after_lines(void)
{
    struct line_reader lr;
    line_reader_init(&lr);
    char *line;
    size_t len;

    /* many short lines in sequence must never fill the buffer */
    for (int i = 0; i < 10000; i++) {
        feed(&lr, "ping\n");
        CHECK(line_reader_next(&lr, &line, &len) == LR_LINE);
        CHECK(line_reader_next(&lr, &line, &len) == LR_AGAIN);
    }
    char *dst;
    CHECK(line_reader_space(&lr, &dst) == PROTO_MAX_LINE);
}

static void test_parse(void)
{
    CHECK(protocol_parse("").type == CMD_EMPTY);
    CHECK(protocol_parse("hello").type == CMD_MESSAGE);
    CHECK(protocol_parse("shutdown").type == CMD_MESSAGE);  /* plain text */
    CHECK(protocol_parse("/quit").type == CMD_QUIT);
    CHECK(protocol_parse("/shutdown").type == CMD_SHUTDOWN);
    CHECK(protocol_parse("/shutdownx").type == CMD_UNKNOWN);
    CHECK(protocol_parse("/nope").type == CMD_UNKNOWN);
    CHECK_STR(protocol_parse("hi there").text, "hi there");
}

int main(void)
{
    test_single_line();
    test_split_across_reads();
    test_coalesced_lines();
    test_empty_lines();
    test_too_long();
    test_space_reclaimed_after_lines();
    test_parse();
    return CHECK_REPORT();
}
