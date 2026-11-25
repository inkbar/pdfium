// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See annots.h. The reports read dictionaries directly; the draw goes through
// EPDF_RenderAnnotBitmap, which draws without writing.

#include "inkbar/annots.h"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fxcrt/retain_ptr.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "inkbar/json.h"
#include "inkbar/report.h"
#include "public/epdf_view.h"
#include "public/fpdf_annot.h"

namespace {

using inkbar::json::AppendName;
using inkbar::json::AppendNumber;
using inkbar::report::AppendFileText;
using inkbar::report::AppendKey;
using inkbar::report::AppendNextKey;
using inkbar::report::AppendNumbers;
using inkbar::report::AppendRef;
using inkbar::report::Boolean;
using inkbar::report::EntryFor;
using inkbar::report::kMaxNumbers;
using inkbar::report::kMaxReportBytes;
using inkbar::report::kMaxTextBytes;

constexpr size_t kMaxAnnots = 200000;
constexpr size_t kMaxStructObjects = 1000000;

const CPDF_Array* ArrayFor(const CPDF_Dictionary* dict,
                           const char* key,
                           RetainPtr<const CPDF_Object>* hold) {
  *hold = EntryFor(dict, key);
  return *hold ? (*hold)->AsArray() : nullptr;
}

// ,"key":[numbers] when |key| is an array; |*budget| is what is left of
// kMaxNumbers for this entry.
void AppendNumbersFor(std::string* out,
                      const CPDF_Dictionary* dict,
                      const char* key,
                      const char* name,
                      size_t* budget,
                      bool* cut) {
  RetainPtr<const CPDF_Object> hold;
  const CPDF_Array* array = ArrayFor(dict, key, &hold);
  if (!array) {
    return;
  }
  AppendNextKey(out, name);
  AppendNumbers(out, array, *budget, cut);
  *budget -= std::min(*budget, array->size());
}

void AppendTextFor(std::string* out,
                   const CPDF_Dictionary* dict,
                   const char* key,
                   const char* name,
                   bool* cut) {
  RetainPtr<const CPDF_Object> value = EntryFor(dict, key);
  if (!value || !value->IsString()) {
    return;
  }
  AppendNextKey(out, name);
  AppendFileText(out, value->GetString(), kMaxTextBytes, cut);
}

void AppendNameFor(std::string* out,
                   const CPDF_Dictionary* dict,
                   const char* key,
                   const char* name) {
  RetainPtr<const CPDF_Object> value = EntryFor(dict, key);
  if (!value || !value->IsName()) {
    return;
  }
  AppendNextKey(out, name);
  AppendName(out, value->GetString());
}

void AppendAnnot(std::string* out, size_t index, const CPDF_Dictionary* annot) {
  bool cut = false;
  out->push_back('{');
  AppendKey(out, "index");
  out->append(std::to_string(index));
  AppendNextKey(out, "subtype");
  AppendName(out, annot->GetNameFor("Subtype"));
  if (annot->GetObjNum() != 0) {
    AppendNextKey(out, "objectNumber");
    out->append(std::to_string(annot->GetObjNum()));
    AppendNextKey(out, "generation");
    out->append(std::to_string(annot->GetGenNum()));
  }
  inkbar::report::AppendRect(out, annot);
  AppendNextKey(out, "flags");
  out->append(std::to_string(annot->GetIntegerFor("F")));
  inkbar::report::AppendAppearance(out, annot);

  // Each array has a budget of its own.
  for (const auto& [key, name] :
       {std::pair{"QuadPoints", "quads"}, std::pair{"C", "color"},
        std::pair{"IC", "interiorColor"}, std::pair{"L", "line"},
        std::pair{"Vertices", "vertices"}}) {
    size_t numbers = kMaxNumbers;
    AppendNumbersFor(out, annot, key, name, &numbers, &cut);
  }

  if (const CPDF_Number* opacity = ToNumber(EntryFor(annot, "CA").Get())) {
    AppendNextKey(out, "opacity");
    AppendNumber(out, opacity->GetNumber());
  }
  AppendTextFor(out, annot, "Contents", "contents", &cut);
  AppendTextFor(out, annot, "T", "author", &cut);
  AppendTextFor(out, annot, "CreationDate", "created", &cut);
  AppendTextFor(out, annot, "M", "modified", &cut);
  AppendNameFor(out, annot, "Name", "icon");
  AppendNameFor(out, annot, "IT", "intent");
  RetainPtr<const CPDF_Object> irt = annot->GetObjectFor("IRT");
  if (irt && irt->IsReference()) {
    AppendNextKey(out, "inReplyTo");
    AppendRef(out, irt.Get());
  }
  RetainPtr<const CPDF_Object> hold;
  if (const CPDF_Array* le = ArrayFor(annot, "LE", &hold)) {
    AppendNextKey(out, "lineEndings");
    inkbar::report::AppendNames(out, le);
  }
  if (const CPDF_Array* ink = ArrayFor(annot, "InkList", &hold)) {
    AppendNextKey(out, "inkLists");
    out->push_back('[');
    size_t left = kMaxNumbers;
    for (size_t i = 0; i < ink->size(); ++i) {
      if (left == 0) {
        cut = true;
        break;
      }
      if (i > 0) {
        out->push_back(',');
      }
      RetainPtr<const CPDF_Object> path = ink->GetDirectObjectAt(i);
      const CPDF_Array* points = path ? path->AsArray() : nullptr;
      if (!points) {
        out->append("null");
        continue;
      }
      AppendNumbers(out, points, left, &cut);
      left -= std::min(left, points->size());
    }
    out->push_back(']');
  }
  inkbar::report::AppendBorder(out, annot);
  AppendTextFor(out, annot, "DA", "da", &cut);
  if (const CPDF_Number* q = ToNumber(EntryFor(annot, "Q").Get())) {
    AppendNextKey(out, "q");
    AppendNumber(out, q->GetNumber());
  }
  if (EntryFor(annot, "PMD")) {
    AppendNextKey(out, "pmd");
    out->append("true");
  }
  RetainPtr<const CPDF_Object> parent = annot->GetObjectFor("Parent");
  if (parent && parent->IsReference()) {
    AppendNextKey(out, "parent");
    AppendRef(out, parent.Get());
  }
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
}

// The object numbers of the annotations in |page|'s /Annots.
std::unordered_set<uint32_t> AnnotNumbers(const CPDF_Dictionary* page) {
  std::unordered_set<uint32_t> numbers;
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Object> entry = annots->GetObjectAt(i);
    const CPDF_Reference* ref = entry ? entry->AsReference() : nullptr;
    if (ref) {
      numbers.insert(ref->GetRefObjNum());
    }
  }
  return numbers;
}

}  // namespace

