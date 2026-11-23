// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See links.h. Reads dictionaries directly, with CPDF_Dest for a destination's
// page and fit, CPDF_Action for a JavaScript action's source and
// CPDF_NameTree for named destinations.

#include "inkbar/links.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
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
#include "core/fpdfapi/parser/cpdf_reference.h"
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
#include "inkbar/json.h"

namespace {

constexpr int kMaxOutlineDepth = 64;
constexpr size_t kMaxOutlineItems = 100000;
// The most entries after an explicit destination's fit whose kinds are given.
constexpr size_t kMaxArgKinds = 8;
// The most bytes a JavaScript action's source writes inside its JSON string,
// escapes included, and the most all the sources of one page's links write.
constexpr size_t kMaxScriptBytes = 4096;
constexpr size_t kMaxPageScriptBytes = 1048576;

// Indexed by the PDFDEST_VIEW_* values CPDF_Dest::GetZoomMode() returns.
constexpr std::array<const char*, 9> kFitNames = {
    "Unknown", "XYZ", "Fit", "FitH", "FitV", "FitR", "FitB", "FitBH", "FitBV"};

using inkbar::json::AppendHex;
using inkbar::json::AppendName;
using inkbar::json::AppendNumber;
using inkbar::json::AppendText;

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

// [number, generation] of the object an explicit destination's first entry
// refers to, or null when that entry is not a reference. PDFium keeps no
// generation in a reference, so it is that of the object the file holds under
// the number, 0 when there is none.
void AppendPageRef(std::string* out, const CPDF_Array* dest) {
  RetainPtr<const CPDF_Object> first = dest->GetObjectAt(0);
  const CPDF_Reference* ref = first ? first->AsReference() : nullptr;
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

// What an explicit destination's array holds, entry by entry, as the file
// wrote it, so a caller can check it as PDF.js does: "argCount", "argKinds"
// for at most the first kMaxArgKinds arguments (each resolved), whether any of
// those is written as a reference ("argIndirect"), "fitKind" and "pageKind".
// Reads at most kMaxArgKinds + 2 entries, however long the array.
void AppendShape(std::string* out, const CPDF_Array* array) {
  const size_t size = array->size();
  out->push_back(',');
  AppendKey(out, "argCount");
  out->append(std::to_string(size > 2 ? size - 2 : 0));
  out->push_back(',');
  AppendKey(out, "argKinds");
  out->push_back('[');
  bool indirect = false;
  for (size_t i = 2; i < size && i < 2 + kMaxArgKinds; ++i) {
    if (i > 2) {
      out->push_back(',');
    }
    RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i);
    indirect = indirect || (entry && entry->IsReference());
    // A reference to no object is the null object.
    RetainPtr<const CPDF_Object> value = entry ? entry->GetDirect() : nullptr;
    out->append(!value || value->IsNull() ? "\"null\""
                : value->IsNumber()        ? "\"number\""
                                           : "\"other\"");
  }
  out->append("],");
  AppendKey(out, "argIndirect");
  out->append(Boolean(indirect));
  out->push_back(',');
  AppendKey(out, "fitKind");
  RetainPtr<const CPDF_Object> fit =
      size >= 2 ? array->GetObjectAt(1) : nullptr;
  out->append(size < 2          ? "\"missing\""
              : !fit             ? "\"other\""
              : fit->IsName()    ? "\"name\""
              : fit->IsString()  ? "\"string\""
                                 : "\"other\"");
  out->push_back(',');
  AppendKey(out, "pageKind");
  const char* page_kind = "\"missing\"";
  if (size > 0) {
    RetainPtr<const CPDF_Object> page = array->GetObjectAt(0);
    page_kind = "\"other\"";
    if (page && page->IsReference()) {
      RetainPtr<const CPDF_Object> target = page->GetDirect();
      if (target && target->IsDictionary()) {
        page_kind = "\"ref\"";
      }
    } else if (const CPDF_Number* number = ToNumber(page.Get())) {
      const float value = number->GetNumber();
      if (number->IsInteger() ||
          (std::isfinite(value) && std::floor(value) == value)) {
        page_kind = "\"integer\"";
      }
    }
  }
  out->append(page_kind);
}

// A local destination names a page of this file. A remote go-to's or a
// launch's /D names a page of another file, and its number is kept as it is.
enum class Where { kLocal, kRemote };

// For a local destination, a page number that is not a page of |doc| is -1.
void AppendDest(std::string* out,
                CPDF_Document* doc,
                const CPDF_Object* dest,
                Where where) {
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
  int page = explicit_dest.GetDestPageIndex(doc);
  if (where == Where::kLocal && (page < 0 || page >= doc->GetPageCount())) {
    page = -1;
  }
  out->append(std::to_string(page));
  out->push_back(',');
  AppendKey(out, "pageRef");
  AppendPageRef(out, array);
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
  out->push_back(']');
  AppendShape(out, array);
  out->push_back('}');
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

// The bytes inkbar::json writes for |c| inside a string: 2 for a quote or a
// backslash, 6 for a control character or DEL (\u00XX), else its UTF-8. A
// surrogate and a value past U+10FFFF become U+FFFD, which takes 3.
size_t WrittenSize(uint32_t c) {
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

// |script| cut after the last character whose written form ends within
// |limit| bytes. Adds the bytes it keeps to |*written| and sets |*cut| when it
// was longer.
WideString CutScript(const WideString& script,
                     size_t limit,
                     size_t* written,
                     bool* cut) {
  size_t bytes = 0;
  size_t length = 0;
  *cut = false;
  for (wchar_t c : script) {
    const size_t size = WrittenSize(static_cast<uint32_t>(c));
    if (bytes + size > limit) {
      *cut = true;
      break;
    }
    bytes += size;
    ++length;
  }
  *written += bytes;
  return *cut ? script.First(length) : script;
}

// |page_scripts|: the bytes the scripts of this page's links have written so
// far, or null outside a page.
void AppendAction(std::string* out,
                  CPDF_Document* doc,
                  const CPDF_Dictionary* action,
                  size_t* page_scripts) {
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
    const bool readable = uri && (uri->IsString() || uri->IsName());
    AppendHex(out, readable ? uri->GetString() : ByteString());
    out->push_back(',');
    AppendKey(out, "uriKind");
    out->append(!readable          ? "null"
                : uri->IsString() ? "\"string\""
                                  : "\"name\"");
  } else if (s == "GoTo") {
    out->push_back(',');
    AppendKey(out, "dest");
    AppendDest(out, doc, EntryFor(action, "D").Get(), Where::kLocal);
  } else if (s == "GoToR" || s == "Launch") {
    out->push_back(',');
    AppendKey(out, "fileHex");
    AppendFile(out, action);
    out->push_back(',');
    AppendKey(out, "fileKind");
    RetainPtr<const CPDF_Object> file = EntryFor(action, "F");
    out->append(!file                 ? "null"
                : file->IsString()     ? "\"string\""
                : file->IsDictionary() ? "\"spec\""
                                       : "null");
    out->push_back(',');
    AppendKey(out, "dest");
    AppendDest(out, doc, EntryFor(action, "D").Get(), Where::kRemote);
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
    size_t outside_page = 0;
    size_t* written = page_scripts ? page_scripts : &outside_page;
    const size_t limit =
        page_scripts
            ? std::min(kMaxScriptBytes, kMaxPageScriptBytes - *page_scripts)
            : kMaxScriptBytes;
    bool cut = false;
    AppendText(out, CutScript(js.GetJavaScript(), limit, written, &cut));
    if (cut) {
      out->push_back(',');
      AppendKey(out, "scriptTruncated");
      out->append("true");
    }
  }
  out->push_back('}');
}

void AppendAnnotEntry(std::string* out,
                      size_t index,
                      const CPDF_Dictionary* annot,
                      bool rect_ok,
                      bool has_quads,
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
  AppendKey(out, "generation");
  out->append(std::to_string(annot->GetGenNum()));
  out->push_back(',');
  AppendKey(out, "rectOk");
  out->append(Boolean(rect_ok));
  out->push_back(',');
  AppendKey(out, "hasQuads");
  out->append(Boolean(has_quads));
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
                const CPDF_Array* quads,
                size_t* page_scripts) {
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
  RetainPtr<const CPDF_Object> dest = EntryFor(annot, "Dest");
  AppendDest(out, doc, dest.Get(), Where::kLocal);
  // /Dest is there, null included, but is no destination: not a string, a
  // name or an array.
  if (annot->KeyExist("Dest") &&
      !(dest && (dest->IsString() || dest->IsName() || dest->IsArray()))) {
    out->push_back(',');
    AppendKey(out, "destInvalid");
    out->append("true");
  }
  out->push_back(',');
  AppendKey(out, "action");
  RetainPtr<const CPDF_Object> a = EntryFor(annot, "A");
  AppendAction(out, doc, a ? a->AsDictionary() : nullptr, page_scripts);
  out->push_back(',');
  AppendKey(out, "aa");
  RetainPtr<const CPDF_Object> aa_object = EntryFor(annot, "AA");
  const CPDF_Dictionary* aa = aa_object ? aa_object->AsDictionary() : nullptr;
  RetainPtr<const CPDF_Dictionary> trigger = aa ? aa->GetDictFor("D") : nullptr;
  if (!trigger && aa) {
    trigger = aa->GetDictFor("U");
  }
  AppendAction(out, doc, trigger.Get(), page_scripts);
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
  AppendDest(out, walk->doc, EntryFor(item, "Dest").Get(), Where::kLocal);
  out->push_back(',');
  AppendKey(out, "action");
  RetainPtr<const CPDF_Object> a = EntryFor(item, "A");
  AppendAction(out, walk->doc, a ? a->AsDictionary() : nullptr, nullptr);
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
  size_t page_scripts = 0;
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    // Only a dictionary is an annotation. GetDictAt() would also give a
    // stream's dictionary.
    RetainPtr<const CPDF_Object> entry = annots->GetDirectObjectAt(i);
    RetainPtr<const CPDF_Dictionary> annot =
        entry ? pdfium::WrapRetain(entry->AsDictionary()) : nullptr;
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
    AppendAnnotEntry(&annots_json, i, annot.Get(), rect_ok, !!quads_object,
                     quads_ok);

    if (annot->GetNameFor("Subtype") != "Link") {
      continue;
    }
    if (!first_link) {
      links_json.push_back(',');
    }
    first_link = false;
    AppendLink(&links_json, pdf, i, annot.Get(), rect_ok ? rect : nullptr,
               quads_well_formed ? quads : nullptr, &page_scripts);
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
  if (!pdf) {
    return nullptr;
  }
  if (!pdf->GetRoot() || !name || len <= 0) {
    return Copy("null");
  }
  const ByteString key(ByteStringView(UNSAFE_BUFFERS(
      pdfium::span<const uint8_t>(name, static_cast<size_t>(len)))));
  RetainPtr<const CPDF_Array> dest = CPDF_NameTree::LookupNamedDest(pdf, key);
  std::string out;
  AppendDest(&out, pdf, dest.Get(), Where::kLocal);
  return Copy(out);
}

void INK_Json_Free(char* json) {
  free(json);
}

}  // extern "C"
