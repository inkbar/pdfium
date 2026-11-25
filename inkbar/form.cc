// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See form.h. The form is read through the same reconciled form
// EPDFForm_LoadModel builds (epdf::BuildReconciledForm's phases), so the
// fields, their widgets and their origin are the model's; the rest is read
// from the dictionaries. Nothing is written.

#include "inkbar/form.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_object.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpdf_interactiveform.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/epdf_form_helpers.h"
#include "inkbar/json.h"
#include "inkbar/report.h"

namespace {

using inkbar::json::AppendHex;
using inkbar::json::AppendName;
using inkbar::json::AppendNumber;
using inkbar::json::AppendText;
using inkbar::report::AppendFileText;
using inkbar::report::AppendKey;
using inkbar::report::AppendNextKey;
using inkbar::report::AppendNumbers;
using inkbar::report::AppendRef;
using inkbar::report::AppendScripts;
using inkbar::report::Boolean;
using inkbar::report::EntryFor;
using inkbar::report::kMaxDepth;
using inkbar::report::kMaxNumbers;
using inkbar::report::kMaxReportBytes;
using inkbar::report::kMaxTextBytes;

constexpr size_t kMaxFields = 50000;
constexpr size_t kMaxNames = 50000;
// Room left for the keys that close a report.
constexpr size_t kClosing = 256;

// Where a dictionary sits: page index and place in that page's /Annots.
using Places = std::map<const CPDF_Dictionary*, std::pair<int, int>>;

// Every dictionary in every page's /Annots, resolved as
// epdf::SweepPageWidgets resolves them, so a control's widget dictionary is
// found by the dictionary itself; its first place wins. Counts the widgets in
// |*widgets|, each place once.
Places SweepPlaces(CPDF_Document* doc, size_t* widgets) {
  Places places;
  const int page_count = doc->GetPageCount();
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = doc->GetPageDictionary(i);
    RetainPtr<const CPDF_Array> annots =
        page ? page->GetArrayFor("Annots") : nullptr;
    for (size_t j = 0; annots && j < annots->size(); ++j) {
      RetainPtr<const CPDF_Object> element = annots->GetObjectAt(j);
      RetainPtr<const CPDF_Dictionary> annot;
      if (const CPDF_Reference* ref =
              element ? element->AsReference() : nullptr) {
        annot =
            ToDictionary(doc->GetOrParseIndirectObject(ref->GetRefObjNum()));
      } else {
        annot = ToDictionary(std::move(element));
      }
      if (annot) {
        places.try_emplace(annot.Get(), i, static_cast<int>(j));
        if (annot->GetNameFor("Subtype") == "Widget") {
          ++*widgets;
        }
      }
    }
  }
  return places;
}

const char* KindOf(CPDF_FormField::Type type) {
  switch (type) {
    case CPDF_FormField::kPushButton:
      return "button";
    case CPDF_FormField::kRadioButton:
      return "radio";
    case CPDF_FormField::kCheckBox:
      return "checkbox";
    case CPDF_FormField::kText:
    case CPDF_FormField::kRichText:
    case CPDF_FormField::kFile:
      return "text";
    case CPDF_FormField::kListBox:
      return "list";
    case CPDF_FormField::kComboBox:
      return "combo";
    case CPDF_FormField::kSign:
      return "signature";
    case CPDF_FormField::kUnknown:
      return "unknown";
  }
  return "unknown";
}

void AppendObjectNumber(std::string* out, const CPDF_Object* object) {
  if (object->GetObjNum() != 0) {
    AppendNextKey(out, "objectNumber");
    out->append(std::to_string(object->GetObjNum()));
    AppendNextKey(out, "generation");
    out->append(std::to_string(object->GetGenNum()));
  }
}

void AppendTextFor(std::string* out,
                   const CPDF_Object* value,
                   const char* name,
                   bool* cut) {
  if (!value || !value->IsString()) {
    return;
  }
  AppendNextKey(out, name);
  AppendFileText(out, value->GetString(), kMaxTextBytes, cut);
}

