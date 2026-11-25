// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Reports what a file says about its form, its JavaScript and itself. Each
// report says what the file holds and decides nothing; a script is its
// source as text, and nothing runs it.

#ifndef INKBAR_FORM_H_
#define INKBAR_FORM_H_

#include "public/fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// Each report is UTF-8 JSON in a malloc'd buffer freed with INK_Json_Free, or
// NULL when there is no document. Text, names, numbers, references and the
// 16 MB cap are as annots.h describes them.
//
// Scripts = { "K": ["source", ...], "A": [...], ... }
// Reading them has a work allowance of 4,000,000 reads a report (an action, a
// /Next array entry, an /AA key, a /Parent level), and each chain, /JS and /AA
// dictionary is read once a report however many places share it; once the
// allowance is spent nothing more is read and the report says "truncated".
// The JavaScript sources of an action chain under each key, in the order the
// chain gives them (the action, then its /Next, depth first, each object once,
// at most 1,000 objects of the chain: actions, /Next arrays and any other
// entry they hold count alike). A source is cut after the last character
// whose written form ends within 4 MB; only the first 16 MB of a /JS stream
// are decoded (one FlateDecode stops there; a stream with a filter chain or
// a predictor that could decode past them, or past what the report may still
// decode, is not decoded, and its source is empty and cut), and a report
// reads and decodes at most 64 MB from /JS streams in all, a chain charged
// what its stages could produce (report.h). Each /JS is decoded once a
// report and held for it, and a report holds at most 16 MB of decoded text
// (4 bytes a character): past that, and once the report's 16 MB are spent,
// nothing more is decoded.
// Sources are charged to the 16 MB as they are written. A key whose chain
// holds no JavaScript is left out. A cut source or chain sets the entry's
// "truncated", wherever the source is reached from.

