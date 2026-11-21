// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See links.h. Reads dictionaries directly, with CPDF_Dest for a destination's
// page and fit, CPDF_Action for a JavaScript action's source and
// CPDF_NameTree for named destinations.

#include "inkbar/links.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfdoc/cpdf_action.h"
#include "core/fpdfdoc/cpdf_dest.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"

namespace {

constexpr int kMaxOutlineDepth = 64;
constexpr size_t kMaxOutlineItems = 100000;

// Indexed by the PDFDEST_VIEW_* values CPDF_Dest::GetZoomMode() returns.
constexpr std::array<const char*, 9> kFitNames = {
    "Unknown", "XYZ", "Fit", "FitH", "FitV", "FitR", "FitB", "FitBH", "FitBV"};

void AppendEscaped(std::string* out, uint32_t c) {
  char escape[8];
  snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
  out->append(escape);
}

// Appends |c| to a JSON string: quotes, backslashes and control characters
// escaped, anything else as UTF-8. A surrogate or a value past U+10FFFF is
// not a character and becomes U+FFFD.
void AppendCodePoint(std::string* out, uint32_t c) {
  if (c == '"' || c == '\\') {
    out->push_back('\\');
    out->push_back(static_cast<char>(c));
  } else if (c < 0x20 || c == 0x7F) {
    AppendEscaped(out, c);
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
void AppendText(std::string* out, const WideString& text) {
  out->push_back('"');
  for (wchar_t c : text) {
    AppendCodePoint(out, static_cast<uint32_t>(c));
  }
  out->push_back('"');
}

// A JSON string of a name's bytes, each byte the code point of its value.
void AppendName(std::string* out, const ByteString& name) {
  out->push_back('"');
  for (char c : name) {
    AppendCodePoint(out, static_cast<unsigned char>(c));
  }
  out->push_back('"');
}

// A JSON string of bytes in lowercase hex.
void AppendHex(std::string* out, const ByteString& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  out->push_back('"');
  for (char c : bytes) {
    const unsigned char byte = static_cast<unsigned char>(c);
    out->push_back(kDigits[byte >> 4]);
    out->push_back(kDigits[byte & 0xF]);
  }
  out->push_back('"');
}

void AppendNumber(std::string* out, float value) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.7g", value);
  out->append(buffer);
}

void AppendKey(std::string* out, const char* key) {
  out->push_back('"');
  out->append(key);
  out->append("\":");
}

const char* Boolean(bool value) {
  return value ? "true" : "false";
}

// A direct object, with a null object taken as absent.
RetainPtr<const CPDF_Object> EntryFor(const CPDF_Dictionary* dict,
                                      const char* key) {
  RetainPtr<const CPDF_Object> object = dict->GetDirectObjectFor(key);
  return object && !object->IsNull() ? object : nullptr;
}

bool IsFiniteNumber(const CPDF_Object* object) {
  const CPDF_Number* number = ToNumber(object);
  return number && std::isfinite(number->GetNumber());
}

// Whether |array| holds only finite numbers.
bool AllFiniteNumbers(const CPDF_Array* array) {
  for (size_t i = 0; i < array->size(); ++i) {
    if (!IsFiniteNumber(array->GetDirectObjectAt(i).Get())) {
      return false;
    }
  }
  return true;
}

void AppendDest(std::string* out, CPDF_Document* doc, const CPDF_Object* dest) {
  if (dest && (dest->IsString() || dest->IsName())) {
    out->push_back('{');
    AppendKey(out, "nameHex");
    AppendHex(out, dest->GetString());
    out->push_back('}');
    return;
  }
  const CPDF_Array* array = dest ? dest->AsArray() : nullptr;
  if (!array) {
    out->append("null");
    return;
  }
  CPDF_Dest explicit_dest(pdfium::WrapRetain(array));
  out->push_back('{');
  AppendKey(out, "page");
  out->append(std::to_string(explicit_dest.GetDestPageIndex(doc)));
  out->push_back(',');
  AppendKey(out, "fit");
  const int mode = explicit_dest.GetZoomMode();
  out->push_back('"');
  out->append(
      kFitNames[mode >= 0 && mode < static_cast<int>(kFitNames.size()) ? mode
                                                                       : 0]);
  out->append("\",");
  AppendKey(out, "args");
  out->push_back('[');
  for (size_t i = 2; i < 6; ++i) {
    if (i > 2) {
      out->push_back(',');
    }
    const CPDF_Number* number = ToNumber(array->GetDirectObjectAt(i).Get());
    if (number) {
      AppendNumber(out, number->GetNumber());
    } else {
      out->append("null");
    }
  }
  out->append("]}");
}

// /F of a remote go-to or a launch: a string, or the first string of a file
// specification's platform entries, in the order PDF.js reads them.
void AppendFile(std::string* out, const CPDF_Dictionary* action) {
  RetainPtr<const CPDF_Object> file = EntryFor(action, "F");
  if (file && file->IsString()) {
    AppendHex(out, file->GetString());
    return;
  }
  if (const CPDF_Dictionary* spec = file ? file->AsDictionary() : nullptr) {
    for (const char* key : {"UF", "F", "Unix", "Mac", "DOS"}) {
      RetainPtr<const CPDF_Object> entry = EntryFor(spec, key);
      if (entry && entry->IsString()) {
        AppendHex(out, entry->GetString());
        return;
      }
    }
  }
  out->append("null");
}

void AppendAction(std::string* out,
                  CPDF_Document* doc,
                  const CPDF_Dictionary* action) {
  if (!action) {
    out->append("null");
    return;
  }
  const ByteString s = action->GetNameFor("S");
  out->push_back('{');
  AppendKey(out, "s");
  AppendName(out, s);
  if (s == "URI") {
    out->push_back(',');
    AppendKey(out, "uriHex");
    RetainPtr<const CPDF_Object> uri = EntryFor(action, "URI");
    AppendHex(out, uri && uri->IsString() ? uri->GetString() : ByteString());
  } else if (s == "GoTo") {
    out->push_back(',');
    AppendKey(out, "dest");
    AppendDest(out, doc, EntryFor(action, "D").Get());
  } else if (s == "GoToR" || s == "Launch") {
    out->push_back(',');
    AppendKey(out, "fileHex");
    AppendFile(out, action);
    out->push_back(',');
    AppendKey(out, "dest");
    AppendDest(out, doc, EntryFor(action, "D").Get());
    out->push_back(',');
    AppendKey(out, "newWindow");
    const CPDF_Boolean* new_window =
        ToBoolean(EntryFor(action, "NewWindow").Get());
    out->append(new_window ? Boolean(new_window->GetInteger() != 0) : "null");
  } else if (s == "Named") {
    out->push_back(',');
    AppendKey(out, "name");
    RetainPtr<const CPDF_Object> name = EntryFor(action, "N");
    AppendName(out, name && name->IsName() ? name->GetString() : ByteString());
  } else if (s == "JavaScript") {
    out->push_back(',');
    AppendKey(out, "js");
    CPDF_Action js(pdfium::WrapRetain(action));
    AppendText(out, js.GetJavaScript());
  }
  out->push_back('}');
}

void AppendAnnotEntry(std::string* out,
                      size_t index,
                      const CPDF_Dictionary* annot,
                      bool rect_ok,
                      bool quads_ok) {
  out->push_back('{');
  AppendKey(out, "index");
  out->append(std::to_string(index));
  out->push_back(',');
  AppendKey(out, "subtype");
  AppendName(out, annot->GetNameFor("Subtype"));
  out->push_back(',');
  AppendKey(out, "flags");
  out->append(std::to_string(annot->GetIntegerFor("F")));
  out->push_back(',');
  AppendKey(out, "obj");
  out->append(std::to_string(annot->GetObjNum()));
  out->push_back(',');
  AppendKey(out, "rectOk");
  out->append(Boolean(rect_ok));
  out->push_back(',');
  AppendKey(out, "quadsOk");
  out->append(Boolean(quads_ok));
  out->push_back('}');
}

void AppendLink(std::string* out,
                CPDF_Document* doc,
                size_t index,
                const CPDF_Dictionary* annot,
                const CPDF_Array* rect,
                const CPDF_Array* quads) {
  out->push_back('{');
  AppendKey(out, "index");
  out->append(std::to_string(index));
  out->push_back(',');
  AppendKey(out, "rect");
  CFX_FloatRect box;
  if (rect) {
    box = CFX_FloatRect(rect->GetFloatAt(0), rect->GetFloatAt(1),
                        rect->GetFloatAt(2), rect->GetFloatAt(3));
    box.Normalize();
  }
  out->push_back('[');
  AppendNumber(out, box.left);
  out->push_back(',');
  AppendNumber(out, box.bottom);
  out->push_back(',');
  AppendNumber(out, box.right);
  out->push_back(',');
  AppendNumber(out, box.top);
  out->append("],");
  AppendKey(out, "quads");
  if (quads) {
    out->push_back('[');
    for (size_t i = 0; i < quads->size(); ++i) {
      if (i > 0) {
        out->push_back(',');
      }
      AppendNumber(out, quads->GetFloatAt(i));
    }
    out->push_back(']');
  } else {
    out->append("null");
  }
  out->push_back(',');
  AppendKey(out, "dest");
  AppendDest(out, doc, EntryFor(annot, "Dest").Get());
  out->push_back(',');
  AppendKey(out, "action");
  RetainPtr<const CPDF_Object> a = EntryFor(annot, "A");
  AppendAction(out, doc, a ? a->AsDictionary() : nullptr);
  out->push_back(',');
  AppendKey(out, "aa");
  RetainPtr<const CPDF_Object> aa_object = EntryFor(annot, "AA");
  const CPDF_Dictionary* aa = aa_object ? aa_object->AsDictionary() : nullptr;
  RetainPtr<const CPDF_Dictionary> trigger = aa ? aa->GetDictFor("D") : nullptr;
  if (!trigger && aa) {
    trigger = aa->GetDictFor("U");
  }
  AppendAction(out, doc, trigger.Get());
  out->push_back('}');
}

struct OutlineWalk {
  CPDF_Document* doc;
  std::string* out;
  std::set<const CPDF_Dictionary*> seen;
  size_t items = 0;
  bool truncated = false;
  bool stopped = false;
};

void AppendOutlineItem(OutlineWalk* walk,
                       const CPDF_Dictionary* item,
                       int depth) {
  std::string* out = walk->out;
  if (walk->items > 0) {
    out->push_back(',');
  }
  out->push_back('{');
  AppendKey(out, "depth");
  out->append(std::to_string(depth));
  out->push_back(',');
  AppendKey(out, "title");
  AppendText(out, item->GetUnicodeTextFor("Title"));
  out->push_back(',');
  AppendKey(out, "flags");
  out->append(std::to_string(item->GetIntegerFor("F")));
  out->push_back(',');
  AppendKey(out, "count");
  out->append(std::to_string(item->GetIntegerFor("Count")));
  out->push_back(',');
  AppendKey(out, "dest");
  AppendDest(out, walk->doc, EntryFor(item, "Dest").Get());
  out->push_back(',');
  AppendKey(out, "action");
  RetainPtr<const CPDF_Object> a = EntryFor(item, "A");
  AppendAction(out, walk->doc, a ? a->AsDictionary() : nullptr);
  out->push_back('}');
  ++walk->items;
}

// Appends the children of |parent|, which sit at |depth|, and theirs.
void WalkOutline(OutlineWalk* walk, const CPDF_Dictionary* parent, int depth) {
  for (RetainPtr<const CPDF_Dictionary> item = parent->GetDictFor("First");
       item; item = item->GetDictFor("Next")) {
    if (walk->stopped) {
      return;
    }
    if (depth >= kMaxOutlineDepth || walk->items >= kMaxOutlineItems ||
        !walk->seen.insert(item.Get()).second) {
      walk->truncated = true;
      walk->stopped = walk->items >= kMaxOutlineItems;
      return;
    }
    AppendOutlineItem(walk, item.Get(), depth);
    WalkOutline(walk, item.Get(), depth + 1);
  }
}

char* Copy(const std::string& text) {
  char* json = static_cast<char*>(malloc(text.size() + 1));
  if (json) {
    memcpy(json, text.c_str(), text.size() + 1);
  }
  return json;
}

}  // namespace

extern "C" {

char* INK_Links_Page(FPDF_DOCUMENT doc, int page_index) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  if (!page) {
    return nullptr;
  }
  std::string annots_json = "{\"annots\":[";
  std::string links_json = "\"links\":[";
  bool first_annot = true;
  bool first_link = true;
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    if (!annot) {
      continue;
    }
    RetainPtr<const CPDF_Object> rect_object = EntryFor(annot.Get(), "Rect");
    const CPDF_Array* rect = rect_object ? rect_object->AsArray() : nullptr;
    const bool rect_ok = rect && rect->size() == 4 && AllFiniteNumbers(rect);
    RetainPtr<const CPDF_Object> quads_object =
        EntryFor(annot.Get(), "QuadPoints");
    const CPDF_Array* quads = quads_object ? quads_object->AsArray() : nullptr;
    const bool quads_well_formed = quads && !quads->IsEmpty() &&
                                   quads->size() % 8 == 0 &&
                                   AllFiniteNumbers(quads);
    const bool quads_ok = !quads_object || quads_well_formed;

    if (!first_annot) {
      annots_json.push_back(',');
    }
    first_annot = false;
    AppendAnnotEntry(&annots_json, i, annot.Get(), rect_ok, quads_ok);

    if (annot->GetNameFor("Subtype") != "Link") {
      continue;
    }
    if (!first_link) {
      links_json.push_back(',');
    }
    first_link = false;
    AppendLink(&links_json, pdf, i, annot.Get(), rect_ok ? rect : nullptr,
               quads_well_formed ? quads : nullptr);
  }
  annots_json.append("],");
  links_json.append("]}");
  return Copy(annots_json + links_json);
}