void AppendWide(std::string* out, const WideString& text, bool* cut) {
  size_t written = 0;
  bool text_cut = false;
  AppendText(out, inkbar::json::CutText(text, kMaxTextBytes, &written,
                                        &text_cut));
  *cut = *cut || text_cut;
}

// ,"<kind_key>":"..." and, for a string, a name or an array, ,"<key>":value.
void AppendValue(std::string* out,
                 const CPDF_Object* value,
                 const char* kind_key,
                 const char* key,
                 bool* cut) {
  AppendNextKey(out, kind_key);
  if (!value || value->IsNull()) {
    out->append("\"none\"");
    return;
  }
  if (value->IsString()) {
    out->append("\"string\"");
    AppendNextKey(out, key);
    AppendFileText(out, value->GetString(), kMaxTextBytes, cut);
  } else if (value->IsName()) {
    out->append("\"name\"");
    AppendNextKey(out, key);
    AppendName(out, value->GetString());
  } else if (const CPDF_Array* array = value->AsArray()) {
    out->append("\"array\"");
    AppendNextKey(out, key);
    out->push_back('[');
    for (size_t i = 0; i < array->size(); ++i) {
      if (i > 0) {
        out->push_back(',');
      }
      RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
      if (entry && entry->IsString()) {
        AppendFileText(out, entry->GetString(), kMaxTextBytes, cut);
      } else {
        out->append("null");
      }
    }
    out->push_back(']');
  } else {
    out->append("\"other\"");
  }
}

// /AA's JavaScript by key along /Parent from |widget|, the nearest first, and
// "A" for the widget's own /A.
void AppendWidgetScripts(std::string* out,
                         const CPDF_Dictionary* widget,
                         bool* cut) {
  std::map<ByteString, std::string> scripts;
  std::set<const CPDF_Dictionary*> seen;
  RetainPtr<const CPDF_Dictionary> level = pdfium::WrapRetain(widget);
  for (int depth = 0; level && depth <= kMaxDepth; ++depth) {
    if (!seen.insert(level.Get()).second) {
      break;
    }
    RetainPtr<const CPDF_Object> aa_object = EntryFor(level.Get(), "AA");
    if (const CPDF_Dictionary* aa =
            aa_object ? aa_object->AsDictionary() : nullptr) {
      CPDF_DictionaryLocker locker(aa);
      for (const auto& item : locker) {
        if (item.first == "A" || scripts.count(item.first)) {
          continue;
        }
        std::string sources;
        if (AppendScripts(&sources, item.second.Get(), cut) > 0) {
          scripts.emplace(item.first, std::move(sources));
        }
      }
    }
    level = level->GetDictFor("Parent");
  }
  std::string own;
  if (AppendScripts(&own, EntryFor(widget, "A").Get(), cut) > 0) {
    scripts.emplace("A", std::move(own));
  }
  AppendNextKey(out, "scripts");
  out->push_back('{');
  bool first = true;
  for (const auto& [key, sources] : scripts) {
    if (!first) {
      out->push_back(',');
    }
    first = false;
    AppendName(out, key);
    out->push_back(':');
    out->append(sources);
  }
  out->push_back('}');
}

