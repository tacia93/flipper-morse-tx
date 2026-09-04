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
#include "morse.h"

#include <furi.h>
#include <string.h>

#define MORSE_UNITS_INTRA_SYMBOL 1u /* gap between dots/dashes of a letter */
#define MORSE_UNITS_INTER_LETTER 3u
#define MORSE_UNITS_INTER_WORD   7u

static const char* const morse_letters[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--..",
};

static const char* const morse_digits[10] = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----.",
};

typedef struct {
    char c;
    const char* pattern;
} MorsePunctuation;

static const MorsePunctuation morse_punctuation[] = {
    {'.', ".-.-.-"},  {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"},  {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},   {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},   {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."},
};

const char* morse_pattern(char c) {
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if(c >= 'A' && c <= 'Z') return morse_letters[c - 'A'];
    if(c >= '0' && c <= '9') return morse_digits[c - '0'];
    for(size_t i = 0; i < COUNT_OF(morse_punctuation); i++) {
        if(morse_punctuation[i].c == c) return morse_punctuation[i].pattern;
    }
    return NULL;
}

bool morse_text_has_content(const char* text) {
    if(!text) return false;
    for(size_t i = 0; text[i]; i++) {
        if(morse_pattern(text[i])) return true;
    }
    return false;
}

static void
    morse_push(MorseSequence* seq, uint8_t level, uint32_t duration, uint8_t ci, uint8_t si) {
    if(seq->count >= seq->capacity) return;
    MorseElement* element = &seq->elements[seq->count++];
    element->start_us = seq->total_us;
    element->duration_us = duration;
    element->level = level;
    element->char_index = ci;
    element->sym_index = si;
    seq->total_us += duration;
}

/**
 * Stretch the trailing silence to `units`. Gaps are never emitted as two
 * consecutive elements: the sub-ghz HAL expects strictly alternating levels.
 */
static void morse_gap_at_least(MorseSequence* seq, uint32_t units) {
    if(seq->count == 0) return;
    MorseElement* element = &seq->elements[seq->count - 1];
    if(element->level) return;
    uint32_t wanted = units * seq->unit_us;
    if(element->duration_us < wanted) {
        seq->total_us += wanted - element->duration_us;
        element->duration_us = wanted;
    }
}

MorseSequence* morse_sequence_alloc(const char* text, uint32_t wpm) {
    if(!text || !morse_text_has_content(text)) return NULL;
    if(wpm < MORSE_WPM_MIN) wpm = MORSE_WPM_MIN;
    if(wpm > MORSE_WPM_MAX) wpm = MORSE_WPM_MAX;

    size_t len = strlen(text);
    if(len > MORSE_MESSAGE_MAX) len = MORSE_MESSAGE_MAX;

    MorseSequence* seq = malloc(sizeof(MorseSequence));
    seq->unit_us = 1200000u / wpm; /* PARIS standard: 1 unit = 1200 ms / WPM */
    seq->capacity = len * 16 + 8; /* worst case: 7 symbols -> 14 elements + gaps */
    seq->elements = malloc(sizeof(MorseElement) * seq->capacity);
    seq->count = 0;
    seq->pos = 0;
    seq->total_us = 0;

    for(size_t i = 0; i < len; i++) {
        char c = text[i];
        if(c == ' ') {
            if(seq->count) morse_gap_at_least(seq, MORSE_UNITS_INTER_WORD);
            continue;
        }
        const char* pattern = morse_pattern(c);
        if(!pattern) continue;

        if(seq->count) morse_gap_at_least(seq, MORSE_UNITS_INTER_LETTER);

        for(uint8_t s = 0; pattern[s]; s++) {
            uint32_t marks = (pattern[s] == '-') ? 3u : 1u;
            morse_push(seq, 1, marks * seq->unit_us, (uint8_t)i, s);
            morse_push(seq, 0, MORSE_UNITS_INTRA_SYMBOL * seq->unit_us, (uint8_t)i, s);
        }
    }

    if(seq->count == 0) {
        morse_sequence_free(seq);
        return NULL;
    }

    /* trailing silence, also acts as the gap before a repeat */
    morse_gap_at_least(seq, MORSE_UNITS_INTER_WORD);
    return seq;
}

void morse_sequence_free(MorseSequence* seq) {
    if(!seq) return;
    if(seq->elements) free(seq->elements);
    free(seq);
}

void morse_sequence_rewind(MorseSequence* seq) {
    if(seq) seq->pos = 0;
}

size_t morse_sequence_locate(const MorseSequence* seq, uint32_t time_us, size_t hint) {
    if(!seq || seq->count == 0) return 0;
    if(hint >= seq->count) hint = 0;
    if(time_us < seq->elements[hint].start_us) hint = 0;

    size_t i = hint;
    while((i + 1) < seq->count && time_us >= seq->elements[i + 1].start_us) {
        i++;
    }
    return i;
}
