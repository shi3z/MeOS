#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Kana -> kanji conversion.
 *
 * The reading is cut into bunsetsu (phrases) by dynamic programming: every phrase is a dictionary word (plain,
 * or a conjugating word whose okurigana is written out) followed by a chain of function words (particles,
 * auxiliary verbs, endings: "は", "ます", "ている" ...) kept as kana.  Fewer, longer phrases win; a one-kana word
 * is penalised.  Each phrase then offers every reading of its words plus the hiragana and katakana spelling.
 */
#define CONV_MAX_READ  64
#define CONV_MAX_SEG   32
#define CONV_MAX_TEXT  48               /* code points shown for one phrase */
#define CONV_MAX_CAND  48
#define CONV_CAND_POOL 1536

typedef struct {
    uint8_t  s, e;                      /* reading[s..e) */
    uint8_t  k;                         /* the dictionary part ends here (rest is tail kana); k == s: no dictionary word */
    bool     ari;                       /* a conjugating word (k is one past the okurigana head), else a plain word */
    uint8_t  sel;                       /* index into the candidate list */
    uint8_t  tlen;
    uint16_t text[CONV_MAX_TEXT];       /* currently selected spelling */
} conv_seg_t;

typedef struct {
    char     pool[CONV_CAND_POOL];      /* NUL-separated UTF-8 candidates */
    uint16_t off[CONV_MAX_CAND];
    uint8_t  n;
} conv_cands_t;

#define CONV_TAIL_MAX 256

typedef struct { uint8_t n, m, cls; uint16_t c[10]; } conv_tail_t;

typedef struct {
    uint16_t   r[CONV_MAX_READ];
    int        rn;
    conv_seg_t seg[CONV_MAX_SEG];
    int        nseg;
    uint8_t    memo_nasi[CONV_MAX_READ][16];       /* 0 unknown, 1 hit, 2 miss */
    uint16_t   memo_ari[CONV_MAX_READ][16];
    uint16_t   memo_cls[CONV_MAX_READ + 1][CONV_MAX_READ + 1];
    uint8_t    memo_tail[3][CONV_MAX_READ + 1][CONV_MAX_READ + 1];
    /* tables and scratch space: conv_t is allocated in PSRAM by the caller, so nothing here costs internal RAM */
    conv_tail_t tail[CONV_TAIL_MAX];
    int        ntail;
    conv_cands_t scratch_cl;
    char       scratch_words[512], scratch_learned[512];
    int        dp_cost[CONV_MAX_READ + 1], dp_back[CONV_MAX_READ + 1], dp_k[CONV_MAX_READ + 1];
    bool       dp_ari[CONV_MAX_READ + 1];
} conv_t;

/* Splits and converts reading[0..n).  Every phrase starts with its best candidate selected. */
void conv_start(conv_t *c, const uint16_t *reading, int n);

/* All spellings of phrase si, best first (the hiragana/katakana spellings are always last). */
void conv_cands(conv_t *c, int si, conv_cands_t *out);

/* Selects candidate idx of a list obtained from conv_cands for the same phrase. */
void conv_select(conv_t *c, int si, const conv_cands_t *cl, int idx);

/* Moves the end of phrase si by delta (+1/-1 kana); the phrases after it are cut again.  False if impossible. */
bool conv_resize(conv_t *c, int si, int delta);

/* The whole text as UTF-8. */
int conv_text(const conv_t *c, char *out, int cap);

/* Tells the learning table which spellings were finally used. */
void conv_learn(const conv_t *c);

