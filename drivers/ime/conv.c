#include "conv.h"
#include "ime_dict.h"
#include "ime_util.h"
#include <string.h>
#include <stddef.h>
#ifdef CONV_DEBUG
#include <stdio.h>
#endif

#define STEM_MAX   14               /* longest dictionary word, in kana */
#define INF        0x3FFFFFFF

/* ---- function words ------------------------------------------------------------------------ */

/* Function words that may follow a dictionary word, written as kana.
 *  m: M_NOUN = after a noun (and symbols), M_WORD = after a conjugating word's okurigana, M_ANY = either.
 *  cls: which okurigana heads it can follow, i.e. which conjugation form the head spells (see okuri_forms);
 *       C_LATE marks words that may also follow another function word (食べる|だけ, 学生|です|か).
 * Without these rules "たべました" would read as the name 田部 + ました and 流す + いた would be allowed. */
#define M_NOUN 1
#define M_WORD 2
#define M_ANY  3
#define M_PLAIN 4                   /* a noun that cannot take する: short ones (今した is not a word) */
#define C_A 1                       /* あ-row: mizenkei (書か)                  */
#define C_I 2                       /* い-row: renyokei (書き)                  */
#define C_U 4                       /* う-row: dictionary form (書く, 高い)      */
#define C_E 8                       /* え-row: ichidan stem (食べ), kateikei    */
#define C_O 16                      /* お-row: volitional (書こ)                */
#define C_X 32                      /* っ ん い: before た/て (書い, 読ん)       */
#define C_LATE 64
#define C_ADJ 128                   /* adjective stem + し, き ... (美し+い)    */
#define C_HEAD 0xBF                 /* everything an okurigana head can be     */
#define TAIL_CHAIN_MAX 3            /* function words in a row; more than that is a run of kana, not a phrase */

