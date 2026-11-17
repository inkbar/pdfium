// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Installs a caller-drawn appearance stream on an annotation, and stores
// Inkbar's annotation data beside it.
//
// An INK_APPEARANCE is a Form XObject being built. INK_Appearance_New starts
// one; the Add functions name resources its content uses;
// INK_Annot_SetAppearance installs it as an annotation's normal appearance, or
// INK_Appearance_AddForm nests it in another. Install, nest or free every
// appearance before closing its document. Every int result is 1 for success
// and 0 for failure.
//
// Resource names must match [A-Za-z0-9]+ and base font names [A-Za-z0-9-]+;
// the Add functions fail for any other name.
//
// Nothing here deletes objects. A replaced appearance, replaced data, and the
// fonts, images and page forms of an appearance freed without being installed
// stay in the document as unreferenced objects. A full save leaves them out;
// an incremental save carries them.

#ifndef INKBAR_APPEARANCE_H_
#define INKBAR_APPEARANCE_H_

#include <stddef.h>

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ink_appearance_t INK_APPEARANCE;

// A Form XObject with /BBox [left bottom right top], an identity /Matrix, the
// content Flate-compressed and an empty /Resources. A nonzero
// |transparency_group| adds /Group << /S /Transparency >>. Returns NULL for a
// NULL document, an empty or non-finite box, or NULL content with a nonzero
// length. Nothing is added to the document until the appearance is installed
// or nested.
INK_APPEARANCE* INK_Appearance_New(FPDF_DOCUMENT doc,
                                   float left,
                                   float bottom,
                                   float right,
                                   float top,
                                   const unsigned char* content,
                                   size_t content_len,
                                   int transparency_group);

// /Resources /Font /<name>: a Type1 font with /BaseFont /<base_font> and
// /Encoding /WinAnsiEncoding. The document holds one such font object per base
// font, shared by every appearance that names it.
int INK_Appearance_AddFont(INK_APPEARANCE* a,
                           const char* name,
                           const char* base_font);

// /Resources /ExtGState /<name> << /ca fill /CA stroke >>. A negative value
// leaves its key out. Fails for a value above 1 or not a number.
int INK_Appearance_AddOpacity(INK_APPEARANCE* a,
                              const char* name,
                              float fill,
                              float stroke);

// /Resources /XObject /<name>: an image XObject from PNG (|format| 0) or JPEG
// (|format| 1) bytes. A PNG's alpha becomes an /SMask. Fails when the bytes
// do not decode as the format.
int INK_Appearance_AddImage(INK_APPEARANCE* a,
                            const char* name,
                            int format,
                            const unsigned char* data,
                            size_t len);

// /Resources /XObject /<name>: a Form XObject of the page's content and
// resources, with the page box as /BBox. The document holds one per page,
// shared by every appearance that names it. It is a copy of the page as it was
// at the first INK_Appearance_AddPage for that page in that document, and does
// not follow later changes to the page.
int INK_Appearance_AddPage(INK_APPEARANCE* a, const char* name, int page_index);

// /Resources /XObject /<name>: |child|'s stream. Takes |child| whether or not
// it succeeds; do not use or free it afterwards. Fails when |child| is NULL,
// is |a| (which it then leaves alone), or belongs to another document.
int INK_Appearance_AddForm(INK_APPEARANCE* a,
                           const char* name,
                           INK_APPEARANCE* child);

// Frees an appearance that was neither installed nor nested. NULL is a no-op.
void INK_Appearance_Free(INK_APPEARANCE* a);

// Sets the annotation's /AP to << /N stream >>, replacing any /AP it had,
// removes its /AS (a stream /N has no states), and sets its /Rect to the
// appearance's box. Takes |a| whether or not it succeeds.
// Fails when the annotation belongs to another document than |a|.
int INK_Annot_SetAppearance(FPDF_ANNOTATION annot, INK_APPEARANCE* a);

// Stores |len| bytes as an indirect Flate stream under the annotation's
// /INKB_Gloss key, replacing what was there. A |len| of 0 removes the key.
int INK_Annot_SetData(FPDF_ANNOTATION annot,
                      const unsigned char* json,
                      size_t len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_APPEARANCE_H_
