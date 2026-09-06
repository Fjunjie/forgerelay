/*
 * fr_error.c - 稳定错误码实现（DECISIONS D-01）。
 */
#include "forgerelay/fr_error.h"

#include <stdio.h>
#include <string.h>

const char *fr_status_name(fr_status code) {
    switch (code) {
    case FR_OK:
        return "FR_OK";
    case FR_E_ARG:
        return "FR_E_ARG";
    case FR_E_RANGE:
        return "FR_E_RANGE";
    case FR_E_OVERFLOW:
        return "FR_E_OVERFLOW";
    case FR_E_NOMEM:
        return "FR_E_NOMEM";
    case FR_E_IO:
        return "FR_E_IO";
    case FR_E_EOF:
        return "FR_E_EOF";
    case FR_E_PROTOCOL:
        return "FR_E_PROTOCOL";
    case FR_E_TRUNCATED:
        return "FR_E_TRUNCATED";
    case FR_E_STATE:
        return "FR_E_STATE";
    case FR_E_NOTFOUND:
        return "FR_E_NOTFOUND";
    case FR_E_EXISTS:
        return "FR_E_EXISTS";
    case FR_E_LIMIT:
        return "FR_E_LIMIT";
    case FR_E_BUSY:
        return "FR_E_BUSY";
    case FR_E_INTERNAL:
        return "FR_E_INTERNAL";
    default:
        return "FR_E_UNKNOWN";
    }
}

const char *fr_status_message(fr_status code) {
    switch (code) {
    case FR_OK:
        return "success";
    case FR_E_ARG:
        return "invalid argument";
    case FR_E_RANGE:
        return "value out of range";
    case FR_E_OVERFLOW:
        return "arithmetic or capacity overflow";
    case FR_E_NOMEM:
        return "out of memory";
    case FR_E_IO:
        return "I/O error";
    case FR_E_EOF:
        return "unexpected end of input";
    case FR_E_PROTOCOL:
        return "protocol violation";
    case FR_E_TRUNCATED:
        return "input truncated";
    case FR_E_STATE:
        return "invalid state";
    case FR_E_NOTFOUND:
        return "not found";
    case FR_E_EXISTS:
        return "already exists";
    case FR_E_LIMIT:
        return "limit exceeded";
    case FR_E_BUSY:
        return "busy";
    case FR_E_INTERNAL:
        return "internal error";
    default:
        return "unknown error";
    }
}

fr_category fr_status_category(fr_status code) {
    switch (code) {
    case FR_OK:
        return FR_CAT_OK;
    case FR_E_ARG:
        return FR_CAT_ARGUMENT;
    case FR_E_RANGE:
        return FR_CAT_RANGE;
    case FR_E_NOMEM:
    case FR_E_LIMIT:
        return FR_CAT_RESOURCE;
    case FR_E_IO:
        return FR_CAT_IO;
    case FR_E_PROTOCOL:
    case FR_E_TRUNCATED:
        return FR_CAT_PROTOCOL;
    case FR_E_EOF:
    case FR_E_STATE:
    case FR_E_NOTFOUND:
    case FR_E_EXISTS:
    case FR_E_BUSY:
        return FR_CAT_STATE;
    case FR_E_OVERFLOW:
    case FR_E_INTERNAL:
    default:
        return FR_CAT_INTERNAL;
    }
}

void fr_error_set(fr_error *err, fr_status code, const char *fmt, ...) {
    if (err == NULL) {
        return;
    }
    err->code = code;
    if (fmt == NULL) {
        err->detail[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(err->detail, sizeof(err->detail), fmt, args);
    va_end(args);
}

void fr_error_set_errno(fr_error *err, fr_status code, int errno_value, const char *what) {
    if (err == NULL) {
        return;
    }
    err->code = code;
    /* strerror 输出由 C 库管理；截断由 snprintf 保证。 */
    (void)snprintf(err->detail, sizeof(err->detail), "%s: %s (errno=%d)",
                   what != NULL ? what : "syscall", strerror(errno_value), errno_value);
}

void fr_error_clear(fr_error *err) {
    if (err == NULL) {
        return;
    }
    err->code = FR_OK;
    err->detail[0] = '\0';
}
