// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See form.h. The form is read through the same reconciled form
// EPDFForm_LoadModel builds (epdf::BuildReconciledForm's phases), so the
// fields, their widgets and their origin are the model's; the rest is read
// from the dictionaries. Nothing is written.

#include "inkbar/form.h"

#include <algorithm>
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
using inkbar::json::AppendText;
using inkbar::report::AppendFileName;
using inkbar::report::AppendFileText;
using inkbar::report::AppendKey;
using inkbar::report::AppendNextKey;
using inkbar::report::AppendNumber;
using inkbar::report::AppendNumbers;
using inkbar::report::AppendRef;
using inkbar::report::ScriptReader;
using inkbar::report::AppendWideText;
using inkbar::report::Boolean;
using inkbar::report::Budget;
using inkbar::report::EntryFor;
using inkbar::report::kEntryCost;
using inkbar::report::kItemCost;
using inkbar::report::kMaxDepth;
using inkbar::report::kMaxNumbers;
using inkbar::report::kMaxReportBytes;
using inkbar::report::kMaxTextBytes;

constexpr size_t kMaxFields = 50000;
// The most names, and the most nodes, read from the /Names /JavaScript tree.
constexpr size_t kMaxNames = 50000;
// Room left for the keys that close a report.
constexpr size_t kClosing = 256;

// Where a dictionary sits: page index and place in that page's /Annots.
using Places = std::map<const CPDF_Dictionary*, std::pair<int, int>>;

// What a walk of /Fields and its /Kids found.
struct FieldTree {
  // Every dictionary reached, each once.
  std::set<const CPDF_Dictionary*> seen;
  // The deepest level reached; 1 for a top-level field.
  int deepest = 0;
  // A dictionary or a /Kids array was reached a second time.
  bool revisits = false;
  // More than kMaxFields dictionaries: the walk stopped there.
  bool too_many = false;
  // A /Kids array lay deeper than kMaxDepth: the walk did not go there.
  bool too_deep = false;
};

// Walks /Fields and its /Kids, each dictionary and each array once, so a
// /Kids array that many fields share is read once.
FieldTree WalkFieldTree(const CPDF_Array* fields) {
  FieldTree tree;
  std::set<const CPDF_Array*> arrays;
  std::vector<std::pair<RetainPtr<const CPDF_Array>, int>> stack;
  if (fields) {
    arrays.insert(fields);
    stack.emplace_back(pdfium::WrapRetain(fields), 1);
  }
  while (!stack.empty()) {
    RetainPtr<const CPDF_Array> array = std::move(stack.back().first);
    const int depth = stack.back().second;
    stack.pop_back();
    for (size_t i = 0; i < array->size(); ++i) {
      RetainPtr<const CPDF_Dictionary> field = array->GetDictAt(i);
      if (!field) {
        continue;
      }
      if (!tree.seen.insert(field.Get()).second) {
        tree.revisits = true;
        continue;
      }
      if (tree.seen.size() > kMaxFields) {
        tree.too_many = true;
        return tree;
      }
      tree.deepest = std::max(tree.deepest, depth);
      RetainPtr<const CPDF_Array> kids = field->GetArrayFor("Kids");
      if (!kids) {
        continue;
      }
      if (!arrays.insert(kids.Get()).second) {
        tree.revisits = true;
        continue;
      }
      if (depth >= kMaxDepth) {
        tree.too_deep = true;
        continue;
      }
      stack.emplace_back(std::move(kids), depth + 1);
    }
  }
  return tree;
}

// Where each dictionary sits in FieldWalk's order.
using WalkIndex = std::map<const CPDF_Dictionary*, int>;

// A depth first walk of /Fields and /Kids, each array in the file's order,
// every dictionary reached once (the first time) and each /Kids array read
// once, at most kMaxDepth levels and kMaxFields dictionaries.
struct FieldWalk {
  std::vector<RetainPtr<const CPDF_Dictionary>> order;
  // For each of |order|, the dictionary whose /Kids the walk reached it
  // through, null for an entry of /Fields.
  std::vector<const CPDF_Dictionary*> parent;
  WalkIndex index;
  // A cap cut the walk short.
  bool cut = false;
};

FieldWalk WalkFields(const CPDF_Array* fields) {
  FieldWalk walk;
  std::set<const CPDF_Array*> arrays;
  // Entries to visit, last first, with their depth and the dictionary whose
  // /Kids holds them.
  struct Entry {
    RetainPtr<const CPDF_Object> object;
    int depth;
    const CPDF_Dictionary* parent;
  };
  std::vector<Entry> stack;
  const auto push = [&](const CPDF_Array* array, int depth,
                        const CPDF_Dictionary* parent) {
    for (size_t i = array->size(); i > 0; --i) {
      RetainPtr<const CPDF_Object> entry = array->GetObjectAt(i - 1);
      if (entry) {
        stack.push_back({std::move(entry), depth, parent});
      }
    }
  };
  if (fields) {
    arrays.insert(fields);
    push(fields, 1, nullptr);
  }
  while (!stack.empty()) {
    RetainPtr<const CPDF_Object> object = stack.back().object->GetDirect();
    const int depth = stack.back().depth;
    const CPDF_Dictionary* parent = stack.back().parent;
    stack.pop_back();
    RetainPtr<const CPDF_Dictionary> dict = ToDictionary(std::move(object));
    if (!dict || walk.index.count(dict.Get())) {
      continue;
    }
    if (walk.order.size() == kMaxFields) {
      walk.cut = true;
      break;
    }
    walk.index.emplace(dict.Get(), static_cast<int>(walk.order.size()));
    walk.order.push_back(dict);
    walk.parent.push_back(parent);
    RetainPtr<const CPDF_Object> kids_object = EntryFor(dict.Get(), "Kids");
    const CPDF_Array* kids = kids_object ? kids_object->AsArray() : nullptr;
    if (!kids || !arrays.insert(kids).second) {
      continue;
    }
    if (depth >= kMaxDepth) {
      walk.cut = true;
      continue;
    }
    push(kids, depth + 1, dict.Get());
  }
  return walk;
}

