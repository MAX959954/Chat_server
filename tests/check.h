/* Tiny assertion helpers for unit tests (no external framework needed). */
#ifndef CHAT_CHECK_H
#define CHAT_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                     \
                    __FILE__, __LINE__, #cond);                              \
            check_failures++;                                                \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *a_ = (actual), *e_ = (expected);                         \
        if (strcmp(a_, e_) != 0) {                                           \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n",          \
                    __FILE__, __LINE__, e_, a_);                             \
            check_failures++;                                                \
        }                                                                    \
    } while (0)

#define CHECK_REPORT()                                                       \
    (check_failures == 0                                                     \
         ? (printf("all checks passed\n"), 0)                                \
         : (printf("%d check(s) failed\n", check_failures), 1))

#endif