typedef struct { const char *s; uint8_t m, cls; } tail_def_t;
#define P(s)    {s, M_ANY, C_U}
#define PI(s)   {s, M_ANY, C_U|C_I}
#define PL(s)   {s, M_ANY, C_U|C_LATE}
#define V(s, c) {s, M_WORD, c}
#define SURU(s) {s, M_NOUN, 0}
static const tail_def_t TAILS[] = {
    P("を"),P("に"),P("へ"),P("や"),P("まで"),P("より"),P("だけ"),P("しか"),P("ほど"),P("くらい"),P("ぐらい"),P("など"),
    P("なんて"),P("って"),P("という"),P("といった"),P("とか"),P("には"),P("では"),P("とは"),P("への"),P("での"),
    P("からの"),P("までの"),P("かも"),P("かな"),P("かしら"),P("もの"),P("ものの"),P("だって"),P("なら"),P("ならば"),
    P("のは"),P("のが"),P("のを"),P("のも"),P("こと"),P("ため"),P("ように"),P("ような"),P("ほうが"),P("じゃ"),P("でも"),
    P("ばかり"),P("ずつ"),P("だの"),P("やら"),P("ほか"),P("で"),
    PI("に"),PI("ながら"),
    PL("は"),PL("が"),PL("と"),PL("も"),PL("の"),PL("か"),PL("ね"),PL("よ"),PL("な"),PL("わ"),PL("さ"),PL("けど"),PL("から"),
    PL("ので"),PL("のに"),PL("し"),PL("けれど"),PL("けれども"),PL("だけ"),PL("まで"),PL("より"),PL("ほど"),PL("くらい"),
    PL("ぐらい"),PL("しか"),PL("こと"),PL("ため"),PL("ように"),PL("ばかり"),PL("など"),PL("って"),PL("という"),
    PL("です"),PL("でした"),PL("でしょう"),PL("だ"),PL("だった"),PL("だろう"),PL("である"),PL("ではない"),PL("じゃない"),
    PL("ですか"),PL("ですね"),PL("ですよ"),PL("ですが"),PL("んです"),PL("んだ"),
    P("なる"),P("なった"),P("なって"),P("なります"),P("ならない"),
    /* endings of conjugating words */
    V("ない", C_A|C_E),V("なかった", C_A|C_E),V("なく", C_A|C_E),V("なくて", C_A|C_E),V("なければ", C_A|C_E),V("ず", C_A|C_E),V("ぬ", C_A|C_E),
    V("れる", C_A),V("せる", C_A),V("られる", C_E),V("させる", C_E),
    V("ます", C_I|C_E),V("ました", C_I|C_E),V("ません", C_I|C_E),V("ませんでした", C_I|C_E),V("ましょう", C_I|C_E),
    V("まして", C_I|C_E),V("ますが", C_I|C_E),V("ますよ", C_I|C_E),V("ますね", C_I|C_E),V("ますか", C_I|C_E),
    V("たい", C_I|C_E),V("たかった", C_I|C_E),V("たくない", C_I|C_E),V("そう", C_I|C_E|C_ADJ),V("すぎる", C_I|C_E|C_ADJ),V("すぎ", C_I|C_E|C_ADJ),
    V("て", C_E|C_X),V("た", C_E|C_X),V("ても", C_E|C_X),V("たら", C_E|C_X),V("たり", C_E|C_X),
    V("だ", C_X),V("だら", C_X),V("だり", C_X),V("で", C_X),V("でも", C_X),
    V("ている", C_E|C_X),V("ていた", C_E|C_X),V("ています", C_E|C_X),V("ていました", C_E|C_X),V("てい", C_E|C_X),
    V("ておく", C_E|C_X),V("てある", C_E|C_X),V("てみる", C_E|C_X),V("てしまう", C_E|C_X),V("てくる", C_E|C_X),
    V("ていく", C_E|C_X),V("てください", C_E|C_X),V("てくれる", C_E|C_X),V("てもらう", C_E|C_X),V("てあげる", C_E|C_X),
    V("でいる", C_X),V("でいた", C_X),V("でいます", C_X),V("でいました", C_X),V("でいく", C_X),V("でくる", C_X),V("でください", C_X),
    V("る", C_E),V("れ", C_E),V("ろ", C_E),V("ば", C_E),V("れば", C_E),
    V("う", C_O),V("よう", C_O|C_E),
    V("い", C_ADJ),V("く", C_ADJ),V("くて", C_ADJ),V("くない", C_ADJ),V("かった", C_ADJ),V("ければ", C_ADJ),
    /* suru verbs: after a noun */
    SURU("する"),SURU("した"),SURU("して"),SURU("します"),SURU("しない"),SURU("しました"),SURU("しません"),SURU("しよう"),
    SURU("される"),SURU("された"),SURU("されて"),SURU("している"),SURU("していた"),SURU("しています"),SURU("していました"),
    SURU("してください"),SURU("できる"),SURU("でき"),SURU("できた"),SURU("できます"),SURU("できない"),
};
#undef P
#undef PI
#undef PL
#undef V
#undef SURU
#define NTAIL ((int)(sizeof TAILS / sizeof TAILS[0]))

static void tails_init(conv_t *c)
{
    c->ntail = NTAIL < CONV_TAIL_MAX ? NTAIL : CONV_TAIL_MAX;
    for (int i = 0; i < c->ntail; i++) {
        c->tail[i].n = (uint8_t)ime_u8_to_cps(TAILS[i].s, c->tail[i].c, 10);
        c->tail[i].m = TAILS[i].m;
        c->tail[i].cls = TAILS[i].cls;
    }
}

static bool tail_at(const conv_t *c, int p, int b, const conv_tail_t *t)
{
    return p + t->n <= b && memcmp(c->r + p, t->c, (size_t)t->n * sizeof(uint16_t)) == 0;
}

/* reading[p..b) as particles / copula only, at most `depth` of them */
static bool late_ok(const conv_t *c, int p, int b, int depth)
{
    if (p == b) return true;
    if (depth == 0) return false;
    for (int t = 0; t < c->ntail; t++)
        if ((c->tail[t].cls & C_LATE) && tail_at(c, p, b, &c->tail[t]) && late_ok(c, p + c->tail[t].n, b, depth - 1)) return true;
    return false;
}

/* Reading[a..b) after a noun (M_NOUN) or standing alone (M_ANY): function words only?  The empty string is fine. */
static bool tail_ok(conv_t *c, int a, int b, int mask)
{
    if (a == b) return true;
    if (a > b) return false;
    uint8_t *m = &c->memo_tail[mask == M_NOUN ? 0 : mask == M_ANY ? 1 : 2][a][b];
    if (!*m) {
        bool ok = false;
        for (int t = 0; t < c->ntail && !ok; t++)
            ok = (mask == M_PLAIN ? c->tail[t].m == M_ANY : (c->tail[t].m & mask) != 0) && tail_at(c, a, b, &c->tail[t]) && late_ok(c, a + c->tail[t].n, b, TAIL_CHAIN_MAX - 1);
        *m = ok ? 1 : 2;
    }
    return *m == 1;
}

