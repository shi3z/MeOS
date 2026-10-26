#include "replay.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static sim_replay replay;
static void check(const char *text, bool success)
{
    FILE *f = tmpfile();
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    rewind(f);
    char error[160] = {0};
    bool ok = sim_replay_load(f, &replay, error, sizeof error);
    fclose(f);
    assert(ok == success);
    if (!ok)
        assert(error[0] && !replay.count);
}
int main(void)
{
    check("# keys\nkey 0 4 0 0\nshot 80 graph\nkey 10 20 1 0\n", true);
    assert(replay.count == 3 && replay.duration == 90 && replay.actions[2].modifiers == 1);
    check("key 0 4 0 1", true); /* no final newline */
    check("", false);
    check("# comment", false);
    check("key -1 4 0 0", false);
    check("key 999999999999999999999999 4 0 0", false);
    check("key 0 3 0 0", false);
    check("key 0 256 0 0", false);
    check("key 0 4 256 0", false);
    check("key 0 4 0 2", false);
    check("key 0 4 0 0 extra", false);
    check("shot 0 ../escape", false);
    check("shot 0 graph\nshot 0 graph", false);
    check("key 60001 4 0 0", false);
    check("unknown 0 x", false);
    check("key 60000 4 0 0\nkey 60000 4 0 0\nkey 60000 4 0 0\nkey 60000 4 0 0\nkey 60000 4 0 "
          "0\nkey 1 4 0 0",
          false);
    unsigned rng = 17;
    char input[320];
    for (unsigned test = 0; test < 2000; test++) {
        size_t n = test % sizeof input;
        for (size_t i = 0; i < n; i++) {
            rng = rng * 1664525u + 1013904223u;
            input[i] = "keyshot #0123456789-_/\n"[rng % 22];
        }
        input[n] = 0;
        FILE *f = tmpfile();
        assert(f);
        fputs(input, f);
        rewind(f);
        char error[160];
        sim_replay_load(f, &replay, error, sizeof error);
        fclose(f);
    }
    puts("Replay parser: valid input, limits, malformed fields and paths PASS");
}
