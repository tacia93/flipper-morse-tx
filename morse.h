/*
 * Morse TX - send text in Morse code from a Flipper Zero.
 * Copyright (C) 2026 Emanuele Colucci
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * General Public License in the LICENSE file for details.
 */
/**
 * Morse code sequence builder for Flipper Zero.
 *
 * The message is pre-compiled into a flat list of on/off elements with
 * absolute timings, so the RF interrupt callback only has to pop one
 * element at a time (no allocation, no string parsing in ISR context).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MORSE_MESSAGE_MAX 64u
#define MORSE_WPM_MIN     5u
#define MORSE_WPM_MAX     40u

/** One keying element: carrier on (level 1) or off (level 0). */
typedef struct {
    uint32_t start_us; /**< offset from the start of the message */
    uint32_t duration_us;
    uint8_t level;
    uint8_t char_index; /**< index of the character in the source text */
    uint8_t sym_index; /**< index of the dot/dash inside that character */
} MorseElement;

typedef struct {
    MorseElement* elements;
    size_t count;
    size_t capacity;
    size_t pos; /**< read cursor, consumed by the async TX callback */
    uint32_t total_us;
    uint32_t unit_us; /**< duration of one "dit" */
} MorseSequence;

/** Morse pattern of a character (".-"), or NULL if not transmittable. */
const char* morse_pattern(char c);

/** True if at least one character of the text can be transmitted. */
bool morse_text_has_content(const char* text);

/** Build the keying sequence. Returns NULL if there is nothing to send. */
MorseSequence* morse_sequence_alloc(const char* text, uint32_t wpm);

void morse_sequence_free(MorseSequence* seq);

/** Reset the read cursor so the sequence can be transmitted again. */
void morse_sequence_rewind(MorseSequence* seq);

/** Index of the element playing at time_us; hint speeds up the scan. */
size_t morse_sequence_locate(const MorseSequence* seq, uint32_t time_us, size_t hint);
