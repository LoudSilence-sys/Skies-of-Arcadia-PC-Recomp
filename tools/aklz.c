/* aklz.c - AKLZ (Sega LZSS) decompressor: aklz in out */
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); if (!f) return 1; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* d = malloc(n); fread(d, 1, n, f); fclose(f);
    if (n < 16 || d[0] != 'A' || d[1] != 'K' || d[2] != 'L' || d[3] != 'Z') return 2;
    unsigned size = d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15];
    unsigned char* o = calloc(1, size + 32); unsigned op = 0; long i = 16;
    while (op < size && i < n) {
        int fl = d[i++];
        for (int b = 0; b < 8 && op < size && i < n; b++) {
            if (fl >> b & 1) o[op++] = d[i++];
            else { int lo = d[i], hi = d[i + 1]; i += 2; int off = lo | ((hi & 0xF0) << 4), ln = (hi & 15) + 3;
                   long src = (long)op - (((long)op - (off + 0x12)) & 0xFFF);
                   for (int k = 0; k < ln && op < size; k++, src++) o[op++] = src >= 0 ? o[src] : 0; }
        }
    }
    f = fopen(argv[2], "wb"); fwrite(o, 1, size, f); fclose(f); return 0;
}
