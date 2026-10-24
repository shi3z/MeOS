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

#pragma once
#include <stddef.h>
#include <stdint.h>
/* MeOS adaptation of Adam Dunkels' uBASIC; see LICENSE and PORT.md.
 * Single instance, shell-owned; source remains valid until execution finishes.
 * No heap, file I/O, raw memory access or process exit. */
typedef void (*ubasic_output)(const char *, size_t, void *);
void ubasic_init(const char *program, ubasic_output output, void *opaque, int clear_variables);
void ubasic_run(void); /* one physical source line; bounded parser work */
int ubasic_finished(void);
const char *ubasic_error(void);
int ubasic_error_line(void);
double ubasic_get_variable(int index);

#define UBASIC_ARRAY_CELLS 1024
/* DIM uses a fixed shared pool; bounds are inclusive, zero based, one dimension.
 * Clear-variable initialization resets scalar variables, dimensions and cells.
 * INPUT pauses between ubasic_run calls, including on the final source line.
 * Invalid replies preserve the target and waiting state; replies are decimal
 * signed decimal binary64 text only. The caller owns line input and cancellation. */
int ubasic_waiting_input(void);
const char *ubasic_supply_input(const char *text);
void ubasic_stop(void); /* cancellation: no pending INPUT or further execution */

/* Optional host graphics; callbacks return NULL on success, an error otherwise.
 * WAIT cancellation sets a flag via ubasic_interrupt(); the engine then performs
 * its local recovery and the session returns a break result. */
enum { UBASIC_SCREEN, UBASIC_CLS, UBASIC_PSET, UBASIC_LINE, UBASIC_TEXT, UBASIC_WAIT };
typedef const char *(*ubasic_graphics)(void *opaque, int op, const int32_t args[5], const char *text, size_t length);
void ubasic_set_graphics(ubasic_graphics);
void ubasic_interrupt(void);
int ubasic_interrupted(void);