// The widget's own /A, its first action only.
void AppendAction(std::string* out, const CPDF_Dictionary* widget, bool* cut) {
  RetainPtr<const CPDF_Object> a = EntryFor(widget, "A");
  const CPDF_Dictionary* action = a ? a->AsDictionary() : nullptr;
  if (!action) {
    return;
  }
  const ByteString s = action->GetNameFor("S");
  AppendNextKey(out, "action");
  out->push_back('{');
  AppendKey(out, "s");
  AppendName(out, s);
  if (s == "URI") {
    RetainPtr<const CPDF_Object> uri = EntryFor(action, "URI");
    AppendNextKey(out, "uriHex");
    AppendHex(out, uri && (uri->IsString() || uri->IsName())
                       ? uri->GetString().First(std::min<size_t>(
                             uri->GetString().GetLength(), kMaxTextBytes / 2))
                       : ByteString());
  } else if (s == "Named") {
    RetainPtr<const CPDF_Object> name = EntryFor(action, "N");
    AppendNextKey(out, "name");
    AppendName(out, name && name->IsName() ? name->GetString() : ByteString());
  } else if (s == "ResetForm") {
    RetainPtr<const CPDF_Object> fields_object = EntryFor(action, "Fields");
    const CPDF_Array* fields =
        fields_object ? fields_object->AsArray() : nullptr;
    AppendNextKey(out, "fields");
    out->push_back('[');
    bool first = true;
    for (size_t i = 0; fields && i < fields->size(); ++i) {
      RetainPtr<const CPDF_Object> entry = fields->GetDirectObjectAt(i);
      if (entry && entry->IsString()) {
        if (!first) {
          out->push_back(',');
        }
        first = false;
        AppendFileText(out, entry->GetString(), kMaxTextBytes, cut);
      }
    }
    out->append("]");
    AppendNextKey(out, "refs");
    out->push_back('[');
    first = true;
    for (size_t i = 0; fields && i < fields->size(); ++i) {
      RetainPtr<const CPDF_Object> entry = fields->GetObjectAt(i);
      if (entry && entry->IsReference()) {
        if (!first) {
          out->push_back(',');
        }
        first = false;
        AppendRef(out, entry.Get());
      }
    }
    out->push_back(']');
    AppendNextKey(out, "flags");
    out->append(std::to_string(action->GetIntegerFor("Flags")));
  }
  out->push_back('}');
}

void AppendWidget(std::string* out,
                  const CPDF_FormControl* control,
                  bool toggle,
                  const Places& places) {
  const CPDF_Dictionary* widget = control->GetWidgetDict().Get();
  bool cut = false;
  out->push_back('{');
  AppendKey(out, "page");
  const auto place = places.find(widget);
  out->append(std::to_string(place != places.end() ? place->second.first
                                                   : -1));
  AppendNextKey(out, "annotIndex");
  out->append(std::to_string(place != places.end() ? place->second.second
                                                   : -1));
  AppendObjectNumber(out, widget);
  inkbar::report::AppendRect(out, widget);
  AppendNextKey(out, "flags");
  out->append(std::to_string(widget->GetIntegerFor("F")));
  inkbar::report::AppendAppearance(out, widget);
  if (toggle) {
    AppendNextKey(out, "onState");
    AppendName(out, control->GetOnStateName());
    AppendNextKey(out, "exportValue");
    AppendWide(out, control->GetExportValue(), &cut);
  }
  inkbar::report::AppendBorder(out, widget);
  RetainPtr<const CPDF_Object> mk_object = EntryFor(widget, "MK");
  if (const CPDF_Dictionary* mk =
          mk_object ? mk_object->AsDictionary() : nullptr) {
    for (const auto& [key, name] :
         {std::pair{"BC", "borderColor"}, std::pair{"BG", "backgroundColor"}}) {
      RetainPtr<const CPDF_Object> colour = EntryFor(mk, key);
      if (const CPDF_Array* array = colour ? colour->AsArray() : nullptr) {
        AppendNextKey(out, name);
        AppendNumbers(out, array, kMaxNumbers, &cut);
      }
    }
    if (const CPDF_Number* r = ToNumber(EntryFor(mk, "R").Get())) {
      AppendNextKey(out, "rotation");
      AppendNumber(out, r->GetNumber());
    }
    AppendTextFor(out, EntryFor(mk, "CA").Get(), "caption", &cut);
  }
  if (const CPDF_Number* q = ToNumber(
          CPDF_FormField::GetFieldAttrForDict(widget, "Q").Get())) {
    AppendNextKey(out, "q");
    AppendNumber(out, q->GetNumber());
  }
  AppendTextFor(out, EntryFor(widget, "TU").Get(), "tooltip", &cut);
  AppendTextFor(out, CPDF_FormField::GetFieldAttrForDict(widget, "DA").Get(),
                "da", &cut);
  if (EntryFor(widget, "PMD")) {
    AppendNextKey(out, "pmd");
    out->append("true");
  }
  AppendWidgetScripts(out, widget, &cut);
  AppendAction(out, widget, &cut);
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
}

