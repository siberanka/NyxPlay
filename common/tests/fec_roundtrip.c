// Reed-Solomon FEC round trip between the two nanors versions in the tree:
//   host:    host/third-party/nanors (19f07b5) through host/src/rswrapper.c, every ISA variant
//   library: common/moonlight-common-c/nanors (a9b1636)
// The host encodes video and audio FEC, the clients decode it. Both must agree bit for bit.
//
// Build once with -DHOST_NANORS and once without, then:
//   fec gen <file>      write deterministic data and the parity of this implementation
//   fec check <file>    verify a file written by any implementation: the local parity must be
//                       identical and shards lost from the file must be recovered exactly

#include "testutil.h"

#include <stdint.h>

#ifdef HOST_NANORS
#include "rswrapper.h"
#define IMPL_NAME "host"
#else
#include <rs.h>
#define IMPL_NAME "library"
#endif

typedef struct {
    const char* name;
    void* (*create)(int, int);
    void (*release)(void*);
    int (*encode)(void*, uint8_t**, int, int);
    int (*decode)(void*, uint8_t**, uint8_t*, int, int);
} VARIANT;

#ifdef HOST_NANORS
#define DECLARE_ISA(sfx) \
    reed_solomon* reed_solomon_new_##sfx(int, int); \
    void reed_solomon_release_##sfx(reed_solomon*); \
    int reed_solomon_encode_##sfx(reed_solomon*, uint8_t**, int, int); \
    int reed_solomon_decode_##sfx(reed_solomon*, uint8_t**, uint8_t*, int, int); \
    void reed_solomon_init_##sfx(void);
#undef reed_solomon_new
#undef reed_solomon_release
#undef reed_solomon_encode
#undef reed_solomon_decode
DECLARE_ISA(def)
#if defined(__x86_64__)
DECLARE_ISA(ssse3)
DECLARE_ISA(avx2)
DECLARE_ISA(avx512)
#endif
#define MAKE_VARIANT(n, sfx) { n, (void* (*)(int, int))reed_solomon_new_##sfx, (void (*)(void*))reed_solomon_release_##sfx, \
                               (int (*)(void*, uint8_t**, int, int))reed_solomon_encode_##sfx, \
                               (int (*)(void*, uint8_t**, uint8_t*, int, int))reed_solomon_decode_##sfx }
static VARIANT variants[4];
static int variantCount;

static void initVariants(void) {
    VARIANT def = MAKE_VARIANT("host-def", def);
    variants[variantCount++] = def;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("ssse3")) {
        VARIANT v = MAKE_VARIANT("host-ssse3", ssse3);
        reed_solomon_init_ssse3();
        variants[variantCount++] = v;
    }
    if (__builtin_cpu_supports("avx2")) {
        VARIANT v = MAKE_VARIANT("host-avx2", avx2);
        reed_solomon_init_avx2();
        variants[variantCount++] = v;
    }
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        VARIANT v = MAKE_VARIANT("host-avx512", avx512);
        reed_solomon_init_avx512();
        variants[variantCount++] = v;
    }
#endif
    reed_solomon_init_def();
}
#else
static VARIANT variants[1];
static int variantCount;

static void initVariants(void) {
    VARIANT v = { "library", (void* (*)(int, int))reed_solomon_new, (void (*)(void*))reed_solomon_release,
                  (int (*)(void*, uint8_t**, int, int))reed_solomon_encode,
                  (int (*)(void*, uint8_t**, uint8_t*, int, int))reed_solomon_decode };
    reed_solomon_init();
    variants[variantCount++] = v;
}
#endif

typedef struct {
    int ds, ps, bs;
} GEOMETRY;

// Video: data shards up to 255 minus parity, block size is the RTP payload size.
// Audio: 4 data and 2 parity shards.
static const GEOMETRY geometries[] = {
    { 1, 1, 1400 }, { 2, 1, 1392 }, { 4, 1, 1408 }, { 4, 2, 1400 }, { 4, 2, 480 }, { 4, 2, 64 },
    { 10, 3, 1392 }, { 20, 5, 1360 }, { 50, 17, 1392 }, { 100, 28, 1392 }, { 128, 127, 1392 },
    { 200, 55, 1392 }, { 254, 1, 1392 }, { 3, 2, 1 }, { 7, 4, 17 }, { 16, 8, 100 }, { 60, 40, 1456 },
};
#define GEOMETRY_COUNT ((int)(sizeof(geometries) / sizeof(geometries[0])))

static uint64_t rngState;
static uint32_t rnd(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return (uint32_t)(rngState >> 16);
}

static void seedFor(const GEOMETRY* g) {
    rngState = 0x9E3779B97F4A7C15ull ^ ((uint64_t)g->ds << 40) ^ ((uint64_t)g->ps << 20) ^ (uint64_t)g->bs;
    for (int i = 0; i < 8; i++) {
        rnd();
    }
}

static uint8_t** allocShards(int n, int bs) {
    uint8_t** s = calloc(n, sizeof(*s));
    for (int i = 0; i < n; i++) {
        s[i] = malloc(bs);
    }
    return s;
}

