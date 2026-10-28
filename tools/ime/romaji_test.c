#include "../../drivers/ime/romaji.h"
#include "../../drivers/ime/ime_util.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void t(const char *in, const char *want)
{
    romaji_t r = {{0}, 0, 0};
    uint16_t buf[64]; int n = 0;
    for (const char *p = in; *p; p++) n += romaji_feed(&r, *p, buf + n);
    n += romaji_flush(&r, buf + n);
    char out[256]; ime_cps_to_u8(buf, n, out, sizeof out);
    if (strcmp(out, want)) { printf("FAIL %s -> %s (want %s)\n", in, out, want); fails++; }
}

int main(void)
{
    t("konnnichiha", "こんにちは");  t("kanzi", "かんじ");  t("nihongo", "にほんご");  t("gakkou", "がっこう");
    t("shinbun", "しんぶん");  t("kannai", "かんない");  t("kannnai", "かんない");  t("konnnichiha", "こんにちは");  t("tte", "って");  t("nn", "ん");  t("n", "ん");  t("onna", "おんな");  t("kyou", "きょう");
    t("xtu", "っ");  t("ltsu", "っ");  t("hon'ya", "ほんや");  t("sha", "しゃ");  t("tsu", "つ");  t("fa", "ふぁ");
    t("jikan", "じかん");  t("chotto", "ちょっと");  t("kk", "っk");  t("q", "q");  t("wo", "を");  t("dhi", "でぃ");
    t("nyuuryoku", "にゅうりょく");  t("kannjya", "かんじゃ");  t("aiueo", "あいうえお");  t("vu", "ゔ");
    printf(fails ? "romaji: %d FAIL\n" : "romaji: all pass\n", fails);
    return fails != 0;
}
