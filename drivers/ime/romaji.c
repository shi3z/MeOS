#include "romaji.h"
#include "ime_util.h"
#include <string.h>

typedef struct { const char *rom; const char *kana; } rule_t;

static const rule_t RULES[] = {
    {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
    {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
    {"ca","か"},{"ci","し"},{"cu","く"},{"ce","せ"},{"co","こ"},
    {"kya","きゃ"},{"kyi","きぃ"},{"kyu","きゅ"},{"kye","きぇ"},{"kyo","きょ"},
    {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
    {"gya","ぎゃ"},{"gyi","ぎぃ"},{"gyu","ぎゅ"},{"gye","ぎぇ"},{"gyo","ぎょ"},
    {"sa","さ"},{"si","し"},{"shi","し"},{"su","す"},{"se","せ"},{"so","そ"},
    {"sha","しゃ"},{"shu","しゅ"},{"she","しぇ"},{"sho","しょ"},
    {"sya","しゃ"},{"syi","しぃ"},{"syu","しゅ"},{"sye","しぇ"},{"syo","しょ"},
    {"za","ざ"},{"zi","じ"},{"ji","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
    {"ja","じゃ"},{"ju","じゅ"},{"je","じぇ"},{"jo","じょ"},
    {"jya","じゃ"},{"jyi","じぃ"},{"jyu","じゅ"},{"jye","じぇ"},{"jyo","じょ"},
    {"zya","じゃ"},{"zyi","じぃ"},{"zyu","じゅ"},{"zye","じぇ"},{"zyo","じょ"},
    {"ta","た"},{"ti","ち"},{"chi","ち"},{"tu","つ"},{"tsu","つ"},{"te","て"},{"to","と"},
    {"cha","ちゃ"},{"chu","ちゅ"},{"che","ちぇ"},{"cho","ちょ"},
    {"tya","ちゃ"},{"tyi","ちぃ"},{"tyu","ちゅ"},{"tye","ちぇ"},{"tyo","ちょ"},
    {"cya","ちゃ"},{"cyi","ちぃ"},{"cyu","ちゅ"},{"cye","ちぇ"},{"cyo","ちょ"},
    {"tsa","つぁ"},{"tsi","つぃ"},{"tse","つぇ"},{"tso","つぉ"},
    {"tha","てゃ"},{"thi","てぃ"},{"thu","てゅ"},{"the","てぇ"},{"tho","てょ"},{"twu","とぅ"},
    {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
    {"dya","ぢゃ"},{"dyi","ぢぃ"},{"dyu","ぢゅ"},{"dye","ぢぇ"},{"dyo","ぢょ"},
    {"dha","でゃ"},{"dhi","でぃ"},{"dhu","でゅ"},{"dhe","でぇ"},{"dho","でょ"},{"dwu","どぅ"},
    {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},{"nn","ん"},{"n'","ん"},
    {"nya","にゃ"},{"nyi","にぃ"},{"nyu","にゅ"},{"nye","にぇ"},{"nyo","にょ"},
    {"ha","は"},{"hi","ひ"},{"hu","ふ"},{"fu","ふ"},{"he","へ"},{"ho","ほ"},
    {"hya","ひゃ"},{"hyi","ひぃ"},{"hyu","ひゅ"},{"hye","ひぇ"},{"hyo","ひょ"},
    {"fa","ふぁ"},{"fi","ふぃ"},{"fe","ふぇ"},{"fo","ふぉ"},{"fya","ふゃ"},{"fyu","ふゅ"},{"fyo","ふょ"},
    {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
    {"bya","びゃ"},{"byi","びぃ"},{"byu","びゅ"},{"bye","びぇ"},{"byo","びょ"},
    {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
    {"pya","ぴゃ"},{"pyi","ぴぃ"},{"pyu","ぴゅ"},{"pye","ぴぇ"},{"pyo","ぴょ"},
    {"va","ゔぁ"},{"vi","ゔぃ"},{"vu","ゔ"},{"ve","ゔぇ"},{"vo","ゔぉ"},
    {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
    {"mya","みゃ"},{"myi","みぃ"},{"myu","みゅ"},{"mye","みぇ"},{"myo","みょ"},
    {"ya","や"},{"yu","ゆ"},{"yo","よ"},{"yi","い"},{"ye","いぇ"},
    {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
    {"rya","りゃ"},{"ryi","りぃ"},{"ryu","りゅ"},{"rye","りぇ"},{"ryo","りょ"},
    {"wa","わ"},{"wi","うぃ"},{"wu","う"},{"we","うぇ"},{"wo","を"},
    {"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
    {"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},
    {"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},{"lya","ゃ"},{"lyu","ゅ"},{"lyo","ょ"},
    {"xtu","っ"},{"xtsu","っ"},{"ltu","っ"},{"ltsu","っ"},{"xwa","ゎ"},{"lwa","ゎ"},
    {"xka","ヵ"},{"xke","ヶ"},{"lka","ヵ"},{"lke","ヶ"},
};
#define NRULES ((int)(sizeof RULES / sizeof RULES[0]))

static int emit_utf8(const char *s, uint16_t *out)
{
    int n = 0;
    while (*s && n < 4) { uint32_t cp; s += ime_u8_dec(s, &cp); out[n++] = (uint16_t)cp; }
    return n;
}

static const rule_t *exact(const char *p, int n)
{
    for (int i = 0; i < NRULES; i++)
        if ((int)strlen(RULES[i].rom) == n && memcmp(RULES[i].rom, p, (size_t)n) == 0) return &RULES[i];
    return 0;
}

static bool is_prefix(const char *p, int n)          /* of a longer rule */
{
    for (int i = 0; i < NRULES; i++)
        if ((int)strlen(RULES[i].rom) > n && memcmp(RULES[i].rom, p, (size_t)n) == 0) return true;
    return false;
}

static void drop_first(romaji_t *r)
{
    memmove(r->pend, r->pend + 1, r->n);
    r->n--;
}

static bool doublable(char c) { return c && strchr("bcdfghjkmpqrstvwyz", c) != 0; }

int romaji_feed(romaji_t *r, char c, uint16_t *out)
{
    int no = 0;
    if (r->tent) {                                                        /* onna = お ん な, kannai = か ん な い */
        r->tent = 0;
        if (r->n == 0 && c && strchr("aiueoy", c)) { r->pend[0] = 'n'; r->n = 1; }
    }
    if (r->n >= (uint8_t)(sizeof r->pend - 1)) r->n = 0;                  /* cannot happen with the rule set; stay safe */
    r->pend[r->n++] = c;
    r->pend[r->n] = 0;
    while (r->n > 0 && no < 6) {
        const rule_t *m = exact(r->pend, r->n);
        if (m) {
            no += emit_utf8(m->kana, out + no);
            r->tent = (r->n == 2 && r->pend[0] == 'n' && r->pend[1] == 'n');
            r->n = 0;
            break;
        }
        if (is_prefix(r->pend, r->n)) break;                              /* wait for more keys */
        if (r->pend[0] == 'n' && r->n >= 2) { out[no++] = 0x3093; drop_first(r); continue; }          /* n + consonant */
        if (r->n >= 2 && r->pend[0] == r->pend[1] && doublable(r->pend[0])) { out[no++] = 0x3063; drop_first(r); continue; }   /* っ */
        out[no++] = (uint16_t)(uint8_t)r->pend[0];                        /* not romaji: keep what was typed */
        drop_first(r);
    }
    return no;
}

int romaji_flush(romaji_t *r, uint16_t *out)
{
    int no = 0;
    r->tent = 0;
    for (int i = 0; i < r->n && no < 6; i++) out[no++] = (r->pend[i] == 'n' && r->n == 1) ? 0x3093 : (uint16_t)(uint8_t)r->pend[i];
    r->n = 0;
    return no;
}

uint16_t romaji_punct(char c)
{
    switch (c) {
    case ',': return 0x3001;  case '.': return 0x3002;  case '[': return 0x300C;  case ']': return 0x300D;
    case '/': return 0x30FB;  case '?': return 0xFF1F;  case '!': return 0xFF01;  case '~': return 0x301C;
    case '-': return 0x30FC;
    default:  return 0;
    }
}