/* After an okurigana head at a-1: which head classes can be followed by exactly reading[a..b)?  (bit set of C_*) */
static int word_tail_classes(conv_t *c, int a, int b)
{
    if (a == b) return C_U;                                              /* the word ends with its head: dictionary form */
    if (a > b) return 0;
    uint16_t *m = &c->memo_cls[a][b];
    if (!*m) {
        int mask = 0;
        for (int t = 0; t < c->ntail; t++)
            if ((c->tail[t].m & M_WORD) && tail_at(c, a, b, &c->tail[t]) && late_ok(c, a + c->tail[t].n, b, TAIL_CHAIN_MAX - 1))
                mask |= c->tail[t].cls & C_HEAD;
        *m = (uint16_t)(0x100 | mask);
    }
    return *m & 0xFF;
}

/* ---- dictionary probes ---------------------------------------------------------------------- */

static bool kana_run(const conv_t *c, int a, int b)
{
    for (int i = a; i < b; i++) if (!ime_is_hira(c->r[i])) return false;
    return true;
}

static int lookup_cps(const uint16_t *cps, int n, char letter, char *out, int cap)
{
    char key[CONV_MAX_READ * 3 + 2];
    int len = ime_cps_to_u8(cps, n, key, (int)sizeof key - 2);
    if (letter) { key[len++] = letter; key[len] = 0; }
    return ime_dict_lookup(key, out, cap);
}

typedef struct { char letter; uint8_t cls; } form_t;

/* What a kana can mean as the first okurigana of a dictionary entry: the key letter and the conjugation form. */
static int okuri_forms(uint16_t k, form_t f[4])
{
    static const char *const ROWS[] = {                       /* kana of one consonant, in a i u e o order */
        "かきくけこ", "がぎぐげご", "さしすせそ", "ざじずぜぞ", "たちつてと", "だぢづでど", "なにぬね_", "ばびぶべぼ", "まみむめも", "らりるれろ",
    };
    static const char LET[] = "kkssttnbmr";
    static const uint8_t CLS[5] = { C_A, C_I | C_ADJ, C_U, C_E, C_O };
    static const char VOW[] = "aiueo";
    if (k == 0x3042 || k == 0x3046 || k == 0x3048 || k == 0x304A) {      /* あ う え お */
        int v = (k - 0x3041) / 2;
        f[0].letter = VOW[v]; f[0].cls = CLS[v];
        return 1;
    }
    if (k == 0x3044) {                                        /* い: 買い (連用) 高い (終止), or 書い 泳い (before た) */
        f[0].letter = 'i'; f[0].cls = C_I | C_U;
        f[1].letter = 'k'; f[1].cls = C_X;
        f[2].letter = 'g'; f[2].cls = C_X;
        return 3;
    }
    if (k == 0x3063) { f[0].letter = 't'; f[1].letter = 'r'; f[2].letter = 'w'; f[3].letter = 'k'; for (int i = 0; i < 4; i++) f[i].cls = C_X; return 4; }
    if (k == 0x3093) { f[0].letter = 'm'; f[1].letter = 'b'; f[2].letter = 'n'; for (int i = 0; i < 3; i++) f[i].cls = C_X; return 3; }
    if (k == 0x308F) { f[0].letter = 'w'; f[0].cls = C_A; return 1; }
    for (int r = 0; r < 10; r++) {
        const char *p = ROWS[r];
        for (int v = 0; v < 5 && *p; v++) {
            uint32_t cp;
            p += ime_u8_dec(p, &cp);
            if (cp == k) {
                f[0].letter = LET[r]; f[0].cls = CLS[v];
                if (LET[r] == 's' && v == 1) f[0].cls |= C_X;         /* 話し(て) 話し(た): no sound change */
                return 1;
            }
        }
    }
    return 0;                                                 /* は行, や行, の, small kana: no dictionary entries start okurigana there */
}

static bool probe_nasi(conv_t *c, int i, int len)
{
    uint8_t *m = &c->memo_nasi[i][len];
    if (!*m) {
        char tmp[64];
        *m = lookup_cps(c->r + i, len, 0, tmp, (int)sizeof tmp) > 0 ? 1 : 2;
    }
    return *m == 1;
}