// { "kind": "none" | "acroform" | "xfa", "needAppearances": false,
//   "da": Text, "q": 0,
//   "fields": [ {
//     "name": "header.ref", "objectNumber": 9, "generation": 0,
//     "origin": "acroform" | "recovered",
//     "kind": "text" | "checkbox" | "radio" | "button" | "combo" | "list" |
//             "signature" | "unknown",
//     "flags": 0, "valueKind": "string", "value": Text,
//     "defaultValueKind": "none", "defaultValueNull": true,
//     "selectedIndices": [1, 2], "options": [ { "exportValue": "gb",
//     "displayValue": "United Kingdom" } ], "maxLength": 6, "topIndex": 0,
//     "tooltip": "Header",
//     "widgets": [ {
//       "objectNumber": 10, "generation": 0, "page": 0, "annotIndex": 17,
//       "rect": [...], "rectOk": true, "flags": 4, "hasAppearance": true,
//       "appearance": "states", "states": ["Off", "Yes"], "as": "Off",
//       "onState": "Yes", "exportValue": "Yes",
//       "borderStyle": {...}, "border": [...], "borderColor": [...],
//       "backgroundColor": [...], "rotation": 90, "caption": Text,
//       "q": 1, "tooltip": Text, "da": Text, "pmd": true,
//       "scripts": Scripts,
//       "action": { "s": "URI", "uriHex": "..." } | { "s": "Named",
//                 "name": "Print" } | { "s": "ResetForm", "fields": [Text],
//                 "refs": [[n, g]], "flags": 0 } | { "s": "Launch" | "GoToR",
//                 "fileKind": "string" | "spec" | "other", "fileHex": "...",
//                 "fileSpecFHex": "...", "fileSpecUFHex": "..." } |
//                 { "s": "<other /S>" },
//       (ResetForm: an entry of /Fields written as a reference is in "refs",
//       whatever it refers to, a text string included; one written as a
//       string is in "fields". PDF.js reads /Fields the same way.)
//       "truncated": true } ],
//     "truncated": true } ],
//   "calculationOrder": [3, -1], "calculationOrderRefs": [[11, 0], null],
//   "fieldTreeDepth": 2, "fieldTreeRevisits": false, "tooLarge": false,
//   "truncated": false }
//
// The form is read as EPDFForm_LoadModel reads it: the /AcroForm field tree
// merged by full name and joined with a sweep of every page's /Annots, so a
// widget /Fields does not reach still gives its field ("origin":
// "recovered"). PDFium loads the field tree 33 levels deep and leaves out a
// field with no name. "kind" is the form's kind as the catalog declares it;
// "needAppearances", "da" and "q" are /AcroForm's.
//
// A field: "name" its full name; "objectNumber" and "generation" absent for
// a direct dictionary; "kind" PDFium's reading of /FT and /Ff; "flags" /Ff,
// inherited. "valueKind" is /V's type, inherited: "none" (absent or null),
// "string", "name", "array" or "other", and "value" is present for the first
// three: a text string's Text, a name, or an array whose strings are Text,
// whose names are { "name": "<name>" } and whose other entries are null. A
// name the report's budget cannot take is null, and the field "truncated".
// "valueNull" is present, and true, when /V is there as null (the nearest
// dictionary along /Parent that has /V gives null); without it a "none" /V is
// absent. A /V that refers to an object the file does not hold counts as
// absent, as PDFium reads it, and the lookup goes on up /Parent; some
// readers take such a reference as null instead. "defaultValueKind",
// "defaultValue" and "defaultValueNull" are /DV's. "selectedIndices" is /I,
// read from the same dictionaries as /V (the field's, then along /Parent, the
// nearest that has it), for a field of any kind: its entries as numbers as
// written (null for one that is not a number), at most 1,000,000, or null
// when /I is not an array; absent when no dictionary has /I or it is null. It
// may disagree with /V and with /Opt; nothing reconciles them. "options" is
// /Opt of a choice field as PDFium reads it. "maxLength" is /MaxLen of a text
// field, present when it is not 0. "topIndex" is a list box's /TI. "tooltip"
// is the field dictionary's own /TU.
//
// A widget, in the order the field lists them: "page" is the index of the
// page whose /Annots holds it and "annotIndex" its place there, both -1 when
// no page holds it. Widgets with no object number cannot be told apart from
// any other direct dictionary, and are matched by the dictionary itself.
// "onState" (the name of the first state of /AP /N that is not Off) and
// "exportValue" (its /Opt entry when the field has /Opt, else onState as
// text) are a checkbox's or a radio's. "borderColor" (/MK /BC),
// "backgroundColor" (/MK /BG) are numbers as written, any length;
// "rotation" is /MK /R and "caption" /MK /CA. "q", "da" are inherited along
// /Parent; "tooltip" is the widget's own /TU and "pmd" true when the widget
// has /PMD. "scripts" holds /AA's actions by their keys, and "A" for the
// widget's own /A. Here the report applies a rule: /AA is merged along
// /Parent, each key from the nearest dictionary (the widget first) whose
// action under it holds JavaScript. That is how PDF.js reads a widget's
// actions, and the viewer's reader follows it today. "action" is the
// widget's own /A when it is a dictionary; only the first action of a chain
// is described.
// For a Launch or GoToR action, "fileKind" says what /F is (absent when /F is
// absent or null): a string, whose bytes are "fileHex", a file specification
// dictionary, whose /F and /UF strings are "fileSpecFHex" and
// "fileSpecUFHex" (each absent when that entry is not a string), or anything
// else. The bytes are the file's, unchanged; no address is built from them
// and nothing judges them. Each byte string, "uriHex" too, writes at most
// 512 KB of bytes. A Launch action's /Win, /Mac and /Unix are not read.
//
// "calculationOrder" has an entry for each entry of /AcroForm /CO: the index
// in "fields" of the field whose full name its dictionary has (as
// CPDF_InteractiveForm::GetFieldInCalculationOrder finds it), or -1;
// "calculationOrderRefs" the same
// entries as references, null for one that is not a reference.
// "fieldTreeDepth" is the deepest level of /Fields and /Kids below /Fields
// (1 for a top-level field), and "fieldTreeRevisits" whether the walk reached
// a dictionary or a /Kids array a second time (a loop, or one shared). The
// walk reads each dictionary and each /Kids array once and goes 1,000 levels
// deep.
//
// "tooLarge": the form is refused, not cut short. When the field tree holds
// more than 50,000 dictionaries, or those and the widgets the page sweep would
// add to the form (widget annotations outside the tree with /FT along
// /Parent) number more than 50,000, PDFium's form is not loaded and "fields"
// is []: its field tree takes time that grows with the square of what it
// loads, about 19 s for 50,000 fields, so no part of the list can be had
// cheaply. /CO is still reported.
//
// "truncated": a cap cut the report short. At most 50,000 fields and 50,000
// /CO entries; the fields take at most half the report, charged as they are
// written (keys 1 KB an entry, a number 16 bytes, text, names and scripts what
// they write), so /CO always fits and objects the file shares between many
// fields or widgets cannot grow one entry past the cap. A field or widget cut
// by the budget has "truncated": true and ends the list.
char* INK_Form(FPDF_DOCUMENT doc);

// { "document": [ { "name": Text, "source": "..." } ],
//   "documentActions": { "OpenAction": [...], "WC": [...], "WS": [...],
//                        "DS": [...], "WP": [...], "DP": [...] },
//   "pageActions": [ { "page": 0, "scripts": { "O": [...], "C": [...] } } ],
//   "truncated": false }
// "document" is the /Names /JavaScript name tree in the tree's order, one
// entry for each source of each name's chain. "documentActions" holds the
// catalog's /OpenAction when it is an action and /AA, as Scripts.
// "pageActions" has an entry for each page whose /AA holds JavaScript, every
// page of the file in order. The name tree is walked 1,000 levels deep, each
// node, /Names array and /Kids array once, and at most 50,000 nodes and 50,000
// names are read. Sources are charged to the 16 MB as they are read, and the
// report stops, "truncated", when it is spent.
char* INK_Scripts(FPDF_DOCUMENT doc);

// { "title": Text, "author": Text, "encrypted": true,
//   "openedAs": "user" | "owner" | "none" }
// "title" and "author" are /Info's /Title and /Author when they are strings.
// "encrypted" says whether the trailer has /Encrypt. "openedAs" is "none"
// when it has not, "owner" when the document holds owner rights (the owner
// password opened it, or EPDF_SetRuntimeOwnerPermissions gave them), else
// "user".
char* INK_Doc_Facts(FPDF_DOCUMENT doc);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // INKBAR_FORM_H_