static void freeShards(uint8_t** s, int n) {
    for (int i = 0; i < n; i++) {
        free(s[i]);
    }
    free(s);
}

static void encodeWith(const VARIANT* v, const GEOMETRY* g, uint8_t** shards) {
    void* rs = v->create(g->ds, g->ps);
    CHECK(rs != NULL);
    if (rs != NULL) {
        CHECK_EQ_INT(v->encode(rs, shards, g->ds + g->ps, g->bs), 0);
        v->release(rs);
    }
}

static int generate(const char* path) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return 1;
    }
    for (int gi = 0; gi < GEOMETRY_COUNT; gi++) {
        const GEOMETRY* g = &geometries[gi];
        int total = g->ds + g->ps;
        uint8_t** shards = allocShards(total, g->bs);
        int32_t hdr[3] = { g->ds, g->ps, g->bs };

        seedFor(g);
        for (int i = 0; i < g->ds; i++) {
            for (int b = 0; b < g->bs; b++) {
                shards[i][b] = (uint8_t)rnd();
            }
        }
        // The first variant is the one the clients would be paired with, the others are compared in check
        encodeWith(&variants[variantCount - 1], g, shards);
        fwrite(hdr, sizeof(hdr), 1, f);
        for (int i = 0; i < total; i++) {
            fwrite(shards[i], 1, g->bs, f);
        }
        freeShards(shards, total);
    }
    fclose(f);
    return 0;
}

static void checkGeometry(const VARIANT* v, int ds, int ps, int bs, uint8_t** original) {
    int total = ds + ps;
    uint8_t** work = allocShards(total, bs);
    uint8_t* marks = malloc(total);
    void* rs;
    int patterns = 0;

    // Parity of the file must equal the parity this implementation computes from the same data
    for (int i = 0; i < ds; i++) {
        memcpy(work[i], original[i], bs);
    }
    encodeWith(v, &(GEOMETRY){ ds, ps, bs }, work);
    for (int i = ds; i < total; i++) {
        CHECK(memcmp(work[i], original[i], bs) == 0);
    }

    rs = v->create(ds, ps);
    CHECK(rs != NULL);
    for (int losses = 1; losses <= ps && rs != NULL; losses++) {
        // Few patterns for large geometries to keep the run short
        int rounds = (total > 100) ? 6 : 24;
        for (int round = 0; round < rounds; round++) {
            int lost = 0;
            int ok = 1;

            memset(marks, 0, total);
            while (lost < losses) {
                int idx = (round == 0) ? lost : (int)(rnd() % (uint32_t)total); // round 0: the first data shards
                if (!marks[idx]) {
                    marks[idx] = 1;
                    lost++;
                }
            }
            for (int i = 0; i < total; i++) {
                if (marks[i]) {
                    memset(work[i], 0xEE, bs);
                }
                else {
                    memcpy(work[i], original[i], bs);
                }
            }
            CHECK_EQ_INT(v->decode(rs, work, marks, total, bs), 0);
            for (int i = 0; i < ds; i++) {
                if (memcmp(work[i], original[i], bs) != 0) {
                    ok = 0;
                }
            }
            CHECK(ok);
            patterns++;
        }
    }
    if (rs != NULL) {
        v->release(rs);
    }
    free(marks);
    freeShards(work, total);
    (void)patterns;
}

static int check(const char* path) {
    FILE* f = fopen(path, "rb");
    int32_t hdr[3];
    int geometriesSeen = 0;

    if (f == NULL) {
        printf("FAIL cannot open %s\n", path);
        return 1;
    }
    while (fread(hdr, sizeof(hdr), 1, f) == 1) {
        int ds = hdr[0], ps = hdr[1], bs = hdr[2];
        int total = ds + ps;
        uint8_t** original = allocShards(total, bs);
        int complete = 1;

        for (int i = 0; i < total; i++) {
            if (fread(original[i], 1, bs, f) != (size_t)bs) {
                complete = 0;
            }
        }
        CHECK(complete);
        if (complete) {
            for (int vi = 0; vi < variantCount; vi++) {
                seedFor(&(GEOMETRY){ ds, ps, bs });
                for (int k = 0; k < 8; k++) {
                    rnd();
                }
                checkGeometry(&variants[vi], ds, ps, bs, original);
            }
        }
        freeShards(original, total);
        geometriesSeen++;
    }
    fclose(f);
    CHECK_EQ_INT(geometriesSeen, GEOMETRY_COUNT);
    printf("fec %s implementation checked %d geometries with %d variant(s) against %s\n", IMPL_NAME, geometriesSeen, variantCount, path);
    return 0;
}

int main(int argc, char** argv) {
    int rc;

    if (argc != 3) {
        printf("usage: fec gen|check <file>\n");
        return 2;
    }
    initVariants();
    if (strcmp(argv[1], "gen") == 0) {
        rc = generate(argv[2]);
        printf("fec %s generated %s\n", IMPL_NAME, argv[2]);
        if (rc != 0) {
            return rc;
        }
        return testFinish(IMPL_NAME "_fec_gen");
    }
    rc = check(argv[2]);
    if (rc != 0) {
        return rc;
    }
    return testFinish(IMPL_NAME "_fec_check");
}