void AppendField(std::string* out,
                 CPDF_FormField* field,
                 bool recovered,
                 const Places& places) {
  const CPDF_Dictionary* dict = field->GetFieldDict().Get();
  bool cut = false;
  out->push_back('{');
  AppendKey(out, "name");
  AppendWide(out, field->GetFullName(), &cut);
  AppendObjectNumber(out, dict);
  AppendNextKey(out, "origin");
  out->append(recovered ? "\"recovered\"" : "\"acroform\"");
  const CPDF_FormField::Type type = field->GetType();
  AppendNextKey(out, "kind");
  out->push_back('"');
  out->append(KindOf(type));
  out->push_back('"');
  AppendNextKey(out, "flags");
  out->append(std::to_string(field->GetFieldFlags()));
  AppendValue(out, CPDF_FormField::GetFieldAttrForDict(dict, "V").Get(),
              "valueKind", "value", &cut);
  AppendValue(out, CPDF_FormField::GetFieldAttrForDict(dict, "DV").Get(),
              "defaultValueKind", "defaultValue", &cut);
  if (type == CPDF_FormField::kComboBox || type == CPDF_FormField::kListBox) {
    AppendNextKey(out, "options");
    out->push_back('[');
    const int count = field->CountOptions();
    for (int i = 0; i < count; ++i) {
      if (i > 0) {
        out->push_back(',');
      }
      out->push_back('{');
      AppendKey(out, "exportValue");
      AppendWide(out, field->GetOptionValue(i), &cut);
      AppendNextKey(out, "displayValue");
      AppendWide(out, field->GetOptionLabel(i), &cut);
      out->push_back('}');
    }
    out->push_back(']');
  }
  if (type == CPDF_FormField::kText || type == CPDF_FormField::kRichText ||
      type == CPDF_FormField::kFile) {
    const int max_len = field->GetMaxLen();
    if (max_len != 0) {
      AppendNextKey(out, "maxLength");
      out->append(std::to_string(max_len));
    }
  }
  if (type == CPDF_FormField::kListBox) {
    if (const CPDF_Number* ti = ToNumber(
            CPDF_FormField::GetFieldAttrForDict(dict, "TI").Get())) {
      AppendNextKey(out, "topIndex");
      out->append(std::to_string(ti->GetInteger()));
    }
  }
  AppendTextFor(out, EntryFor(dict, "TU").Get(), "tooltip", &cut);
  const bool toggle = type == CPDF_FormField::kCheckBox ||
                      type == CPDF_FormField::kRadioButton;
  AppendNextKey(out, "widgets");
  out->push_back('[');
  const int controls = field->CountControls();
  bool first = true;
  for (int i = 0; i < controls; ++i) {
    const CPDF_FormControl* control = field->GetControl(i);
    if (!control) {
      continue;
    }
    if (!first) {
      out->push_back(',');
    }
    first = false;
    AppendWidget(out, control, toggle, places);
  }
  out->push_back(']');
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
}

// The deepest level of /Fields and its /Kids, how many dictionaries they hold
// and whether a /Kids entry names a dictionary already reached. Walks 1,000
// levels deep; sets |*truncated| when there was more.
void WalkFieldTree(const CPDF_Array* fields,
                   int* deepest,
                   size_t* nodes,
                   bool* loops,
                   bool* truncated) {
  std::set<const CPDF_Dictionary*> seen;
  std::vector<std::pair<RetainPtr<const CPDF_Dictionary>, int>> stack;
  for (size_t i = 0; fields && i < fields->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> field = fields->GetDictAt(i);
    if (field) {
      stack.emplace_back(std::move(field), 1);
    }
  }
  while (!stack.empty()) {
    RetainPtr<const CPDF_Dictionary> field = std::move(stack.back().first);
    const int depth = stack.back().second;
    stack.pop_back();
    if (!seen.insert(field.Get()).second) {
      *loops = true;
      continue;
    }
    *deepest = std::max(*deepest, depth);
    ++*nodes;
    RetainPtr<const CPDF_Array> kids = field->GetArrayFor("Kids");
    for (size_t i = 0; kids && i < kids->size(); ++i) {
      RetainPtr<const CPDF_Dictionary> kid = kids->GetDictAt(i);
      if (!kid) {
        continue;
      }
      if (depth >= kMaxDepth) {
        *truncated = true;
        break;
      }
      stack.emplace_back(std::move(kid), depth + 1);
    }
  }
}

