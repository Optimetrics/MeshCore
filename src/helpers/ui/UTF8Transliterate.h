#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Converts UTF-8 to printable ASCII for fonts that only have glyphs 0x20-0x7E:
// accented Latin letters lose their accents ("é" -> "e", "ß" -> "ss"), typographic
// punctuation becomes its ASCII look-alike, and anything else (emoji, other scripts)
// becomes `unknown`, so it shows up as a placeholder instead of vanishing.
// Tables generated from Unicode NFKD decompositions, plus hand-picked look-alikes.

static const char utf8_latin_ascii[224][4] = {   // U+00A0 .. U+017F
  " ", "!", "c", "L", "?", "Y", "|", "S", "\"", "(c)", "a", "\"", "-", "", "(R)", "-",   // U+00A0
  "o", "+-", "2", "3", "'", "u", "P", ".", ",", "1", "o", "\"", "1/4", "1/2", "3/4", "?",   // U+00B0
  "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",   // U+00C0
  "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",   // U+00D0
  "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",   // U+00E0
  "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",   // U+00F0
  "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d",   // U+0100
  "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",   // U+0110
  "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i",   // U+0120
  "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",   // U+0130
  "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "N", "n", "O", "o", "O", "o",   // U+0140
  "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",   // U+0150
  "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u",   // U+0160
  "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s",   // U+0170
};

static inline const char* utf8_punct_ascii(uint32_t cp) {
  switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
    case 0x2020: case 0x2021: return "+";
    case 0x2022: case 0x2023: case 0x2043: return "*";
    case 0x2026: return "...";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    // zero-width joiners, variation selectors and skin-tone modifiers: part of the emoji before them
    case 0x200B: case 0x200C: case 0x200D: case 0xFE0E: case 0xFE0F: return "";
  }
  if (cp >= 0x1F3FB && cp <= 0x1F3FF) return "";
  return NULL;
}

static inline void utf8_to_ascii(char* dest, const char* src, size_t dest_size, char unknown) {
  size_t j = 0;
  const unsigned char* s = (const unsigned char*)src;
  while (*s && j < dest_size - 1) {
    uint32_t cp;
    int extra;
    unsigned char c = *s;
    if (c < 0x80)      { cp = c;        extra = 0; }
    else if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    else               { cp = 0xFFFD;   extra = 0; }   // stray continuation byte
    s++;
    for (; extra > 0 && (*s & 0xC0) == 0x80; extra--, s++) cp = (cp << 6) | (*s & 0x3F);
    if (extra > 0) cp = 0xFFFD;   // truncated sequence

    const char* rep;
    char one[2] = { 0, 0 };
    if (cp >= 32 && cp <= 126) { one[0] = (char)cp; rep = one; }
    else if (cp == '\n' || cp == '\t') rep = " ";
    else if (cp < 0xA0) rep = "";   // other control characters
    else if (cp < 0x180) rep = utf8_latin_ascii[cp - 0xA0];
    else if ((rep = utf8_punct_ascii(cp)) == NULL) { one[0] = unknown; rep = one; }

    size_t n = strlen(rep);
    if (j + n > dest_size - 1) break;   // don't split a replacement
    memcpy(&dest[j], rep, n);
    j += n;
  }
  dest[j] = 0;
}
