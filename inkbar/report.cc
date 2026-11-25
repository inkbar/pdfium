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

void AppendWideText(std::string* out,
                    const WideString& text,
                    size_t limit,
                    Budget* budget,
                    bool* cut) {
  const size_t room = std::min(limit, budget->left());
  size_t written = 0;
  bool text_cut = false;
  json::AppendText(out, json::CutText(text, room, &written, &text_cut));
  budget->Take(written);
  if (text_cut) {
    *cut = true;
    if (room < limit) {
      budget->Spend();
    }
  }
}

void AppendFileText(std::string* out,
                    const ByteString& bytes,
                    size_t limit,
                    Budget* budget,
                    bool* cut) {
  if (budget->left() == 0) {
    // Decode nothing once the budget is spent.
    budget->Spend();
    *cut = true;
    out->append("\"\"");
    return;
  }
  if (IsText(bytes)) {
    AppendWideText(out, PDF_DecodeText(bytes.unsigned_span()), limit, budget,
                   cut);
    return;
  }
  const size_t room = std::min(limit, budget->left());
  const size_t keep = std::min(bytes.GetLength(), room / 2);
  budget->Take(keep * 2);
  if (keep < bytes.GetLength()) {
    *cut = true;
    if (room < limit) {
      budget->Spend();
    }
  }
  out->append("{\"bytes\":");
  json::AppendHex(out, bytes.First(keep));
  out->push_back('}');
}

