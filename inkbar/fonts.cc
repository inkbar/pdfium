// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// See fonts.h. The mapper's charset for a font comes from two places in
// PDFium: the code page the font loader passes (CPDF_CIDFont::LoadSubstFace
// from the font's CID set, CPDF_FaceBasedSimpleFont::SubstCodePage for a
// simple font) and
// CFX_FontMapper::FindSubstFace, which turns it into a charset and asks
// MapFont. INK_Fonts_PageNeeds follows both steps with PDFium's own CMap and
// charset tables, and MapFont and PageNeeds share KeyForCharset, so a page's
// needs cover what rendering it asks for.

#include "inkbar/fonts.h"

#include <stdint.h>
#include <string.h>

#include <array>
#include <optional>
#include <set>
#include <vector>

#include "core/fpdfapi/font/cpdf_cidfont.h"
#include "core/fpdfapi/font/cpdf_cmap.h"
#include "core/fpdfapi/font/cpdf_cmapparser.h"
#include "core/fpdfapi/font/cpdf_facebasedsimplefont.h"
#include "core/fpdfapi/font/cpdf_fontglobals.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fxcrt/fx_codepage.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxge/cfx_fontmapper.h"
#include "core/fxge/cfx_standardfont.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "public/fpdf_sysfontinfo.h"

