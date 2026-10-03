/* Minimal timestamped logging to stderr. */
#ifndef CHAT_LOG_H
#define CHAT_LOG_H

#include <errno.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define CHAT_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define CHAT_PRINTF(fmt, args)
#endif

void log_write(const char *level, const char *fmt, ...) CHAT_PRINTF(2, 3);

#define LOG_INFO(...)  log_write("INFO", __VA_ARGS__)
#define LOG_ERROR(...) log_write("ERROR", __VA_ARGS__)
/* Log `what` together with the current errno. */
#define LOG_ERRNO(what) log_write("ERROR", "%s: %s", (what), strerror(errno))

#endif