char* INK_Outline(FPDF_DOCUMENT doc) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf) {
    return nullptr;
  }
  std::string out = "{\"items\":[";
  OutlineWalk walk{pdf, &out};
  const CPDF_Dictionary* root = pdf->GetRoot();
  RetainPtr<const CPDF_Dictionary> outlines =
      root ? root->GetDictFor("Outlines") : nullptr;
  if (outlines) {
    walk.seen.insert(outlines.Get());
    WalkOutline(&walk, outlines.Get(), 0);
  }
  out.append("],\"truncated\":");
  out.append(Boolean(walk.truncated));
  out.push_back('}');
  return Copy(out);
}

char* INK_Dest_Named(FPDF_DOCUMENT doc, const unsigned char* name, int len) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || len < 0 || (!name && len > 0)) {
    return nullptr;
  }
  if (!pdf->GetRoot()) {
    return Copy("null");
  }
  ByteString key;
  if (len > 0) {
    key = ByteString(ByteStringView(UNSAFE_BUFFERS(
        pdfium::span<const uint8_t>(name, static_cast<size_t>(len)))));
  }
  RetainPtr<const CPDF_Array> dest = CPDF_NameTree::LookupNamedDest(pdf, key);
  std::string out;
  AppendDest(&out, pdf, dest.Get());
  return Copy(out);
}

void INK_Json_Free(char* json) {
  free(json);
}

}  // extern "C"