namespace {

constexpr int kKeyCount = 5;
// Deeper nesting than PDFium draws (its form limit is 40) is not followed.
constexpr int kMaxDepth = 64;

std::array<std::vector<uint8_t>, kKeyCount>& Registered() {
  static auto* fonts = new std::array<std::vector<uint8_t>, kKeyCount>;
  return *fonts;
}

int g_misses = 0;

// The slot of a key of one bit, or -1.
int SlotOf(int key_bit) {
  for (int i = 0; i < kKeyCount; ++i) {
    if (key_bit == 1 << i) {
      return i;
    }
  }
  return -1;
}

int KeyForCharset(int charset) {
  switch (charset) {
    case FXFONT_SHIFTJIS_CHARSET:
      return 1;
    case FXFONT_GB2312_CHARSET:
      return 2;
    case FXFONT_CHINESEBIG5_CHARSET:
      return 4;
    case FXFONT_HANGEUL_CHARSET:
      return 8;
    case FXFONT_ANSI_CHARSET:
    case FXFONT_DEFAULT_CHARSET:
    case FXFONT_SYMBOL_CHARSET:
      return 0;
    default:
      return 16;
  }
}

// FPDF_SYSFONTINFO. A font handle points at its slot's bytes.

void* MapFont(FPDF_SYSFONTINFO*, int, FPDF_BOOL, int charset, int,
              const char*, FPDF_BOOL* exact) {
  if (exact) {
    *exact = false;
  }
  const int key = KeyForCharset(charset);
  if (!key) {
    return nullptr;
  }
  std::vector<uint8_t>& font = Registered()[SlotOf(key)];
  if (font.empty()) {
    g_misses |= key;
    return nullptr;
  }
  return &font;
}

unsigned long GetFontData(FPDF_SYSFONTINFO*, void* handle, unsigned int table,
                          unsigned char* buffer, unsigned long buf_size) {
  // Only the whole file: a registered font is never a collection.
  if (!handle || table != 0) {
    return 0;
  }
  const auto* font = static_cast<const std::vector<uint8_t>*>(handle);
  if (buffer && buf_size >= font->size()) {
    memcpy(buffer, font->data(), font->size());
  }
  return static_cast<unsigned long>(font->size());
}

// One name per key, so every font the mapper sends to a key shares one
// cached face, whatever the file calls the font.
unsigned long GetFaceName(FPDF_SYSFONTINFO*, void* handle, char* buffer,
                          unsigned long buf_size) {
  if (!handle) {
    return 0;
  }
  const auto* font = static_cast<const std::vector<uint8_t>*>(handle);
  const int slot = static_cast<int>(font - Registered().data());
  char name[] = "InkbarFallback0";
  name[sizeof(name) - 2] = static_cast<char>('0' + slot);
  if (buffer && buf_size >= sizeof(name)) {
    memcpy(buffer, name, sizeof(name));
  }
  return sizeof(name);
}

void DeleteFont(FPDF_SYSFONTINFO*, void*) {}

FPDF_SYSFONTINFO g_font_info = {
    /*version=*/1,
    /*Release=*/nullptr,
    /*EnumFonts=*/nullptr,
    MapFont,
    /*GetFont=*/nullptr,
    GetFontData,
    GetFaceName,
    /*GetFontCharset=*/nullptr,
    DeleteFont,
};

// The code page CPDF_CIDFont::LoadSubstFace passes for a CID set.
FX_CodePage CodePageOf(CIDSet set) {
  switch (set) {
    case CIDSet::kGB1:
      return FX_CodePage::kChineseSimplified;
    case CIDSet::kCNS1:
      return FX_CodePage::kChineseTraditional;
    case CIDSet::kJapan1:
      return FX_CodePage::kShiftJIS;
    case CIDSet::kKorea1:
      return FX_CodePage::kHangul;
    case CIDSet::kUnicode:
      return FX_CodePage::kUTF16LE;
    default:
      return FX_CodePage::kDefANSI;
  }
}

// The key the mapper asks for when CFX_FontMapper::FindSubstFace substitutes
// |name| with |code_page|. Symbol and ZapfDingbats draw from built-in faces
// before it asks. kDefANSI gives ANSI or Symbol, which it never asks for.
int KeyForSubstitute(const ByteString& base_font,
                     bool is_truetype,
                     FX_CodePage code_page) {
  const ByteString name = CFX_FontMapper::GetSubstName(base_font, is_truetype);
  if ((name == "Symbol" && !is_truetype) || name == "ZapfDingbats") {
    return 0;
  }
  if (code_page == FX_CodePage::kDefANSI) {
    return 0;
  }
  return KeyForCharset(static_cast<int>(FX_GetCharsetFromCodePage(code_page)));
}

// As CPDF_Font::LoadFontDescriptor finds a font file.
bool Embedded(const CPDF_Dictionary* descriptor) {
  return descriptor && (descriptor->GetStreamFor("FontFile") ||
                        descriptor->GetStreamFor("FontFile2") ||
                        descriptor->GetStreamFor("FontFile3"));
}

// The first four bytes of the GBK names CPDF_Font::Create loads as a GB1
// CID font when a TrueType font has no FontFile2.
bool IsChineseFontName(const ByteString& base_font) {
  static const char* const kNames[] = {
      "\xCB\xCE\xCC\xE5", "\xBF\xAC\xCC\xE5", "\xBA\xDA\xCC\xE5",
      "\xB7\xC2\xCB\xCE", "\xD0\xC2\xCB\xCE",
  };
  const ByteString tag = base_font.First(4);
  for (const char* name : kNames) {
    if (tag == name) {
      return true;
    }
  }
  return false;
}

// The key a Type 0 font needs, as CPDF_CIDFont::Load decides its CID set: the
// CMap's, else the descendant's CIDSystemInfo ordering. 0 when PDFium would
// not load the font or it is embedded.
int Type0Key(const CPDF_Dictionary* font) {
  RetainPtr<const CPDF_Array> descendants = font->GetArrayFor("DescendantFonts");
  if (!descendants || descendants->size() != 1) {
    return 0;
  }
  RetainPtr<const CPDF_Dictionary> cid = descendants->GetDictAt(0);
  if (!cid) {
    return 0;
  }
  RetainPtr<const CPDF_Object> encoding = font->GetDirectObjectFor("Encoding");
  if (!encoding || (!encoding->IsName() && !encoding->IsStream())) {
    return 0;
  }
  if (Embedded(cid->GetDictFor("FontDescriptor").Get())) {
    return 0;
  }
  CIDSet set = CIDSet::kUnknown;
  if (const CPDF_Stream* stream = encoding->AsStream()) {
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
    acc->LoadAllDataFiltered();
    set = pdfium::MakeRetain<CPDF_CMap>(acc->GetSpan())->GetCharset();
  } else {
    set = CPDF_FontGlobals::GetInstance()
              ->GetPredefinedCMap(encoding->GetString())
              ->GetCharset();
  }
  if (set == CIDSet::kUnknown) {
    RetainPtr<const CPDF_Dictionary> info = cid->GetDictFor("CIDSystemInfo");
    if (info) {
      set = CPDF_CMapParser::CharsetFromOrdering(
          info->GetByteStringFor("Ordering").AsStringView());
    }
  }
  return KeyForSubstitute(cid->GetByteStringFor("BaseFont"),
                          cid->GetByteStringFor("Subtype") != "CIDFontType0",
                          CodePageOf(set));
}

// Walks resources for fonts, once per object.
class NeedsWalker {
 public:
  int keys() const { return keys_; }

