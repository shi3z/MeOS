#pragma once
#include <stdint.h>

/* Romaji -> kana, one ASCII key at a time (Hepburn and Kunrei spellings, "nn", "n'", doubled consonants = っ). */
typedef struct {
    char    pend[6];            /* romaji typed but not yet turned into kana, e.g. "k" or "ky" */
    uint8_t n;
    uint8_t tent;               /* "nn" was just turned into ん: the second n may still start な, にゃ ... */
} romaji_t;

/* Feeds one lower-case key; writes the finished characters (kana, or the key itself when it cannot start
 * a romaji sequence) to out[] (room for 8) and returns how many. */
int romaji_feed(romaji_t *r, char c, uint16_t *out);

/* Ends the sequence in progress: a lone "n" becomes ん, anything else is emitted as typed. */
int romaji_flush(romaji_t *r, uint16_t *out);

/* Japanese punctuation for a typed key (',' -> 、 ...), or 0 when the key has none. */
uint16_t romaji_punct(char c);
