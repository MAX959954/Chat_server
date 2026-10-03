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

static struct command parse(char *buf, const char *line)
{
    struct command cmd;
    strcpy(buf, line);
    protocol_parse(buf, &cmd);
    return cmd;
}

static void test_parse(void)
{
    char b[256];
    struct command c;

    CHECK(parse(b, "").type == CMD_EMPTY);

    c = parse(b, "hello world");
    CHECK(c.type == CMD_MESSAGE);
    CHECK_STR(c.text, "hello world");

    c = parse(b, "//not a command");             /* escaped slash */
    CHECK(c.type == CMD_MESSAGE);
    CHECK_STR(c.text, "/not a command");

    c = parse(b, "/nick alice");
    CHECK(c.type == CMD_NICK && c.usage == NULL);
    CHECK_STR(c.arg, "alice");
    CHECK(parse(b, "/nick").usage != NULL);        /* missing argument */
    CHECK(parse(b, "/nick a b").usage != NULL);    /* extra argument */

    c = parse(b, "/msg  bob   hi there ");
    CHECK(c.type == CMD_MSG && c.usage == NULL);
    CHECK_STR(c.arg, "bob");
    CHECK_STR(c.text, "hi there ");
    CHECK(parse(b, "/msg bob").usage != NULL);     /* no text */

    c = parse(b, "/join dev");
    CHECK(c.type == CMD_JOIN);
    CHECK_STR(c.arg, "dev");

    CHECK(parse(b, "/list").type == CMD_LIST);
    CHECK(parse(b, "/list x").usage != NULL);
    CHECK(parse(b, "/rooms").type == CMD_ROOMS);
    CHECK(parse(b, "/help").type == CMD_HELP);

    c = parse(b, "/quit");
    CHECK(c.type == CMD_QUIT && c.usage == NULL);
    CHECK_STR(c.text, "");
    c = parse(b, "/quit see you later");
    CHECK_STR(c.text, "see you later");

    c = parse(b, "/shutdown");
    CHECK(c.type == CMD_SHUTDOWN && c.arg == NULL && c.usage == NULL);
    c = parse(b, "/shutdown s3cret");
    CHECK_STR(c.arg, "s3cret");

    c = parse(b, "/nope arg");
    CHECK(c.type == CMD_UNKNOWN);
    CHECK_STR(c.name, "nope");
    CHECK(parse(b, "/").type == CMD_UNKNOWN);
    CHECK(parse(b, "/NICK x").type == CMD_UNKNOWN); /* commands are lowercase */
}

static void test_names(void)
{
    CHECK(protocol_valid_name("alice"));
    CHECK(protocol_valid_name("Bob_2-x"));
    CHECK(protocol_valid_name("abcdefghijklmnop"));    /* 16 chars */
    CHECK(!protocol_valid_name("abcdefghijklmnopq"));  /* 17 chars */
    CHECK(!protocol_valid_name(""));
    CHECK(!protocol_valid_name("1abc"));
    CHECK(!protocol_valid_name("_abc"));
    CHECK(!protocol_valid_name("a b"));
    CHECK(!protocol_valid_name("al!ce"));
}

static void test_sanitize(void)
{
    char s[] = "hi\x1b[31mred\x07\tend\x7f";
    protocol_sanitize(s);
    CHECK_STR(s, "hi?[31mred? end?");
    char utf8[] = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";  /* "привет" */
    protocol_sanitize(utf8);
    CHECK_STR(utf8, "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
}

static void test_error_names(void)
{
    CHECK_STR(proto_error_name(ERR_NICK_IN_USE), "NICK_IN_USE");
    CHECK_STR(proto_error_name(ERR_RATE_LIMITED), "RATE_LIMITED");
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
    test_names();
    test_sanitize();
    test_error_names();
    return CHECK_REPORT();
}