  void Resources(const CPDF_Dictionary* resources, int depth) {
    if (!resources || depth > kMaxDepth ||
        !seen_.insert(pdfium::WrapRetain(resources)).second) {
      return;
    }
    ForEach(resources->GetDictFor("Font").Get(), [&](const CPDF_Object* v) {
      if (const CPDF_Dictionary* font = v->AsDictionary()) {
        Font(font, depth);
      }
    });
    ForEach(resources->GetDictFor("XObject").Get(), [&](const CPDF_Object* v) {
      const CPDF_Stream* form = v->AsStream();
      if (form && form->GetDict()->GetNameFor("Subtype") == "Form") {
        Stream(form, depth);
      }
    });
    // Tiling patterns are streams; shading patterns hold no resources.
    ForEach(resources->GetDictFor("Pattern").Get(), [&](const CPDF_Object* v) {
      if (const CPDF_Stream* pattern = v->AsStream()) {
        Stream(pattern, depth);
      }
    });
    ForEach(resources->GetDictFor("ExtGState").Get(),
            [&](const CPDF_Object* v) {
              const CPDF_Dictionary* state = v->AsDictionary();
              RetainPtr<const CPDF_Dictionary> mask =
                  state ? state->GetDictFor("SMask") : nullptr;
              RetainPtr<const CPDF_Stream> group =
                  mask ? mask->GetStreamFor("G") : nullptr;
              if (group) {
                Stream(group.Get(), depth);
              }
            });
  }

  void Stream(const CPDF_Stream* stream, int depth) {
    Resources(stream->GetDict()->GetDictFor("Resources").Get(), depth + 1);
  }

 private:
  template <typename Fn>
  static void ForEach(const CPDF_Dictionary* dict, Fn fn) {
    if (!dict) {
      return;
    }
    CPDF_DictionaryLocker locker(dict);
    for (const auto& [name, value] : locker) {
      RetainPtr<const CPDF_Object> direct = value->GetDirect();
      if (direct) {
        fn(direct.Get());
      }
    }
  }

  void Font(const CPDF_Dictionary* font, int depth) {
    if (!seen_.insert(pdfium::WrapRetain(font)).second) {
      return;
    }
    const ByteString subtype = font->GetByteStringFor("Subtype");
    if (subtype == "Type3") {
      Resources(font->GetDictFor("Resources").Get(), depth + 1);
    } else if (subtype == "Type0") {
      keys_ |= Type0Key(font);
    } else {
      SimpleFont(font);
    }
  }