extern "C" {

char* INK_Annots_Page(FPDF_DOCUMENT doc, int page_index) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  if (!page) {
    return nullptr;
  }
  std::string out = "{\"annots\":[";
  bool truncated = false;
  size_t listed = 0;
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    // Only a dictionary is an annotation. GetDictAt() would also give a
    // stream's dictionary.
    RetainPtr<const CPDF_Object> entry = annots->GetDirectObjectAt(i);
    const CPDF_Dictionary* annot = entry ? entry->AsDictionary() : nullptr;
    if (!annot) {
      continue;
    }
    if (listed == kMaxAnnots) {
      truncated = true;
      break;
    }
    const size_t mark = out.size();
    if (listed > 0) {
      out.push_back(',');
    }
    AppendAnnot(&out, i, annot);
    if (out.size() > kMaxReportBytes - 64) {
      out.resize(mark);
      truncated = true;
      break;
    }
    ++listed;
  }
  out.append("],\"tabs\":");
  RetainPtr<const CPDF_Object> tabs = EntryFor(page.Get(), "Tabs");
  if (tabs && tabs->IsName()) {
    AppendName(&out, tabs->GetString());
  } else {
    out.append("null");
  }
  out.append(",\"truncated\":");
  out.append(Boolean(truncated));
  out.push_back('}');
  return inkbar::report::Copy(out);
}

