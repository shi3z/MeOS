#include "basic.h"
#include <string.h>

#define STEP_LIMIT 100000u
static void text(basic_session *s, const char *p) { s->output(p, strlen(p), s->opaque); }
static size_t decimal(unsigned n, char *out)
{
    char rev[10]; size_t len = 0, pos = 0;
    do { rev[len++] = '0' + n % 10; n /= 10; } while (n);
    while (len) out[pos++] = rev[--len];
    return pos;
}
static int command(const char *p, const char *name)
{
    while (*name) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != *name++) return 0;
    }
    while (*p == ' ' || *p == '\t') p++;
    return !*p;
}
void basic_session_init(basic_session *s, ubasic_output out, int (*cancel)(void *), void *opaque)
{
    memset(s, 0, sizeof *s); s->output = out; s->cancel = cancel; s->opaque = opaque;
    ubasic_init("", out, opaque, 1);
}
static int execute_inner(basic_session *s, int reset)
{
    ubasic_init(s->program, s->output, s->opaque, reset);
    ubasic_set_graphics(s->graphics);
    unsigned steps = 0;
    while (!ubasic_finished()) {
        if (ubasic_waiting_input()) {
            if (s->graphics) { const int32_t off[5] = {0}; s->graphics(s->opaque, UBASIC_SCREEN, off, NULL, 0); }
            if (!s->input) { ubasic_stop(); text(s, "Error: INPUT is unavailable\n"); return -1; }
            char reply[BASIC_LINE_CAP] = {0};
            int rc = s->input(s->opaque, reply, sizeof reply);
            reply[sizeof reply - 1] = 0;
            if (rc < 0) {
                ubasic_stop();
                text(s, rc == -1 ? "Break\n" : "Error: input read failed\n");
                return rc == -1 ? -2 : -1;
            }
            const char *error = rc > 0 ? "invalid or overlong input line" : ubasic_supply_input(reply);
            if (error) { text(s, "?Redo: "); text(s, error); text(s, "\n? "); }
            continue;
        }
        if (s->cancel && s->cancel(s->opaque)) { ubasic_stop(); text(s, "Break\n"); return -2; }
        if (++steps > STEP_LIMIT) { ubasic_stop(); text(s, "Break: 100000-line execution limit\n"); return -2; }
        ubasic_run();
    }
    if (ubasic_interrupted()) { text(s, "Break\n"); return -2; }
    if (ubasic_error()) {
        char n[10]; size_t len = decimal((unsigned)ubasic_error_line(), n);
        text(s, "Error at line "); s->output(n, len, s->opaque);
        text(s, ": "); text(s, ubasic_error()); text(s, "\n");
        return -1;
    }
    return 0;
}
static int execute(basic_session *s, int reset)
{
    int rc = execute_inner(s, reset);
    if (s->graphics) { const int32_t off[5] = {0}; s->graphics(s->opaque, UBASIC_SCREEN, off, NULL, 0); }
    return rc;
}
int basic_session_submit(basic_session *s, const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 0;
    if (strlen(line) >= BASIC_LINE_CAP) { text(s, "Error: input line too long\n"); return 0; }
    if (*line >= '0' && *line <= '9') {
        unsigned number = 0;
        while (*line >= '0' && *line <= '9') {
            number = number * 10 + (*line++ - '0');
            if (number > 65535) { text(s, "Error: line number must be 1..65535\n"); return 0; }
        }
        if (!number || (*line && *line != ' ' && *line != '\t')) { text(s, "Error: expected line number and space\n"); return 0; }
        while (*line == ' ' || *line == '\t') line++;
        unsigned pos = 0; while (pos < s->source.count && s->source.lines[pos].number < number) pos++;
        int exists = pos < s->source.count && s->source.lines[pos].number == number;
        if (!*line) {
            if (exists) { memmove(&s->source.lines[pos], &s->source.lines[pos+1], (s->source.count-pos-1)*sizeof s->source.lines[0]); s->source.count--; }
        } else {
            if (!exists) {
                if (s->source.count == BASIC_MAX_LINES) { text(s, "Error: program full (128 lines)\n"); return 0; }
                memmove(&s->source.lines[pos+1], &s->source.lines[pos], (s->source.count-pos)*sizeof s->source.lines[0]); s->source.count++;
            }
            s->source.lines[pos].number = number; strcpy(s->source.lines[pos].text, line);
        }
        return 0;
    }
    const char *names[] = { "LOAD", "SAVE", "EDIT" };
    for (int action = BASIC_LOAD; action <= BASIC_EDIT; action++) {
        const char *p = line, *name = names[action];
        while (*name && *p) {
            char c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
            if (c != *name) break;
            p++; name++;
        }
        if (*name || (*p && *p != ' ' && *p != '\t')) continue;
        while (*p == ' ' || *p == '\t') p++;
        char path[BASIC_LINE_CAP]; size_t len = strlen(p);
        while (len && (p[len-1] == ' ' || p[len-1] == '\t')) len--;
        if (len && p[0] == '"') {
            if (len < 2 || p[len-1] != '"') { text(s, "Error: unterminated path quote\n"); return 0; }
            p++; len -= 2;
            if (!len) { text(s, "Error: empty path\n"); return 0; }
        }
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '"' || (unsigned char)p[i] < 0x20) { text(s, "Error: invalid path\n"); return 0; }
        }
        memcpy(path, p, len); path[len] = 0;
        if (s->file) s->file(s->opaque, action, path);
        else text(s, "Error: file commands unavailable\n");
        return 0;
    }
    if (command(line, "BYE")) return 1;
    if (command(line, "HELP")) {
        text(s, "Numeric BASIC: A-Z, LET, PRINT, INPUT, DIM, IF/THEN/ELSE, FOR/TO/NEXT, GOTO, GOSUB/RETURN, REM, END\n"
                "Enter numbered lines; number alone deletes. LIST, RUN, NEW, HELP, BYE. LOAD/SAVE/EDIT [path]. Ctrl-C interrupts RUN.\n"
                "Graphics (Tab5): SCREEN 1/0, CLS [color], PSET x,y,color, LINE x,y,x2,y2,color, TEXT x,y,\"text\",color, WAIT ms.\n"
                "Math: SIN COS TAN SQR/SQRT LOG/LN LOG10 EXP ABS INT FIX, PI, ^. RAD default; DEG/RAD change angle mode.\n"
                "DIM A(9): indices 0..9, 1024 total elements. INPUT [\"prompt\";] A or A(index).\n"
                "FOR X=start TO end STEP delta: floating/descending loops; default is integer +1.\n"
                "128 lines, 127 input bytes, 100000 executed lines per RUN. SAVE uses VFS; /home is currently volatile, /sd read-only. BYE discards unsaved source.\n");
        return 0;
    }
    if (command(line, "NEW")) { s->source.count = 0; ubasic_init("", s->output, s->opaque, 1); return 0; }
    if (command(line, "LIST") || command(line, "RUN")) {
        if (command(line, "LIST")) {
            size_t len = basic_session_export(s); s->output(s->program, len, s->opaque);
        } else basic_session_run(s);
        return 0;
    }
    /* The single-line wrapper provides immediate mode without a second parser. */
    size_t len = strlen(line);
    memcpy(s->program, "1 ", 2); memcpy(s->program+2, line, len);
    s->program[len+2] = '\n'; s->program[len+3] = 0;
    execute(s, 0);
    return 0;
}