  // A simple font, or a TrueType font with one of PDFium's Chinese names,
  // which it loads as a GB1 CID font (CPDF_CIDFont::LoadGB2312) unless it
  // has a FontFile2.
  void SimpleFont(const CPDF_Dictionary* font) {
    const bool truetype = font->GetByteStringFor("Subtype") == "TrueType";
    RetainPtr<const CPDF_Dictionary> descriptor =
        font->GetDictFor("FontDescriptor");
    ByteString name = font->GetByteStringFor("BaseFont");
    if (truetype && IsChineseFontName(name) &&
        !(descriptor && descriptor->KeyExist("FontFile2"))) {
      if (!Embedded(descriptor.Get())) {
        keys_ |= KeyForSubstitute(name, /*is_truetype=*/true,
                                  FX_CodePage::kChineseSimplified);
      }
      return;
    }
    if (Embedded(descriptor.Get())) {
      return;
    }
    // CPDF_Type1Font substitutes a standard font by its canonical name.
    std::optional<CFX_StandardFont::Index> standard =
        truetype ? std::nullopt : CFX_StandardFont::GetStandardFontIndex(name);
    if (standard.has_value()) {
      name = CFX_StandardFont::GetCanonicalFontName(standard.value());
    }
    keys_ |= KeyForSubstitute(name, truetype,
                              CPDF_FaceBasedSimpleFont::SubstCodePage(
                                  font, name, truetype));
  }

  // Held, so an address is not reused while the walk runs.
  std::set<RetainPtr<const CPDF_Object>> seen_;
  int keys_ = 0;
};

// The page's /Resources, inherited from the page tree when it has none.
const CPDF_Dictionary* PageResources(const CPDF_Dictionary* page) {
  for (int level = 0; page && level < kMaxDepth; ++level) {
    RetainPtr<const CPDF_Dictionary> resources = page->GetDictFor("Resources");
    if (resources) {
      return resources.Get();
    }
    page = page->GetDictFor("Parent").Get();
  }
  return nullptr;
}

}  // namespace

extern "C" {

void INK_Fonts_Install(void) {
  FPDF_SetSystemFontInfo(&g_font_info);
}

int INK_Fonts_Register(int key_bit, const unsigned char* data, size_t len) {
  const int slot = SlotOf(key_bit);
  if (slot < 0 || !data || !len) {
    return 0;
  }
  Registered()[slot].assign(data, data + len);
  return 1;
}

int INK_Fonts_PageNeeds(FPDF_DOCUMENT doc, int page_index) {
  ScopedFPDFDocumentView view(doc);
  CPDF_Document* pdf = view.Get();
  if (!pdf || page_index < 0 || page_index >= pdf->GetPageCount()) {
    return 0;
  }
  RetainPtr<const CPDF_Dictionary> page = pdf->GetPageDictionary(page_index);
  if (!page) {
    return 0;
  }
  NeedsWalker walker;
  walker.Resources(PageResources(page.Get()), 0);
  RetainPtr<const CPDF_Array> annots = page->GetArrayFor("Annots");
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<const CPDF_Dictionary> annot = annots->GetDictAt(i);
    RetainPtr<const CPDF_Dictionary> ap =
        annot ? annot->GetDictFor("AP") : nullptr;
    RetainPtr<const CPDF_Object> normal =
        ap ? ap->GetDirectObjectFor("N") : nullptr;
    if (!normal) {
      continue;
    }
    if (const CPDF_Stream* stream = normal->AsStream()) {
      walker.Stream(stream, 0);
    } else if (const CPDF_Dictionary* states = normal->AsDictionary()) {
      CPDF_DictionaryLocker locker(states);
      for (const auto& [name, value] : locker) {
        RetainPtr<const CPDF_Stream> stream = ToStream(value->GetDirect());
        if (stream) {
          walker.Stream(stream.Get(), 0);
        }
      }
    }
  }
  return walker.keys();
}

int INK_Fonts_TakeMisses(void) {
  const int misses = g_misses;
  g_misses = 0;
  return misses;
}

}  // extern "C"