char* INK_Struct_Order(FPDF_DOCUMENT doc, int page_index) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return nullptr;
  }
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  if (!page) {
    return nullptr;
  }
  const std::unordered_set<uint32_t> on_page = AnnotNumbers(page.Get());
  std::string out = "{\"order\":[";
  bool truncated = false;
  std::unordered_set<uint32_t> listed;
  std::set<const CPDF_Object*> seen;
  size_t read = 0;
  const CPDF_Dictionary* root = pdf->GetRoot();
  RetainPtr<const CPDF_Dictionary> tree =
      root ? root->GetDictFor("StructTreeRoot") : nullptr;
  // Depth first, /K in order: kids are pushed last first.
  std::vector<std::pair<RetainPtr<const CPDF_Object>, int>> stack;
  if (tree) {
    RetainPtr<const CPDF_Object> k = tree->GetObjectFor("K");
    if (k) {
      stack.emplace_back(std::move(k), 0);
    }
  }
  while (!stack.empty() && !truncated) {
    RetainPtr<const CPDF_Object> object = stack.back().first->GetDirect();
    const int depth = stack.back().second;
    stack.pop_back();
    if (!object || !(object->IsArray() || object->IsDictionary()) ||
        !seen.insert(object.Get()).second) {
      continue;
    }
    if (++read > kMaxStructObjects) {
      truncated = true;
      break;
    }
    if (const CPDF_Array* kids = object->AsArray()) {
      for (size_t i = kids->size(); i > 0; --i) {
        RetainPtr<const CPDF_Object> kid = kids->GetObjectAt(i - 1);
        if (kid) {
          stack.emplace_back(std::move(kid), depth);
        }
      }
      continue;
    }
    const CPDF_Dictionary* dict = object->AsDictionary();
    if (dict->GetNameFor("Type") == "OBJR") {
      RetainPtr<const CPDF_Object> obj = dict->GetObjectFor("Obj");
      const CPDF_Reference* ref = obj ? obj->AsReference() : nullptr;
      const uint32_t number = ref ? ref->GetRefObjNum() : 0;
      if (number != 0 && on_page.count(number) &&
          listed.insert(number).second) {
        if (listed.size() > 1) {
          out.push_back(',');
        }
        out.append(std::to_string(number));
      }
      continue;
    }
    RetainPtr<const CPDF_Object> k = dict->GetObjectFor("K");
    if (!k) {
      continue;
    }
    if (depth + 1 > inkbar::report::kMaxDepth) {
      truncated = true;
      break;
    }
    stack.emplace_back(std::move(k), depth + 1);
  }
  out.append("],\"truncated\":");
  out.append(Boolean(truncated));
  out.push_back('}');
  return inkbar::report::Copy(out);
}

int INK_Annot_Draw(FPDF_BITMAP bitmap,
                   FPDF_PAGE page,
                   int annot_index,
                   const char* state,
                   const float* matrix) {
  if (!bitmap || !page || annot_index < 0) {
    return 0;
  }
  {
    ScopedFPDFPageView view(page);
    if (!view || view.Get()->GetRenderContext()) {
      return 0;
    }
  }
  // FPDFPage_GetAnnot takes only a dictionary, as the reports do.
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annot_index);
  if (!annot) {
    return 0;
  }
  FS_MATRIX m;
  if (matrix) {
    m.a = matrix[0];
    m.b = matrix[1];
    m.c = matrix[2];
    m.d = matrix[3];
    m.e = matrix[4];
    m.f = matrix[5];
  }
  const FPDF_BOOL drew = EPDF_RenderAnnotBitmap(
      bitmap, page, annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, state,
      matrix ? &m : nullptr, 0);
  FPDFPage_CloseAnnot(annot);
  return drew ? 1 : 0;
}

}  // extern "C"
