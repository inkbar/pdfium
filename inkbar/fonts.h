// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Fallback fonts for text whose font a file does not embed. A caller
// registers a font file for a key, and PDFium's font mapper is served it when
// it asks for that key's charset. A key is one bit:
//
//   1  Japanese             (Shift-JIS charset)
//   2  Simplified Chinese   (GB2312 charset)
//   4  Traditional Chinese  (Big5 charset)
//   8  Korean               (Hangul charset)
//   16 default              (any charset but ANSI, Symbol, default and the four above)
//
// PDFium draws ANSI, Symbol and default charsets from its built-in faces and
// never asks for them. In page rendering it asks for the four CJK charsets
// for Type 0 fonts, and for the Cyrillic or Greek charset (key 16) for a
// simple font whose /Differences name a letter of that script
// (CPDF_FaceBasedSimpleFont::SubstCodePage). Every other simple font is mapped
// as ANSI or Symbol. A Type 0 font with an ordering PDFium does not know is
// mapped as ANSI: no key draws it.
//
// The mapper asks when a document loads a font, and PDFium keeps each loaded
// font with its document. Fonts registered after a document drew a page reach
// that page only once the document is loaded again.

#ifndef INKBAR_FONTS_H_
#define INKBAR_FONTS_H_

#include <stddef.h>

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Serves |len| bytes of a font file (TrueType or OpenType, not a collection)
// for |key_bit|, replacing any font registered for it. Copies the bytes.
// Returns 1 on success, 0 for a key that is not one of the five bits or for
// NULL or empty data. Registrations outlive INK_Destroy and INK_Init.
int INK_Fonts_Register(int key_bit, const unsigned char* data, size_t len);

// The keys page |page_index| of |doc| needs for its unembedded fonts,
// registered or not: the fonts of the page's resources, of the forms, tiling
// patterns, soft mask groups and Type 3 fonts they use, at any depth, and of
// every normal appearance of the page's annotations. A font counts as
// unembedded when its descriptor, or for a Type 0 font its descendant's, has
// no FontFile, FontFile2 or FontFile3 stream. Each font is classified as
// PDFium's font loader and mapper would ask for it. Reads the file without
// loading any font. Returns 0 for a NULL document or no such page.
int INK_Fonts_PageNeeds(FPDF_DOCUMENT doc, int page_index);

// The keys the font mapper asked for since the last call and found
// unregistered. Clears the log.
int INK_Fonts_TakeMisses(void);

// Installs the font mapper's source of registered fonts. INK_Init calls it
// after starting the library. Not exported.
void INK_Fonts_Install(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_FONTS_H_
