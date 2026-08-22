#ifndef STUB_LOG_H
#define STUB_LOG_H
#include <stdio.h>
#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#ifdef TEST_VERBOSE
#define LOG_ERR(fmt, ...) fprintf(stderr, "ERR: " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) fprintf(stderr, "WRN: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) fprintf(stderr, "INF: " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) fprintf(stderr, "DBG: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(...)
#define LOG_WRN(...)
#define LOG_INF(...)
#define LOG_DBG(...)
#endif
#endif
