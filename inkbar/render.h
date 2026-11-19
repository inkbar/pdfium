// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Draws a page in time slices, leaving out the annotations a caller lists, and
// reads a page's /UserUnit, box and rotation.
//
// An InkRender is one render of one page into one bitmap. INK_Render_Start
// begins it, INK_Render_Continue runs it a slice at a time and
// INK_Render_Close ends it, finished or not. The page and the bitmap must stay
// open until INK_Render_Close. A page holds one render at a time: Start fails
// while another InkRender on the page is open, and FPDF_RenderPageBitmap on
// the page while one is open ends that render, whose next Continue returns -1.
// Such a stale handle never touches a later render of the page: its Continue
// returns -1 and its Close only frees it.

#ifndef INKBAR_RENDER_H_
#define INKBAR_RENDER_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct InkRender InkRender;

// Draws |page| into |bitmap| as FPDF_RenderPageBitmap_Start would (|start_x|
// and |start_y| may be negative to draw a region), leaving out the
// annotations whose object numbers are in |skip|. |flags|: FPDF_ANNOT and
// EPDF_RENDER_WIDGETS as given. |skip| is read before Start returns, and a
// listed annotation is hidden only while Start lists the page's annotations:
// the document is as it was when Start returns, so a save during the render
// writes what it would have written without it. Returns NULL on failure: a
// NULL bitmap or page, a negative |skip_count|, NULL |skip| with a positive
// count, a page another render holds, or a render PDFium cannot start.
InkRender* INK_Render_Start(FPDF_BITMAP bitmap,
                            FPDF_PAGE page,
                            int start_x,
                            int start_y,
                            int size_x,
                            int size_y,
                            int rotate,
                            int flags,
                            const unsigned int* skip,
                            int skip_count);

// Runs for at most |slice_ms| milliseconds (0 or less: until the first point
// where the render can pause). Returns 1 when the page is finished, 0 when
// there is more to do, -1 on failure. A finished render returns 1 again; a
// failed one -1, as does NULL.
int INK_Render_Continue(InkRender* r, int slice_ms);

// Ends the render, finished or not, and frees it. Safe on NULL.
void INK_Render_Close(InkRender* r);

// The page's /UserUnit: 1 when absent, not a number, not finite or not
// positive, and for a NULL page.
float INK_Page_UserUnit(FPDF_PAGE page);

// Reads page |page_index|'s facts from its dictionary without loading its
// content, so no font is loaded: PDFium picks a page's substitute fonts when
// the page is loaded and keeps them for the document's life. |out| receives
// six floats: left, bottom, right and top of the page box (the rectangle
// FPDF_GetPageBoundingBox reports for the loaded page), the rotation in
// degrees (0, 90, 180 or 270: FPDFPage_GetRotation times 90) and the user unit
// (as INK_Page_UserUnit reports it). The loaded page's width and height are the
// box's, swapped when it is turned 90 or 270 degrees. Returns 1 on success, 0
// for a NULL document or |out|, a bad index or a page that cannot be read.
int INK_Page_Facts(FPDF_DOCUMENT doc, int page_index, float* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_RENDER_H_
