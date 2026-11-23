// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See text.h. A thin layer over FPDFText_LoadPage and its per-character
// getters: it copies what they report into flat arrays, so a caller crosses
// into the module once per page and not once per character.

#include "inkbar/text.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/font/cpdf_type3font.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdftext/cpdf_textpage.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxge/cfx_font.h"
#include "core/fxge/cfx_standardfont.h"
#include "core/fxge/fx_font.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "inkbar/json.h"
#include "public/epdf_view.h"
#include "public/fpdf_text.h"

struct InkText {
  std::vector<unsigned int> codes;
  std::vector<float> geometry;
  std::vector<unsigned char> flags;
  std::vector<unsigned short> fonts;
  std::string font_table;
  bool truncated = false;
};

namespace {

constexpr int kGeometryPerChar = 15;
constexpr unsigned char kGenerated = 1;
constexpr unsigned char kHyphen = 2;
constexpr unsigned char kNoUnicode = 4;
// A Type 3 character below this, a control code, stays without a mapping.
constexpr unsigned int kFirstPrintable = 32;
constexpr size_t kMaxFonts = 65535;
// The most characters read from one page.
constexpr int kMaxChars = 1000000;

bool IsTimes(const ByteString& name) {
  std::optional<CFX_StandardFont::Index> index =
      CFX_StandardFont::GetStandardFontIndex(name);
  return index.has_value() &&
         (index.value() == CFX_StandardFont::kTimes ||
          index.value() == CFX_StandardFont::kTimesBold ||
          index.value() == CFX_StandardFont::kTimesBoldOblique ||
          index.value() == CFX_StandardFont::kTimesOblique);
}

// The font descriptor's |key| divided by 1000, as the file gives it, or null
// when there is no descriptor or the key is absent or not a number. A Type 0
// font's descriptor is its descendant's.
void AppendDescriptorMetric(std::string* out,
                            const CPDF_Font* font,
                            const char* key) {
  RetainPtr<const CPDF_Dictionary> dict = font->GetFontDict();
  if (dict && font->IsCIDFont()) {
    RetainPtr<const CPDF_Array> descendants =
        dict->GetArrayFor("DescendantFonts");
    dict = descendants ? descendants->GetDictAt(0) : nullptr;
  }
  RetainPtr<const CPDF_Dictionary> descriptor =
      dict ? dict->GetDictFor("FontDescriptor") : nullptr;
  RetainPtr<const CPDF_Object> value =
      descriptor ? descriptor->GetDirectObjectFor(key) : nullptr;
  const CPDF_Number* number = ToNumber(value.Get());
  if (number) {
    inkbar::json::AppendNumber(out, number->GetNumber() / 1000.f);
  } else {
    out->append("null");
  }
}

// One font table entry. Each property is the font descriptor's flag or, where
// PDFium can tell, the face it loaded for the font (a standard font has no
// descriptor).
void AppendFont(std::string* out, const CPDF_Font* font) {
  const int flags = font->GetFontFlags();
  const CFX_Font* face = font->GetFont();
  const ByteString name = font->GetBaseFontName();
  const bool fixed_pitch =
      (flags & pdfium::kFontStyleFixedPitch) || face->IsFixedWidth();
  const bool serif = (flags & pdfium::kFontStyleSerif) || IsTimes(name);
  const bool italic = (flags & pdfium::kFontStyleItalic) || face->IsItalic();
  const bool bold = (flags & pdfium::kFontStyleForceBold) || face->IsBold();
  auto boolean = [](bool value) { return value ? "true" : "false"; };
  out->append("{\"name\":");
  inkbar::json::AppendName(out, name);
  out->append(",\"fixedPitch\":");
  out->append(boolean(fixed_pitch));
  out->append(",\"serif\":");
  out->append(boolean(serif));
  out->append(",\"italic\":");
  out->append(boolean(italic));
  out->append(",\"bold\":");
  out->append(boolean(bold));
  out->append(",\"vertical\":");
  out->append(boolean(font->IsVertWriting()));
  // In em units, as FPDFFont_GetAscent and FPDFFont_GetDescent give them at
  // size 1. A Type 3 font's glyph space is its own, and PDFium fills its
  // figures only once a glyph is drawn, so it gives none.
  const int ascent = font->GetTypeAscent();
  const int descent = font->GetTypeDescent();
  const bool metrics = !font->IsType3Font() && (ascent != 0 || descent != 0);
  out->append(",\"ascent\":");
  if (metrics) {
    inkbar::json::AppendNumber(out, ascent / 1000.f);
  } else {
    out->append("null");
  }
  out->append(",\"descent\":");
  if (metrics) {
    inkbar::json::AppendNumber(out, descent / 1000.f);
  } else {
    out->append("null");
  }
  out->append(",\"descriptorAscent\":");
  AppendDescriptorMetric(out, font, "Ascent");
  out->append(",\"descriptorDescent\":");
  AppendDescriptorMetric(out, font, "Descent");
  // The standard font PDFium draws it with: a Type 1 font of the standard 14
  // with no font file. PDFium stores its canonical name as the base name.
  out->append(",\"standard\":");
  std::optional<CFX_StandardFont::Index> index =
      font->IsStandardFont()
          ? CFX_StandardFont::GetStandardFontIndex(name)
          : std::nullopt;
  if (index.has_value()) {
    inkbar::json::AppendName(
        out, CFX_StandardFont::GetCanonicalFontName(index.value()));
  } else {
    out->append("null");
  }
  out->push_back('}');
}

// The font of character |i|'s text object, or null for a character PDFium
// generated without one.
RetainPtr<CPDF_Font> FontOf(FPDF_TEXTPAGE text_page, int i) {
  const CPDF_PageObject* object =
      CPDFPageObjectFromFPDFPageObject(FPDFText_GetTextObject(text_page, i));
  const CPDF_TextObject* text_object = object ? object->AsText() : nullptr;
  return text_object ? text_object->GetFont() : nullptr;
}

// For character |i|, which has no Unicode mapping: when its font is Type 3,
// the code point of its glyph's name or, when the name means nothing, the
// character code itself. 0 for any other font, and for a value that is no
// Unicode scalar (a surrogate or past U+10FFFF).
unsigned int Type3Unicode(FPDF_TEXTPAGE text_page, int i) {
  RetainPtr<CPDF_Font> font = FontOf(text_page, i);
  const CPDF_Type3Font* type3 = font ? font->AsType3Font() : nullptr;
  if (!type3) {
    return 0;
  }
  const uint32_t char_code =
      CPDFTextPageFromFPDFTextPage(text_page)->GetCharInfo(i).char_code();
  const char* name = type3->GetCharProcName(char_code);
  const wchar_t from_name = name ? UnicodeFromAdobeName(name) : 0;
  const uint32_t value =
      from_name ? static_cast<uint32_t>(from_name) : char_code;
  if ((value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
    return 0;
  }
  return value;
}

// Reads |text_page| into |t|. False when the page has more fonts than an
// index can number.
bool Fill(FPDF_TEXTPAGE text_page, InkText* t) {
  int count = FPDFText_CountChars(text_page);
  if (count > kMaxChars) {
    count = kMaxChars;
    t->truncated = true;
  }
  if (count <= 0) {
    t->font_table = "[]";
    return true;
  }
  t->codes.reserve(count);
  t->geometry.reserve(static_cast<size_t>(count) * kGeometryPerChar);
  t->flags.reserve(count);
  t->fonts.reserve(count);

  std::map<const CPDF_Font*, unsigned short> font_index;
  std::vector<RetainPtr<CPDF_Font>> fonts;
  // A generated character takes the font and size of the character before
  // it; one before any character with a font takes those of the first such
  // character, which is font 0.
  unsigned short last_font = 0;
  float last_size = 0;
  for (int i = 0; i < count; ++i) {
    if (FontOf(text_page, i)) {
      last_size = static_cast<float>(FPDFText_GetFontSize(text_page, i));
      break;
    }
  }
  for (int i = 0; i < count; ++i) {
    unsigned char flags = 0;
    if (FPDFText_IsGenerated(text_page, i) == 1) {
      flags |= kGenerated;
    }
    if (FPDFText_IsHyphen(text_page, i) == 1) {
      flags |= kHyphen;
    }
    unsigned int code = FPDFText_GetUnicode(text_page, i);
    if (FPDFText_HasUnicodeMapError(text_page, i) == 1) {
      code = Type3Unicode(text_page, i);
      if (code < kFirstPrintable) {
        flags |= kNoUnicode;
        code = 0;
      }
    }
    t->flags.push_back(flags);
    t->codes.push_back(code);

    double left = 0;
    double right = 0;
    double bottom = 0;
    double top = 0;
    FPDFText_GetCharBox(text_page, i, &left, &right, &bottom, &top);
    FS_RECTF loose = {0, 0, 0, 0};
    FPDFText_GetLooseCharBox(text_page, i, &loose);
    double x = 0;
    double y = 0;
    FPDFText_GetCharOrigin(text_page, i, &x, &y);
    FS_MATRIX matrix = {0, 0, 0, 0, 0, 0};
    FPDFText_GetMatrix(text_page, i, &matrix);

    RetainPtr<CPDF_Font> font = FontOf(text_page, i);
    if (font) {
      auto it = font_index.find(font.Get());
      if (it == font_index.end()) {
        if (fonts.size() >= kMaxFonts) {
          return false;
        }
        it = font_index
                 .emplace(font.Get(), static_cast<unsigned short>(fonts.size()))
                 .first;
        fonts.push_back(font);
      }
      last_font = it->second;
      last_size = static_cast<float>(FPDFText_GetFontSize(text_page, i));
    }
    t->fonts.push_back(last_font);

    const float geometry[kGeometryPerChar] = {static_cast<float>(left),
                                              static_cast<float>(bottom),
                                              static_cast<float>(right),
                                              static_cast<float>(top),
                                              loose.left,
                                              loose.bottom,
                                              loose.right,
                                              loose.top,
                                              static_cast<float>(x),
                                              static_cast<float>(y),
                                              last_size,
                                              matrix.a,
                                              matrix.b,
                                              matrix.c,
                                              matrix.d};
    t->geometry.insert(t->geometry.end(), geometry,
                       geometry + kGeometryPerChar);
  }

  t->font_table.push_back('[');
  for (size_t i = 0; i < fonts.size(); ++i) {
    if (i > 0) {
      t->font_table.push_back(',');
    }
    AppendFont(&t->font_table, fonts[i].Get());
  }
  t->font_table.push_back(']');
  return true;
}

}  // namespace

extern "C" {

InkText* INK_Text_Page(FPDF_DOCUMENT doc, int page_index) {
  if (!doc || page_index < 0 || page_index >= FPDF_GetPageCount(doc)) {
    return nullptr;
  }
  // Loaded as if /Rotate were 0: PDFium orders a page's text in display
  // space, so on a turned page it would read across the lines. Boxes and
  // origins are in user space either way.
  FPDF_PAGE page = EPDF_LoadPageNormalized(doc, page_index, nullptr);
  if (!page) {
    return nullptr;
  }
  FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
  if (!text_page) {
    FPDF_ClosePage(page);
    return nullptr;
  }
  auto t = std::make_unique<InkText>();
  const bool filled = Fill(text_page, t.get());
  FPDFText_ClosePage(text_page);
  FPDF_ClosePage(page);
  return filled ? t.release() : nullptr;
}

int INK_Text_Count(const InkText* t) {
  return t ? static_cast<int>(t->codes.size()) : 0;
}

const unsigned int* INK_Text_Codes(const InkText* t) {
  return t ? t->codes.data() : nullptr;
}

const float* INK_Text_Geometry(const InkText* t) {
  return t ? t->geometry.data() : nullptr;
}

const unsigned char* INK_Text_Flags(const InkText* t) {
  return t ? t->flags.data() : nullptr;
}

const unsigned short* INK_Text_Fonts(const InkText* t) {
  return t ? t->fonts.data() : nullptr;
}

const char* INK_Text_FontTable(const InkText* t) {
  return t ? t->font_table.c_str() : nullptr;
}

int INK_Text_Truncated(const InkText* t) {
  return t && t->truncated ? 1 : 0;
}

void INK_Text_Free(InkText* t) {
  delete t;
}

}  // extern "C"
