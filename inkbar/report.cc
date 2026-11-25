// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See report.h.

#include "inkbar/report.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfdoc/cpdf_action.h"
#include "core/fxcrt/span.h"
#include "inkbar/json.h"

namespace inkbar::report {

namespace {

// Whether |bytes| from |start| are UTF-16 in the byte order given, with every
// surrogate paired.
bool IsUtf16(const ByteString& bytes, size_t start, bool big_endian) {
  if ((bytes.GetLength() - start) % 2 != 0) {
    return false;
  }
  bool want_low = false;
  for (size_t i = start; i < bytes.GetLength(); i += 2) {
    const uint8_t a = static_cast<uint8_t>(bytes[i]);
    const uint8_t b = static_cast<uint8_t>(bytes[i + 1]);
    const uint16_t unit = big_endian ? (a << 8 | b) : (b << 8 | a);
    const bool high = unit >= 0xD800 && unit <= 0xDBFF;
    const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
    if (want_low != low) {
      return false;
    }
    want_low = high;
  }
  return !want_low;
}

// Whether |bytes| from |start| are well-formed UTF-8.
bool IsUtf8(const ByteString& bytes, size_t start) {
  const size_t size = bytes.GetLength();
  size_t i = start;
  while (i < size) {
    const uint8_t c = static_cast<uint8_t>(bytes[i]);
    size_t more = 0;
    uint32_t value = 0;
    uint32_t least = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      more = 1;
      value = c & 0x1F;
      least = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      more = 2;
      value = c & 0x0F;
      least = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      more = 3;
      value = c & 0x07;
      least = 0x10000;
    } else {
      return false;
    }
    if (i + more >= size) {
      return false;
    }
    for (size_t k = 1; k <= more; ++k) {
      const uint8_t next = static_cast<uint8_t>(bytes[i + k]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      value = value << 6 | (next & 0x3F);
    }
    if (value < least || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      return false;
    }
    i += more + 1;
  }
  return true;
}

// Whether PDFDocEncoding defines every byte of |bytes|.
bool IsPdfDoc(const ByteString& bytes) {
  for (char c : bytes) {
    const uint8_t byte = static_cast<uint8_t>(c);
    if (byte == 0x7F || byte == 0x9F || byte == 0xAD) {
      return false;
    }
  }
  return true;
}

bool IsText(const ByteString& bytes) {
  const size_t size = bytes.GetLength();
  const auto at = [&bytes](size_t i) { return static_cast<uint8_t>(bytes[i]); };
  if (size >= 2 && at(0) == 0xFE && at(1) == 0xFF) {
    return IsUtf16(bytes, 2, true);
  }
  if (size >= 2 && at(0) == 0xFF && at(1) == 0xFE) {
    return IsUtf16(bytes, 2, false);
  }
  if (size >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
    return IsUtf8(bytes, 3);
  }
  return IsPdfDoc(bytes);
}

}  // namespace

const char* Boolean(bool value) {
  return value ? "true" : "false";
}

void AppendKey(std::string* out, const char* key) {
  out->push_back('"');
  out->append(key);
  out->append("\":");
}

void AppendNextKey(std::string* out, const char* key) {
  out->push_back(',');
  AppendKey(out, key);
}

RetainPtr<const CPDF_Object> EntryFor(const CPDF_Dictionary* dict,
                                      const char* key) {
  if (!dict) {
    return nullptr;
  }
  RetainPtr<const CPDF_Object> object = dict->GetDirectObjectFor(key);
  return object && !object->IsNull() ? object : nullptr;
}

void AppendFileText(std::string* out,
                    const ByteString& bytes,
                    size_t limit,
                    bool* cut) {
  if (IsText(bytes)) {
    size_t written = 0;
    bool text_cut = false;
    json::AppendText(out, json::CutText(PDF_DecodeText(bytes.unsigned_span()),
                                        limit, &written, &text_cut));
    *cut = *cut || text_cut;
    return;
  }
  const size_t keep = std::min(bytes.GetLength(), limit / 2);
  if (keep < bytes.GetLength()) {
    *cut = true;
  }
  out->append("{\"bytes\":");
  json::AppendHex(out, bytes.First(keep));
  out->push_back('}');
}

void AppendRef(std::string* out, const CPDF_Object* object) {
  const CPDF_Reference* ref = object ? object->AsReference() : nullptr;
  if (!ref) {
    out->append("null");
    return;
  }
  RetainPtr<const CPDF_Object> target = ref->GetDirect();
  out->push_back('[');
  out->append(std::to_string(ref->GetRefObjNum()));
  out->push_back(',');
  out->append(std::to_string(target ? target->GetGenNum() : 0));
  out->push_back(']');
}

void AppendNumbers(std::string* out,
                   const CPDF_Array* array,
                   size_t max,
                   bool* cut) {
  out->push_back('[');
  const size_t size = array ? array->size() : 0;
  for (size_t i = 0; i < size && i < max; ++i) {
    if (i > 0) {
      out->push_back(',');
    }
    const CPDF_Number* number = ToNumber(array->GetDirectObjectAt(i).Get());
    if (number) {
      json::AppendNumber(out, number->GetNumber());
    } else {
      out->append("null");
    }
  }
  if (size > max) {
    *cut = true;
  }
  out->push_back(']');
}

void AppendNames(std::string* out, const CPDF_Array* array) {
  out->push_back('[');
  for (size_t i = 0; array && i < array->size(); ++i) {
    if (i > 0) {
      out->push_back(',');
    }
    RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
    if (entry && entry->IsName()) {
      json::AppendName(out, entry->GetString());
    } else {
      out->append("null");
    }
  }
  out->push_back(']');
}

void AppendRect(std::string* out, const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Object> object = EntryFor(dict, "Rect");
  const CPDF_Array* rect = object ? object->AsArray() : nullptr;
  AppendNextKey(out, "rect");
  bool ok = rect && rect->size() == 4;
  if (rect) {
    bool more = false;
    AppendNumbers(out, rect, 4, &more);
    for (size_t i = 0; ok && i < 4; ++i) {
      const CPDF_Number* number = ToNumber(rect->GetDirectObjectAt(i).Get());
      ok = number && std::isfinite(number->GetNumber());
    }
  } else {
    out->append("null");
  }
  AppendNextKey(out, "rectOk");
  out->append(Boolean(ok));
}

void AppendAppearance(std::string* out, const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Object> ap_object = EntryFor(dict, "AP");
  const CPDF_Dictionary* ap = ap_object ? ap_object->AsDictionary() : nullptr;
  RetainPtr<const CPDF_Object> normal = EntryFor(ap, "N");
  const CPDF_Dictionary* states =
      normal && !normal->IsStream() ? normal->AsDictionary() : nullptr;
  RetainPtr<const CPDF_Object> as = EntryFor(dict, "AS");
  const bool has_as = as && as->IsName();
  bool has_appearance = normal && normal->IsStream();
  if (states && has_as) {
    RetainPtr<const CPDF_Object> chosen =
        EntryFor(states, as->GetString().c_str());
    has_appearance = chosen && chosen->IsStream();
  }
  AppendNextKey(out, "hasAppearance");
  out->append(Boolean(has_appearance));
  AppendNextKey(out, "appearance");
  out->append(normal && normal->IsStream() ? "\"stream\""
              : states                     ? "\"states\""
                                           : "null");
  if (states) {
    AppendNextKey(out, "states");
    out->push_back('[');
    bool first = true;
    CPDF_DictionaryLocker locker(states);
    for (const auto& item : locker) {
      if (!first) {
        out->push_back(',');
      }
      first = false;
      json::AppendName(out, item.first);
    }
    out->push_back(']');
  }
  if (has_as) {
    AppendNextKey(out, "as");
    json::AppendName(out, as->GetString());
  }
}

void AppendBorder(std::string* out, const CPDF_Dictionary* dict) {
  RetainPtr<const CPDF_Object> bs_object = EntryFor(dict, "BS");
  if (const CPDF_Dictionary* bs = bs_object ? bs_object->AsDictionary()
                                            : nullptr) {
    AppendNextKey(out, "borderStyle");
    out->push_back('{');
    AppendKey(out, "width");
    const CPDF_Number* width = ToNumber(EntryFor(bs, "W").Get());
    if (width) {
      json::AppendNumber(out, width->GetNumber());
    } else {
      out->append("null");
    }
    AppendNextKey(out, "style");
    RetainPtr<const CPDF_Object> style = EntryFor(bs, "S");
    if (style && style->IsName()) {
      json::AppendName(out, style->GetString());
    } else {
      out->append("null");
    }
    out->push_back('}');
  }
  RetainPtr<const CPDF_Object> border_object = EntryFor(dict, "Border");
  if (const CPDF_Array* border =
          border_object ? border_object->AsArray() : nullptr) {
    AppendNextKey(out, "border");
    bool more = false;
    AppendNumbers(out, border, 3, &more);
  }
}

std::vector<WideString> CollectScripts(const CPDF_Object* action, bool* cut) {
  std::vector<WideString> sources;
  size_t read = 0;
  std::set<const CPDF_Object*> seen;
  // Depth first: an action, then its /Next in order. Pushed last first.
  std::vector<RetainPtr<const CPDF_Object>> stack;
  if (action) {
    stack.push_back(pdfium::WrapRetain(action));
  }
  while (!stack.empty()) {
    RetainPtr<const CPDF_Object> object = stack.back()->GetDirect();
    stack.pop_back();
    if (!object || !seen.insert(object.Get()).second) {
      continue;
    }
    if (const CPDF_Array* array = object->AsArray()) {
      for (size_t i = array->size(); i > 0; --i) {
        RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i - 1);
        if (entry) {
          stack.push_back(std::move(entry));
        }
      }
      continue;
    }
    const CPDF_Dictionary* dict = object->AsDictionary();
    if (!dict) {
      continue;
    }
    if (++read > kMaxChain) {
      *cut = true;
      break;
    }
    if (dict->GetNameFor("S") == "JavaScript") {
      CPDF_Action js(pdfium::WrapRetain(dict));
      size_t written = 0;
      bool source_cut = false;
      sources.push_back(json::CutText(js.GetJavaScript(), kMaxScriptBytes,
                                      &written, &source_cut));
      *cut = *cut || source_cut;
    }
    RetainPtr<const CPDF_Object> next = dict->GetObjectFor("Next");
    if (next) {
      stack.push_back(std::move(next));
    }
  }
  return sources;
}

size_t AppendScripts(std::string* out, const CPDF_Object* action, bool* cut) {
  const std::vector<WideString> sources = CollectScripts(action, cut);
  out->push_back('[');
  for (size_t i = 0; i < sources.size(); ++i) {
    if (i > 0) {
      out->push_back(',');
    }
    json::AppendText(out, sources[i]);
  }
  out->push_back(']');
  return sources.size();
}

char* Copy(const std::string& text) {
  char* json = static_cast<char*>(malloc(text.size() + 1));
  if (json) {
    memcpy(json, text.c_str(), text.size() + 1);
  }
  return json;
}

}  // namespace inkbar::report
