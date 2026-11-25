// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// JSON writing shared by text.cc, links.cc and report.cc: strings escaped the
// same way in each, and floats written so that they read back as the same
// float.

#ifndef INKBAR_JSON_H_
#define INKBAR_JSON_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cmath>
#include <string>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/widestring.h"

namespace inkbar::json {

// Appends |c| to a JSON string: quotes and backslashes escaped, control
// characters and DEL as \u escapes, anything else as UTF-8. A surrogate or a
// value past U+10FFFF is not a character and becomes U+FFFD.
inline void AppendCodePoint(std::string* out, uint32_t c) {
  if (c == '"' || c == '\\') {
    out->push_back('\\');
    out->push_back(static_cast<char>(c));
  } else if (c < 0x20 || c == 0x7F) {
    char escape[8];
    snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
    out->append(escape);
  } else if (c < 0x80) {
    out->push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (c >> 6)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
    AppendCodePoint(out, 0xFFFD);
  } else if (c < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (c >> 12)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (c >> 18)));
    out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// A JSON string of text PDFium decoded.
inline void AppendText(std::string* out, const WideString& text) {
  out->push_back('"');
  for (wchar_t c : text) {
    AppendCodePoint(out, static_cast<uint32_t>(c));
  }
  out->push_back('"');
}

// A JSON string of a name's bytes, each byte the code point of its value.
inline void AppendName(std::string* out, const ByteString& name) {
  out->push_back('"');
  for (char c : name) {
    AppendCodePoint(out, static_cast<unsigned char>(c));
  }
  out->push_back('"');
}

// A JSON string of bytes in lowercase hex.
inline void AppendHex(std::string* out, const ByteString& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  out->push_back('"');
  for (char c : bytes) {
    const unsigned char byte = static_cast<unsigned char>(c);
    out->push_back(kDigits[byte >> 4]);
    out->push_back(kDigits[byte & 0xF]);
  }
  out->push_back('"');
}

// A float with 9 significant digits, which reads back as the same float.
// null when it is not finite.
inline void AppendNumber(std::string* out, float value) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.9g", value);
  out->append(buffer);
}

// A float as the shortest decimal (at most 9 significant digits) that reads
// back as the same float: 0.4f is written 0.4, not 0.400000006. null when it
// is not finite.
inline void AppendShortestNumber(std::string* out, float value) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  char buffer[32];
  for (int digits = 1; digits <= 9; ++digits) {
    snprintf(buffer, sizeof(buffer), "%.*g", digits,
             static_cast<double>(value));
    if (strtof(buffer, nullptr) == value) {
      break;
    }
  }
  out->append(buffer);
}

// The bytes AppendCodePoint writes for |c|: 2 for a quote or a backslash, 6
// for a control character or DEL (\u00XX), else its UTF-8. A surrogate and a
// value past U+10FFFF become U+FFFD, which takes 3.
inline size_t WrittenSize(uint32_t c) {
  if (c == '"' || c == '\\') {
    return 2;
  }
  if (c < 0x20 || c == 0x7F) {
    return 6;
  }
  if (c < 0x80) {
    return 1;
  }
  if (c < 0x800) {
    return 2;
  }
  if (c < 0x10000 || c > 0x10FFFF) {
    return 3;
  }
  return 4;
}

// |text| cut after the last character whose written form ends within |limit|
// bytes. Adds the bytes it keeps to |*written| and sets |*cut| when it was
// longer.
inline WideString CutText(const WideString& text,
                          size_t limit,
                          size_t* written,
                          bool* cut) {
  size_t bytes = 0;
  size_t length = 0;
  *cut = false;
  for (wchar_t c : text) {
    const size_t size = WrittenSize(static_cast<uint32_t>(c));
    if (bytes + size > limit) {
      *cut = true;
      break;
    }
    bytes += size;
    ++length;
  }
  *written += bytes;
  return *cut ? text.First(length) : text;
}

}  // namespace inkbar::json

#endif  // INKBAR_JSON_H_
