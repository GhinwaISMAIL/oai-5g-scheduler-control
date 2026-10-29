#ifndef STUB_LOG_H
#define STUB_LOG_H
#include <stdio.h>
enum { NR_MAC = 1 };
#define LOG_I(c, ...) fprintf(stderr, "[I] " __VA_ARGS__)
#define LOG_W(c, ...) fprintf(stderr, "[W] " __VA_ARGS__)
#define LOG_E(c, ...) fprintf(stderr, "[E] " __VA_ARGS__)
#endif
