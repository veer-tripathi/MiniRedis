#include "logging.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>

void msg(const char *m) {
    fprintf(stderr, "%s\n", m);
}

void msg_errno(const char *m) {
    fprintf(stderr, "[errno:%d] %s\n", errno, m);
}

void die(const char *m) {
    fprintf(stderr, "[%d] %s\n", errno, m);
    abort();
}