/* stem = reading[i..i+len), okurigana head = reading[i+len]; the classes of the head that have an entry */
static int probe_ari(conv_t *c, int i, int len)
{
    uint16_t *m = &c->memo_ari[i][len];
    if (!*m) {
        char tmp[64];
        form_t f[4];
        int nf = okuri_forms(c->r[i + len], f), hit = 0;
        for (int j = 0; j < nf; j++)
            if (lookup_cps(c->r + i, len, f[j].letter, tmp, (int)sizeof tmp) > 0) hit |= f[j].cls;
        *m = (uint16_t)(0x100 | hit);
    }
    return *m & 0xFF;
}

/* 見ます 着た 寝ない: a one-kanji ichidan verb's okurigana starts with the ending itself, so the "head" is already
 * the first kana of one complete ending (not a chain: that would let 割 + たし through). */
static bool ichidan_ending(const conv_t *c, int a, int b)
{
    for (int t = 0; t < c->ntail; t++)
        if ((c->tail[t].m & M_WORD) && (c->tail[t].cls & C_E) && c->tail[t].n == b - a && tail_at(c, a, b, &c->tail[t])) return true;
    return false;
}

/* Does reading[s..e) read as a conjugating word with a stem of len kana (+ head + tail)? */
static bool ari_fits(conv_t *c, int s, int e, int len)
{
    int k = s + len + 1;                                       /* one past the okurigana head */
    if (len < 1 || len > 15 || k > e) return false;
    int hit = probe_ari(c, s, len);
    if (!hit) return false;
    /* 見ます, 着た, 寝ない: the okurigana of a one-kanji ichidan verb starts with the ending itself */
    return (hit & word_tail_classes(c, k, e)) != 0 || (len == 1 && ichidan_ending(c, k - 1, e));
}

static bool nasi_fits(conv_t *c, int s, int e, int len)
{
    return len >= 1 && len <= STEM_MAX && s + len <= e && tail_ok(c, s + len, e, len >= 4 ? M_NOUN : M_PLAIN) && probe_nasi(c, s, len);
}

/* ---- phrase scoring ------------------------------------------------------------------------- */

#define SEG_BASE    1200
#define WORD_BONUS  150
#define ONE_KANA    700
#define CONJ_BONUS  30             /* 降る beats the name 古: only real verbs and adjectives get here */
#define TAIL_BONUS  200            /* a phrase that ends in function words is the normal shape of Japanese */
#define TAIL_ONLY   1100
#define UNKNOWN     1500
#define UNKNOWN_CH  150

/* Cost of reading[i..j) as one phrase, where its dictionary part ends (k == i: no word) and of which kind. */
static int phrase_cost(conv_t *c, int i, int j, int *bestk, bool *is_ari)
{
    int best = INF, bk = i;
    bool ari = false;
    if (kana_run(c, i, j)) {
        int maxl = j - i < STEM_MAX ? j - i : STEM_MAX;
        for (int l = 1; l <= maxl; l++) {
            int k = i + l;
            if (nasi_fits(c, i, j, l)) {
                int cost = SEG_BASE - (l < 6 ? l : 6) * WORD_BONUS + (l == 1 ? ONE_KANA : 0) - (k < j ? TAIL_BONUS : 0);
                if (cost < best) { best = cost; bk = k; ari = false; }
            }
            if (k < j && ari_fits(c, i, j, l)) {
                int cost = SEG_BASE - (l < 5 ? l + 1 : 6) * WORD_BONUS - CONJ_BONUS - (k + 1 < j ? TAIL_BONUS : 0);
                if (cost < best) { best = cost; bk = k + 1; ari = true; }
            }
        }
        if (best == INF && tail_ok(c, i, j, M_ANY)) { best = TAIL_ONLY; bk = i; }
        if (best == INF) { best = UNKNOWN + UNKNOWN_CH * (j - i); bk = i; }
    } else {
        for (int p = i; p < j; p++) if (ime_is_hira(c->r[p])) return INF;       /* symbol runs never swallow kana */
        best = 100 * (j - i);
    }
    *bestk = bk;
    *is_ari = ari;
    return best;
}

static bool same_class(const conv_t *c, int a, int b) { return ime_is_hira(c->r[a]) == ime_is_hira(c->r[b]); }

