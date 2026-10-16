/* xxscan FILE SIZE STEP HASH... : find 32-byte-aligned windows of SIZE bytes whose XXH64 (seed 0) matches
   one of the given hashes (Dolphin's dumped-texture names). */
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: xxscan FILE SIZE STEP HASH...\n"); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) return 1; fclose(f);
    long size = atol(argv[2]), step = atol(argv[3]);
    int nh = argc - 4; unsigned long long* hs = malloc(8 * nh);
    for (int i = 0; i < nh; i++) hs[i] = strtoull(argv[i + 4], 0, 16);
    for (long o = 0; o + size <= n; o += step) {
        unsigned long long h = XXH64(b + o, size, 0);
        for (int i = 0; i < nh; i++) if (h == hs[i]) printf("%s: offset %08lX matches %016llx\n", argv[1], o, h);
    }
    return 0;
}