void AppendFileName(std::string* out,
                    const ByteString& name,
                    Budget* budget,
                    bool* cut) {
  const size_t mark = out->size();
  json::AppendName(out, name);
  if (!budget->Take(out->size() - mark)) {
    out->resize(mark);
    out->append("null");
    *cut = true;
  }
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
                   Budget* budget,
                   bool* cut) {
  out->push_back('[');
  const size_t size = array ? array->size() : 0;
  for (size_t i = 0; i < size && i < max; ++i) {
    if (!budget->Take(16)) {
      *cut = true;
      break;
    }
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

void AppendNames(std::string* out,
                 const CPDF_Array* array,
                 Budget* budget,
                 bool* cut) {
  out->push_back('[');
  for (size_t i = 0; array && i < array->size(); ++i) {
    const size_t mark = out->size();
    if (i > 0) {
      out->push_back(',');
    }
    RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
    if (entry && entry->IsName()) {
      json::AppendName(out, entry->GetString());
    } else {
      out->append("null");
    }
    if (!budget->Take(out->size() - mark)) {
      out->resize(mark);
      *cut = true;
      break;
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
    // Four numbers at most: within the entry's own cost.
    Budget room(4 * 16);
    AppendNumbers(out, rect, 4, &room, &more);
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

void AppendAppearance(std::string* out,
                      const CPDF_Dictionary* dict,
                      Budget* budget,
                      bool* cut) {
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
        states->GetDirectObjectFor(as->GetString().AsStringView());
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
      const size_t mark = out->size();
      if (!first) {
        out->push_back(',');
      }
      json::AppendName(out, item.first);
      if (!budget->Take(out->size() - mark)) {
        out->resize(mark);
        *cut = true;
        break;
      }
      first = false;
    }
    out->push_back(']');
  }
  if (has_as) {
    AppendNextKey(out, "as");
    const size_t mark = out->size();
    json::AppendName(out, as->GetString());
    if (!budget->Take(out->size() - mark)) {
      out->resize(mark);
      out->append("null");
      *cut = true;
    }
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
    Budget room(3 * 16);
    AppendNumbers(out, border, 3, &room, &more);
  }
}

ScriptReader::ScriptReader(Budget* budget) : budget_(budget) {}

ScriptReader::~ScriptReader() = default;

bool ScriptReader::Work(size_t units) {
  if (units > work_left_) {
    work_left_ = 0;
    exhausted_ = true;
    return false;
  }
  work_left_ -= units;
  return true;
}

const WideString& ScriptReader::Decode(const CPDF_Dictionary* action,
                                       bool* cut) {
  static const WideString* const kEmpty = new WideString();
  RetainPtr<const CPDF_Object> js = action->GetDirectObjectFor("JS");
  if (!js || !(js->IsString() || js->IsStream())) {
    return *kEmpty;
  }
  auto it = decoded_.find(js.Get());
  if (it != decoded_.end()) {
    return it->second;
  }
  // Decode nothing once the budget is spent.
  if (budget_->left() == 0) {
    budget_->Spend();
    *cut = true;
    return *kEmpty;
  }
  size_t written = 0;
  bool source_cut = false;
  WideString text = json::CutText(js->GetUnicodeText(), kMaxScriptBytes,
                                  &written, &source_cut);
  *cut = *cut || source_cut;
  return decoded_.emplace(js.Get(), std::move(text)).first->second;
}

const std::vector<WideString>& ScriptReader::Sources(const CPDF_Object* action,
                                                     bool* cut) {
  if (!action || !Work(1)) {
    if (action) {
      *cut = true;
    }
    return none_;
  }
  RetainPtr<const CPDF_Object> first = action->GetDirect();
  if (!first) {
    return none_;
  }
  auto memo = chains_.find(first.Get());
  if (memo != chains_.end()) {
    *cut = *cut || memo->second.cut;
    return memo->second.sources;
  }
  Chain chain;
  size_t read = 0;
  std::set<const CPDF_Object*> seen;
  // Depth first: an action, then its /Next in order. Pushed last first.
  std::vector<RetainPtr<const CPDF_Object>> stack;
  stack.push_back(first);
  while (!stack.empty()) {
    RetainPtr<const CPDF_Object> object = stack.back()->GetDirect();
    stack.pop_back();
    if (!object || !seen.insert(object.Get()).second) {
      continue;
    }
    if (++read > kMaxChain || !Work(1)) {
      chain.cut = true;
      break;
    }
    if (const CPDF_Array* array = object->AsArray()) {
      if (!Work(array->size())) {
        chain.cut = true;
        break;
      }
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
    if (dict->GetNameFor("S") == "JavaScript") {
      chain.sources.push_back(Decode(dict, &chain.cut));
    }
    RetainPtr<const CPDF_Object> next = dict->GetObjectFor("Next");
    if (next) {
      stack.push_back(std::move(next));
    }
  }
  *cut = *cut || chain.cut;
  return chains_.emplace(first.Get(), std::move(chain)).first->second.sources;
}

const ScriptReader::Keyed& ScriptReader::KeysWithScripts(
    const CPDF_Dictionary* aa,
    bool* cut) {
  if (!aa || !Work(1)) {
    if (aa) {
      *cut = true;
    }
    return no_keys_;
  }
  auto memo = keyed_.find(aa);
  if (memo != keyed_.end()) {
    return memo->second;
  }
  Keyed keys;
  CPDF_DictionaryLocker locker(aa);
  for (const auto& item : locker) {
    if (!Work(1)) {
      *cut = true;
      break;
    }
    if (!Sources(item.second.Get(), cut).empty()) {
      keys.emplace_back(item.first, item.second);
    }
  }
  return keyed_.emplace(aa, std::move(keys)).first->second;
}

size_t AppendSources(std::string* out,
                     const std::vector<WideString>& sources,
                     Budget* budget,
                     bool* cut) {
  // The brackets.
  if (!budget->Take(2)) {
    *cut = true;
  }
  out->push_back('[');
  size_t count = 0;
  for (const WideString& source : sources) {
    // Quotes and a comma.
    if (!budget->Take(3)) {
      *cut = true;
      break;
    }
    if (count > 0) {
      out->push_back(',');
    }
    AppendWideText(out, source, kMaxScriptBytes, budget, cut);
    ++count;
    if (budget->spent()) {
      break;
    }
  }
  out->push_back(']');
  return count;
}

size_t AppendScripts(std::string* out,
                     ScriptReader* reader,
                     const CPDF_Object* action,
                     bool* cut) {
  return AppendSources(out, reader->Sources(action, cut), reader->budget(),
                       cut);
}

char* Copy(const std::string& text) {
  char* json = static_cast<char*>(malloc(text.size() + 1));
  if (json) {
    memcpy(json, text.c_str(), text.size() + 1);
  }
  return json;
}

}  // namespace inkbar::report