/* Cuts reading[from..rn) into phrases appended at c->seg[c->nseg..]. */
static void split_from(conv_t *c, int from)
{
    int n = c->rn;
    int *cost = c->dp_cost, *back = c->dp_back, *bk = c->dp_k;
    bool *ar = c->dp_ari;
    for (int i = from; i <= n; i++) cost[i] = INF;
    cost[from] = 0;
    for (int i = from; i < n; i++) {
        if (cost[i] >= INF) continue;
        int jmax = i + STEM_MAX + 12 < n ? i + STEM_MAX + 12 : n;
        for (int j = i + 1; j <= jmax; j++) {
            if (!same_class(c, i, j - 1)) break;
            int k;
            bool a;
            int e = phrase_cost(c, i, j, &k, &a);
            if (e >= INF) continue;
            if (cost[i] + e < cost[j]) { cost[j] = cost[i] + e; back[j] = i; bk[j] = k; ar[j] = a; }
        }
    }
    int starts[CONV_MAX_READ + 1], ks[CONV_MAX_READ + 1], m = 0;
    bool aris[CONV_MAX_READ + 1];
    for (int j = n; j > from; j = back[j]) { starts[m] = back[j]; ks[m] = bk[j]; aris[m] = ar[j]; m++; }
    for (int q = m - 1; q >= 0 && c->nseg < CONV_MAX_SEG; q--) {
        conv_seg_t *s = &c->seg[c->nseg++];
        s->s = (uint8_t)starts[q];
        s->e = (uint8_t)(q > 0 ? starts[q - 1] : n);
        s->k = (uint8_t)ks[q];
        s->ari = aris[q];
        s->sel = 0;
        s->tlen = 0;
    }
    if (c->nseg == CONV_MAX_SEG && c->seg[c->nseg - 1].e < n) c->seg[c->nseg - 1].e = (uint8_t)n;   /* too many: last one takes the rest */
}

/* ---- candidates ----------------------------------------------------------------------------- */

static bool cl_add(conv_cands_t *cl, const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || cl->n >= CONV_MAX_CAND) return false;
    for (int i = 0; i < cl->n; i++) if (strcmp(cl->pool + cl->off[i], s) == 0) return false;
    uint16_t end = cl->n ? (uint16_t)(cl->off[cl->n - 1] + strlen(cl->pool + cl->off[cl->n - 1]) + 1) : 0;
    if (end + n + 1 > CONV_CAND_POOL) return false;
    memcpy(cl->pool + end, s, n + 1);
    cl->off[cl->n++] = end;
    return true;
}

/* Adds the spellings of reading[s..e) as a word ending at k (a plain word) or as a conjugating word whose
 * okurigana head is reading[k-1]; whatever follows is copied as kana. */
static void add_words(conv_t *c, conv_cands_t *cl, int s, int e, int k, bool ari)
{
    char *words = c->scratch_words;
    char tail[96];
    int stem_end = ari ? k - 1 : k;
    if (stem_end <= s) return;
    ime_cps_to_u8(c->r + stem_end, e - stem_end, tail, (int)sizeof tail);
    form_t f[4];
    int nf = 1, tailcls = 0;
    if (ari) { nf = okuri_forms(c->r[stem_end], f); tailcls = word_tail_classes(c, k, e); }
    else     { f[0].letter = 0; f[0].cls = 0; }
    for (int j = 0; j < nf; j++) {
        if (ari && !(f[j].cls & tailcls) && !(stem_end - s == 1 && ichidan_ending(c, k - 1, e))) continue;     /* this head cannot take this ending */
        if (lookup_cps(c->r + s, stem_end - s, f[j].letter, words, 512) <= 0) continue;
        for (const char *w = words; *w; w += strlen(w) + 1) {
            char full[160];
            if (strlen(w) + strlen(tail) >= sizeof full) continue;
            strcpy(full, w); strcat(full, tail);
            cl_add(cl, full);
        }
    }
}

