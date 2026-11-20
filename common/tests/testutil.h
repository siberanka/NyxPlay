#pragma once

// Minimal assertion helpers for the library tests. Every test program prints one
// "RESULT <suite> <passed|failed> <checks> <failures>" line that run-tests.sh collects.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static int testChecks;
static int testFailures;

#define CHECK(cond) \
    do { \
        testChecks++; \
        if (!(cond)) { \
            testFailures++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            fflush(stdout); \
        } \
    } while (0)

#define CHECK_EQ_INT(actual, expected) \
    do { \
        long long a_ = (long long)(actual); \
        long long e_ = (long long)(expected); \
        testChecks++; \
        if (a_ != e_) { \
            testFailures++; \
            printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); \
            fflush(stdout); \
        } \
    } while (0)

#define RUN(fn) \
    do { \
        int before_ = testFailures; \
        printf("[ RUN  ] %s\n", #fn); \
        fflush(stdout); \
        fn(); \
        printf("[ %s ] %s\n", testFailures == before_ ? "  OK " : "FAIL ", #fn); \
        fflush(stdout); \
    } while (0)

static int testFinish(const char* suite) {
    printf("RESULT %s %s %d %d\n", suite, testFailures == 0 ? "passed" : "failed", testChecks, testFailures);
    fflush(stdout);
    return testFailures == 0 ? 0 : 1;
}

static void hexToBytes(const char* hex, uint8_t* out, size_t* outLen) {
    size_t n = 0;
    while (hex[0] && hex[1]) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    *outLen = n;
}
