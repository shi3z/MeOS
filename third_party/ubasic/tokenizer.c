/*
 * Copyright (c) 2006, Adam Dunkels
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/* MeOS port: bounded parsing, checked int32 arithmetic, callback I/O, no OS exit. */
#include "tokenizer.h"
#include <stdint.h>
#include <m5os/number.h>
#include <string.h>

static const char *ptr, *nextptr, *error;
static int token;
static double number;
static size_t string_length;
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int alpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
static int digit(char c) { return c >= '0' && c <= '9'; }
static const struct { const char *name; int token; } keywords[] = {
    {"let",TOKENIZER_LET}, {"print",TOKENIZER_PRINT}, {"if",TOKENIZER_IF},
    {"input",TOKENIZER_INPUT}, {"dim",TOKENIZER_DIM},
    {"sin",TOKENIZER_SIN}, {"cos",TOKENIZER_COS}, {"tan",TOKENIZER_TAN},
    {"sqr",TOKENIZER_SQRT}, {"sqrt",TOKENIZER_SQRT},
    {"log",TOKENIZER_LOG}, {"ln",TOKENIZER_LOG}, {"log10",TOKENIZER_LOG10},
    {"exp",TOKENIZER_EXP}, {"abs",TOKENIZER_ABS}, {"pi",TOKENIZER_PI},
    {"deg",TOKENIZER_DEG}, {"rad",TOKENIZER_RAD},
    {"int",TOKENIZER_INT}, {"fix",TOKENIZER_FIX}, {"step",TOKENIZER_STEP},
    {"screen",TOKENIZER_SCREEN}, {"cls",TOKENIZER_CLS}, {"pset",TOKENIZER_PSET},
    {"line",TOKENIZER_LINE}, {"text",TOKENIZER_TEXT}, {"wait",TOKENIZER_WAIT},
    {"then",TOKENIZER_THEN}, {"else",TOKENIZER_ELSE}, {"for",TOKENIZER_FOR},
    {"to",TOKENIZER_TO}, {"next",TOKENIZER_NEXT}, {"goto",TOKENIZER_GOTO},
    {"gosub",TOKENIZER_GOSUB}, {"return",TOKENIZER_RETURN},
    {"rem",TOKENIZER_REM}, {"end",TOKENIZER_END}
};
static int scan(void)
{
    while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r') ptr++;
    nextptr = ptr;
    error = NULL;
    if (!*ptr) return TOKENIZER_ENDOFINPUT;
    if (digit(*ptr) || (*ptr == '.' && digit(ptr[1]))) {
        error = number_parse(ptr, &nextptr, &number);
        return error ? TOKENIZER_ERROR : TOKENIZER_NUMBER;
    }
    if (*ptr == '"') {
        nextptr++;
        while (*nextptr && *nextptr != '\n' && *nextptr != '\r' && *nextptr != '"') nextptr++;
        if (*nextptr != '"') { error = "unterminated string"; return TOKENIZER_ERROR; }
        string_length = (size_t)(nextptr - ptr - 1);
        nextptr++; return TOKENIZER_STRING;
    }
    if (alpha(*ptr)) {
        while (alpha(*nextptr) || digit(*nextptr) || *nextptr == '_') nextptr++;
        size_t len = (size_t)(nextptr - ptr);
        for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++) {
            const char *k = keywords[i].name;
            size_t j = 0;
            while (j < len && k[j] && lower(ptr[j]) == k[j]) j++;
            if (j == len && !k[j]) {
                if (keywords[i].token == TOKENIZER_REM) {
                    while (*nextptr && *nextptr != '\n') nextptr++;
                }
                return keywords[i].token;
            }
        }
        if (len == 1) return TOKENIZER_VARIABLE;
        error = "unknown keyword (variables are A-Z)"; return TOKENIZER_ERROR;
    }
    nextptr++;
    switch (*ptr) {
    case '\n': return TOKENIZER_CR;
    case ',': return TOKENIZER_COMMA;
    case ';': return TOKENIZER_SEMICOLON;
    case '+': return TOKENIZER_PLUS;
    case '-': return TOKENIZER_MINUS;
    case '&': return TOKENIZER_AND;
    case '|': return TOKENIZER_OR;
    case '*': return TOKENIZER_ASTR;
    case '/': return TOKENIZER_SLASH;
    case '%': return TOKENIZER_MOD;
    case '^': return TOKENIZER_POWER;
    case '(': return TOKENIZER_LEFTPAREN;
    case ')': return TOKENIZER_RIGHTPAREN;
    case '<': return TOKENIZER_LT;
    case '>': return TOKENIZER_GT;
    case '=': return TOKENIZER_EQ;
    default: error = "invalid character"; return TOKENIZER_ERROR;
    }
}
void tokenizer_goto(const char *program) { ptr = program; token = scan(); }
void tokenizer_init(const char *program) { tokenizer_goto(program); }
void tokenizer_next(void) { ptr = nextptr; token = scan(); }
int tokenizer_token(void) { return token; }
int tokenizer_finished(void) { return token == TOKENIZER_ENDOFINPUT; }
double tokenizer_num(void) { return number; }
int tokenizer_variable_num(void) { return lower(*ptr) - 'a'; }
void tokenizer_string(char *dest, int len)
{
    if (len <= 0) return;
    size_t n = string_length < (size_t)(len - 1) ? string_length : (size_t)(len - 1);
    memcpy(dest, ptr + 1, n); dest[n] = 0;
}
size_t tokenizer_string_length(void) { return string_length; }
const char *tokenizer_pos(void) { return ptr; }
const char *tokenizer_error(void) { return error; }
void tokenizer_error_print(void) { /* Caller reports errors through its callback. */ }