void conv_cands(conv_t *c, int si, conv_cands_t *cl)
{
    memset(cl, 0, sizeof *cl);
    const conv_seg_t *sg = &c->seg[si];
    int s = sg->s, e = sg->e;
    {   /* what the user picked for exactly this phrase before goes first */
        char *learned = c->scratch_learned;
        char key[160];
        ime_cps_to_u8(c->r + s, e - s, key, (int)sizeof key);
        if (ime_dict_learned(key, learned, 512) > 0)
            for (const char *w = learned; *w; w += strlen(w) + 1) cl_add(cl, w);
    }
    if (kana_run(c, s, e)) {
        /* the reading the scorer preferred first, then every other way to read the phrase.
         * k is the end of the dictionary part for a plain word, one past the okurigana head for a conjugating one. */
        for (int pass = 0; pass < 3; pass++) {
            for (int k = e; k > s; k--) {
                bool plain = nasi_fits(c, s, e, k - s), conj = ari_fits(c, s, e, k - 1 - s);
                if (pass == 0) {
                    if (k != sg->k) continue;
                    if (sg->ari ? conj : plain) add_words(c, cl, s, e, k, sg->ari);
                } else if (pass == 1) {
                    if (k != sg->k) continue;
                    if (sg->ari ? plain : conj) add_words(c, cl, s, e, k, !sg->ari);
                } else {
                    if (k == sg->k) continue;
                    if (plain) add_words(c, cl, s, e, k, false);
                    if (conj) add_words(c, cl, s, e, k, true);
                }
            }
        }
    }
    char hira[160], kata[160];
    uint16_t tmp[CONV_MAX_READ];
    int n = e - s;
    ime_cps_to_u8(c->r + s, n, hira, (int)sizeof hira);
    for (int i = 0; i < n; i++) tmp[i] = ime_to_kata(c->r[s + i]);
    ime_cps_to_u8(tmp, n, kata, (int)sizeof kata);
    cl_add(cl, hira);
    cl_add(cl, kata);
}

void conv_select(conv_t *c, int si, const conv_cands_t *cl, int idx)
{
    conv_seg_t *sg = &c->seg[si];
    if (idx < 0 || idx >= cl->n) return;
    sg->sel = (uint8_t)idx;
    sg->tlen = (uint8_t)ime_u8_to_cps(cl->pool + cl->off[idx], sg->text, CONV_MAX_TEXT);
}

static void fill_default(conv_t *c, int si)
{
    conv_cands_t *clp = &c->scratch_cl;
    conv_cands(c, si, clp);
    conv_select(c, si, clp, 0);
}

void conv_start(conv_t *c, const uint16_t *reading, int n)
{
    if (n > CONV_MAX_READ) n = CONV_MAX_READ;
    memset(c, 0, offsetof(conv_t, tail));                          /* the tail table is built once, below */
    if (c->ntail == 0) tails_init(c);
    memcpy(c->r, reading, (size_t)n * sizeof(uint16_t));
    c->rn = n;
    split_from(c, 0);
    for (int i = 0; i < c->nseg; i++) fill_default(c, i);
}

bool conv_resize(conv_t *c, int si, int delta)
{
    conv_seg_t *sg = &c->seg[si];
    int ne = sg->e + delta;
    if (ne <= sg->s || ne > c->rn) return false;
    if (ne > sg->s && !same_class(c, sg->s, ne - 1)) return false;
    int k;
    bool ari;
    if (phrase_cost(c, sg->s, ne, &k, &ari) >= INF) return false;
    sg->e = (uint8_t)ne;
    sg->k = (uint8_t)k;
    sg->ari = ari;
    c->nseg = si + 1;
    if (ne < c->rn) split_from(c, ne);
    for (int i = si; i < c->nseg; i++) fill_default(c, i);
    return true;
}

int conv_text(const conv_t *c, char *out, int cap)
{
    int len = 0;
    out[0] = 0;
    for (int i = 0; i < c->nseg; i++) {
        char t[CONV_MAX_TEXT * 3 + 1];
        int n = ime_cps_to_u8(c->seg[i].text, c->seg[i].tlen, t, (int)sizeof t);
        if (len + n >= cap) break;
        memcpy(out + len, t, (size_t)n + 1);
        len += n;
    }
    return len;
}

void conv_learn(const conv_t *c)
{
    for (int i = 0; i < c->nseg; i++) {
        char key[160], val[CONV_MAX_TEXT * 3 + 1];
        if (c->seg[i].sel == 0) continue;                      /* the default needs no remembering */
        ime_cps_to_u8(c->r + c->seg[i].s, c->seg[i].e - c->seg[i].s, key, (int)sizeof key);
        ime_cps_to_u8(c->seg[i].text, c->seg[i].tlen, val, (int)sizeof val);
        ime_dict_learn(key, val);
    }
}