size_t basic_session_export(basic_session *s)
{
    size_t len = 0;
    for (unsigned i = 0; i < s->source.count; i++) {
        len += decimal(s->source.lines[i].number, s->program+len);
        s->program[len++] = ' ';
        size_t n = strlen(s->source.lines[i].text);
        memcpy(s->program+len, s->source.lines[i].text, n); len += n;
        s->program[len++] = '\n';
    }
    s->program[len] = 0;
    return len;
}
int basic_session_run(basic_session *s)
{
    basic_session_export(s);
    return execute(s, 1);
}
const char *basic_program_parse(basic_program *dst, const char *data, size_t length, unsigned *error_line)
{
    memset(dst, 0, sizeof *dst);
    *error_line = 1;
    if (length >= BASIC_PROGRAM_CAP) return "file too large";
    size_t pos = 0;
    if (length >= 3 && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) pos = 3;
    while (pos < length) {
        size_t start = pos;
        while (pos < length && data[pos] != '\r' && data[pos] != '\n') {
            unsigned char c = (unsigned char)data[pos++];
            if ((c < 0x20 && c != '\t') || c == 0x7f) return "invalid control byte";
        }
        size_t end = pos;
        if (pos < length && data[pos++] == '\r' && pos < length && data[pos] == '\n') pos++;
        if (end - start >= BASIC_LINE_CAP) return "input line too long";
        while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
        while (end > start && (data[end-1] == ' ' || data[end-1] == '\t')) end--;
        if (start != end) {
            unsigned number = 0;
            while (start < end && data[start] >= '0' && data[start] <= '9') {
                number = number * 10 + (data[start++] - '0');
                if (number > 65535) return "line number must be 1..65535";
            }
            if (!number || start == end || (data[start] != ' ' && data[start] != '\t')) return "expected numbered BASIC source";
            while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
            if (start == end) return "empty numbered line";
            if (dst->count == BASIC_MAX_LINES) return "program full (128 lines)";
            unsigned at = 0;
            while (at < dst->count && dst->lines[at].number < number) at++;
            if (at < dst->count && dst->lines[at].number == number) return "duplicate line number";
            memmove(&dst->lines[at+1], &dst->lines[at], (dst->count-at)*sizeof dst->lines[0]);
            dst->lines[at].number = number;
            memcpy(dst->lines[at].text, data+start, end-start);
            dst->lines[at].text[end-start] = 0; dst->count++;
        }
        (*error_line)++;
    }
    *error_line = 0;
    return NULL;
}