// Every dictionary in every page's /Annots, resolved as
// epdf::SweepPageWidgets resolves them, so a control's widget dictionary is
// found by the dictionary itself; its first place wins. Counts in
// |*reconciled| the widgets the sweep would add to the form: those the field
// tree does not hold that have /FT along /Parent, each once.
Places SweepPlaces(CPDF_Document* doc,
                   const std::set<const CPDF_Dictionary*>& tree,
                   size_t* reconciled) {
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
      if (!annot ||
          !places.try_emplace(annot.Get(), i, static_cast<int>(j)).second) {
        continue;
      }
      if (annot->GetNameFor("Subtype") == "Widget" &&
          !tree.count(annot.Get()) &&
          CPDF_FormField::GetFieldAttrForDict(annot.Get(), "FT")) {
        ++*reconciled;
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
                   Budget* budget,
                   bool* cut) {
  if (!value || !value->IsString()) {
    return;
  }
  AppendNextKey(out, name);
  AppendFileText(out, value->GetString(), kMaxTextBytes, budget, cut);
}

// An array entry that is a name as { "name": "<name>" }, the wrapper within
// the entry's kItemCost and the name charged what it writes; null when
// |budget| cannot take the name.
void AppendArrayName(std::string* out,
                     const ByteString& name,
                     Budget* budget,
                     bool* cut) {
  std::string written;
  inkbar::json::AppendName(&written, name);
  if (budget->Take(written.size())) {
    out->append("{\"name\":");
    out->append(written);
    out->push_back('}');
  } else {
    *cut = true;
    out->append("null");
  }
}

// ,"<kind_key>":"..." and, for a string, a name or an array, ,"<key>":value;
// for a value present as null, ,"<null_key>":true.
void AppendValue(std::string* out,
                 const CPDF_Object* value,
                 const char* kind_key,
                 const char* key,
                 const char* null_key,
                 Budget* budget,
                 bool* cut) {
  AppendNextKey(out, kind_key);
  if (!value || value->IsNull()) {
    out->append("\"none\"");
    if (value) {
      AppendNextKey(out, null_key);
      out->append("true");
    }
    return;
  }
  if (value->IsString()) {
    out->append("\"string\"");
    AppendNextKey(out, key);
    AppendFileText(out, value->GetString(), kMaxTextBytes, budget, cut);
  } else if (value->IsName()) {
    out->append("\"name\"");
    AppendNextKey(out, key);
    AppendFileName(out, value->GetString(), budget, cut);
  } else if (const CPDF_Array* array = value->AsArray()) {
    out->append("\"array\"");
    AppendNextKey(out, key);
    out->push_back('[');
    for (size_t i = 0; i < array->size(); ++i) {
      if (!budget->Take(kItemCost)) {
        *cut = true;
        break;
      }
      if (i > 0) {
        out->push_back(',');
      }
      RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
      if (entry && entry->IsString()) {
        AppendFileText(out, entry->GetString(), kMaxTextBytes, budget, cut);
      } else if (entry && entry->IsName()) {
        AppendArrayName(out, entry->GetString(), budget, cut);
      } else {
        out->append("null");
      }
    }
    out->push_back(']');
  } else {
    out->append("\"other\"");
  }
}

// Writes |sources| as a JSON array under |key| in |scripts|, charging the
// key with its quotes, colon and comma.
void AppendKeyedScripts(std::string* scripts,
                        bool* first,
                        const ByteString& key,
                        const std::vector<WideString>& sources,
                        Budget* budget,
                        bool* cut) {
  // A key is a JSON object key, never null: one the budget cannot take is
  // left out with its sources.
  std::string name;
  inkbar::json::AppendName(&name, key);
  if (!budget->Take(name.size() + 4)) {
    *cut = true;
    return;
  }
  if (!*first) {
    scripts->push_back(',');
  }
  scripts->append(name);
  scripts->push_back(':');
  inkbar::report::AppendSources(scripts, sources, budget, cut);
  *first = false;
}

// /AA's JavaScript by key along /Parent from |widget|, and "A" for the
// widget's own /A. For each key the nearest dictionary (the widget first)
// whose action under it holds JavaScript gives the sources: the rule PDF.js
// applies, which the viewer's reader follows today. Each level and each key
// costs |reader| a unit of work.
void AppendWidgetScripts(std::string* out,
                         const CPDF_Dictionary* widget,
                         ScriptReader* reader,
                         bool* cut) {
  std::map<ByteString, RetainPtr<const CPDF_Object>> chains;
  std::set<const CPDF_Dictionary*> seen;
  RetainPtr<const CPDF_Dictionary> level = pdfium::WrapRetain(widget);
  for (int depth = 0; level && depth <= kMaxDepth; ++depth) {
    if (!seen.insert(level.Get()).second) {
      break;
    }
    if (!reader->Work(1)) {
      *cut = true;
      break;
    }
    RetainPtr<const CPDF_Object> aa_object = EntryFor(level.Get(), "AA");
    if (const CPDF_Dictionary* aa =
            aa_object ? aa_object->AsDictionary() : nullptr) {
      for (const auto& [key, chain] : reader->KeysWithScripts(aa, cut)) {
        if (!reader->Work(1)) {
          *cut = true;
          break;
        }
        if (key != "A") {
          chains.try_emplace(key, chain);
        }
      }
    }
    level = level->GetDictFor("Parent");
  }
  RetainPtr<const CPDF_Object> own = EntryFor(widget, "A");
  if (own && !reader->Sources(own.Get(), cut).empty()) {
    chains.emplace("A", own);
  }
  AppendNextKey(out, "scripts");
  out->push_back('{');
  bool first = true;
  for (const auto& [key, chain] : chains) {
    AppendKeyedScripts(out, &first, key, reader->Sources(chain.Get(), cut),
                       reader->budget(), cut);
  }
  out->push_back('}');
}

// A byte string as hex, at most kMaxTextBytes / 2 bytes of it and as many as
// |budget| allows.
void AppendHexBytes(std::string* out,
                    const ByteString& bytes,
                    Budget* budget,
                    bool* cut) {
  const size_t keep =
      std::min({bytes.GetLength(), kMaxTextBytes / 2, budget->left() / 2});
  budget->Take(keep * 2);
  if (keep < bytes.GetLength()) {
    *cut = true;
  }
  AppendHex(out, bytes.First(keep));
}

// The widget's own /A, its first action only.
void AppendAction(std::string* out,
                  const CPDF_Dictionary* widget,
                  Budget* budget,
                  bool* cut) {
  RetainPtr<const CPDF_Object> a = EntryFor(widget, "A");
  const CPDF_Dictionary* action = a ? a->AsDictionary() : nullptr;
  if (!action) {
    return;
  }
  const ByteString s = action->GetNameFor("S");
  AppendNextKey(out, "action");
  out->push_back('{');
  AppendKey(out, "s");
  AppendFileName(out, s, budget, cut);
  if (s == "URI") {
    RetainPtr<const CPDF_Object> uri = EntryFor(action, "URI");
    const ByteString bytes = uri && (uri->IsString() || uri->IsName())
                                 ? uri->GetString()
                                 : ByteString();
    AppendNextKey(out, "uriHex");
    AppendHexBytes(out, bytes, budget, cut);
  } else if (s == "Launch" || s == "GoToR") {
    // /F as the file gives it: a string, or a file specification's /F and
    // /UF. No address is built from them.
    RetainPtr<const CPDF_Object> file = EntryFor(action, "F");
    if (file) {
      const CPDF_Dictionary* spec = file->AsDictionary();
      AppendNextKey(out, "fileKind");
      out->append(file->IsString() ? "\"string\""
                  : spec           ? "\"spec\""
                                   : "\"other\"");
      if (file->IsString()) {
        AppendNextKey(out, "fileHex");
        AppendHexBytes(out, file->GetString(), budget, cut);
      }
      for (const auto& [key, name] : {std::pair{"F", "fileSpecFHex"},
                                      std::pair{"UF", "fileSpecUFHex"}}) {
        RetainPtr<const CPDF_Object> entry = EntryFor(spec, key);
        if (entry && entry->IsString()) {
          AppendNextKey(out, name);
          AppendHexBytes(out, entry->GetString(), budget, cut);
        }
      }
    }
  } else if (s == "Named") {
    RetainPtr<const CPDF_Object> name = EntryFor(action, "N");
    AppendNextKey(out, "name");
    AppendFileName(out, name && name->IsName() ? name->GetString() : ByteString(),
                   budget, cut);
  } else if (s == "ResetForm") {
    RetainPtr<const CPDF_Object> fields_object = EntryFor(action, "Fields");
    const CPDF_Array* fields =
        fields_object ? fields_object->AsArray() : nullptr;
    std::string names = "[";
    std::string refs = "[";
    for (size_t i = 0; fields && i < fields->size(); ++i) {
      if (!budget->Take(kItemCost)) {
        *cut = true;
        break;
      }
      RetainPtr<const CPDF_Object> entry = fields->GetObjectAt(i);
      if (entry && entry->IsReference()) {
        if (refs.size() > 1) {
          refs.push_back(',');
        }
        AppendRef(&refs, entry.Get());
        continue;
      }
      RetainPtr<const CPDF_Object> value = entry ? entry->GetDirect() : nullptr;
      if (value && value->IsString()) {
        if (names.size() > 1) {
          names.push_back(',');
        }
        AppendFileText(&names, value->GetString(), kMaxTextBytes, budget, cut);
      }
    }
    names.push_back(']');
    refs.push_back(']');
    AppendNextKey(out, "fields");
    out->append(names);
    AppendNextKey(out, "refs");
    out->append(refs);
    AppendNextKey(out, "flags");
    out->append(std::to_string(action->GetIntegerFor("Flags")));
  }
  out->push_back('}');
}

// An array's text strings as Text, its names as { "name": "<name>" }, each
// other entry null, at most
// kMaxNumbers entries and as many as |budget| allows; null for no array.
void AppendTexts(std::string* out,
                 const CPDF_Array* array,
                 Budget* budget,
                 bool* cut) {
  if (!array) {
    out->append("null");
    return;
  }
  out->push_back('[');
  for (size_t i = 0; i < array->size(); ++i) {
    if (i == kMaxNumbers || !budget->Take(kItemCost)) {
      *cut = true;
      break;
    }
    if (i > 0) {
      out->push_back(',');
    }
    RetainPtr<const CPDF_Object> entry = array->GetDirectObjectAt(i);
    if (entry && entry->IsString()) {
      AppendFileText(out, entry->GetString(), kMaxTextBytes, budget, cut);
    } else if (entry && entry->IsName()) {
      AppendArrayName(out, entry->GetString(), budget, cut);
    } else {
      out->append("null");
    }
  }
  out->push_back(']');
}

// ,"t": the dictionary's own /T, Text when it is a string, null when it is
// anything else, nothing when it has none; and ,"parentIsWalkParent":false
// when its /Parent is not the dictionary |walk_parent| (null for an entry of
// /Fields, whose /Parent should be absent).
void AppendNameFacts(std::string* out,
                     const CPDF_Dictionary* dict,
                     const CPDF_Dictionary* walk_parent,
                     Budget* budget,
                     bool* cut) {
  RetainPtr<const CPDF_Object> t = EntryFor(dict, "T");
  if (t) {
    AppendNextKey(out, "t");
    if (t->IsString()) {
      AppendFileText(out, t->GetString(), kMaxTextBytes, budget, cut);
    } else {
      out->append("null");
    }
  }
  if (dict->GetDictFor("Parent").Get() != walk_parent) {
    AppendNextKey(out, "parentIsWalkParent");
    out->append("false");
  }
}

// { "ref": [n, g] | null, "dictionary": bool, "hasT": bool, "hasKids": bool }
// for an entry of /Fields or /Kids; at most about 85 bytes.
void AppendKid(std::string* out, const CPDF_Object* entry) {
  out->append("{\"ref\":");
  AppendRef(out, entry);
  RetainPtr<const CPDF_Object> target = entry ? entry->GetDirect() : nullptr;
  const CPDF_Dictionary* kid = target ? target->AsDictionary() : nullptr;
  AppendNextKey(out, "dictionary");
  out->append(Boolean(kid));
  if (kid) {
    AppendNextKey(out, "hasT");
    out->append(Boolean(kid->KeyExist("T")));
    AppendNextKey(out, "hasKids");
    out->append(Boolean(kid->KeyExist("Kids")));
  }
  out->push_back('}');
}

// The entries of /Kids or /Fields as AppendKid writes them, at most kMaxFields
// and as many as |budget| allows.
void AppendKids(std::string* out,
                const CPDF_Array* kids,
                Budget* budget,
                bool* cut) {
  out->push_back('[');
  for (size_t i = 0; kids && i < kids->size(); ++i) {
    if (i == kMaxFields || !budget->Take(3 * kItemCost)) {
      *cut = true;
      break;
    }
    if (i > 0) {
      out->push_back(',');
    }
    AppendKid(out, kids->GetObjectAt(i).Get());
  }
  out->push_back(']');
}

// "borderColor" (/MK /BC), "backgroundColor" (/MK /BG), "rotation" (/MK /R)
// and "caption" (/MK /CA) of |dict|'s own /MK, each when present.
void AppendMk(std::string* out,
              const CPDF_Dictionary* dict,
              Budget* budget,
              bool* cut) {
  RetainPtr<const CPDF_Object> mk_object = EntryFor(dict, "MK");
  const CPDF_Dictionary* mk = mk_object ? mk_object->AsDictionary() : nullptr;
  if (!mk) {
    return;
  }
  for (const auto& [key, name] :
       {std::pair{"BC", "borderColor"}, std::pair{"BG", "backgroundColor"}}) {
    RetainPtr<const CPDF_Object> colour = EntryFor(mk, key);
    if (const CPDF_Array* array = colour ? colour->AsArray() : nullptr) {
      AppendNextKey(out, name);
      AppendNumbers(out, array, kMaxNumbers, budget, cut);
    }
  }
  if (const CPDF_Number* r = ToNumber(EntryFor(mk, "R").Get())) {
    AppendNextKey(out, "rotation");
    AppendNumber(out, r);
  }
  AppendTextFor(out, EntryFor(mk, "CA").Get(), "caption", budget, cut);
}

// /I's entries as numbers, or null when it is not an array.
void AppendIndices(std::string* out,
                   const CPDF_Object* indices,
                   Budget* budget,
                   bool* cut) {
  if (const CPDF_Array* array = indices->AsArray()) {
    AppendNumbers(out, array, kMaxNumbers, budget, cut);
  } else {
    out->append("null");
  }
}

// Returns whether it cut anything.
bool AppendWidget(std::string* out,
                  const CPDF_FormControl* control,
                  const CPDF_Dictionary* field_dict,
                  bool toggle,
                  bool choice,
                  const Places& places,
                  ScriptReader* reader) {
  Budget* budget = reader->budget();
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
  inkbar::report::AppendAppearance(out, widget, budget, &cut);
  if (toggle) {
    AppendNextKey(out, "onState");
    AppendFileName(out, control->GetOnStateName(), budget, &cut);
    AppendNextKey(out, "exportValue");
    AppendWideText(out, control->GetExportValue(), kMaxTextBytes, budget,
                   &cut);
  }
  inkbar::report::AppendBorder(out, widget, budget, &cut);
  AppendMk(out, widget, budget, &cut);
  if (toggle && field_dict != widget) {
    // The widget's own /Opt, where PDF.js starts its lookup.
    RetainPtr<const CPDF_Object> own = EntryFor(widget, "Opt");
    if (own) {
      AppendNextKey(out, "opt");
      AppendTexts(out, own->AsArray(), budget, &cut);
    }
  }
  if (choice && field_dict != widget) {
    // The widget's own /I, which PDF.js reads from the widget alone.
    RetainPtr<const CPDF_Object> own = EntryFor(widget, "I");
    if (own) {
      AppendNextKey(out, "selectedIndices");
      AppendIndices(out, own.Get(), budget, &cut);
    }
  }
  if (const CPDF_Number* q = ToNumber(
          CPDF_FormField::GetFieldAttrForDict(widget, "Q").Get())) {
    AppendNextKey(out, "q");
    AppendNumber(out, q);
  }
  AppendTextFor(out, EntryFor(widget, "TU").Get(), "tooltip", budget, &cut);
  AppendTextFor(out, CPDF_FormField::GetFieldAttrForDict(widget, "DA").Get(),
                "da", budget, &cut);
  if (EntryFor(widget, "PMD")) {
    AppendNextKey(out, "pmd");
    out->append("true");
  }
  AppendWidgetScripts(out, widget, reader, &cut);
  AppendAction(out, widget, budget, &cut);
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
  return cut;
}

// Writes as much of the field as |budget| allows, and says "truncated" when
// it cut anything; returns whether it did.
bool AppendField(std::string* out,
                 CPDF_FormField* field,
                 bool recovered,
                 const Places& places,
                 const FieldWalk& walk,
                 ScriptReader* reader) {
  Budget* budget = reader->budget();
  const CPDF_Dictionary* dict = field->GetFieldDict().Get();
  bool cut = false;
  out->push_back('{');
  AppendKey(out, "name");
  AppendWideText(out, field->GetFullName(), kMaxTextBytes, budget, &cut);
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
              "valueKind", "value", "valueNull", budget, &cut);
  AppendValue(out, CPDF_FormField::GetFieldAttrForDict(dict, "DV").Get(),
              "defaultValueKind", "defaultValue", "defaultValueNull", budget,
              &cut);
  // /I, inherited as /V is, from the same dictionaries.
  RetainPtr<const CPDF_Object> selected =
      CPDF_FormField::GetFieldAttrForDict(dict, "I");
  if (selected && !selected->IsNull()) {
    AppendNextKey(out, "selectedIndices");
    AppendIndices(out, selected.Get(), budget, &cut);
    AppendNextKey(out, "selectedIndicesOwn");
    out->append(Boolean(EntryFor(dict, "I") != nullptr));
  }
  if (type == CPDF_FormField::kComboBox || type == CPDF_FormField::kListBox) {
    AppendNextKey(out, "options");
    out->push_back('[');
    const int count = field->CountOptions();
    for (int i = 0; i < count; ++i) {
      if (!budget->Take(kItemCost)) {
        cut = true;
        break;
      }
      if (i > 0) {
        out->push_back(',');
      }
      out->push_back('{');
      AppendKey(out, "exportValue");
      AppendWideText(out, field->GetOptionValue(i), kMaxTextBytes, budget,
                     &cut);
      AppendNextKey(out, "displayValue");
      AppendWideText(out, field->GetOptionLabel(i), kMaxTextBytes, budget,
                     &cut);
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
  AppendTextFor(out, EntryFor(dict, "TU").Get(), "tooltip", budget, &cut);
  const bool toggle = type == CPDF_FormField::kCheckBox ||
                      type == CPDF_FormField::kRadioButton;
  const bool choice =
      type == CPDF_FormField::kComboBox || type == CPDF_FormField::kListBox;
  if (toggle) {
    // /Opt as the file gives it, inherited as /V is.
    RetainPtr<const CPDF_Object> opt =
        CPDF_FormField::GetFieldAttrForDict(dict, "Opt");
    if (opt && !opt->IsNull()) {
      AppendNextKey(out, "opt");
      AppendTexts(out, opt->AsArray(), budget, &cut);
    }
  }
  const auto walked = walk.index.find(dict);
  if (walked != walk.index.end()) {
    AppendNextKey(out, "walkIndex");
    out->append(std::to_string(walked->second));
    AppendNameFacts(out, dict, walk.parent[walked->second], budget, &cut);
  }
  AppendNextKey(out, "widgets");
  out->push_back('[');
  const int controls = field->CountControls();
  bool first = true;
  for (int i = 0; i < controls; ++i) {
    const CPDF_FormControl* control = field->GetControl(i);
    if (!control) {
      continue;
    }
    if (!budget->Take(kEntryCost)) {
      cut = true;
      break;
    }
    if (!first) {
      out->push_back(',');
    }
    first = false;
    // A field with a cut widget is cut too.
    if (AppendWidget(out, control, dict, toggle, choice, places, reader)) {
      cut = true;
    }
  }
  out->push_back(']');
  cut = cut || budget->spent();
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
  return cut;
}

// The index of each page dictionary, read the first time one is asked for.
class Pages {
 public:
  explicit Pages(CPDF_Document* doc) : doc_(doc) {}
  // -1 when |page| is not a page of the document.
  int IndexOf(const CPDF_Dictionary* page) {
    if (!loaded_) {
      loaded_ = true;
      const int count = doc_->GetPageCount();
      for (int i = 0; i < count; ++i) {
        RetainPtr<const CPDF_Dictionary> dict = doc_->GetPageDictionary(i);
        if (dict) {
          index_.try_emplace(dict.Get(), i);
        }
      }
    }
    const auto it = index_.find(page);
    return it != index_.end() ? it->second : -1;
  }

 private:
  CPDF_Document* const doc_;
  bool loaded_ = false;
  std::map<const CPDF_Dictionary*, int> index_;
};

// A dictionary of the walk that has a /Kids array: its name and own /T, its
// place in the walk and in "fields", its /Subtype, /Kids, /MK, /P and
// scripts. Returns whether it cut anything.
bool AppendGroup(std::string* out,
                 const FieldWalk& walk,
                 size_t walk_index,
                 int field_index,
                 Pages* pages,
                 ScriptReader* reader) {
  Budget* budget = reader->budget();
  const CPDF_Dictionary* dict = walk.order[walk_index].Get();
  bool cut = false;
  out->push_back('{');
  AppendKey(out, "name");
  AppendWideText(out, CPDF_FormField::GetFullNameForDict(dict), kMaxTextBytes,
                 budget, &cut);
  AppendObjectNumber(out, dict);
  AppendNextKey(out, "walkIndex");
  out->append(std::to_string(walk_index));
  if (field_index >= 0) {
    AppendNextKey(out, "field");
    out->append(std::to_string(field_index));
  }
  AppendNameFacts(out, dict, walk.parent[walk_index], budget, &cut);
  RetainPtr<const CPDF_Object> subtype = EntryFor(dict, "Subtype");
  if (subtype && subtype->IsName()) {
    AppendNextKey(out, "subtype");
    AppendFileName(out, subtype->GetString(), budget, &cut);
  }
  AppendNextKey(out, "kids");
  RetainPtr<const CPDF_Object> kids = EntryFor(dict, "Kids");
  AppendKids(out, kids ? kids->AsArray() : nullptr, budget, &cut);
  AppendMk(out, dict, budget, &cut);
  if (RetainPtr<const CPDF_Object> p = EntryFor(dict, "P")) {
    AppendNextKey(out, "page");
    out->append(std::to_string(pages->IndexOf(p->AsDictionary())));
  }
  AppendWidgetScripts(out, dict, reader, &cut);
  cut = cut || budget->spent();
  if (cut) {
    AppendNextKey(out, "truncated");
    out->append("true");
  }
  out->push_back('}');
  return cut;
}

// The JavaScript under each key of |dict| that holds some, as "key":[...]
// pairs; returns whether any did.
bool AppendScriptsByKey(std::string* out,
                        const CPDF_Dictionary* dict,
                        ScriptReader* reader,
                        bool* cut) {
  bool first = true;
  for (const auto& [key, chain] : reader->KeysWithScripts(dict, cut)) {
    if (reader->budget()->spent() || !reader->Work(1)) {
      *cut = true;
      break;
    }
    AppendKeyedScripts(out, &first, key, reader->Sources(chain.Get(), cut),
                       reader->budget(), cut);
  }
  return !first;
}

// The /Names /JavaScript tree in the tree's order, each node, /Names array
// and /Kids array once, at most kMaxNames names and kMaxNames nodes.
void AppendDocumentScripts(std::string* out,
                           const CPDF_Dictionary* tree,
                           ScriptReader* reader,
                           bool* truncated) {
  Budget* budget = reader->budget();
  std::set<const CPDF_Object*> seen;
  std::vector<std::pair<RetainPtr<const CPDF_Dictionary>, int>> stack;
  if (tree) {
    seen.insert(tree);
    stack.emplace_back(pdfium::WrapRetain(tree), 0);
  }
  size_t names = 0;
  size_t nodes = 0;
  bool first = true;
  while (!stack.empty()) {
    RetainPtr<const CPDF_Dictionary> node = std::move(stack.back().first);
    const int depth = stack.back().second;
    stack.pop_back();
    if (++nodes > kMaxNames) {
      *truncated = true;
      return;
    }
    RetainPtr<const CPDF_Array> pairs = node->GetArrayFor("Names");
    if (pairs && !seen.insert(pairs.Get()).second) {
      pairs = nullptr;
    }
    for (size_t i = 0; pairs && i + 1 < pairs->size(); i += 2) {
      if (names++ == kMaxNames || budget->spent()) {
        *truncated = true;
        return;
      }
      RetainPtr<const CPDF_Object> name = pairs->GetDirectObjectAt(i);
      bool cut = false;
      std::string entry_name;
      if (name && name->IsString()) {
        AppendFileText(&entry_name, name->GetString(), kMaxTextBytes, budget,
                       &cut);
      } else {
        entry_name = "null";
      }
      const std::vector<WideString>& sources =
          reader->Sources(pairs->GetObjectAt(i + 1).Get(), &cut);
      for (const WideString& source : sources) {
        // The name is charged again for each source that repeats it.
        if (!budget->Take(kItemCost + entry_name.size())) {
          *truncated = true;
          return;
        }
        const size_t mark = out->size();
        if (!first) {
          out->push_back(',');
        }
        out->append("{\"name\":");
        out->append(entry_name);
        out->append(",\"source\":");
        AppendWideText(out, source, inkbar::report::kMaxScriptBytes, budget,
                       &cut);
        out->push_back('}');
        // A backstop: the budget charges at least what is written.
        if (out->size() > kMaxReportBytes - kClosing) {
          out->resize(mark);
          *truncated = true;
          return;
        }
        first = false;
      }
      *truncated = *truncated || cut;
    }
    RetainPtr<const CPDF_Array> kids = node->GetArrayFor("Kids");
    if (!kids || !seen.insert(kids.Get()).second) {
      continue;
    }
    if (depth + 1 > kMaxDepth) {
      *truncated = true;
      continue;
    }
    // Pushed last first, so the first kid is read next.
    std::vector<RetainPtr<const CPDF_Dictionary>> fresh;
    for (size_t i = 0; i < kids->size(); ++i) {
      RetainPtr<const CPDF_Dictionary> kid = kids->GetDictAt(i);
      if (kid && seen.insert(kid.Get()).second) {
        fresh.push_back(std::move(kid));
      }
    }
    for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
      stack.emplace_back(std::move(*it), depth + 1);
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
  // Half the report for /AcroForm's own keys and the fields, the rest for
  // /CO and the closing keys, so /CO always fits.
  Budget budget(kMaxReportBytes / 2);
  ScriptReader reader(&budget);
  AppendTextFor(&out, EntryFor(acro_form.Get(), "DA").Get(), "da", &budget,
                &truncated);
  if (const CPDF_Number* q = ToNumber(EntryFor(acro_form.Get(), "Q").Get())) {
    AppendNextKey(&out, "q");
    AppendNumber(&out, q);
  }

  const FieldTree tree = WalkFieldTree(
      acro_form ? acro_form->GetArrayFor("Fields").Get() : nullptr);
  truncated = truncated || tree.too_deep;
  size_t reconciled = 0;
  const Places places = SweepPlaces(pdf, tree.seen, &reconciled);
  // PDFium's field tree takes time that grows with the square of what it
  // loads, about 19 s for 50,000 fields, so above that it is not loaded.
  const bool too_large =
      tree.too_many || tree.seen.size() + reconciled > kMaxFields;

  AppendNextKey(&out, "fields");
  out.push_back('[');
  std::map<WideString, int> index_of;
  std::string groups = "[";
  RetainPtr<const CPDF_Array> root_fields =
      acro_form ? acro_form->GetArrayFor("Fields") : nullptr;
  if (!too_large) {
    const FieldWalk walk = WalkFields(root_fields.Get());
    truncated = truncated || walk.cut;
    // As EPDFForm_LoadModel: the declared tree, then the page sweep.
    CPDF_InteractiveForm form(pdf);
    const std::set<const CPDF_Dictionary*> declared =
        epdf::CollectFieldDicts(form);
    epdf::SweepPageWidgets(pdf, &form);
    const size_t count = epdf::CountFormFields(form);
    int written = 0;
    // Each written field's dictionary and its index in "fields".
    std::map<const CPDF_Dictionary*, int> field_index;
    for (size_t i = 0; i < count; ++i) {
      CPDF_FormField* field = form.GetField(i, WideString());
      if (!field) {
        continue;
      }
      if (static_cast<size_t>(written) == kMaxFields ||
          !budget.Take(kEntryCost)) {
        truncated = true;
        break;
      }
      const size_t mark = out.size();
      if (written > 0) {
        out.push_back(',');
      }
      const CPDF_Dictionary* dict = field->GetFieldDict().Get();
      // A cut field cuts the report.
      if (AppendField(&out, field, !declared.count(dict), places, walk,
                      &reader)) {
        truncated = true;
      }
      // The budget charges at least what is written; this is a backstop.
      if (out.size() > kMaxReportBytes / 2) {
        out.resize(mark);
        truncated = true;
        break;
      }
      index_of.try_emplace(field->GetFullName(), written);
      field_index.try_emplace(dict, written);
      ++written;
      // A field the budget cut ends the list.
      if (budget.spent()) {
        truncated = true;
        break;
      }
    }

    // Every dictionary of the walk with a /Kids array, in walk order.
    Pages pages(pdf);
    bool first_group = true;
    for (size_t i = 0; i < walk.order.size() && !budget.spent(); ++i) {
      const CPDF_Dictionary* dict = walk.order[i].Get();
      RetainPtr<const CPDF_Object> kids = EntryFor(dict, "Kids");
      if (!kids || !kids->IsArray()) {
        continue;
      }
      const auto field = field_index.find(dict);
      if (!budget.Take(kEntryCost)) {
        truncated = true;
        break;
      }
      const size_t mark = groups.size();
      if (!first_group) {
        groups.push_back(',');
      }
      if (AppendGroup(&groups, walk, i,
                      field != field_index.end() ? field->second : -1, &pages,
                      &reader)) {
        truncated = true;
      }
      // The budget charges at least what is written; this is a backstop.
      if (out.size() + groups.size() > kMaxReportBytes / 2) {
        groups.resize(mark);
        truncated = true;
        break;
      }
      first_group = false;
    }
  }
  out.push_back(']');
  groups.push_back(']');
  AppendNextKey(&out, "groups");
  out.append(groups);
  // /Fields itself, so a reader can walk from the root; [] for a refused
  // form, as "groups".
  AppendNextKey(&out, "fieldsRoot");
  {
    const size_t mark = out.size();
    AppendKids(&out, too_large ? nullptr : root_fields.Get(), &budget,
               &truncated);
    // The budget charges at least what is written; this is a backstop.
    if (out.size() > kMaxReportBytes / 2) {
      out.resize(mark);
      out.append("[]");
      truncated = true;
    }
  }
  truncated = truncated || reader.exhausted();

  // Each /CO entry names the field whose full name its dictionary has, as
  // CPDF_InteractiveForm::GetFieldInCalculationOrder finds it. An entry costs
  // at most 2 * kItemCost: kMaxFields of them fit in what is left.
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
  out.append(std::to_string(tree.deepest));
  AppendNextKey(&out, "fieldTreeRevisits");
  out.append(Boolean(tree.revisits));
  AppendNextKey(&out, "tooLarge");
  out.append(Boolean(too_large));
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
  Budget budget(kMaxReportBytes - kClosing);
  ScriptReader reader(&budget);
  std::string out = "{\"document\":[";
  RetainPtr<const CPDF_Dictionary> names =
      root ? root->GetDictFor("Names") : nullptr;
  AppendDocumentScripts(
      &out, names ? names->GetDictFor("JavaScript").Get() : nullptr, &reader,
      &truncated);
  out.append("],\"documentActions\":{");
  std::string actions;
  bool first = true;
  RetainPtr<const CPDF_Object> open = EntryFor(root, "OpenAction");
  const std::vector<WideString>& opening =
      reader.Sources(open.Get(), &truncated);
  if (!opening.empty()) {
    AppendKeyedScripts(&actions, &first, "OpenAction", opening, &budget,
                       &truncated);
  }
  RetainPtr<const CPDF_Object> aa_object = EntryFor(root, "AA");
  if (const CPDF_Dictionary* aa =
          aa_object ? aa_object->AsDictionary() : nullptr) {
    std::string by_key;
    if (AppendScriptsByKey(&by_key, aa, &reader, &truncated)) {
      if (!first) {
        actions.push_back(',');
      }
      actions.append(by_key);
    }
  }
  // A backstop: the budget charges at least what is written.
  if (out.size() + actions.size() > kMaxReportBytes - kClosing) {
    truncated = true;
  } else {
    out.append(actions);
  }
  out.append("},\"pageActions\":[");
  const int page_count = pdf->GetPageCount();
  first = true;
  for (int i = 0; i < page_count && !budget.spent(); ++i) {
    RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(i);
    RetainPtr<const CPDF_Object> page_aa =
        page ? EntryFor(page.Get(), "AA") : nullptr;
    const CPDF_Dictionary* aa = page_aa ? page_aa->AsDictionary() : nullptr;
    if (!aa) {
      continue;
    }
    std::string by_key;
    if (!AppendScriptsByKey(&by_key, aa, &reader, &truncated)) {
      continue;
    }
    if (!budget.Take(kItemCost)) {
      truncated = true;
      break;
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
    // A backstop: the budget charges at least what is written.
    if (out.size() > kMaxReportBytes - kClosing) {
      out.resize(mark);
      truncated = true;
      break;
    }
    first = false;
  }
  truncated = truncated || budget.spent() || reader.exhausted();
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
  Budget budget(kMaxReportBytes - kClosing);
  AppendTextFor(&text, EntryFor(info.Get(), "Title").Get(), "title", &budget,
                &cut);
  AppendTextFor(&text, EntryFor(info.Get(), "Author").Get(), "author",
                &budget, &cut);
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
