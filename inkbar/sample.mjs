// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
//
// Writes inkbar/out/sample.pdf: hello_world.pdf with one annotation of each
// kind Inkbar writes, each with a hand-written appearance that differs on
// purpose from what a viewer would draw from the annotation's own keys. A
// viewer that shows the hand-written look kept the appearance; one that shows
// the plain look redrew it. Run after inkbar/build.sh: node inkbar/sample.mjs
import assert from 'node:assert/strict'
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import createModule from './out/inkbar-pdfium.mjs'

const root = new URL('../', import.meta.url)
const input = fileURLToPath(new URL('testing/resources/hello_world.pdf', root))
const output = fileURLToPath(new URL('out/sample.pdf', import.meta.url))

const ANNOT = { TEXT: 1, FREETEXT: 3, SQUARE: 5, HIGHLIGHT: 9, STAMP: 13, INK: 15 }
const COLOR = 0
const PRINT = 4
const HELVETICA = 4

const m = await createModule()

function heapBytes(bytes) {
  const ptr = m._malloc(Math.max(bytes.length, 1))
  m.HEAPU8.set(bytes, ptr)
  return ptr
}

// Calls fn with each argument copied into the heap, then frees them.
function withHeap(args, fn) {
  const ptrs = args.map(heapBytes)
  try {
    return fn(...ptrs)
  } finally {
    ptrs.forEach((ptr) => m._free(ptr))
  }
}

const cString = (text) => Buffer.from(`${text}\0`, 'latin1')
const wideString = (text) => Buffer.from(`${text}\0`, 'utf16le')
const floats = (values) => Buffer.from(new Float32Array(values).buffer)

function ok(result, what) {
  assert.ok(result, `${what} failed`)
  return result
}

function appearance(doc, box, content, group = false) {
  const bytes = Buffer.from(content, 'latin1')
  return ok(
    withHeap([bytes], (ptr) => m._INK_Appearance_New(doc, ...box, ptr, bytes.length, group ? 1 : 0)),
    'INK_Appearance_New',
  )
}

const addFont = (a, name, base) =>
  ok(withHeap([cString(name), cString(base)], (n, b) => m._INK_Appearance_AddFont(a, n, b)), `font ${name}`)
const addOpacity = (a, name, fill, stroke) =>
  ok(withHeap([cString(name)], (n) => m._INK_Appearance_AddOpacity(a, n, fill, stroke)), `opacity ${name}`)
const addPage = (a, name, index) =>
  ok(withHeap([cString(name)], (n) => m._INK_Appearance_AddPage(a, n, index)), `page ${name}`)
const addForm = (a, name, child) =>
  ok(withHeap([cString(name)], (n) => m._INK_Appearance_AddForm(a, n, child)), `form ${name}`)

function annotation(page, subtype, rect, color) {
  const annot = ok(m._FPDFPage_CreateAnnot(page, subtype), `annotation ${subtype}`)
  ok(withHeap([floats(rect)], (r) => m._FPDFAnnot_SetRect(annot, r)), 'rect')
  ok(m._FPDFAnnot_SetFlags(annot, PRINT), 'flags')
  if (color) ok(m._EPDFAnnot_SetColor(annot, COLOR, ...color), 'colour')
  return annot
}

const setContents = (annot, text) =>
  ok(withHeap([cString('Contents'), wideString(text)], (k, v) => m._FPDFAnnot_SetStringValue(annot, k, v)), 'contents')
const setName = (annot, name) => ok(withHeap([cString(name)], (n) => m._EPDFAnnot_SetName(annot, n)), 'name')

function install(annot, a) {
  ok(m._INK_Annot_SetAppearance(annot, a), 'INK_Annot_SetAppearance')
  m._FPDFPage_CloseAnnot(annot)
}

// A circle as four Bezier curves, closed.
function circle(cx, cy, r) {
  const k = 0.5523 * r
  return [
    `${cx + r} ${cy} m`,
    `${cx + r} ${cy + k} ${cx + k} ${cy + r} ${cx} ${cy + r} c`,
    `${cx - k} ${cy + r} ${cx - r} ${cy + k} ${cx - r} ${cy} c`,
    `${cx - r} ${cy - k} ${cx - k} ${cy - r} ${cx} ${cy - r} c`,
    `${cx + k} ${cy - r} ${cx + r} ${cy - k} ${cx + r} ${cy} c h`,
  ].join(' ')
}

// A five-pointed star, closed.
function star(cx, cy, outer, inner) {
  const points = Array.from({ length: 10 }, (_, i) => {
    const r = i % 2 ? inner : outer
    const angle = Math.PI / 2 + (i * Math.PI) / 5
    return `${(cx + r * Math.cos(angle)).toFixed(2)} ${(cy + r * Math.sin(angle)).toFixed(2)}`
  })
  return `${points[0]} m ${points.slice(1).map((p) => `${p} l`).join(' ')} h`
}