// ,"<key>":{...} with the JavaScript under each key of |dict| that holds
// some; returns whether any did.
bool AppendScriptsByKey(std::string* out,
                        const CPDF_Dictionary* dict,
                        bool* cut) {
  std::string scripts;
  bool any = false;
  CPDF_DictionaryLocker locker(dict);
  for (const auto& item : locker) {
    std::string sources;
    if (AppendScripts(&sources, item.second.Get(), cut) == 0) {
      continue;
    }
    if (any) {
      scripts.push_back(',');
    }
    any = true;
    AppendName(&scripts, item.first);
    scripts.push_back(':');
    scripts.append(sources);
  }
  out->append(scripts);
  return any;
}

// The /Names /JavaScript tree, each node once, in the tree's order.
void AppendDocumentScripts(std::string* out,
                           const CPDF_Dictionary* tree,
                           bool* truncated) {
  std::set<const CPDF_Dictionary*> seen;
  std::vector<std::pair<RetainPtr<const CPDF_Dictionary>, int>> stack;
  if (tree) {
    stack.emplace_back(pdfium::WrapRetain(tree), 0);
  }
  size_t names = 0;
  bool first = true;
  while (!stack.empty()) {
    RetainPtr<const CPDF_Dictionary> node = std::move(stack.back().first);
    const int depth = stack.back().second;
    stack.pop_back();
    if (!seen.insert(node.Get()).second) {
      continue;
    }
    RetainPtr<const CPDF_Array> pairs = node->GetArrayFor("Names");
    for (size_t i = 0; pairs && i + 1 < pairs->size(); i += 2) {
      if (names++ == kMaxNames) {
        *truncated = true;
        return;
      }
      RetainPtr<const CPDF_Object> name = pairs->GetDirectObjectAt(i);
      bool cut = false;
      const std::vector<WideString> sources =
          inkbar::report::CollectScripts(pairs->GetObjectAt(i + 1).Get(), &cut);
      std::string entry_name;
      if (name && name->IsString()) {
        AppendFileText(&entry_name, name->GetString(), kMaxTextBytes, &cut);
      } else {
        entry_name = "null";
      }
      *truncated = *truncated || cut;
      for (const WideString& source : sources) {
        const size_t mark = out->size();
        if (!first) {
          out->push_back(',');
        }
        out->append("{\"name\":");
        out->append(entry_name);
        out->append(",\"source\":");
        AppendText(out, source);
        out->push_back('}');
        // The document's scripts take at most half the report.
        if (out->size() > kMaxReportBytes / 2) {
          out->resize(mark);
          *truncated = true;
          return;
        }
        first = false;
      }
    }
    RetainPtr<const CPDF_Array> kids = node->GetArrayFor("Kids");
    if (kids && depth + 1 > kMaxDepth) {
      *truncated = true;
      continue;
    }
    for (size_t i = kids ? kids->size() : 0; i > 0; --i) {
      RetainPtr<const CPDF_Dictionary> kid = kids->GetDictAt(i - 1);
      if (kid) {
        stack.emplace_back(std::move(kid), depth + 1);
      }
    }
  }
}

}  // namespace

