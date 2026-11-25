// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// What annots.cc and form.cc share to write a report of what the file says:
// its strings as text or bytes, its numbers, its names, a report's caps and
// the JavaScript an action chain holds.

#ifndef INKBAR_REPORT_H_
#define INKBAR_REPORT_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/widestring.h"

class CPDF_Array;
class CPDF_Dictionary;
class CPDF_Object;

namespace inkbar::report {

// The most bytes one report writes. An entry that would pass it is left out,
// and the report stops there and says "truncated".
constexpr size_t kMaxReportBytes = 16 * 1024 * 1024;
// The most bytes a text string of the file writes inside its JSON string,
// escapes included; hex bytes take half as many of the file's bytes.
constexpr size_t kMaxTextBytes = 1024 * 1024;
// The most bytes one script writes inside its JSON string.
constexpr size_t kMaxScriptBytes = 4 * 1024 * 1024;
// The most numbers read from one array of numbers (/InkList as a whole,
// /Vertices, /QuadPoints).
constexpr size_t kMaxNumbers = 1000000;
// The deepest a walk goes: a field tree, a /Parent chain, a structure tree.
constexpr int kMaxDepth = 1000;
// The most actions read from one chain of /Next.
constexpr size_t kMaxChain = 1000;

// The cost charged for the keys and punctuation of one annotation, field or
// widget entry, and for one entry of an array; a number is charged 16 bytes,
// text and names what they write. Charges are at least what is written.
constexpr size_t kEntryCost = 1024;
constexpr size_t kItemCost = 32;

// The bytes a report may still write. The writers below take what they write
// from it before they write it; once too few are left they write no more, set
// their |*cut| and leave the budget spent, so every later writer stops too.
class Budget {
 public:
  explicit Budget(size_t bytes) : left_(bytes) {}
  // Takes |bytes| when that many are left. Otherwise spends the budget and
  // returns false.
  bool Take(size_t bytes) {
    if (spent_ || bytes > left_) {
      Spend();
      return false;
    }
    left_ -= bytes;
    return true;
  }
  void Spend() {
    left_ = 0;
    spent_ = true;
  }
  size_t left() const { return left_; }
  bool spent() const { return spent_; }

 private:
  size_t left_;
  bool spent_ = false;
};

const char* Boolean(bool value);

// "key":
void AppendKey(std::string* out, const char* key);
// ,"key":
void AppendNextKey(std::string* out, const char* key);

// A direct object, with a null object taken as absent.
RetainPtr<const CPDF_Object> EntryFor(const CPDF_Dictionary* dict,
                                      const char* key);

// A text string's bytes as a JSON string of the text, or as
// { "bytes": "<hex>" } when they are neither UTF-16 with a byte order mark
// (an odd length or an unpaired surrogate is not), UTF-8 with one, nor
// PDFDocEncoding (0x7F, 0x9F and 0xAD are undefined there). The text is
// PDFium's decoding, with language escapes taken out; it is cut after the
// last character that ends within |limit| written bytes, and hex after
// |limit| / 2 bytes, and within what |budget| has left. Sets |*cut| when it
// was cut and leaves it otherwise.
void AppendFileText(std::string* out,
                    const ByteString& bytes,
                    size_t limit,
                    Budget* budget,
                    bool* cut);
// Text PDFium decoded, cut the same way.
void AppendWideText(std::string* out,
                    const WideString& text,
                    size_t limit,
                    Budget* budget,
                    bool* cut);

// A name's bytes as json::AppendName writes them, or null when |budget| has
// too little left.
void AppendFileName(std::string* out,
                    const ByteString& name,
                    Budget* budget,
                    bool* cut);

// [n, g] of a reference: its object number and the generation of the object
// the file holds under it (PDFium keeps none in a reference), 0 when none.
void AppendRef(std::string* out, const CPDF_Object* object);

// An array's entries as numbers, each null when it is not a finite number,
// at most |max| of them and as many as |budget| allows. Sets |*cut| when
// there were more.
void AppendNumbers(std::string* out,
                   const CPDF_Array* array,
                   size_t max,
                   Budget* budget,
                   bool* cut);

// An array's names, each null when it is not a name, as many as |budget|
// allows.
void AppendNames(std::string* out,
                 const CPDF_Array* array,
                 Budget* budget,
                 bool* cut);

// The facts an annotation and a widget share, each written as ,"key":value.
//
// "rect": /Rect's four entries as written (corners not put in order), each a
// number or null; null when /Rect is not an array. "rectOk": four finite
// numbers. An array of more than four keeps its first four, and is not ok.
void AppendRect(std::string* out, const CPDF_Dictionary* dict);
// "hasAppearance": /AP /N is a stream, or a dictionary whose entry under /AS
// is one. "appearance": "stream", "states" (a dictionary) or null. "states":
// the names of /N's entries, in byte order (PDFium keeps a dictionary
// sorted), when /N is a dictionary.
// "as": /AS when it is a name.
void AppendAppearance(std::string* out,
                      const CPDF_Dictionary* dict,
                      Budget* budget,
                      bool* cut);
// "borderStyle": { "width": /W or null, "style": /S or null } when /BS is a
// dictionary. "border": /Border's first three entries when it is an array.
void AppendBorder(std::string* out, const CPDF_Dictionary* dict);

// The sources of the JavaScript actions in the chain |action| starts, in the
// order /Next gives them, depth first, each object once and at most
// kMaxChain actions. Each source is cut as CutText cuts it at
// kMaxScriptBytes, and the sources together take at most what |budget| has
// left (no source is decoded once it is spent); sets |*cut| when one was cut
// or the chain was longer.
std::vector<WideString> CollectScripts(const CPDF_Object* action,
                                       Budget* budget,
                                       bool* cut);
// The same as a JSON array, [] when there are none; returns how many.
size_t AppendScripts(std::string* out,
                     const CPDF_Object* action,
                     Budget* budget,
                     bool* cut);

// A malloc'd copy of |text| with its NUL, freed with INK_Json_Free.
char* Copy(const std::string& text);

}  // namespace inkbar::report

#endif  // INKBAR_REPORT_H_