m._INK_Init()
const bytes = readFileSync(input)
const buf = heapBytes(bytes)
const doc = ok(m._FPDF_LoadMemDocument64(buf, bytes.length, 0), 'load')
const page = ok(m._FPDF_LoadPage(doc, 0), 'page')

// Highlight over "Hello, world!". Plain look: a yellow fill. Kept: a dashed red outline, no fill.
{
  const annot = annotation(page, ANNOT.HIGHLIGHT, [16, 44, 96, 64], [255, 255, 0])
  ok(withHeap([floats([18, 61, 94, 61, 18, 46, 94, 46])], (q) => m._FPDFAnnot_AppendAttachmentPoints(annot, q)), 'quad')
  install(annot, appearance(doc, [16, 44, 96, 64], '1 0 0 RG 1.5 w [4 2] 0 d 18 46 76 15 re S'))
}

// Square. Plain look: a blue square outline. Kept: a filled green diamond.
{
  const annot = annotation(page, ANNOT.SQUARE, [10, 140, 54, 184], [0, 0, 255])
  install(annot, appearance(doc, [10, 140, 54, 184], '0 0.6 0 rg 32 142 m 52 162 l 32 182 l 12 162 l h f'))
}

// Ink. Plain look: one straight black diagonal. Kept: a thick dotted purple wave.
{
  const annot = annotation(page, ANNOT.INK, [60, 140, 124, 184], [0, 0, 0])
  ok(withHeap([floats([64, 144, 120, 180])], (p) => m._FPDFAnnot_AddInkStroke(annot, p, 2)) >= 0, 'ink stroke')
  install(
    annot,
    appearance(doc, [60, 140, 124, 184], '0.5 0 0.8 RG 3 w 1 J [0 6] 0 d 62 162 m 77 190 92 134 107 162 c 114 175 118 170 122 162 c S'),
  )
}

// FreeText. Plain look: "Plain text" in blue Helvetica. Kept: "Kept as drawn" in red Courier on a yellow box with a red border.
{
  const annot = annotation(page, ANNOT.FREETEXT, [100, 8, 194, 40])
  setContents(annot, 'Plain text')
  ok(m._EPDFAnnot_SetDefaultAppearance(annot, HELVETICA, 10, 0, 0, 255), 'default appearance')
  const a = appearance(doc, [100, 8, 194, 40], '1 1 0.6 rg 1 0 0 RG 2 w 101 9 92 30 re B BT 1 0 0 rg /F0 9 Tf 105 20 Td (Kept as drawn) Tj ET')
  addFont(a, 'F0', 'Courier')
  install(annot, a)
}

// Text (a note). Plain look: the viewer's yellow note icon. Kept: an orange star.
{
  const annot = annotation(page, ANNOT.TEXT, [10, 8, 34, 32], [255, 255, 0])
  setContents(annot, 'Note')
  install(annot, appearance(doc, [10, 8, 34, 32], `1 0.5 0 rg 0.6 0.3 0 RG 1 w ${star(22, 20, 11, 4.5)} B`))
}

// Stamp. Plain look: the viewer's "Approved" stamp, or nothing. Kept: page 1
// at twice its size, clipped to a circle with a blue rim.
{
  const annot = annotation(page, ANNOT.STAMP, [130, 130, 190, 190])
  setName(annot, 'Approved')
  const ring = circle(160, 160, 28)
  const a = appearance(
    doc,
    [130, 130, 190, 190],
    `q ${ring} W n 0.9 0.95 1 rg 130 130 60 60 re f 0 g q 2 0 0 2 60 50 cm /P0 Do Q Q 0 0.4 0.8 RG 2 w ${ring} S`,
  )
  addPage(a, 'P0', 0)
  install(annot, a)
}

// Stamp holding a transparency group at 50% opacity. Plain look: the
// viewer's "Draft" stamp, or nothing. Kept: a blue and a red square, both at
// half strength, where the red covers the blue in the overlap: one pink
// square over one light blue, no purple.
{
  const annot = annotation(page, ANNOT.STAMP, [146, 50, 196, 96])
  setName(annot, 'Draft')
  const group = appearance(doc, [146, 50, 196, 96], '0 0 1 rg 148 52 30 30 re f 1 0 0 rg 164 64 30 30 re f', true)
  const a = appearance(doc, [146, 50, 196, 96], '/GS0 gs /Fm0 Do')
  addOpacity(a, 'GS0', 0.5, 0.5)
  addForm(a, 'Fm0', group)
  install(annot, a)
}

const writer = ok(m._INK_Save(doc, 2), 'save')
const data = m._INK_WriterData(writer)
mkdirSync(fileURLToPath(new URL('out/', import.meta.url)), { recursive: true })
writeFileSync(output, m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
m._INK_WriterFree(writer)
m._FPDF_ClosePage(page)
m._FPDF_CloseDocument(doc)
m._free(buf)
m._INK_Destroy()
console.log(output)