extern "C" {

char* INK_Form(FPDF_DOCUMENT doc) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf) {
    return nullptr;
  }
  const CPDF_Dictionary* root = pdf->GetRoot();
  RetainPtr<const CPDF_Dictionary> acro_form =
      root ? root->GetDictFor("AcroForm") : nullptr;
  std::string out = "{\"kind\":";
  out.append(!acro_form                 ? "\"none\""
             : acro_form->KeyExist("XFA") ? "\"xfa\""
                                          : "\"acroform\"");
  AppendNextKey(&out, "needAppearances");
  out.append(Boolean(acro_form &&
                     acro_form->GetBooleanFor("NeedAppearances", false)));
  bool truncated = false;
  AppendTextFor(&out, EntryFor(acro_form.Get(), "DA").Get(), "da", &truncated);
  if (const CPDF_Number* q = ToNumber(EntryFor(acro_form.Get(), "Q").Get())) {
    AppendNextKey(&out, "q");
    AppendNumber(&out, q->GetNumber());
  }

  int deepest = 0;
  size_t nodes = 0;
  bool loops = false;
  WalkFieldTree(acro_form ? acro_form->GetArrayFor("Fields").Get() : nullptr,
                &deepest, &nodes, &loops, &truncated);
  size_t widgets = 0;
  const Places places = SweepPlaces(pdf, &widgets);

  AppendNextKey(&out, "fields");
  out.push_back('[');
  std::map<WideString, int> index_of;
  // PDFium's field tree takes time that grows with the square of the fields
  // it loads, about 19 s for 50,000. Past the cap no field is read.
  std::unique_ptr<CPDF_InteractiveForm> form;
  if (nodes > kMaxFields || widgets > kMaxFields) {
    truncated = true;
  } else {
    // As EPDFForm_LoadModel: the declared tree, then the page sweep.
    form = std::make_unique<CPDF_InteractiveForm>(pdf);
    const std::set<const CPDF_Dictionary*> declared =
        epdf::CollectFieldDicts(*form);
    epdf::SweepPageWidgets(pdf, form.get());
    const size_t count = epdf::CountFormFields(*form);
    int written = 0;
    for (size_t i = 0; i < count; ++i) {
      CPDF_FormField* field = form->GetField(i, WideString());
      if (!field) {
        continue;
      }
      if (static_cast<size_t>(written) == kMaxFields) {
        truncated = true;
        break;
      }
      const size_t mark = out.size();
      if (written > 0) {
        out.push_back(',');
      }
      const CPDF_Dictionary* dict = field->GetFieldDict().Get();
      AppendField(&out, field, !declared.count(dict), places);
      if (out.size() > kMaxReportBytes / 2) {
        // Half the report is the most fields take, so /CO always fits.
        out.resize(mark);
        truncated = true;
        break;
      }
      index_of.try_emplace(field->GetFullName(), written);
      ++written;
    }
  }
  out.push_back(']');

  // Each /CO entry names the field whose full name its dictionary has, as
  // CPDF_InteractiveForm::GetFieldInCalculationOrder finds it.
  AppendNextKey(&out, "calculationOrder");
  out.push_back('[');
  std::string refs = "[";
  RetainPtr<const CPDF_Array> co =
      acro_form ? acro_form->GetArrayFor("CO") : nullptr;
  for (size_t i = 0; co && i < co->size(); ++i) {
    if (i == kMaxFields) {
      truncated = true;
      break;
    }
    if (i > 0) {
      out.push_back(',');
      refs.push_back(',');
    }
    RetainPtr<const CPDF_Dictionary> entry =
        ToDictionary(co->GetDirectObjectAt(i));
    const auto it = entry ? index_of.find(CPDF_FormField::GetFullNameForDict(
                                entry.Get()))
                          : index_of.end();
    out.append(std::to_string(it != index_of.end() ? it->second : -1));
    AppendRef(&refs, co->GetObjectAt(i).Get());
  }
  out.push_back(']');
  refs.push_back(']');
  AppendNextKey(&out, "calculationOrderRefs");
  out.append(refs);

  AppendNextKey(&out, "fieldTreeDepth");
  out.append(std::to_string(deepest));
  AppendNextKey(&out, "fieldTreeLoops");
  out.append(Boolean(loops));
  AppendNextKey(&out, "truncated");
  out.append(Boolean(truncated));
  out.push_back('}');
  return inkbar::report::Copy(out);
}

