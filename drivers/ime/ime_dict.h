#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Dictionary access for the input method.
 *
 * A dictionary is a read-only blob in the "IMD1" format that tools/ime_mkdict.py writes from SKK
 * dictionaries (SKK-JISYO.L/M/S ...).  Several can be registered; their candidates are merged in the order the
 * blobs were added.  A key is the reading in hiragana; an entry for a word with okurigana (a conjugating
 * verb or adjective) has the stem plus the consonant letter of the okurigana as key, e.g. 食べる = "たb".
 *
 *   u8  "IMD1"   u32 count   u32 total_size   u32 offset[count]      (all little endian, offsets from blob start)
 *   entry: u8 key_len, key (UTF-8), u16 data_len, data = candidates, each NUL-terminated
 * Entries are sorted by key bytes, so lookup is a binary search.
 */
#define IME_DICT_MAGIC 0x31444D49u        /* "IMD1" */
#define IME_DICT_HEADER 12u

/* Registers a blob (not copied: it must stay valid).  Returns false if it is not a valid IMD1 blob. */
bool ime_dict_add(const uint8_t *blob, uint32_t size);
void ime_dict_clear(void);

/* Candidates for `key` (UTF-8) as NUL-separated strings in out[] (terminated by an empty string).  Returns the count. */
int ime_dict_lookup(const char *key, char *out, int cap);

/* What the user picked for exactly this reading before (newest first), same layout as above. */
int ime_dict_learned(const char *key, char *out, int cap);

/* Remembers that `text` was chosen for the reading `key`, so it is offered first from now on. */
void ime_dict_learn(const char *key, const char *text);
