/* Host-side check of the conversion engine:  host_conv dict.bin  reading...   (readings in hiragana, UTF-8) */
#include "../../drivers/ime/conv.h"
#include "../../drivers/ime/ime_dict.h"
#include "../../drivers/ime/ime_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static conv_t cv;

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("dict"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)sz);
    if (fread(blob, 1, (size_t)sz, f) != (size_t)sz) return 1;
    if (!ime_dict_add(blob, (uint32_t)sz)) { fprintf(stderr, "bad dict\n"); return 1; }
    for (int a = 2; a < argc; a++) {
        uint16_t r[CONV_MAX_READ];
        int n = ime_u8_to_cps(argv[a], r, CONV_MAX_READ);
        conv_start(&cv, r, n);
        char txt[512];
        conv_text(&cv, txt, sizeof txt);
        printf("%s -> %s   [", argv[a], txt);
        for (int i = 0; i < cv.nseg; i++) {
            char t[200];
            ime_cps_to_u8(cv.seg[i].text, cv.seg[i].tlen, t, sizeof t);
            printf("%s%s", i ? " | " : "", t);
        }
        printf("]\n");
        if (getenv("CANDS"))
            for (int i = 0; i < cv.nseg; i++) {
                conv_cands_t cl; conv_cands(&cv, i, &cl);
                printf("   seg%d:", i);
                for (int k = 0; k < cl.n && k < 12; k++) printf(" %s", cl.pool + cl.off[k]);
                printf("\n");
            }
    }
    return 0;
}