char* INK_Scripts(FPDF_DOCUMENT doc) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf) {
    return nullptr;
  }
  const CPDF_Dictionary* root = pdf->GetRoot();
  bool truncated = false;
  std::string out = "{\"document\":[";
  RetainPtr<const CPDF_Dictionary> names =
      root ? root->GetDictFor("Names") : nullptr;
  AppendDocumentScripts(
      &out, names ? names->GetDictFor("JavaScript").Get() : nullptr,
      &truncated);
  out.append("],\"documentActions\":{");
  std::string actions;
  std::string sources;
  if (AppendScripts(&sources, EntryFor(root, "OpenAction").Get(),
                    &truncated) > 0) {
    actions.append("\"OpenAction\":");
    actions.append(sources);
  }
  RetainPtr<const CPDF_Object> aa_object = EntryFor(root, "AA");
  if (const CPDF_Dictionary* aa =
          aa_object ? aa_object->AsDictionary() : nullptr) {
    std::string by_key;
    if (AppendScriptsByKey(&by_key, aa, &truncated)) {
      if (!actions.empty()) {
        actions.push_back(',');
      }
      actions.append(by_key);
    }
  }
  if (out.size() + actions.size() > kMaxReportBytes * 3 / 4) {
    truncated = true;
  } else {
    out.append(actions);
  }
  out.append("},\"pageActions\":[");
  const int page_count = pdf->GetPageCount();
  bool first = true;
  for (int i = 0; i < page_count; ++i) {
    RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(i);
    RetainPtr<const CPDF_Object> page_aa =
        page ? EntryFor(page.Get(), "AA") : nullptr;
    const CPDF_Dictionary* aa = page_aa ? page_aa->AsDictionary() : nullptr;
    if (!aa) {
      continue;
    }
    std::string by_key;
    if (!AppendScriptsByKey(&by_key, aa, &truncated)) {
      continue;
    }
    const size_t mark = out.size();
    if (!first) {
      out.push_back(',');
    }
    out.append("{\"page\":");
    out.append(std::to_string(i));
    out.append(",\"scripts\":{");
    out.append(by_key);
    out.append("}}");
    if (out.size() > kMaxReportBytes - kClosing) {
      out.resize(mark);
      truncated = true;
      break;
    }
    first = false;
  }
  out.append("],\"truncated\":");
  out.append(Boolean(truncated));
  out.push_back('}');
  return inkbar::report::Copy(out);
}

char* INK_Doc_Facts(FPDF_DOCUMENT doc) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf) {
    return nullptr;
  }
  CPDF_Parser* parser = pdf->GetParser();
  // /Info as FPDF_GetMetaText reads it, or a direct dictionary in the
  // trailer, which GetInfo() does not read.
  RetainPtr<const CPDF_Dictionary> info = pdf->GetInfo();
  if (!info && parser && parser->GetTrailer()) {
    info = parser->GetTrailer()->GetDictFor("Info");
  }
  std::string out = "{";
  bool cut = false;
  std::string text;
  AppendTextFor(&text, EntryFor(info.Get(), "Title").Get(), "title", &cut);
  AppendTextFor(&text, EntryFor(info.Get(), "Author").Get(), "author", &cut);
  // Each key above wrote a leading comma.
  if (!text.empty()) {
    out.append(text, 1, std::string::npos);
    out.push_back(',');
  }
  const bool encrypted = parser && parser->GetEncryptDict();
  AppendKey(&out, "encrypted");
  out.append(Boolean(encrypted));
  AppendNextKey(&out, "openedAs");
  const CPDF_SecurityHandler* security =
      parser ? parser->GetSecurityHandler().Get() : nullptr;
  out.append(!encrypted                              ? "\"none\""
             : security && security->IsOwnerUnlocked() ? "\"owner\""
                                                        : "\"user\"");
  if (cut) {
    AppendNextKey(&out, "truncated");
    out.append("true");
  }
  out.push_back('}');
  return inkbar::report::Copy(out);
}

}  // extern "C"
