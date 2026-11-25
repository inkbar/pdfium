// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { constants, deflateRawSync, deflateSync, inflateSync } from 'node:zlib'
import createModule from './out/inkbar-pdfium.mjs'

const pdfPath = process.argv[2]
assert.ok(pdfPath, 'usage: node inkbar/smoke.mjs <one-page.pdf>')

const exportsUrl = new URL('./out/exports.txt', import.meta.url)
const names = readFileSync(fileURLToPath(exportsUrl), 'utf8').split('\n').filter(Boolean)
assert.equal(names.length, 112)

const m = await createModule()
for (const name of names) {
  assert.equal(typeof m[`_${name}`], 'function', `missing export ${name}`)
}
// The module exports the allowlist and nothing else. Emscripten's own runtime
// helpers start with "__" or "_emscripten_" and are not counted.
const exported = Object.keys(m)
  .filter((key) => key.startsWith('_') && typeof m[key] === 'function')
  .filter((key) => !key.startsWith('__') && !key.startsWith('_emscripten_'))
  .map((key) => key.slice(1))
  .sort()
assert.deepEqual(exported, [...names].sort())

const FPDF_ANNOT = 0x01
const EPDF_RENDER_WIDGETS = 0x8000
const FPDF_ANNOT_SQUARE = 5
const NO_INCREMENTAL = 2

// Copies bytes into the module heap. The caller frees the pointer.
function heapBytes(bytes) {
  const ptr = m._malloc(Math.max(bytes.length, 1))
  m.HEAPU8.set(bytes, ptr)
  return ptr
}

function heapString(text) {
  const size = m.lengthBytesUTF8(text) + 1
  const ptr = m._malloc(size)
  m.stringToUTF8(text, ptr, size)
  return ptr
}

function withStrings(texts, fn) {
  const ptrs = texts.map(heapString)
  try {
    return fn(...ptrs)
  } finally {
    ptrs.forEach((ptr) => m._free(ptr))
  }
}

// How many times |text| occurs in |bytes|.
function count(bytes, text) {
  return bytes.toString('latin1').split(text).length - 1
}

// The decoded data of the stream that |key| references in |bytes|.
function streamFor(bytes, key) {
  const text = bytes.toString('latin1')
  const ref = new RegExp(`/${key}\\s+(\\d+)\\s+0\\s+R`).exec(text)
  assert.ok(ref, `no reference for /${key}`)
  const obj = text.search(new RegExp(`(^|\\s)${ref[1]}\\s+0\\s+obj`))
  assert.ok(obj >= 0, `no object ${ref[1]}`)
  const start = /stream\r?\n/.exec(text.slice(obj))
  const begin = obj + start.index + start[0].length
  return inflateSync(bytes.subarray(begin, text.indexOf('endstream', begin)))
}

// /AS as a key, not as the start of a longer name.
const AS_KEY = /\/AS(?![A-Za-z0-9#._-])/

function newAppearance(doc, [left, bottom, right, top], content, group) {
  const bytes = Buffer.from(content, 'latin1')
  const ptr = heapBytes(bytes)
  const a = m._INK_Appearance_New(doc, left, bottom, right, top, ptr, bytes.length, group ? 1 : 0)
  m._free(ptr)
  assert.notEqual(a, 0, 'INK_Appearance_New failed')
  return a
}

function save(doc) {
  const writer = m._INK_Save(doc, NO_INCREMENTAL)
  assert.notEqual(writer, 0, 'save failed')
  const data = m._INK_WriterData(writer)
  const bytes = Buffer.from(m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
  m._INK_WriterFree(writer)
  return bytes
}

function load(bytes) {
  const buf = heapBytes(bytes)
  const doc = m._FPDF_LoadMemDocument64(buf, bytes.length, 0)
  assert.notEqual(doc, 0, `load failed, error ${m._FPDF_GetLastError()}`)
  return { doc, buf }
}

// Renders page 0 at one pixel per point and returns the BGRA pixel at (x, y)
// in PDF user space.
function pixelAt(doc, x, y) {
  const page = m._FPDF_LoadPage(doc, 0)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page))
  const height = Math.round(m._FPDF_GetPageHeightF(page))
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff)
  m._FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT)
  const row = height - 1 - Math.floor(y)
  const offset = m._FPDFBitmap_GetBuffer(bitmap) + row * m._FPDFBitmap_GetStride(bitmap) + Math.floor(x) * 4
  const pixel = [...m.HEAPU8.slice(offset, offset + 4)]
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return pixel
}

// A PNG of |width| x |height| 8-bit pixels: RGB (colour type 2) for 3
// |channels| or RGBA (colour type 6) for 4. |pixel(x, y)| returns the channels.
function makePng(width, height, channels, pixel) {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    return c >>> 0
  })
  const crc = (bytes) => {
    let c = 0xffffffff
    for (const byte of bytes) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8)
    return (c ^ 0xffffffff) >>> 0
  }
  const chunk = (type, data) => {
    const body = Buffer.concat([Buffer.from(type, 'latin1'), data])
    const out = Buffer.alloc(body.length + 8)
    out.writeUInt32BE(data.length, 0)
    body.copy(out, 4)
    out.writeUInt32BE(crc(body), body.length + 4)
    return out
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0)
  ihdr.writeUInt32BE(height, 4)
  ihdr.set([8, channels === 4 ? 6 : 2, 0, 0, 0], 8)
  const rows = []
  for (let y = 0; y < height; y++) {
    rows.push(0)
    for (let x = 0; x < width; x++) rows.push(...pixel(x, y))
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(Buffer.from(rows))),
    chunk('IEND', Buffer.alloc(0)),
  ])
}

// A 2x2 RGBA PNG with transparent pixels, so the image carries an /SMask.
function tinyPng() {
  const pixels = [
    [255, 0, 0, 255],
    [0, 0, 255, 0],
    [0, 255, 0, 0],
    [255, 0, 0, 255],
  ]
  return makePng(2, 2, 4, (x, y) => pixels[y * 2 + x])
}

m._INK_Init()
const original = readFileSync(pdfPath)
const { doc, buf } = load(original)
assert.equal(m._FPDF_GetPageCount(doc), 1)

const plain = save(doc)
assert.equal(plain.subarray(0, 5).toString('latin1'), '%PDF-')

// The centre of the part of the rect [100 100 300 200] that lies on the 200 pt page.
const probe = [150, 150]
const before = pixelAt(doc, ...probe)

const page = m._FPDF_LoadPage(doc, 0)
const annot = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
assert.notEqual(annot, 0)
// An appearance state that installing a stream /N must remove.
const asKey = heapString('AS')
const asValue = heapBytes(Buffer.from('Off\0', 'utf16le'))
assert.equal(m._FPDFAnnot_SetStringValue(annot, asKey, asValue), 1)
m._free(asKey)
m._free(asValue)
assert.match(save(doc).toString('latin1'), AS_KEY, 'the setup left no /AS')

const a = newAppearance(
  doc,
  [100, 100, 300, 200],
  '0.5 g /GS0 gs 100 100 200 100 re f BT /F0 12 Tf 110 150 Td (Hello) Tj ET /P0 Do /Fm0 Do',
  false,
)
withStrings(['F0', 'Helvetica'], (name, base) => assert.equal(m._INK_Appearance_AddFont(a, name, base), 1))
withStrings(['GS0'], (name) => assert.equal(m._INK_Appearance_AddOpacity(a, name, 0.5, -1), 1))
withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(a, name, 0), 1))
withStrings(['P1'], (name) => assert.equal(m._INK_Appearance_AddPage(a, name, 1), 0, 'page out of range'))
// Names are [A-Za-z0-9]+, base fonts [A-Za-z0-9-]+.
for (const bad of ['F 0', '/F0', '']) {
  withStrings([bad, 'Helvetica'], (name, base) => assert.equal(m._INK_Appearance_AddFont(a, name, base), 0, `font name "${bad}"`))
  withStrings([bad], (name) => {
    assert.equal(m._INK_Appearance_AddOpacity(a, name, 0.5, -1), 0, `opacity name "${bad}"`)
    assert.equal(m._INK_Appearance_AddPage(a, name, 0), 0, `page name "${bad}"`)
  })
}
withStrings(['F9', 'Times Roman'], (name, base) => assert.equal(m._INK_Appearance_AddFont(a, name, base), 0, 'base font with a space'))
withStrings(['F9', 'Times-Roman'], (name, base) => assert.equal(m._INK_Appearance_AddFont(a, name, base), 1, 'base font with a dash'))
const child = newAppearance(doc, [0, 0, 10, 10], '1 0 0 rg 0 0 10 10 re f', true)
// The child names the same font and page again; the document holds one of each.
withStrings(['F1', 'Helvetica'], (name, base) => assert.equal(m._INK_Appearance_AddFont(child, name, base), 1))
withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(child, name, 0), 1))
const spare = newAppearance(doc, [0, 0, 10, 10], '', false)
withStrings(['Fm 1'], (name) => assert.equal(m._INK_Appearance_AddForm(a, name, spare), 0, 'form name with a space'))
withStrings(['Fm0'], (name) => assert.equal(m._INK_Appearance_AddForm(a, name, child), 1))

const png = tinyPng()
const pngPtr = heapBytes(png)
withStrings(['Im 0'], (name) => assert.equal(m._INK_Appearance_AddImage(a, name, 0, pngPtr, png.length), 0, 'image name with a space'))
withStrings(['Im0'], (name) => {
  assert.equal(m._INK_Appearance_AddImage(a, name, 0, pngPtr, png.length), 1)
  assert.equal(m._INK_Appearance_AddImage(a, name, 1, pngPtr, png.length), 0, 'PNG bytes are not a JPEG')
  assert.equal(m._INK_Appearance_AddImage(a, name, 2, pngPtr, png.length), 0, 'unknown image format')
})
m._free(pngPtr)

assert.equal(m._INK_Annot_SetAppearance(annot, a), 1)

const json = Buffer.from('{"v":1}', 'latin1')
const jsonPtr = heapBytes(json)
assert.equal(m._INK_Annot_SetData(annot, jsonPtr, json.length), 1)
m._free(jsonPtr)

const saved = save(doc)
const savedText = saved.toString('latin1')
assert.equal(streamFor(saved, 'INKB_Gloss').toString('latin1'), '{"v":1}')
// Helvetica twice and Times-Roman once add two fonts; page 0 twice adds one form.
assert.equal(count(saved, '/WinAnsiEncoding') - count(plain, '/WinAnsiEncoding'), 2, 'one font object per base font')
assert.equal(count(saved, '/FormType') - count(plain, '/FormType'), 1, 'one Form XObject per page')
assert.equal(count(saved, '/INKB_PageCopy true'), 1, 'the page form is not tagged as a page copy')
assert.ok(savedText.includes('/SMask'), 'saved bytes lack the PNG /SMask')
assert.doesNotMatch(savedText, AS_KEY, 'installing the appearance left /AS')

assert.equal(m._INK_Annot_SetData(annot, 0, 0), 1)
assert.ok(!save(doc).toString('latin1').includes('/INKB_Gloss'), 'length 0 left /INKB_Gloss')

m._FPDFPage_CloseAnnot(annot)
m._FPDF_ClosePage(page)
m._FPDF_CloseDocument(doc)
m._free(buf)

const reopened = load(saved)
const after = pixelAt(reopened.doc, ...probe)
assert.notDeepEqual(after, before, `the pixel at ${probe} did not change: ${after}`)
// 0.5 g at fill opacity 0.5 over white: 0.5 * 0.5 + 0.5 * 1 = 0.75 of 255.
const expected = Math.round(0.75 * 255)
for (const channel of after.slice(0, 3)) {
  assert.ok(Math.abs(channel - expected) <= 3, `the pixel at ${probe} is ${after}, not grey ${expected}`)
}
m._FPDF_CloseDocument(reopened.doc)
m._free(reopened.buf)

// The text of indirect object |num| in |text|, from "obj" to "endobj".
function objectText(text, num) {
  const at = text.search(new RegExp(`(^|\\s)${num}\\s+0\\s+obj`))
  assert.ok(at >= 0, `no object ${num}`)
  return text.slice(at, text.indexOf('endobj', at))
}

// A text box in a document without a form, and a dashed square border.
const FPDF_ANNOT_FREETEXT = 3
const FPDF_FONT_HELVETICA = 4
const FPDF_ANNOT_BS_DASHED = 2
const second = load(original)
assert.ok(!original.toString('latin1').includes('/AcroForm'), 'the input already has a form')
const formPage = m._FPDF_LoadPage(second.doc, 0)
// FS_RECTF: left, top, right, bottom.
const rectPtr = m._malloc(16)
m.HEAPF32.set([20, 180, 120, 120], rectPtr / 4)
const text = m._FPDFPage_CreateAnnot(formPage, FPDF_ANNOT_FREETEXT)
assert.notEqual(text, 0)
assert.equal(m._FPDFAnnot_SetRect(text, rectPtr), 1)
assert.equal(m._EPDFAnnot_SetDefaultAppearance(text, FPDF_FONT_HELVETICA, 12, 0, 0, 0), 1)
const formSaved = save(second.doc)
const formText = formSaved.toString('latin1')
const formRef = /\/AcroForm\s+(\d+)\s+0\s+R/.exec(formText)
assert.ok(formRef, 'setting the default appearance created no /AcroForm reference')
assert.match(objectText(formText, formRef[1]), /\/Fields\s*\[/, 'the created /AcroForm has no /Fields array')

const square = m._FPDFPage_CreateAnnot(formPage, FPDF_ANNOT_SQUARE)
assert.notEqual(square, 0)
assert.equal(m._FPDFAnnot_SetRect(square, rectPtr), 1)
m._free(rectPtr)
assert.equal(m._EPDFAnnot_SetBorderStyle(square, FPDF_ANNOT_BS_DASHED, 2), 1)
const dashPtr = m._malloc(8)
m.HEAPF32.set([3, 1], dashPtr / 4)
assert.equal(m._EPDFAnnot_SetBorderDashPattern(square, dashPtr, 2), 1)
m._free(dashPtr)
const borderSaved = save(second.doc)
const squareNum = m._EPDFAnnot_GetObjectNumber(square)
assert.ok(squareNum > 0, 'the square has no object number')
const squareText = objectText(borderSaved.toString('latin1'), squareNum)
const bs = /\/BS\s*<<((?:[^<>]|<<[^<>]*>>)*)>>/.exec(squareText)
assert.ok(bs, 'the square has no /BS')
assert.match(bs[1], /\/S\s*\/D(?![A-Za-z0-9])/, `/BS ${bs[1]} is not dashed`)
assert.match(bs[1], /\/D\s*\[\s*3(\.0*)?\s+1(\.0*)?\s*\]/, `/BS ${bs[1]} has no /D [3 1]`)

// A reply points at an annotation that already exists: look it up by object
// number, link a new Text annotation to it under /IRT, and release both handles.
const FPDF_ANNOT_TEXT = 1
const target = m._FPDFPage_CreateAnnot(formPage, FPDF_ANNOT_TEXT)
assert.notEqual(target, 0)
const targetNum = m._EPDFAnnot_GetObjectNumber(target)
assert.ok(targetNum > 0, 'the target has no object number')
m._FPDFPage_CloseAnnot(target)
const found = m._EPDFPage_GetAnnotByObjectNumber(formPage, targetNum)
assert.notEqual(found, 0, 'lookup by object number found nothing')
assert.equal(m._EPDFAnnot_GetObjectNumber(found), targetNum)
const reply = m._FPDFPage_CreateAnnot(formPage, FPDF_ANNOT_TEXT)
assert.notEqual(reply, 0)
const replyNum = m._EPDFAnnot_GetObjectNumber(reply)
assert.ok(replyNum > 0 && replyNum !== targetNum, 'the reply has no distinct object number')
withStrings(['IRT'], (key) => assert.equal(m._EPDFAnnot_SetLinkedAnnot(reply, key, found), 1))
m._FPDFPage_CloseAnnot(found)
m._FPDFPage_CloseAnnot(reply)
const replySaved = save(second.doc).toString('latin1')
assert.match(
  objectText(replySaved, replyNum),
  new RegExp(`/IRT\\s+${targetNum}\\s+0\\s+R`),
  `the reply's /IRT does not refer to object ${targetNum}`,
)
// An object number that is not an annotation on this page, and one that is no object.
const pageNum = Number(/(\d+)\s+0\s+obj(?:(?!endobj)[\s\S])*?\/Type\s*\/Page(?![A-Za-z])/.exec(replySaved)?.[1])
assert.ok(pageNum > 0, 'found no page object')
assert.equal(m._EPDFPage_GetAnnotByObjectNumber(formPage, pageNum), 0, 'the page itself is not an annotation')
assert.equal(m._EPDFPage_GetAnnotByObjectNumber(formPage, 999999), 0, 'no such object')
assert.equal(m._EPDFPage_GetAnnotByObjectNumber(formPage, 0), 0, 'object number 0')
m._FPDFPage_CloseAnnot(text)
m._FPDFPage_CloseAnnot(square)
m._FPDF_ClosePage(formPage)
m._FPDF_CloseDocument(second.doc)
m._free(second.buf)

// Form reads on a document with a text box, a combo box and a list box.
const EPDF_FORMFIELD_FAMILY_TEXT = 4
const EPDF_FORMFIELD_FAMILY_COMBOBOX = 5
const EPDF_FORMFIELD_FAMILY_LISTBOX = 6
const EPDF_FORM_VALUE_NONE = 0
const EPDF_FORM_VALUE_ARRAY = 2
const formsBytes = readFileSync(new URL('../testing/resources/multiple_form_types.pdf', import.meta.url))
const forms = load(formsBytes)
const model = m._EPDFForm_LoadModel(forms.doc)
assert.notEqual(model, 0, 'no form model')
// A UTF-16LE string from a getter that takes (buffer, length) and returns the
// byte length with the trailing NUL, or 0 for none.
const utf16 = (read) => {
  const size = read(0, 0)
  if (size === 0) return ''
  const ptr = m._malloc(size)
  read(ptr, size)
  const text = new TextDecoder('utf-16le').decode(m.HEAPU8.slice(ptr, ptr + size - 2))
  m._free(ptr)
  return text
}
let sawText = false
let sawCombo = false
let sawList = false
for (let i = 0; i < m._EPDFForm_CountFields(model); i++) {
  const family = m._EPDFForm_GetFieldFamily(model, i)
  const kind = m._EPDFForm_GetFieldValueKind(model, i)
  const flags = m._EPDFForm_GetFieldFlags(model, i) >>> 0
  const options = m._EPDFForm_CountFieldOptions(model, i)
  const first = utf16((ptr, size) => m._EPDFForm_GetFieldOptionValue(model, i, 0, ptr, size))
  const name = utf16((ptr, size) => m._EPDFForm_GetFieldName(model, i, ptr, size))
  if (family === EPDF_FORMFIELD_FAMILY_TEXT) {
    sawText = true
    assert.equal(kind, EPDF_FORM_VALUE_NONE, `${name} has no /V`)
    assert.equal(options, 0, `${name} has no options`)
    assert.equal(first, '')
  } else if (family === EPDF_FORMFIELD_FAMILY_COMBOBOX || family === EPDF_FORMFIELD_FAMILY_LISTBOX) {
    assert.ok(options > 0, `${name} has no options`)
    assert.notEqual(first, '', `${name} has no first option value`)
  }
  if (name === 'Combo_Editable') {
    sawCombo = true
    assert.equal(options, 3)
    assert.equal(first, 'foo', 'the export value, not the label')
    assert.equal(flags, 393216, 'combo and edit bits of /Ff')
  }
  if (name === 'Listbox_MultiSelectMultipleSelected') {
    sawList = true
    assert.equal(kind, EPDF_FORM_VALUE_ARRAY)
    assert.equal(options, 5)
    assert.equal(first, 'Alpha')
    assert.equal(flags, 2097152, 'multi-select bit of /Ff')
  }
}
assert.ok(sawText && sawCombo && sawList, 'multiple_form_types.pdf lacks a text, combo or list field')
m._EPDFForm_CloseModel(model)
m._FPDF_CloseDocument(forms.doc)
m._free(forms.buf)

// Added images are marked /Interpolate true, image and soft mask alike, so an
// enlarged one is smoothed as Inkbar's screen draws it. Two 4x4 PNGs with a
// hard vertical edge between columns 1 and 2 are each stretched over 160 pt:
// one RGB black and white, one black with opaque and transparent alpha (an
// /SMask). A row through each is counted for distinct grey levels, in the saved
// file and in a copy with every "/Interpolate true" blanked out.

// The distinct grey levels (blue channel) in the bitmap row at |y| points, from
// |x0| to |x1| points, rendering page 0 at |scale| pixels per point.
function greyLevels(doc, scale, y, x0, x1) {
  const page = m._FPDF_LoadPage(doc, 0)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page) * scale)
  const height = Math.round(m._FPDF_GetPageHeightF(page) * scale)
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff)
  m._FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT)
  const row = m._FPDFBitmap_GetBuffer(bitmap) + (height - 1 - Math.floor(y * scale)) * m._FPDFBitmap_GetStride(bitmap)
  const levels = new Set()
  for (let x = Math.ceil(x0 * scale); x < Math.floor(x1 * scale); x++) levels.add(m.HEAPU8[row + x * 4])
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return levels.size
}

const edgeRgb = makePng(4, 4, 3, (x) => (x < 2 ? [0, 0, 0] : [255, 255, 255]))
const edgeAlpha = makePng(4, 4, 4, (x) => [0, 0, 0, x < 2 ? 255 : 0])
const third = load(original)
const imagePage = m._FPDF_LoadPage(third.doc, 0)
const imageAnnot = m._FPDFPage_CreateAnnot(imagePage, FPDF_ANNOT_SQUARE)
assert.notEqual(imageAnnot, 0)
// The page is 200 pt square. Rgb covers x 20..180, y 100..180; Alpha x 20..180, y 10..90.
const images = newAppearance(
  third.doc,
  [0, 0, 200, 200],
  'q 160 0 0 80 20 100 cm /Rgb Do Q q 160 0 0 80 20 10 cm /Alpha Do Q',
  false,
)
for (const [name, png] of [
  ['Rgb', edgeRgb],
  ['Alpha', edgeAlpha],
]) {
  const ptr = heapBytes(png)
  withStrings([name], (n) => assert.equal(m._INK_Appearance_AddImage(images, n, 0, ptr, png.length), 1))
  m._free(ptr)
}
assert.equal(m._INK_Annot_SetAppearance(imageAnnot, images), 1)
m._FPDFPage_CloseAnnot(imageAnnot)
m._FPDF_ClosePage(imagePage)
const imagesSaved = save(third.doc)
m._FPDF_CloseDocument(third.doc)
m._free(third.buf)
const imagesText = imagesSaved.toString('latin1')
assert.equal(count(imagesSaved, '/Interpolate true'), 3, 'two images and one soft mask are not all marked /Interpolate true')
const smaskRef = /\/SMask\s+(\d+)\s+0\s+R/.exec(imagesText)
assert.ok(smaskRef, 'the alpha PNG has no /SMask')
assert.match(objectText(imagesText, smaskRef[1]), /\/Interpolate\s+true/, 'the soft mask lacks /Interpolate true')
// Blanked to the same length, so the cross-reference offsets stay right.
const entry = '/Interpolate true'
const stripped = Buffer.from(imagesText.replaceAll(entry, ' '.repeat(entry.length)), 'latin1')
const SCALE = 2
const measured = {}
for (const [label, bytes] of [
  ['with', imagesSaved],
  ['without', stripped],
]) {
  const opened = load(bytes)
  measured[label] = {
    rgb: greyLevels(opened.doc, SCALE, 140, 20, 180),
    alpha: greyLevels(opened.doc, SCALE, 50, 20, 180),
  }
  m._FPDF_CloseDocument(opened.doc)
  m._free(opened.buf)
}
console.log(`grey levels across the edge at ${SCALE}x: with ${JSON.stringify(measured.with)}, without ${JSON.stringify(measured.without)}`)
// PDFium draws an enlarged image nearest-neighbour unless /Interpolate is set.
assert.deepEqual(measured.without, { rgb: 2, alpha: 2 }, 'without /Interpolate the enlarged edge is not hard')
assert.ok(measured.with.rgb > 2, 'with /Interpolate the enlarged image is not smoothed')
assert.ok(measured.with.alpha > 2, 'with /Interpolate the enlarged soft mask is not smoothed')

// A redact annotation removes the text under its quad and keeps the text beside it.
function buildTextPdf(content) {
  const stream = deflateSync(Buffer.from(content, 'latin1'))
  const objects = [
    Buffer.from('<< /Type /Catalog /Pages 2 0 R >>'),
    Buffer.from('<< /Type /Pages /Kids [3 0 R] /Count 1 >>'),
    Buffer.from('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>'),
    Buffer.concat([
      Buffer.from(`<< /Length ${stream.length} /Filter /FlateDecode >>\nstream\n`),
      stream,
      Buffer.from('\nendstream'),
    ]),
    Buffer.from('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'),
  ]
  const parts = [Buffer.from('%PDF-1.7\n')]
  const offsets = []
  let length = parts[0].length
  objects.forEach((body, i) => {
    offsets.push(length)
    const piece = Buffer.concat([Buffer.from(`${i + 1} 0 obj\n`), body, Buffer.from('\nendobj\n')])
    parts.push(piece)
    length += piece.length
  })
  const xref = [`xref\n0 ${objects.length + 1}\n`, '0000000000 65535 f \n']
  for (const offset of offsets) xref.push(`${String(offset).padStart(10, '0')} 00000 n \n`)
  xref.push(`trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${length}\n%%EOF\n`)
  parts.push(Buffer.from(xref.join('')))
  return Buffer.concat(parts)
}

// The bytes of |pdf| with every Flate stream inflated in place of its data.
function inflateStreams(pdf) {
  const text = pdf.toString('latin1')
  const out = [text]
  let from = 0
  for (;;) {
    const start = /(?<!end)stream\r?\n/g
    start.lastIndex = from
    const found = start.exec(text)
    if (!found) break
    const begin = found.index + found[0].length
    const end = text.indexOf('endstream', begin)
    if (end < 0) break
    try {
      out.push(inflateSync(pdf.subarray(begin, end)).toString('latin1'))
    } catch {
      // Not a Flate stream.
    }
    from = end
  }
  return out.join('\n')
}

// The text of every string operand in a content stream, literal (...) with its
// escapes and hex <...>, the elements of TJ arrays included, joined in order.
function shownText(content) {
  const out = []
  let i = 0
  while (i < content.length) {
    const c = content[i]
    if (c === '%') {
      while (i < content.length && content[i] !== '\n' && content[i] !== '\r') i++
    } else if (c === '(') {
      let depth = 1
      i++
      while (i < content.length && depth > 0) {
        const d = content[i++]
        if (d === '\\') {
          const e = content[i++]
          if (e === undefined) break
          const named = { n: '\n', r: '\r', t: '\t', b: '\b', f: '\f' }
          if (e in named) out.push(named[e])
          else if (/[0-7]/.test(e)) {
            let octal = e
            while (octal.length < 3 && /[0-7]/.test(content[i] ?? '')) octal += content[i++]
            out.push(String.fromCharCode(parseInt(octal, 8) & 0xff))
          } else if (e === '\r') {
            if (content[i] === '\n') i++
          } else if (e !== '\n') out.push(e)
        } else if (d === '(') {
          depth++
          out.push(d)
        } else if (d === ')') {
          if (--depth > 0) out.push(d)
        } else out.push(d)
      }
    } else if (c === '<' && content[i + 1] !== '<') {
      const end = content.indexOf('>', i)
      if (end < 0) break
      let hex = content.slice(i + 1, end).replace(/[^0-9A-Fa-f]/g, '')
      if (hex.length % 2) hex += '0'
      out.push(Buffer.from(hex, 'hex').toString('latin1'))
      i = end + 1
    } else if (c === '<') {
      i += 2
    } else i++
  }
  return out.join('')
}

// The shown text of each Flate stream in |pdf|, one string per stream.
function streamTexts(pdf) {
  const text = pdf.toString('latin1')
  const texts = []
  let from = 0
  for (;;) {
    const start = /(?<!end)stream\r?\n/g
    start.lastIndex = from
    const found = start.exec(text)
    if (!found) break
    const begin = found.index + found[0].length
    const end = text.indexOf('endstream', begin)
    if (end < 0) break
    try {
      texts.push(shownText(inflateSync(pdf.subarray(begin, end)).toString('latin1')))
    } catch {
      // Not a Flate stream.
    }
    from = end
  }
  return texts
}

const redactSource = buildTextPdf('BT /F1 20 Tf 50 100 Td (SECRET-ALPHA) Tj 200 0 Td (KEEP-BETA) Tj ET')
const redactBefore = inflateStreams(redactSource)
assert.ok(redactBefore.includes('SECRET-ALPHA') && redactBefore.includes('KEEP-BETA'), 'the setup lost its text')
assert.ok(streamTexts(redactSource).some((t) => t.includes('SECRET-ALPHA') && t.includes('KEEP-BETA')), 'the decoder missed the text')
const redactOpened = load(redactSource)
const redactPage = m._FPDF_LoadPage(redactOpened.doc, 0)
assert.notEqual(redactPage, 0)
const FPDF_ANNOT_REDACT = 28
const mark = m._FPDFPage_CreateAnnot(redactPage, FPDF_ANNOT_REDACT)
assert.notEqual(mark, 0, 'no redact annotation')
// FS_RECTF: left, top, right, bottom. FS_QUADPOINTSF: top-left, top-right, bottom-left, bottom-right.
const markRect = m._malloc(16)
// The mark covers SECRET-ALPHA (x 50 to about 203) with room to spare and ends
// well before KEEP-BETA, which starts at x 250.
m.HEAPF32.set([45, 125, 230, 95], markRect / 4)
assert.equal(m._FPDFAnnot_SetRect(mark, markRect), 1)
m._free(markRect)
const markQuad = m._malloc(32)
m.HEAPF32.set([45, 125, 230, 125, 45, 95, 230, 95], markQuad / 4)
assert.equal(m._FPDFAnnot_AppendAttachmentPoints(mark, markQuad), 1)
m._free(markQuad)
const removedPtr = m._malloc(4)
// Marks NULL with count 0 applies every REDACT annotation of the page; the last
// argument receives the number of other annotations the apply removed.
assert.equal(m._EPDFPage_ApplyRedactions(redactPage, 0, 0, removedPtr), 1, 'no redaction applied')
assert.equal(m.HEAPU32[removedPtr / 4], 0, 'the redaction removed other annotations')
m._free(removedPtr)
// epdf_redact.h: the caller closes the mark's handle after the apply, then generates the content.
m._FPDFPage_CloseAnnot(mark)
assert.equal(m._FPDFPage_GenerateContent(redactPage), 1)
m._FPDF_ClosePage(redactPage)
const redactSaved = save(redactOpened.doc)
m._FPDF_CloseDocument(redactOpened.doc)
m._free(redactOpened.buf)
const redactAfter = inflateStreams(redactSaved)
// PDFium rewrites the surviving text as a hex string, so look for both spellings.
const shows = (text, word) => text.includes(word) || text.toUpperCase().includes(Buffer.from(word, 'latin1').toString('hex').toUpperCase())
assert.ok(shows(redactAfter, 'KEEP-BETA'), 'the text beside the mark was removed')
assert.ok(!shows(redactAfter, 'SECRET-ALPHA'), 'the text under the mark survived')
const redactShown = streamTexts(redactSaved)
assert.ok(redactShown.some((t) => t.includes('KEEP-BETA')), 'the shown text lost KEEP-BETA')
for (const word of ['SECRET', 'ALPHA']) {
  assert.ok(!redactShown.some((t) => t.includes(word)), `the shown text still holds ${word}`)
}
assert.ok(!redactAfter.includes('/Redact'), 'the redact annotation survived')

// The redaction wrapper, the sanitiser and the inspector.

// A PDF of |bodies|, object n at bodies[n - 1], with /Root 1 0 R and |trailer|
// added to the trailer dictionary.
function buildPdf(bodies, trailer = '') {
  const parts = [Buffer.from('%PDF-1.7\n')]
  const offsets = []
  let length = parts[0].length
  bodies.forEach((body, i) => {
    offsets.push(length)
    const piece = Buffer.concat([Buffer.from(`${i + 1} 0 obj\n`), Buffer.from(body, 'latin1'), Buffer.from('\nendobj\n')])
    parts.push(piece)
    length += piece.length
  })
  const xref = [`xref\n0 ${bodies.length + 1}\n`, '0000000000 65535 f \n']
  for (const offset of offsets) xref.push(`${String(offset).padStart(10, '0')} 00000 n \n`)
  xref.push(`trailer\n<< /Size ${bodies.length + 1} /Root 1 0 R ${trailer} >>\nstartxref\n${length}\n%%EOF\n`)
  parts.push(Buffer.from(xref.join('')))
  return Buffer.concat(parts)
}

// A stream object: |dict| entries, |data| Flate-compressed unless |plain|.
function streamObject(dict, data, plain = false) {
  const raw = Buffer.isBuffer(data) ? data : Buffer.from(data, 'latin1')
  const body = plain ? raw : deflateSync(raw)
  return Buffer.concat([
    Buffer.from(`<< ${dict} /Length ${body.length}${plain ? '' : ' /Filter /FlateDecode'} >>\nstream\n`),
    body,
    Buffer.from('\nendstream'),
  ])
}

// One page, 400 x 200 pt unless |mediaBox| says otherwise, drawing |content| with Helvetica as /F1. Objects 1 to
// 5 are the catalog, the page tree, the page, its content and the font;
// |extra| objects follow from 6.
function onePagePdf(content, { resources = '', catalog = '', page = '', trailer = '', mediaBox = '[0 0 400 200]' } = {}, extra = []) {
  return buildPdf(
    [
      `<< /Type /Catalog /Pages 2 0 R ${catalog} >>`,
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      `<< /Type /Page /Parent 2 0 R /MediaBox ${mediaBox} /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> ${resources} >> ${page} >>`,
      streamObject('', content),
      '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
      ...extra,
    ],
    trailer,
  )
}

// [left, bottom, right, top] as a quad: x1 y1 (upper left) to x4 y4 (lower right).
const quadOf = ([l, b, r, t]) => [l, t, r, t, l, b, r, b]

function report(ptr) {
  assert.notEqual(ptr, 0, 'no report')
  let end = ptr
  while (m.HEAPU8[end]) end++
  const text = Buffer.from(m.HEAPU8.slice(ptr, end)).toString('utf8')
  m._INK_Inspect_Free(ptr)
  return JSON.parse(text)
}

// The inspector's regions for |rects| on page |index|, or its candidates when
// |rects| is empty.
function inspectPage(doc, index, rects) {
  return inspectQuads(doc, index, rects.map(quadOf))
}

// The inspector's regions for |quads| (8 numbers each) on page |index|.
function inspectQuads(doc, index, quads) {
  const ptr = m._malloc(Math.max(quads.length * 32, 4))
  m.HEAPF32.set(quads.flat(), ptr / 4)
  const result = report(m._INK_Inspect_Page(doc, index, quads.length ? ptr : 0, quads.length))
  m._free(ptr)
  return result.regions
}

function inspectFile(doc, bytes) {
  const ptr = heapBytes(bytes)
  const result = report(m._INK_Inspect_File(doc, ptr, bytes.length))
  m._free(ptr)
  return result
}

// A REDACT annotation over |rect| with the rect as its one quad.
function addMark(page, [l, b, r, t]) {
  const mark = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_REDACT)
  assert.notEqual(mark, 0, 'no redact annotation')
  const rectPtr = m._malloc(16)
  m.HEAPF32.set([l, t, r, b], rectPtr / 4)
  assert.equal(m._FPDFAnnot_SetRect(mark, rectPtr), 1)
  m._free(rectPtr)
  const quadPtr = m._malloc(32)
  m.HEAPF32.set(quadOf([l, b, r, t]), quadPtr / 4)
  assert.equal(m._FPDFAnnot_AppendAttachmentPoints(mark, quadPtr), 1)
  m._free(quadPtr)
  return mark
}

function setData(annot, json) {
  const bytes = Buffer.from(json, 'latin1')
  const ptr = heapBytes(bytes)
  assert.equal(m._INK_Annot_SetData(annot, ptr, bytes.length), 1)
  m._free(ptr)
}

function applyPage(doc, page, marks, ownLocked) {
  const marksPtr = m._malloc(Math.max(marks.length * 4, 4))
  m.HEAPU32.set(marks, marksPtr / 4)
  const out = m._malloc(8)
  const result = m._INK_Redact_ApplyPage(doc, page, marks.length ? marksPtr : 0, marks.length, ownLocked ? 1 : 0, out, out + 4)
  const removed = m.HEAPU32[out / 4]
  const left = m.HEAPU32[out / 4 + 1]
  m._free(out)
  m._free(marksPtr)
  return { result, removed, left }
}

// Loads |bytes|, runs |fn(doc)|, closes the document.
function withDoc(bytes, fn) {
  const opened = load(bytes)
  try {
    return fn(opened.doc)
  } finally {
    m._FPDF_CloseDocument(opened.doc)
    m._free(opened.buf)
  }
}

// Loads |bytes|, runs |fn(doc, page)| on page 0, saves, and returns the saved bytes.
function editPage(bytes, fn) {
  return withDoc(bytes, (doc) => {
    const page = m._FPDF_LoadPage(doc, 0)
    assert.notEqual(page, 0)
    fn(doc, page)
    m._FPDF_ClosePage(page)
    return save(doc)
  })
}

const SECRET_RECT = [45, 95, 230, 125]
const BETA_RECT = [245, 95, 360, 125]
const textContent = 'BT /F1 20 Tf 50 100 Td (SECRET-ALPHA) Tj 200 0 Td (KEEP-BETA) Tj ET'

// The raw pointer INK_Inspect_Page returns for |quads|, freed; 0 for NULL.
function inspectPointer(doc, index, quads, count = quads.length) {
  const ptr = quads ? m._malloc(Math.max(quads.length * 32, 4)) : 0
  if (quads) m.HEAPF32.set(quads.flat(), ptr / 4)
  const result = m._INK_Inspect_Page(doc, index, ptr, count)
  if (ptr) m._free(ptr)
  if (result) m._INK_Inspect_Free(result)
  return result
}

// One page whose text sits in forms nested |levels| deep.
function nestedFormsPdf(levels) {
  const forms = []
  for (let level = 1; level <= levels; level++) {
    const content = level < levels ? '/Fm Do' : 'BT /F1 20 Tf 50 100 Td (DEEP) Tj ET'
    forms.push(streamObject(`/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /Font << /F1 5 0 R >> ${level < levels ? `/XObject << /Fm ${6 + level} 0 R >>` : ''} >>`, content))
  }
  return onePagePdf('/Fm Do', { resources: '/XObject << /Fm 6 0 R >>' }, forms)
}

// returns NULL when it cannot inspect
{
  withDoc(nestedFormsPdf(40), (doc) => {
    assert.ok(inspectPage(doc, 0, [SECRET_RECT])[0].textChars > 0, 'text 40 forms deep')
  })
  withDoc(nestedFormsPdf(41), (doc) => {
    assert.equal(inspectPointer(doc, 0, [quadOf(SECRET_RECT)]), 0, 'text 41 forms deep, which PDFium does not parse')
    assert.equal(inspectPointer(doc, 0, []), 0, 'candidates over forms 41 deep')
  })
  withDoc(onePagePdf(textContent), (doc) => {
    assert.equal(inspectPointer(doc, 1, [quadOf(SECRET_RECT)]), 0, 'a page out of range')
    assert.equal(inspectPointer(doc, -1, [quadOf(SECRET_RECT)]), 0, 'a negative page index')
    assert.equal(inspectPointer(doc, 0, [[45, 125, NaN, 125, 45, 95, 230, 95]]), 0, 'a NaN in a quad')
    assert.equal(inspectPointer(doc, 0, null, 1), 0, 'a count with NULL quads')
  })
}

// a mark with enormous quads is clamped to the page
{
  const huge = `1${'0'.repeat(30)}.0`
  // A dashed line whose one dash lies off the page, running far along the
  // mark's edge.
  const source = onePagePdf(`${textContent} [1 ${huge}] 0 d -10 0.5 m ${huge} 0.5 l S`, { page: '/Annots [6 0 R]' }, [
    `<< /Type /Annot /Subtype /Redact /Rect [0 0 ${huge} ${huge}] /QuadPoints [0 ${huge} ${huge} ${huge} 0 0 ${huge} 0] >>`,
  ])
  const started = Date.now()
  const [mark] = withDoc(source, (doc) => inspectPage(doc, 0, []))
  assert.ok(Date.now() - started < 5000, 'the inspection took more than 5 s')
  assert.equal(mark.kind, 'redact-annotation')
  assert.ok(mark.textChars > 0 && Number.isFinite(mark.rect[2]), `a mark over the whole page: ${JSON.stringify(mark)}`)
}

// inspects text under a region
{
  const source = onePagePdf(textContent)
  const [before] = withDoc(source, (doc) => inspectPage(doc, 0, [SECRET_RECT]))
  assert.equal(before.kind, 'region')
  assert.deepEqual(before.rect, SECRET_RECT)
  assert.ok(before.textChars > 0, `text under the region: ${JSON.stringify(before)}`)
  assert.equal(before.paths + before.imagesNotBlank + before.patternContent, 0)
  // Only the mark carrying /INKB_Gloss is applied with own_locked; the other stays.
  let outcome
  const applied = editPage(source, (doc, page) => {
    const own = addMark(page, SECRET_RECT)
    setData(own, '{"v":1}')
    const other = addMark(page, BETA_RECT)
    const candidates = inspectPage(doc, 0, [])
    assert.equal(candidates.filter((c) => c.kind === 'redact-annotation' && c.textChars > 0).length, 2, JSON.stringify(candidates))
    m._FPDFPage_CloseAnnot(own)
    m._FPDFPage_CloseAnnot(other)
    outcome = applyPage(doc, page, [], true)
    assert.deepEqual(applyPage(doc, page, [], true), { result: 0, removed: 0, left: 1 }, 'a second apply found something')
    assert.deepEqual(applyPage(doc, page, [], false), { result: 0, removed: 0, left: 1 }, 'nothing named, nothing applied')
  })
  assert.deepEqual(outcome, { result: 1, removed: 0, left: 1 })
  withDoc(applied, (doc) => {
    const [secret, beta] = inspectPage(doc, 0, [SECRET_RECT, BETA_RECT])
    assert.equal(secret.textChars, 0, `text left under the applied mark: ${JSON.stringify(secret)}`)
    assert.ok(beta.textChars > 0, 'the mark without /INKB_Gloss was applied')
    const left = inspectPage(doc, 0, []).filter((c) => c.kind === 'redact-annotation')
    assert.equal(left.length, 1, 'the unapplied mark is not a candidate')
  })
}

// lists the quads of the page's own marks
{
  withDoc(onePagePdf(textContent), (doc) => {
    const page = m._FPDF_LoadPage(doc, 0)
    const twoQuads = addMark(page, [45, 95, 100, 125])
    const second = m._malloc(32)
    m.HEAPF32.set(quadOf([110, 95, 230, 125]), second / 4)
    assert.equal(m._FPDFAnnot_AppendAttachmentPoints(twoQuads, second), 1)
    m._free(second)
    setData(twoQuads, '{"v":1}')
    const foreign = addMark(page, [245, 95, 360, 125])
    const rectOnly = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_REDACT)
    const rect = m._malloc(16)
    m.HEAPF32.set([10, 40, 30, 20], rect / 4)
    assert.equal(m._FPDFAnnot_SetRect(rectOnly, rect), 1)
    m._free(rect)
    setData(rectOnly, '{"v":1}')
    assert.equal(m._INK_Redact_OwnLockedQuads(doc, page, 0, 0), 3, 'the count-only call')
    const out = m._malloc(3 * 32)
    assert.equal(m._INK_Redact_OwnLockedQuads(doc, page, out, 3), 3)
    const quads = [...m.HEAPF32.slice(out / 4, out / 4 + 24)]
    assert.deepEqual(quads, [...quadOf([45, 95, 100, 125]), ...quadOf([110, 95, 230, 125]), ...quadOf([10, 20, 30, 40])])
    assert.equal(m._INK_Redact_OwnLockedQuads(doc, page, out, 1), 3, 'a short buffer still gets the total')
    // 2^29 + 1 quads of 8 floats would wrap a 32-bit size.
    assert.equal(m._INK_Redact_OwnLockedQuads(doc, page, out, 2 ** 29 + 1), -1, 'a capacity past 2^29 quads')
    assert.deepEqual([...m.HEAPF32.slice(out / 4, out / 4 + 8)], quadOf([45, 95, 100, 125]))
    m._free(out)
    ;[twoQuads, foreign, rectOnly].forEach((a) => m._FPDFPage_CloseAnnot(a))
    m._FPDF_ClosePage(page)
  })
}

// tags the overlay of an own mark that has none of its own
{
  const [l, b, r, t] = SECRET_RECT
  const source = onePagePdf(textContent, { page: '/Annots [6 0 R]' }, [
    `<< /Type /Annot /Subtype /Redact /Rect [${l} ${b} ${r} ${t}] /QuadPoints [${quadOf(SECRET_RECT).join(' ')}] /IC [0 0 0] /INKB_Gloss 7 0 R >>`,
    streamObject('', '{"v":1}'),
  ])
  let quads
  const applied = editPage(source, (doc, page) => {
    const out = m._malloc(32)
    assert.equal(m._INK_Redact_OwnLockedQuads(doc, page, out, 1), 1)
    quads = [[...m.HEAPF32.slice(out / 4, out / 4 + 8)]]
    m._free(out)
    assert.equal(applyPage(doc, page, [], true).result, 1)
  })
  assert.equal(count(applied, '/INKB_Overlay true'), 1, 'the synthesised overlay is not tagged')
  const [after] = withDoc(applied, (doc) => inspectQuads(doc, 0, quads))
  assert.deepEqual(
    [after.textChars, after.imagesNotBlank, after.paths, after.patternContent, after.annotationText, after.annotationImages],
    [0, 0, 0, 0, 0, 0],
    `under the own mark after the apply: ${JSON.stringify(after)}`,
  )
}

// inspects an image under a region
{
  const pixels = Buffer.alloc(16 * 16 * 3)
  for (let i = 0; i < 256; i++) pixels.set([(i % 16) * 16, Math.floor(i / 16) * 16, 128], i * 3)
  const source = onePagePdf(
    'q 100 0 0 100 50 50 cm /Im0 Do Q',
    { resources: '/XObject << /Im0 6 0 R >>' },
    [streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', pixels)],
  )
  const under = [60, 60, 100, 100]
  const beside = [120, 120, 140, 140]
  const [before] = withDoc(source, (doc) => inspectPage(doc, 0, [under]))
  assert.equal(before.imagesNotBlank, 1, `image under the region: ${JSON.stringify(before)}`)
  const applied = editPage(source, (doc, page) => {
    const mark = addMark(page, under)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
  })
  withDoc(applied, (doc) => {
    const [after, elsewhere] = inspectPage(doc, 0, [under, beside])
    assert.equal(after.imagesNotBlank, 0, `image samples left under the mark: ${JSON.stringify(after)}`)
    assert.equal(elsewhere.imagesNotBlank, 1, 'the image beside the mark reads as blank')
  })
}

// inspects an inline image, a JPEG and text in a nested form under a region
{
  const inline = Buffer.alloc(4 * 4 * 3)
  for (let i = 0; i < 16; i++) inline.set([i * 16, 255 - i * 16, 64], i * 3)
  const jpeg = readFileSync(new URL('../testing/resources/mona_lisa.jpg', import.meta.url))
  const content = Buffer.concat([
    Buffer.from('q 100 0 0 100 20 50 cm BI /W 4 /H 4 /CS /RGB /BPC 8 ID '),
    inline,
    Buffer.from(' EI Q q 120 0 0 120 140 40 cm /Jp Do Q /Fm0 Do'),
  ])
  const source = onePagePdf(
    content,
    { resources: '/XObject << /Jp 6 0 R /Fm0 7 0 R >>' },
    [
      streamObject('/Type /XObject /Subtype /Image /Width 120 /Height 120 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode', jpeg, true),
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /XObject << /Fm1 8 0 R >> >>', '/Fm1 Do'),
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 10 Tf 300 100 Td (NESTED) Tj ET'),
    ],
  )
  const regions = [
    [40, 70, 80, 110],
    [170, 70, 230, 130],
    [295, 95, 345, 115],
  ]
  const before = withDoc(source, (doc) => inspectPage(doc, 0, regions))
  assert.deepEqual(
    [before[0].imagesNotBlank, before[1].imagesNotBlank, before[2].textChars > 0],
    [1, 1, true],
    `content under the regions: ${JSON.stringify(before)}`,
  )
  const applied = editPage(source, (doc, page) => {
    const marks = regions.map((r) => addMark(page, r))
    assert.equal(applyPage(doc, page, marks, false).result, 1)
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  assert.ok(applied.toString('latin1').includes('/DCTDecode'), 'the JPEG is no longer a JPEG')
  const after = withDoc(applied, (doc) => inspectPage(doc, 0, regions))
  assert.deepEqual(
    after.map((r) => [r.imagesNotBlank, r.textChars]),
    [
      [0, 0],
      [0, 0],
      [0, 0],
    ],
    `content left under the marks: ${JSON.stringify(after)}`,
  )
}

// holds a JPEG to the tight tolerance outside the blocks a region's edge crosses
{
  // 64 x 64 gray, 4:4:4: vertical stripes 4 samples wide of 255 and 215.
  const faint = Buffer.from(
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/wAALCABAAEABAREA/8QAFQABAQAAAAAAAAAAAAAAAAAAAAn/xAAYEAACAwAAAAAAAAAAAAAAAAAACEWEw//aAAgBAQAAPwC1jIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnIMjC2cgyMLZyDIwtnI//9k=',
    'base64',
  )
  // One sample a point: the image covers x 300 to 364, y 20 to 84. The region
  // spans samples 8 to 20 across and down, 12 wide: block 8 to 16 lies
  // wholly inside it.
  const source = onePagePdf('q 64 0 0 64 300 20 cm /Jf Do Q', { resources: '/XObject << /Jf 6 0 R >>' }, [
    streamObject('/Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode', faint, true),
  ])
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[308, 64, 320, 76]]))
  assert.equal(region.imagesNotBlank, 1, `faint stripes under a narrow region: ${JSON.stringify(region)}`)
}

// gives one stream its own fill in each role it plays
{
  // A 1-bit stream of zeros: as an image's /Mask it is painted everywhere,
  // which is the apply's fill for a mask; drawn as a stencil it paints
  // everywhere, where the fill is unpainted.
  const white = Buffer.alloc(16 * 16 * 3, 255)
  const source = onePagePdf(
    'q 100 0 0 100 50 50 cm /Im0 Do Q 0 g q 100 0 0 100 250 50 cm /St Do Q',
    { resources: '/XObject << /Im0 6 0 R /St 7 0 R >>' },
    [
      streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Mask 7 0 R', white),
      streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ImageMask true /BitsPerComponent 1', Buffer.alloc(32)),
    ],
  )
  const regions = withDoc(source, (doc) => inspectPage(doc, 0, [[60, 60, 100, 100], [260, 60, 300, 100]]))
  assert.deepEqual(regions.map((r) => r.imagesNotBlank), [0, 1], `a mask and the same stream as a stencil: ${JSON.stringify(regions)}`)
}

// reads a 4:2:0 JPEG by its 16 x 16 blocks
{
  // 64 x 64 RGB, 4:2:0 (16 x 16 MCUs), stripes 4 samples wide: strong ones
  // (255 and 96 in red and green) and faint ones (255 and 215, within 48 of
  // white).
  const strong = Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCABAAEADASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAj/xAAYEAACAwAAAAAAAAAAAAAAAAAACUaExP/EABYBAQEBAAAAAAAAAAAAAAAAAAAFB//EABsRAAEEAwAAAAAAAAAAAAAAAAADBUNFgYTB/9oADAMBAAIRAxEAPwCl2AwO/nDAYHfzhgMDv5wwGB385tLDSbHScrLgMBgd/OGAwO/nDAYHfzhgMDv5ww0mx0Ky4DAYHfzhgMDv5wwGB384YDA7+cMNJsdCsuAwGB384YDA7+cMBgd/OGAwO/nDDSbHQrLgMBgd/OGAwO/nDAYHfzhgMDv5ww0mx0Ky4DAYHfzhgMDv5wwGB384YDA7+cMNJsdCsuAwGB384YDA7+cMBgd/OGAwO/nDDSbHQrLgMBgd/OGAwO/nDAYHfzhgMDv5ww0mx0Ky4DAYHfzhgMDv5wwGB384YDA7+cMNJsdCsuAwGB384YDA7+cMBgd/OGAwO/nDDSbHQrLgMBgd/OGAwO/nDAYHfzhgMDv5ww0mx0Ky4DAYHfzhgMDv5wwGB384YDA7+cMNJsdCsuAwGB384YDA7+cMBgd/OGAwO/nDDSbHQrLgMBgd/OGAwO/nDAYHfzhgMDv5ww0mx0Ky4DAYHfzhgMDv5wwGB384YDA7+cMNJsdCsuAwGB384YDA7+cMBgd/OGAwO/nDDSbHQrLg/9k=', 'base64')
  const faint = Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCABAAEADASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAn/xAAYEAACAwAAAAAAAAAAAAAAAAAAB0WDwv/EABQBAQAAAAAAAAAAAAAAAAAAAAD/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIRAxEAPwCjrJjrMhkx1mQyY6zIZMdZkAyY6zIZMdZkMmOsyGTHWZAMmOsyGTHWZDJjrMhkx1mQDJjrMhkx1mQyY6zIZMdZkAyY6zIZMdZkMmOsyGTHWZAMmOsyGTHWZDJjrMhkx1mQDJjrMhkx1mQyY6zIZMdZkAyY6zIZMdZkMmOsyGTHWZAMmOsyGTHWZDJjrMhkx1mQDJjrMhkx1mQyY6zIZMdZkAyY6zIZMdZkMmOsyGTHWZAMmOsyGTHWZDJjrMhkx1mQDJjrMhkx1mQyY6zIZMdZkAyY6zIZMdZkMmOsyGTHWZAMmOsyGTHWZDJjrMhkx1mQDJjrMhkx1mQyY6zIZMdZkD//2Q==', 'base64')
  const jpeg = (data) =>
    streamObject('/Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode', data, true)
  // One sample a point: the strong image covers x 300 to 364, the faint one
  // x 100 to 164, both y 20 to 84 (sample row r from the top is at y 84 - r).
  const source = onePagePdf('q 64 0 0 64 300 20 cm /Js Do Q q 64 0 0 64 100 20 cm /Jf Do Q', { resources: '/XObject << /Js 6 0 R /Jf 7 0 R >>' }, [jpeg(strong), jpeg(faint)])
  // Samples 20 to 32 across and down: under 16 wide, inside no whole block.
  const narrow = (x) => [x + 20, 52, x + 32, 64]
  // Samples 4 to 44: off the block grid, holding the whole block 16 to 32.
  const shifted = (x) => [x + 4, 40, x + 44, 80]
  const regions = [narrow(300), shifted(300), narrow(100), shifted(100)]
  const before = withDoc(source, (doc) => inspectPage(doc, 0, regions))
  // The faint stripes under the narrow region lie in crossed blocks and within
  // 48 of white, so the check does not see them: the stated limit.
  assert.deepEqual(before.map((r) => r.imagesNotBlank), [1, 1, 0, 1], `before the apply: ${JSON.stringify(before)}`)
  const applied = editPage(source, (doc, page) => {
    const marks = regions.map((r) => addMark(page, r))
    assert.equal(applyPage(doc, page, marks, false).result, 1)
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  const after = withDoc(applied, (doc) => inspectPage(doc, 0, regions))
  assert.deepEqual(after.map((r) => r.imagesNotBlank), [0, 0, 0, 0], `after the apply: ${JSON.stringify(after)}`)
}

// counts a flat image that is not the apply's fill when regions are given
{
  const red = Buffer.alloc(16 * 16 * 3)
  for (let i = 0; i < 256; i++) red.set([255, 0, 0], i * 3)
  const image = streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', red)
  const region = [60, 60, 100, 100]
  const source = onePagePdf('q 100 0 0 100 50 50 cm /Im0 Do Q', { resources: '/XObject << /Im0 6 0 R >>' }, [image])
  const [before] = withDoc(source, (doc) => inspectPage(doc, 0, [region]))
  assert.equal(before.imagesNotBlank, 1, `a solid red image under the region: ${JSON.stringify(before)}`)
  const applied = editPage(source, (doc, page) => {
    const mark = addMark(page, region)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
  })
  const [after] = withDoc(applied, (doc) => inspectPage(doc, 0, [region]))
  assert.equal(after.imagesNotBlank, 0, `the red image after the apply: ${JSON.stringify(after)}`)
  // Without regions a flat image under a box is still blank.
  const boxed = onePagePdf('q 100 0 0 100 50 50 cm /Im0 Do Q 0 g 60 60 40 40 re f', { resources: '/XObject << /Im0 6 0 R >>' }, [image])
  const [box] = withDoc(boxed, (doc) => inspectPage(doc, 0, []))
  assert.deepEqual([box.kind, box.imagesNotBlank], ['filled-box', 0], `a flat image under a black box: ${JSON.stringify(box)}`)
}

// inspects an image whose shape is only in its masks
{
  const black = Buffer.alloc(16 * 16 * 3)
  const shape = Buffer.alloc(16 * 16)
  for (let i = 0; i < 256; i++) shape[i] = (i % 16) * 16
  // A 1-bit stencil mask: a checkerboard of 2-sample squares, 2 bytes a row.
  const stencil = Buffer.alloc(16 * 2)
  for (let y = 0; y < 16; y++) stencil.fill(Math.floor(y / 2) % 2 ? 0x33 : 0xcc, y * 2, y * 2 + 2)
  const rgb = '/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8'
  const source = onePagePdf(
    'q 100 0 0 100 50 50 cm /Im0 Do Q q 100 0 0 100 250 50 cm /Im1 Do Q',
    { resources: '/XObject << /Im0 6 0 R /Im1 8 0 R >>' },
    [
      streamObject(`${rgb} /SMask 7 0 R`, black),
      streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8', shape),
      streamObject(`${rgb} /Mask 9 0 R`, black),
      streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ImageMask true /BitsPerComponent 1', stencil),
    ],
  )
  const regions = [
    [60, 60, 100, 100],
    [260, 60, 300, 100],
  ]
  const before = withDoc(source, (doc) => inspectPage(doc, 0, regions))
  assert.deepEqual(before.map((r) => r.imagesNotBlank), [1, 1], `masks under the regions: ${JSON.stringify(before)}`)
  const applied = editPage(source, (doc, page) => {
    const marks = regions.map((r) => addMark(page, r))
    assert.equal(applyPage(doc, page, marks, false).result, 1)
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  const after = withDoc(applied, (doc) => inspectPage(doc, 0, regions))
  assert.deepEqual(after.map((r) => r.imagesNotBlank), [0, 0], `mask samples left under the marks: ${JSON.stringify(after)}`)
}

// inspects a path under a region
{
  const source = onePagePdf('0 0 1 rg 50 50 100 100 re f q 200 50 100 100 re W n /Sh0 sh Q', {
    resources:
      '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [200 50 300 50] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>',
  })
  const onPath = [80, 80, 120, 120]
  const onShading = [230, 80, 270, 120]
  const before = withDoc(source, (doc) => inspectPage(doc, 0, [onPath, onShading]))
  assert.equal(before[0].paths, 1, `path under the region: ${JSON.stringify(before[0])}`)
  assert.equal(before[1].paths, 1, `shading under the region: ${JSON.stringify(before[1])}`)
  const applied = editPage(source, (doc, page) => {
    const marks = [addMark(page, onPath), addMark(page, onShading)]
    assert.equal(applyPage(doc, page, marks, false).result, 1)
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  withDoc(applied, (doc) => {
    const after = inspectPage(doc, 0, [onPath, onShading])
    assert.equal(after[0].paths, 0, `path left under the mark: ${JSON.stringify(after[0])}`)
    assert.equal(after[1].paths, 0, `shading left under the mark: ${JSON.stringify(after[1])}`)
    // The cut path still surrounds the hole.
    assert.equal(inspectPage(doc, 0, [[55, 55, 75, 75]])[0].paths, 1, 'the rest of the path is gone')
  })
}

// keeps a shading held directly in /Resources beside a mark: the rewritten
// content names it, so the save must write it
{
  const source = onePagePdf('0 0 1 rg 50 50 100 100 re f q 200 50 100 100 re W n /Sh0 sh Q', {
    resources:
      '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [200 50 300 50] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>',
  })
  const applied = editPage(source, (doc, page) => {
    const marks = [addMark(page, [80, 80, 120, 120])]
    assert.equal(applyPage(doc, page, marks, false).result, 1)
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  withDoc(applied, (doc) => {
    const [kept] = inspectPage(doc, 0, [[230, 80, 270, 120]])
    assert.equal(kept.paths, 1, `the shading beside the mark is gone: ${JSON.stringify(kept)}`)
  })
}

// counts a shading whose clip does not exclude the region
{
  const shading = '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [200 50 300 50] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>'
  // Two clips: the shading's box, then the box with the region cut out of it.
  // Under the even-odd rule (W*) the cut excludes the region; under the
  // nonzero rule (W) the two same-way rectangles do not, and the shading
  // still paints there.
  const clipped = (rule) => onePagePdf(`q 200 50 100 100 re W n q 200 50 100 100 re 230 80 40 40 re ${rule} n /Sh0 sh Q Q`, { resources: shading })
  const region = [235, 85, 265, 115]
  const [excluded] = withDoc(clipped('W*'), (doc) => inspectPage(doc, 0, [region]))
  assert.equal(excluded.paths, 0, `a shading clipped away from the region: ${JSON.stringify(excluded)}`)
  const [wrong] = withDoc(clipped('W'), (doc) => inspectPage(doc, 0, [region]))
  assert.equal(wrong.paths, 1, `a shading whose clip keeps the region: ${JSON.stringify(wrong)}`)
}

// samples a shading's curved clip to within a quarter point of the curve
{
  // One cubic arch over 0..400, its top at y 300. The region lies between the
  // arch and the chord a flattening into 16 lines would draw at x 181, about
  // 1.2 pt below the arch there: inside the clip.
  const source = onePagePdf('0 0 m 0 400 400 400 400 0 c h W n /Sh0 sh', {
    mediaBox: '[0 0 400 400]',
    resources: '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 400 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>',
  })
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[180.5, 297.95, 182, 298.55]]))
  assert.equal(region.paths, 1, `a shading under a curved clip: ${JSON.stringify(region)}`)
}

// gives up on a shading clip too large to flatten, and counts the shading
{
  const big = `1${'0'.repeat(20)}.0`
  // Under a cm scaling by 1e15, three hundred curves out to 1e20 and back,
  // each looping through the region.
  const loops = Array.from({ length: 300 }, () => `${big} ${big} -${big} -${big} 0.0000000000002 0.0000000000001 c`).join(' ')
  const source = onePagePdf(
    `q 1000000000000000.0 0 0 1000000000000000.0 0 0 cm 0.0000000000002 0.0000000000001 m ${loops} h W n /Sh0 sh Q`,
    {
      resources:
        '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 1 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>',
    },
  )
  const started = Date.now()
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[180, 80, 220, 120]]))
  assert.ok(Date.now() - started < 5000, 'the inspection took more than 5 s')
  assert.ok(region.paths > 0, `a shading under huge curved clips: ${JSON.stringify(region)}`)
}

// caps the samples under a region on an enormous page, and counts what it cannot finish
{
  const e29 = `1${'0'.repeat(29)}.0`
  const e28 = `1${'0'.repeat(28)}.0`
  // A shading over the whole page, clipped to the page with a hole cut out
  // (even-odd) that holds the region: it paints nothing there, but proving
  // that would take far more samples than the cap allows.
  const source = onePagePdf(`q 0 0 ${e29} ${e29} re W n 0 0 ${e29} ${e29} re 100 100 ${e28} ${e28} re W* n /Sh0 sh Q`, {
    mediaBox: `[0 0 ${e29} ${e29}]`,
    resources: '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 1 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>',
  })
  const started = Date.now()
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[200, 200, 1e27, 1e27]]))
  assert.ok(Date.now() - started < 5000, 'the inspection took more than 5 s')
  assert.equal(region.paths, 1, `a shading the sampler cannot finish: ${JSON.stringify(region)}`)
}

const axialShading =
  '/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 400 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >>'

// A closed path of |points| points around the whole page: an ellipse about
// its middle.
function roundPath(points) {
  const parts = []
  for (let i = 0; i < points; i++) {
    const angle = (2 * Math.PI * i) / points
    parts.push(`${(200 + 300 * Math.cos(angle)).toFixed(3)} ${(100 + 160 * Math.sin(angle)).toFixed(3)} ${i ? 'l' : 'm'}`)
  }
  return `${parts.join(' ')} h`
}

// Nearly the whole page, and two clips that keep a shading out of it: a path
// of |points| points that covers it, then the page with a hole (even-odd)
// that holds it. The shading paints nothing in the region, and the redactor's
// test says so from the second clip alone, so only the sampler reads the
// first.
const WIDE_RECT = [5, 5, 395, 195]
const clippedAway = (points) => `${roundPath(points)} W n 0 0 400 200 re 3 3 394 194 re W* n /Sh0 sh`

// samples a shading painted again and again under one clip once
{
  const source = onePagePdf(`q ${clippedAway(3000)} ${'/Sh0 sh '.repeat(2000)}Q`, { resources: axialShading })
  const started = Date.now()
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [WIDE_RECT]))
  assert.ok(Date.now() - started < 5000, 'the inspection took more than 5 s')
  assert.equal(region.paths, 0, `shadings clipped away from the region: ${JSON.stringify(region)}`)
}

// returns NULL when a page needs more work than one inspection allows
{
  // The region comes from the file, as a redact annotation. The form is
  // parsed afresh each time the page draws it, so each of its shadings has
  // clip paths of its own and is sampled again.
  const form = streamObject(`/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << ${axialShading} >>`, clippedAway(64))
  const [l, b, r, t] = WIDE_RECT
  const source = onePagePdf('/Fm Do '.repeat(8000), { resources: '/XObject << /Fm 6 0 R >>', page: '/Annots [7 0 R]' }, [
    form,
    `<< /Type /Annot /Subtype /Redact /Rect [${l} ${b} ${r} ${t}] >>`,
  ])
  const started = Date.now()
  const pointer = withDoc(source, (doc) => inspectPointer(doc, 0, []))
  assert.ok(Date.now() - started < 10000, 'the inspection took more than 10 s')
  assert.equal(pointer, 0, 'an inspection past its work budget returned a report')
  // A page the budget covers still gets its report.
  const few = onePagePdf('/Fm Do '.repeat(100), { resources: '/XObject << /Fm 6 0 R >>', page: '/Annots [7 0 R]' }, [
    form,
    `<< /Type /Annot /Subtype /Redact /Rect [${l} ${b} ${r} ${t}] >>`,
  ])
  const [mark] = withDoc(few, (doc) => inspectPage(doc, 0, []))
  assert.equal(mark.paths, 0, `shadings clipped away from the mark: ${JSON.stringify(mark)}`)
}

// counts a shading under a strip too long to sample a point apart
{
  const e29 = `1${'0'.repeat(29)}.0`
  const e28 = `1${'0'.repeat(28)}.0`
  // As above, the shading paints nothing in the hole that holds the region.
  // The strip is 1e27 pt long and 1 pt high: within the cap on points, but
  // only when they lie far more than 1 pt apart.
  const source = onePagePdf(`q 0 0 ${e29} ${e29} re 100 100 ${e28} ${e28} re W* n /Sh0 sh Q`, {
    mediaBox: `[0 0 ${e29} ${e29}]`,
    resources: axialShading,
  })
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[200, 200, 1e27, 201]]))
  assert.equal(region.paths, 1, `a shading under a strip the sampler cannot cover: ${JSON.stringify(region)}`)
  // A line through the first quad of a mark whose second quad lies 5e26 pt
  // away: sampled over the box of both, its points would fall far apart.
  const e26 = `5${'0'.repeat(26)}.0`
  const e27 = `1${'0'.repeat(27)}.0`
  const marked = onePagePdf(`0 205 m ${e28} 205 l ${e28} 305 l S`, { mediaBox: `[0 0 ${e29} ${e29}]`, page: '/Annots [6 0 R]' }, [
    `<< /Type /Annot /Subtype /Redact /Rect [200 200 ${e27} 310] /QuadPoints [200 210 210 210 200 200 210 200 ${e26} 310 ${e27} 310 ${e26} 300 ${e27} 300] >>`,
  ])
  const [mark] = withDoc(marked, (doc) => inspectPage(doc, 0, []))
  assert.equal(mark.paths, 1, `a line through one quad of a mark with two far apart: ${JSON.stringify(mark)}`)
}

// counts a shading whose place on the page is not finite
{
  // The matrix the form is drawn under is finite and so are the clip's
  // points, but their products overflow one way and the other, and the sum
  // is not a number: the clip has no box on the page.
  const e30 = `1${'0'.repeat(30)}.0`
  const e15 = `1${'0'.repeat(15)}.0`
  const form = streamObject(
    `/Type /XObject /Subtype /Form /BBox [-${e30} -${e30} ${e30} ${e30}] /Resources << ${axialShading} >>`,
    `-${e30} -${e30} m ${e30} -${e30} l ${e30} ${e30} l -${e30} ${e30} l h W n /Sh0 sh`,
  )
  const source = onePagePdf(`q ${e15} 0 ${e15} ${e15} 0 0 cm /Fm Do Q`, { resources: '/XObject << /Fm 6 0 R >>' }, [form])
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[180, 80, 220, 120]]))
  assert.equal(region.paths, 1, `a shading under a matrix that overflows: ${JSON.stringify(region)}`)
}

// tags a copy of an overlay the file shares with the page
{
  const [l, b, r, t] = SECRET_RECT
  // The own mark's /RO is also drawn by the page, where it shows the text
  // beside the mark. Tagging the shared stream would hide that text from
  // the check.
  const source = onePagePdf(`${textContent.replace('(KEEP-BETA) Tj', '')} /Fm Do`, { page: '/Annots [6 0 R]', resources: '/XObject << /Fm 8 0 R >>' }, [
    `<< /Type /Annot /Subtype /Redact /Rect [${l} ${b} ${r} ${t}] /QuadPoints [${quadOf(SECRET_RECT).join(' ')}] /RO 8 0 R /INKB_Gloss 7 0 R >>`,
    streamObject('', '{"v":1}'),
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 20 Tf 250 100 Td (KEEP-BETA) Tj ET'),
  ])
  const [shown] = withDoc(source, (doc) => inspectPage(doc, 0, [BETA_RECT]))
  assert.ok(shown.textChars > 0, `the shared form's text before the apply: ${JSON.stringify(shown)}`)
  const applied = editPage(source, (doc, page) => {
    assert.equal(applyPage(doc, page, [], true).result, 1)
  })
  const [beside] = withDoc(applied, (doc) => inspectPage(doc, 0, [BETA_RECT]))
  assert.ok(beside.textChars > 0, `the shared form's text is hidden from the check: ${JSON.stringify(beside)}`)
}

// What the apply cannot rewrite: a tiling pattern's cell, a mesh or
// function-based shading, a soft mask's group, a Type 3 glyph procedure.
// The apply cuts or clips the object that paints with one and leaves the
// carrier in the file, so INK_Redact_Unremovable is asked before the apply,
// while the object still says what it covers, and an apply over a region it
// counts in is refused.
const PATTERN_MARK = [150, 60, 250, 140]
const BESIDE_MARK = [20, 20, 100, 45]
const tiling = (bbox, xStep, yStep, content, entries = '/PaintType 1', resources = '') =>
  streamObject(`/Type /Pattern /PatternType 1 ${entries} /TilingType 1 /BBox [${bbox.join(' ')}] /XStep ${xStep} /YStep ${yStep} /Resources << ${resources} >>`, content)
// A cell the size of the page: a quadrilateral under the mark and a square beside it.
const pageCell = tiling([0, 0, 400, 200], 400, 200, '1 0 1 rg 170 80 m 230 85 l 220 120 l 180 115 l h f 0 0 1 rg 300 80 40 40 re f')
const hatchCell = tiling([0, 0, 10, 10], 10, 10, '0 G 1 w 0 0 m 10 10 l S')
// A page that fills |fill| (path operators) with /P0, the first of |patterns|.
const patternPage = (fill, patterns, colour = '/Pattern cs /P0 scn', resources = '') =>
  onePagePdf(`q ${colour} ${fill} Q`, { resources: `/Pattern << /P0 6 0 R >> ${resources}` }, patterns)
// One Gouraud triangle (type 4) over PATTERN_MARK, 8 bits to a number.
const meshShading = streamObject(
  '/ShadingType 4 /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 400 0 200 0 1 0 1 0 1]',
  Buffer.from([0, 96, 77, 255, 0, 0, 0, 159, 77, 0, 255, 0, 0, 128, 179, 0, 0, 255]),
)
const axial = '/ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 400 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >>'
// A page that paints |fill| through /GS0, a soft mask with |group| (object 6).
const maskedPage = (fill, group, extra = []) =>
  onePagePdf(`q /GS0 gs 0 0 1 rg ${fill} Q BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [group, ...extra])
const maskGroup = (content, resources = '/Font << /F1 5 0 R >>') =>
  streamObject(`/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Group << /S /Transparency /CS /DeviceGray >> /Resources << ${resources} >>`, content)
// A Type 3 font: /a declares a box a tenth of its em and draws three ems
// wide, /b draws inside the box it declares.
const type3Font = [
  '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 3000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R /b 8 0 R >> /Encoding << /Type /Encoding /Differences [97 /a /b] >> /FirstChar 97 /LastChar 98 /Widths [100 1000] >>',
  streamObject('', '100 0 0 0 100 100 d1 0 0 3000 1000 re f'),
  streamObject('', '1000 0 0 0 1000 1000 d1 100 100 800 800 re f'),
]
const type3Page = (text) => onePagePdf(`BT /T3 40 Tf ${text} ET`, { resources: '/Font << /T3 6 0 R >>' }, type3Font)

// The counts INK_Redact_Unremovable gives for |rects| on page 0 of |source|:
// patterns, shadings, mask groups, glyphs.
function unremovableIn(doc, rects = [PATTERN_MARK]) {
  const quads = rects.map(quadOf)
  const ptr = m._malloc(quads.length * 32)
  m.HEAPF32.set(quads.flat(), ptr / 4)
  const { regions } = report(m._INK_Redact_Unremovable(doc, 0, ptr, quads.length))
  m._free(ptr)
  return regions.map((r) => [r.patterns, r.shadings, r.maskGroups, r.glyphs])
}
const unremovable = (source, rects) => withDoc(source, (doc) => unremovableIn(doc, rects))
// The raw pointer INK_Redact_Unremovable returns for PATTERN_MARK, freed; 0 for NULL.
function unremovablePointer(source) {
  return withDoc(source, (doc) => {
    const ptr = m._malloc(32)
    m.HEAPF32.set(quadOf(PATTERN_MARK), ptr / 4)
    const result = m._INK_Redact_Unremovable(doc, 0, ptr, 1)
    m._free(ptr)
    if (result) m._INK_Inspect_Free(result)
    return result
  })
}
// The result of applying marks over |rects| on |source|, and the saved bytes.
function tryApplyMarks(source, rects) {
  let result
  const bytes = editPage(source, (doc, page) => {
    const marks = rects.map((rect) => addMark(page, rect))
    result = applyPage(doc, page, marks, false).result
    marks.forEach((mark) => m._FPDFPage_CloseAnnot(mark))
  })
  return { result, bytes }
}
const tryApply = (source, rect = PATTERN_MARK) => tryApplyMarks(source, [rect])
// The counts INK_Redact_Unremovable gives for one quad (8 numbers) on page 0.
function unremovableQuad(source, quad) {
  return withDoc(source, (doc) => {
    const ptr = m._malloc(32)
    m.HEAPF32.set(quad, ptr / 4)
    const { regions } = report(m._INK_Redact_Unremovable(doc, 0, ptr, 1))
    m._free(ptr)
    return regions.map((r) => [r.patterns, r.shadings, r.maskGroups, r.glyphs])[0]
  })
}
// |source| with a mark applied, whatever INK_Redact_Unremovable says.
function forceApply(source, rect = PATTERN_MARK) {
  return editPage(source, (doc, page) => {
    const mark = addMark(page, rect)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
  })
}
// What the check finds under |rect| in |bytes|.
const under = (bytes, rect = PATTERN_MARK) => withDoc(bytes, (doc) => inspectPage(doc, 0, [rect]))[0]
const zeros = (r) => [r.textChars, r.imagesNotBlank, r.paths, r.patternContent, r.annotationText, r.annotationImages]

// says before an apply what lies under a mark that the apply cannot rewrite
{
  const none = [0, 0, 0, 0]
  const band = '20 50 360 100 re f'
  // Tiling patterns: whatever the cell holds, an object painting with one in
  // the region counts.
  assert.deepEqual(unremovable(patternPage(band, [hatchCell]), [PATTERN_MARK, BESIDE_MARK]), [[1, 0, 0, 0], none], 'a hatched bar under the mark and beside it')
  assert.deepEqual(unremovable(patternPage(band, [pageCell])), [[1, 0, 0, 0]], 'a bar filled with a large cell')
  assert.deepEqual(unremovable(patternPage('20 50 360 100 re S', [hatchCell], '/Pattern CS /P0 SCN 10 w')), [none], 'a pattern stroke around the mark')
  assert.deepEqual(unremovable(patternPage('20 50 360 100 re S', [hatchCell], '/Pattern CS /P0 SCN 30 w')), [[1, 0, 0, 0]], 'a pattern stroke that enters the mark')
  assert.deepEqual(
    unremovable(onePagePdf('BT /Pattern cs /P0 scn /F1 40 Tf 160 80 Td (ABCDEFGHIJ) Tj ET', { resources: '/Pattern << /P0 6 0 R >>' }, [hatchCell])),
    [[1, 0, 0, 0]],
    'text shown with a pattern colour',
  )
  // A form's content does not carry the clip its form object is painted
  // under: the crop decides whether the fill inside paints in the region.
  const cropped = (crop) =>
    onePagePdf(`q ${crop} re W n /Fm0 Do Q`, { resources: '/XObject << /Fm0 7 0 R >>' }, [
      hatchCell,
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /Pattern << /P0 6 0 R >> >>', `/Pattern cs /P0 scn ${band}`),
    ])
  assert.deepEqual(unremovable(cropped('260 40 130 120')), [none], 'a pattern fill in a form cropped away from the mark')
  assert.deepEqual(unremovable(cropped('100 40 200 120')), [[1, 0, 0, 0]], 'a pattern fill in a form cropped over the mark')
  // Shadings: a mesh or a function can hold a picture, a ramp cannot.
  const mesh = onePagePdf('/Sh0 sh', { resources: '/Shading << /Sh0 6 0 R >>' }, [meshShading])
  assert.deepEqual(unremovable(mesh, [PATTERN_MARK, BESIDE_MARK]), [[0, 1, 0, 0], none], 'a mesh shading under the mark and beside it')
  assert.deepEqual(unremovable(onePagePdf('q 20 50 360 100 re W n /Sh0 sh Q', { resources: `/Shading << /Sh0 << ${axial} >> >>` })), [none], 'an axial shading under the mark')
  const meshFill = onePagePdf(`q /Pattern cs /P0 scn ${band} Q`, { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading 6 0 R >> >>' }, [meshShading])
  assert.deepEqual(unremovable(meshFill, [PATTERN_MARK, BESIDE_MARK]), [[0, 1, 0, 0], none], 'a path filled with a mesh shading pattern')
  assert.deepEqual(unremovable(onePagePdf(`q /Pattern cs /P0 scn ${band} Q`, { resources: `/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading << ${axial} >> >> >>` })), [none], 'a path filled with an axial shading pattern')
  // Soft masks: the object must paint in the region and the group must hold
  // something there.
  const secret = maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET')
  assert.deepEqual(unremovable(maskedPage(band, secret)), [[0, 0, 1, 0]], 'a mask group with text under the mark, on an object under it')
  assert.deepEqual(unremovable(maskedPage('20 150 360 40 re f', secret)), [none], 'the same group on an object beside the mark')
  assert.deepEqual(unremovable(maskedPage(band, maskGroup('1 g 300 60 40 20 re f'))), [none], 'a mask group that holds nothing under the mark')
  // Type 3: the text redactor goes by the box a glyph declares.
  assert.deepEqual(unremovable(type3Page('100 80 Td (a) Tj')), [[0, 0, 0, 1]], 'a glyph that draws out of its box into the mark')
  assert.deepEqual(unremovable(type3Page('180 80 Td (b) Tj')), [none], 'an ordinary glyph under the mark')
  assert.deepEqual(unremovable(type3Page('300 80 Td (b) Tj')), [none], 'an ordinary glyph beside the mark')
  // NULL when it cannot look.
  withDoc(mesh, (doc) => {
    const call = (index, quad, count = 1) => {
      const ptr = m._malloc(32)
      m.HEAPF32.set(quad, ptr / 4)
      const result = m._INK_Redact_Unremovable(doc, index, ptr, count)
      m._free(ptr)
      if (result) m._INK_Inspect_Free(result)
      return result
    }
    assert.notEqual(call(0, quadOf(PATTERN_MARK)), 0)
    assert.equal(call(1, quadOf(PATTERN_MARK)), 0, 'a page out of range')
    assert.equal(call(0, [150, 140, NaN, 140, 150, 60, 250, 60]), 0, 'a NaN in a quad')
    assert.equal(call(0, quadOf(PATTERN_MARK), 0), 0, 'no regions')
    assert.equal(m._INK_Redact_Unremovable(doc, 0, 0, 1), 0, 'a count with NULL quads')
  })
}

// does not count carriers painted elsewhere on the page
{
  // A hatched swatch, a texture whose one large cell draws where the region
  // is, and a mask group with a rectangle over the whole page, each on an
  // object away from the region, which lies over plain text.
  const source = onePagePdf(
    `${textContent} q /Pattern cs /P0 scn 300 150 60 30 re f Q q /Pattern cs /P1 scn 260 20 120 40 re f Q q /GS0 gs 0 0 1 rg 20 150 60 30 re f Q`,
    { resources: '/Pattern << /P0 6 0 R /P1 7 0 R >> /ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 8 0 R >> >> >>' },
    [hatchCell, tiling([0, 0, 400, 200], 400, 200, '1 0 1 rg 60 100 m 120 105 l 110 120 l h f'), maskGroup('1 g 0 0 400 200 re f')],
  )
  assert.deepEqual(unremovable(source, [SECRET_RECT]), [[0, 0, 0, 0]])
  const before = under(source, SECRET_RECT)
  assert.ok(before.textChars > 0, `the text under the region: ${JSON.stringify(before)}`)
  assert.deepEqual(zeros(before).slice(1), [0, 0, 0, 0, 0], `carriers elsewhere counted before the apply: ${JSON.stringify(before)}`)
  const after = under(forceApply(source, SECRET_RECT), SECRET_RECT)
  assert.deepEqual(zeros(after), [0, 0, 0, 0, 0, 0], `carriers elsewhere counted after the apply: ${JSON.stringify(after)}`)
}

// counts a pattern's cell for an object that paints in the region
{
  const band = '20 50 360 100 re f'
  const words = tiling([0, 0, 400, 200], 400, 200, 'BT /F1 12 Tf 165 100 Td (CELLSECRET) Tj ET', '/PaintType 1', '/Font << /F1 5 0 R >>')
  const text = under(patternPage(band, [words]))
  assert.ok(text.patternContent > 0, `a cell's text: ${JSON.stringify(text)}`)
  assert.equal(text.textChars, 0, 'pattern text counted as page text')
  const hex = Array.from({ length: 48 }, (_, i) => ((i * 37) % 256).toString(16).padStart(2, '0')).join('')
  const inline = under(patternPage(band, [tiling([0, 0, 400, 200], 400, 200, `q 60 0 0 40 160 80 cm BI /W 4 /H 4 /CS /RGB /BPC 8 /F /AHx ID ${hex}> EI Q`)]))
  assert.ok(inline.patternContent > 0, `a cell's inline image: ${JSON.stringify(inline)}`)
  const drawing = under(patternPage(band, [pageCell]))
  assert.equal(drawing.paths, 2, `the bar and its cell's drawing: ${JSON.stringify(drawing)}`)
  assert.equal(under(patternPage(band, [words]), BESIDE_MARK).patternContent, 0, 'a cell counted where its object does not paint')
}

// counts a path filled with a shading pattern by the path's own rule
{
  const source = onePagePdf('q /Pattern cs /P0 scn 20 50 360 100 re f Q', { resources: `/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading << ${axial} >> >> >>` })
  assert.equal(under(source).paths, 1, 'a shading pattern fill before the apply')
  assert.equal(under(forceApply(source)).paths, 0, 'a shading pattern fill under the applied mark')
  // A mesh used as a pattern counts once more, for the picture it can hold.
  const mesh = onePagePdf('q /Pattern cs /P0 scn 20 50 360 100 re f Q', { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading 6 0 R >> >>' }, [meshShading])
  assert.equal(under(mesh).paths, 2, 'a mesh pattern fill before the apply')
  assert.equal(under(mesh, BESIDE_MARK).paths, 0, 'a mesh pattern fill beside the region')
}

// still counts a mesh or function-based shading under a clip that excludes the region
{
  // A file in which something clipped such a shading away from a box, as an
  // apply does. (The fork's own apply refuses to, so the files are made by
  // hand.) The clip does not hide the shading: all clips are ignored for
  // these types.
  const hole = '0 0 400 200 re 150 60 100 80 re W* n'
  const mesh = onePagePdf(`q ${hole} /Sh0 sh Q`, { resources: '/Shading << /Sh0 6 0 R >>' }, [meshShading])
  assert.equal(under(mesh).paths, 1, 'a mesh shading clipped away from the region')
  assert.equal(under(mesh, BESIDE_MARK).paths, 0, 'a mesh shading beside the region')
  // The same in candidate mode, under a black box drawn over the mesh.
  const boxed = onePagePdf('/Sh0 sh 0 g 150 60 100 80 re f', { resources: '/Shading << /Sh0 6 0 R >>' }, [meshShading])
  const box = withDoc(boxed, (doc) => inspectPage(doc, 0, [])).find((c) => c.kind === 'filled-box')
  assert.ok(box && box.paths > 0, `a mesh shading under a black box: ${JSON.stringify(box)}`)
  const colours = streamObject('/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]', '{ 2 copy add 2 div }', true)
  const functionBased = (clip) =>
    onePagePdf(`q ${clip} /Sh0 sh Q`, {
      resources: '/Shading << /Sh0 << /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [100 0 0 80 150 60] /Function 6 0 R >> >>',
    }, [colours])
  assert.equal(under(functionBased(`120 50 160 100 re W n ${hole}`)).paths, 1, 'a function-based shading clipped away from the region')
  assert.equal(under(functionBased(`120 50 160 100 re W n ${hole}`), BESIDE_MARK).paths, 0, 'a function-based shading beside the region')
  assert.deepEqual(unremovable(functionBased('120 50 160 100 re W n')), [[0, 1, 0, 0]], 'a function-based shading under the mark')
}

// counts what a soft mask group holds for an object that paints in the region
{
  const band = '20 50 360 100 re f'
  const text = under(maskedPage(band, maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET')))
  assert.ok(text.textChars > 0, `a mask group's text: ${JSON.stringify(text)}`)
  const pixels = Buffer.alloc(16 * 16)
  for (let i = 0; i < 256; i++) pixels[i] = i
  const image = under(
    maskedPage(band, maskGroup('q 80 0 0 60 160 70 cm /Im0 Do Q', '/XObject << /Im0 7 0 R >>'), [
      streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8', pixels),
    ]),
  )
  assert.ok(image.imagesNotBlank > 0, `a mask group's image: ${JSON.stringify(image)}`)
  const drawing = under(maskedPage(band, maskGroup('1 g 170 80 m 230 85 l 220 120 l h f')))
  assert.equal(drawing.paths, 2, `the bar and its mask group's drawing: ${JSON.stringify(drawing)}`)
  const outside = under(maskedPage(band, maskGroup('1 g BT /F1 12 Tf 300 100 Td (ELSEWHERE) Tj ET 300 60 40 20 re f')))
  assert.deepEqual(zeros(outside), [0, 0, 1, 0, 0, 0], `a mask group that holds nothing under the mark: ${JSON.stringify(outside)}`)
  // A mask set inside a form lies where the form's matrix puts it, and one
  // set in an annotation's appearance counts as annotation text.
  const masked = '/Resources << /ExtGState << /GS0 << /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >> >>'
  const inForm = onePagePdf('/Fm0 Do', { resources: '/XObject << /Fm0 7 0 R >>' }, [
    maskGroup('1 g BT /F1 12 Tf 65 100 Td (MASKSECRET) Tj ET'),
    streamObject(`/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Matrix [1 0 0 1 100 0] ${masked}`, '/GS0 gs 0 0 1 rg 0 50 260 100 re f'),
  ])
  assert.ok(under(inForm).textChars > 0, 'the group of a mask set inside a form')
  assert.deepEqual(unremovable(inForm), [[0, 0, 1, 0]], 'the group of a mask set inside a form')
  const inAppearance = onePagePdf('', { page: '/Annots [7 0 R]' }, [
    maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET'),
    '<< /Type /Annot /Subtype /Square /Rect [20 50 380 150] /AP << /N 8 0 R >> >>',
    streamObject(`/Type /XObject /Subtype /Form /BBox [20 50 380 150] ${masked}`, '/GS0 gs 0 0 1 rg 20 50 360 100 re f'),
  ])
  assert.ok(under(inAppearance).annotationText > 0, 'the group of a mask set in an appearance')
  // The apply removes this annotation, so the gate does not read it.
  assert.deepEqual(unremovable(inAppearance), [[0, 0, 0, 0]], 'the group of a mask set in an appearance')
}

// counts a Type 3 glyph that draws out of its box into the region
{
  const reach = under(type3Page('100 80 Td (a) Tj'))
  assert.ok(reach.textChars > 0, `a glyph drawn into the region from outside its box: ${JSON.stringify(reach)}`)
  assert.equal(under(type3Page('300 80 Td (b) Tj')).textChars, 0, 'an ordinary glyph beside the region')
}

// closes the gaps a review found in the gate
{
  const none = [0, 0, 0, 0]
  const band = '20 50 360 100 re f'
  const patterned = (content, patterns = [hatchCell], resources = '') => onePagePdf(content, { resources: `/Pattern << /P0 6 0 R >> ${resources}` }, patterns)
  // A bitmap glyph is read the same before and after the page was rendered,
  // which drops the glyph's parsed form from the font's cache.
  const wide = [
    '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 3000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 /Widths [100] >>',
    streamObject('', '100 0 0 0 100 100 d1 q 3000 0 0 1000 0 0 cm BI /IM true /W 8 /H 8 /BPC 1 /D [1 0] /F /AHx ID 8142241818244281> EI Q', true),
  ]
  const bitmapOut = onePagePdf('BT /T3 40 Tf 100 80 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, wide)
  withDoc(bitmapOut, (doc) => {
    pixelAt(doc, 10, 10)
    assert.deepEqual(unremovableIn(doc), [[0, 0, 0, 1]], 'a bitmap glyph drawn out of its box, after a render')
    assert.ok(inspectPage(doc, 0, [PATTERN_MARK])[0].textChars > 0, 'the same in the inspector, after a render')
  })
  // Tags a file carries are not trusted before an apply.
  for (const tag of ['INKB_Overlay', 'INKB_PageCopy']) {
    const tagged = onePagePdf('/Fm0 Do', { resources: '/XObject << /Fm0 7 0 R >>' }, [
      hatchCell,
      streamObject(`/Type /XObject /Subtype /Form /BBox [0 0 400 200] /${tag} true /Resources << /Pattern << /P0 6 0 R >> >>`, `/Pattern cs /P0 scn ${band}`),
    ])
    assert.deepEqual(unremovable(tagged), [[1, 0, 0, 0]], `a pattern fill in a form tagged /${tag}`)
  }
  // A ramp is plain only when its function is.
  const ramp = (fn) => onePagePdf('q 20 50 360 100 re W n /Sh0 sh Q', { resources: `/Shading << /Sh0 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 400 0] /Function ${fn} >> >>` }, [
    streamObject('/FunctionType 0 /Domain [0 1] /Range [0 1 0 1 0 1] /Size [4] /BitsPerSample 8', Buffer.from([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0])),
  ])
  const exponent = '<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >>'
  assert.deepEqual(unremovable(ramp('6 0 R')), [[0, 1, 0, 0]], 'an axial shading with a sampled function')
  assert.ok(under(ramp('6 0 R')).paths > 0)
  assert.deepEqual(unremovable(ramp(exponent)), [none], 'an axial shading with an exponential function')
  assert.deepEqual(unremovable(ramp(`<< /FunctionType 3 /Domain [0 1] /Functions [${exponent} ${exponent}] /Bounds [0.5] /Encode [0 1 0 1] >>`)), [none], 'an axial shading with two stitched stops')
  assert.deepEqual(
    unremovable(ramp(`<< /FunctionType 3 /Domain [0 1] /Functions [${Array(9).fill(exponent).join(' ')}] /Bounds [0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8] /Encode [${Array(9).fill('0 1').join(' ')}] >>`)),
    [[0, 1, 0, 0]],
    'an axial shading with nine stitched stops',
  )
  // What cannot be decoded counts.
  const garbage = streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Filter /FlateDecode', 'this is not Flate data', true)
  assert.deepEqual(unremovable(maskedPage(band, garbage)), [[0, 0, 1, 0]], 'a mask group that cannot be decoded')
  const broken = onePagePdf('BT /T3 40 Tf 300 80 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, [wide[0], streamObject('/Filter /FlateDecode', 'this is not Flate data', true)])
  assert.deepEqual(unremovable(broken), [[0, 0, 0, 1]], 'a glyph procedure that cannot be decoded')
  // Text: the colour its rendering mode uses, and its stroke's width.
  assert.deepEqual(unremovable(patterned('BT /Pattern CS /P0 SCN 30 w 1 Tr /F1 40 Tf 256 80 Td (I) Tj ET')), [[1, 0, 0, 0]], 'the stroke of pattern-stroked text beside the mark')
  assert.deepEqual(unremovable(patterned('BT /Pattern CS /P0 SCN 1 w 1 Tr /F1 40 Tf 256 80 Td (I) Tj ET')), [none], 'thin pattern-stroked text beside the mark')
  assert.deepEqual(unremovable(patterned('BT /Pattern CS /P0 SCN 0 g /F1 40 Tf 160 80 Td (AB) Tj ET')), [none], 'filled text with a pattern stroke colour it does not use')
  assert.deepEqual(unremovable(patterned('BT /Pattern cs /P0 scn 3 Tr /F1 40 Tf 160 80 Td (AB) Tj ET')), [none], 'invisible text with a pattern colour')
  assert.deepEqual(unremovable(patterned('BT /Pattern cs /P0 scn 2 Tr /F1 40 Tf 160 80 Td (ABCDEFGHIJ) Tj ET')), [[1, 0, 0, 0]], 'filled and stroked text with a pattern fill')
  // Other painters.
  const uncoloured = patternPage(band, [tiling([0, 0, 10, 10], 10, 10, '0 0 m 10 10 l S', '/PaintType 2')], '/Cs0 cs 1 0 0 /P0 scn', '/ColorSpace << /Cs0 [/Pattern /DeviceRGB] >>')
  assert.deepEqual(unremovable(uncoloured), [[1, 0, 0, 0]], 'an uncoloured pattern')
  const image = (dict, data) =>
    onePagePdf('q /Pattern cs /P0 scn 360 0 0 100 20 50 cm /Im0 Do Q', { resources: '/Pattern << /P0 6 0 R >> /XObject << /Im0 7 0 R >>' }, [hatchCell, streamObject(`/Type /XObject /Subtype /Image /Width 8 /Height 8 ${dict}`, data)])
  assert.deepEqual(unremovable(image('/ImageMask true /BitsPerComponent 1', Buffer.alloc(8, 0x55))), [[1, 0, 0, 0]], 'an image mask painted with a pattern colour')
  assert.deepEqual(unremovable(image('/ColorSpace /DeviceGray /BitsPerComponent 8', Buffer.alloc(64, 0x55))), [none], 'an image beside a pattern colour it does not use')
  assert.deepEqual(unremovable(patterned(`q 260 40 130 120 re W n /Pattern cs /P0 scn ${band} Q`)), [none], 'a pattern fill clipped away from the mark')
  assert.deepEqual(unremovable(patterned(`q 100 40 200 120 re W n /Pattern cs /P0 scn ${band} Q`)), [[1, 0, 0, 0]], 'a pattern fill clipped over the mark')
  // Annotations: the apply removes one whose /Rect meets a region, and
  // another cannot paint in it.
  for (const rect of ['20 50 380 150', '20 150 380 190']) {
    const [l, b, r, t] = rect.split(' ').map(Number)
    const inAppearance = onePagePdf('', { page: '/Annots [7 0 R]' }, [
      hatchCell,
      `<< /Type /Annot /Subtype /Square /Rect [${rect}] /AP << /N 8 0 R >> >>`,
      streamObject(`/Type /XObject /Subtype /Form /BBox [${rect}] /Resources << /Pattern << /P0 6 0 R >> >>`, `/Pattern cs /P0 scn ${l} ${b} ${r - l} ${t - b} re f`),
    ])
    assert.deepEqual(unremovable(inAppearance), [none], `a pattern in an appearance at ${rect}`)
  }
  // Fades: a mask group holding nothing but ramps and rectangles is how
  // gradient transparency is drawn, and does not count.
  const fadeOver = (content, group) =>
    onePagePdf(`q /GS0 gs ${content} Q BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [
      maskGroup(group, `/Shading << /Sh0 << ${axial} >> >>`),
    ])
  const fadedText = fadeOver('BT /F1 20 Tf 160 90 Td (FADED) Tj ET', '0 0 400 200 re f /Sh0 sh')
  assert.deepEqual(unremovable(fadedText), [none], 'text under a fade')
  const faded = tryApply(fadedText)
  assert.equal(faded.result, 1, 'an apply over text under a fade')
  assert.ok(!shows(inflateStreams(faded.bytes), 'FADED') && shows(inflateStreams(faded.bytes), 'KEEP'), 'the text under the fade was not removed')
  assert.deepEqual(unremovable(fadeOver(`0 0 1 rg ${band}`, '0 0 400 200 re f /Sh0 sh')), [none], 'a bar under a fade')
  assert.deepEqual(unremovable(fadeOver(`0 0 1 rg ${band}`, '/Sh0 sh 170 80 m 230 85 l 220 120 l h f')), [[0, 0, 1, 0]], 'a fade group that also holds a shape')
  // A mask set on the page reaches into a form painted under it, and lies
  // where it was set: the form's matrix does not move it again.
  const inherited = onePagePdf('/GS0 gs /Fm0 Do', { resources: '/XObject << /Fm0 7 0 R >> /ExtGState << /GS0 << /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [
    maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET'),
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Matrix [1 0 0 1 100 0]', '0 0 1 rg 0 50 260 100 re f'),
  ])
  assert.deepEqual(unremovable(inherited, [PATTERN_MARK, [262, 60, 350, 140]]), [[0, 0, 1, 0], none], 'a mask inherited across Do')
  const reset = onePagePdf(`/GS0 gs /GS1 gs 0 0 1 rg ${band}`, { resources: '/ExtGState << /GS0 << /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> /GS1 << /SMask /None >> >>' }, [
    maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET'),
  ])
  assert.deepEqual(unremovable(reset), [none], 'a mask reset with /SMask /None')
  // NULL at the limits.
  assert.equal(unremovablePointer(nestedFormsPdf(41)), 0, 'forms 41 deep')
  const heavy = onePagePdf(
    Array.from({ length: 160 }, (_, i) => `q 1 0 0 1 ${(i / 100).toFixed(2)} 0 cm /GS0 gs ${band} Q`).join(' '),
    { resources: '/ExtGState << /GS0 << /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' },
    [maskGroup(' '.repeat(8 << 20))],
  )
  const started = Date.now()
  assert.equal(unremovablePointer(heavy), 0, 'a page past the work budget returned a report')
  assert.ok(Date.now() - started < 60000, 'the gate took more than 60 s')
}

// refuses an apply over what it cannot rewrite
{
  // INK_Redact_ApplyPage asks the same question of the marks it is about to
  // apply, so a caller that did not ask INK_Redact_Unremovable first cannot
  // cut a carrier and leave its content behind. The page is left as it was.
  const band = '20 50 360 100 re f'
  for (const [name, source] of [
    ['a pattern fill', patternPage(band, [pageCell])],
    ['a mesh shading pattern fill', onePagePdf(`q /Pattern cs /P0 scn ${band} Q`, { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading 6 0 R >> >>' }, [meshShading])],
    ['a mesh shading', onePagePdf('/Sh0 sh', { resources: '/Shading << /Sh0 6 0 R >>' }, [meshShading])],
    ['a masked fill', maskedPage(band, maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET'))],
    ['a glyph drawn out of its box', type3Page('100 80 Td (a) Tj')],
    ['forms nested too deep to read', nestedFormsPdf(41)],
  ]) {
    const before = withDoc(source, (doc) => (inspectPointer(doc, 0, [quadOf(PATTERN_MARK)]) ? JSON.stringify(inspectPage(doc, 0, [PATTERN_MARK])) : 'NULL'))
    const forced = tryApply(source)
    assert.equal(forced.result, -1, `${name}: the apply went ahead`)
    const after = withDoc(forced.bytes, (doc) => (inspectPointer(doc, 0, [quadOf(PATTERN_MARK)]) ? JSON.stringify(inspectPage(doc, 0, [PATTERN_MARK])) : 'NULL'))
    assert.equal(after, before, `${name}: the refused apply changed the page`)
    assert.equal(count(inflateStreams(forced.bytes), '/Redact'), 1, `${name}: the mark is gone`)
  }
  // The same for an own mark applied with own_locked.
  let outcome
  editPage(patternPage(band, [pageCell]), (doc, page) => {
    const own = addMark(page, PATTERN_MARK)
    setData(own, '{"v":1}')
    m._FPDFPage_CloseAnnot(own)
    outcome = applyPage(doc, page, [], true)
  })
  assert.equal(outcome.result, -1, 'an own mark over a pattern fill was applied')
}

// refuses a carrier even when the mark covers the whole shape
{
  // The apply would remove such an object, but it cannot be shown to remove
  // every reference that keeps what the object carried in the file: a later
  // object's colour state can still name the pattern without painting with
  // it, a mask or a cell can be in effect on another object elsewhere, and a
  // stroke's corner can overshoot the box the apply goes by, so that the
  // shape is cut and not removed. So a carrier counts wherever its shape
  // ends.
  const solid = tiling([0, 0, 10, 10], 10, 10, '0 0 1 rg 0 0 9.75 10 re f')
  const small = streamObject(
    '/ShadingType 4 /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 400 0 200 0 1 0 1 0 1]',
    Buffer.from([0, 105, 90, 255, 0, 0, 0, 150, 90, 0, 255, 0, 0, 128, 160, 0, 0, 255]),
  )
  const keep = 'BT /F1 12 Tf 20 20 Td (KEEP) Tj ET'
  const patterned = (content) => onePagePdf(`q ${content} Q ${keep}`, { resources: '/Pattern << /P0 6 0 R >>' }, [solid])
  const meshFill = (fill) => onePagePdf(`q /Pattern cs /P0 scn ${fill} Q ${keep}`, { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading 6 0 R >> >>' }, [small])
  const functionBased = (content) =>
    onePagePdf(`${content} ${keep}`, {
      resources: '/Shading << /Sh0 << /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [80 0 0 60 160 70] /Function 6 0 R >> >>',
    }, [streamObject('/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]', '{ 2 copy add 2.125 div }', true)])
  const group = maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET')
  for (const [name, source, counts] of [
    ['a pattern-filled path inside the mark', patterned('/Pattern cs /P0 scn 160 70 80 60 re f'), [1, 0, 0, 0]],
    // The line never paints with the pattern, but its colour state names it,
    // and the rewrite writes that state out.
    ['a pattern fill inside the mark and a later line that keeps the fill colour', patterned('/Pattern cs /P0 scn 160 70 80 60 re f 0 G 2 w 20 35 m 380 35 l S'), [1, 0, 0, 0]],
    ['pattern-coloured text inside the mark', patterned('BT /Pattern cs /P0 scn /F1 20 Tf 160 90 Td (AB) Tj ET'), [1, 0, 0, 0]],
    ['a pattern-stroked box inside the mark', patterned('/Pattern CS /P0 SCN 4 w 170 80 60 40 re S'), [1, 0, 0, 0]],
    ['a closed pattern-stroked triangle inside the mark', patterned('/Pattern CS /P0 SCN 4 w 170 80 m 230 80 l 200 120 l h S'), [1, 0, 0, 0]],
    ['two fills of one pattern, inside the mark and elsewhere', patterned('/Pattern cs /P0 scn 160 70 80 60 re f 20 160 100 30 re f'), [1, 0, 0, 0]],
    ['a mesh shading inside the mark', onePagePdf(`/Sh0 sh ${keep}`, { resources: '/Shading << /Sh0 6 0 R >>' }, [small]), [0, 1, 0, 0]],
    ['a mesh shading-pattern fill inside the mark', meshFill('160 70 80 60 re f'), [0, 1, 0, 0]],
    ['a function-based shading clipped inside the mark', functionBased('q 160 70 80 60 re W n /Sh0 sh Q'), [0, 1, 0, 0]],
    ['an unclipped function-based shading whose domain lies inside the mark', functionBased('/Sh0 sh'), [0, 1, 0, 0]],
    ['a masked object inside the mark', maskedPage('160 70 80 60 re f', group), [0, 0, 1, 0]],
    ['one mask over a fill inside the mark and a fill elsewhere', maskedPage('160 70 80 60 re f 20 160 100 30 re f', group), [0, 0, 1, 0]],
    ['a large pattern fill clipped to a window inside the mark', patterned('160 70 80 60 re W n /Pattern cs /P0 scn 20 50 360 100 re f'), [1, 0, 0, 0]],
  ]) {
    assert.deepEqual(unremovable(source), [counts], name)
    assert.equal(tryApply(source).result, -1, `${name}: the apply went ahead`)
  }
}

// closes the gaps a second review found in the gate
{
  const none = [0, 0, 0, 0]
  const band = '20 50 360 100 re f'
  // A fade is ramps and a few rectangles under no clip but a rectangle. A
  // gradient through a shape is the shape.
  const fadeGroup = (content) => maskedPage(band, maskGroup(content, `/Shading << /Sh0 << ${axial} >> >>`))
  assert.deepEqual(unremovable(fadeGroup('q 0 0 400 200 re W n /Sh0 sh Q')), [none], 'a fade under a rectangular clip')
  assert.deepEqual(unremovable(fadeGroup('q 170 80 m 230 85 l 220 120 l h W n /Sh0 sh Q')), [[0, 0, 1, 0]], 'a ramp clipped to a shape')
  assert.deepEqual(unremovable(fadeGroup('q 170 80 m 230 85 l 220 120 l h W n 0 0 400 200 re f Q')), [[0, 0, 1, 0]], 'a rectangle clipped to a shape')
  assert.deepEqual(unremovable(fadeGroup(Array.from({ length: 4 }, (_, i) => `${160 + i * 20} 80 10 40 re f`).join(' '))), [none], 'four rectangles')
  assert.deepEqual(unremovable(fadeGroup(Array.from({ length: 5 }, (_, i) => `${160 + i * 15} 80 10 40 re f`).join(' '))), [[0, 0, 1, 0]], 'five rectangles')
  assert.deepEqual(unremovable(fadeGroup('0 0 400 200 re f q 0 0 400 200 re W n /Sh0 sh Q')), [none], 'one ramp and one rectangle')
  assert.deepEqual(unremovable(fadeGroup(Array.from({ length: 5 }, () => 'q 0 0 400 200 re W n /Sh0 sh Q').join(' '))), [[0, 0, 1, 0]], 'five ramps')
  // A picture drawn out of many small clipped ramps is not a fade.
  const dots = Array.from({ length: 300 }, (_, i) => `q ${160 + (i % 30) * 2} ${80 + Math.floor(i / 30) * 2} 2 2 re W n /Sh0 sh Q`).filter((_, i) => i % 3).join(' ')
  assert.deepEqual(unremovable(fadeGroup(dots)), [[0, 0, 1, 0]], 'a picture drawn out of clipped ramps')
  // A rotated mark that leaves the page is read as its box cut to the page;
  // a fill across the real quad's edge counts there.
  const solid = tiling([0, 0, 10, 10], 10, 10, '0 0 1 rg 0 0 9.75 10 re f')
  const corner = onePagePdf('q /Pattern cs /P0 scn 330 60 40 40 re f Q', { resources: '/Pattern << /P0 6 0 R >>' }, [solid])
  assert.deepEqual(unremovableQuad(corner, [320, 100, 380, 160, 380, 40, 440, 100]), [1, 0, 0, 0], 'a fill across the edge of a rotated mark that leaves the page')
  // A glyph procedure without resources of its own is read with those of
  // the content that shows it, and one naming what cannot be found is taken
  // to reach the region.
  const procFont = (proc, extra = '') => [
    `<< /Type /Font /Subtype /Type3 /FontBBox [0 0 3000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 /Widths [100] ${extra} >>`,
    streamObject('', proc, true),
  ]
  const inForm = onePagePdf('/Fm0 Do', { resources: '/XObject << /Fm0 8 0 R >>' }, [
    ...procFont('100 0 0 0 100 100 d1 q 3000 0 0 1000 0 0 cm /Im0 Do Q'),
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Resources << /Font << /T3 6 0 R >> /XObject << /Im0 9 0 R >> >>', 'BT /T3 40 Tf 100 80 Td (a) Tj ET'),
    streamObject('/Type /XObject /Subtype /Image /Width 8 /Height 8 /ImageMask true /BitsPerComponent 1', Buffer.alloc(8, 0x55)),
  ])
  assert.deepEqual(unremovable(inForm), [[0, 0, 0, 1]], 'a glyph whose image is named by the form that shows it')
  const unnamed = onePagePdf('BT /T3 40 Tf 300 80 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, procFont('100 0 0 0 100 100 d1 q 3000 0 0 1000 0 0 cm /Nowhere Do Q'))
  assert.deepEqual(unremovable(unnamed), [[0, 0, 0, 1]], 'a glyph that names a resource nothing defines')
  // A shading painted by a glyph procedure has no box of its own there: the
  // glyph is taken to paint its font's box.
  const shaded = onePagePdf('BT /T3 40 Tf 100 80 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, procFont('100 0 d0 /Sh0 sh', `/Resources << /Shading << /Sh0 << ${axial} >> >> >>`))
  assert.deepEqual(unremovable(shaded), [[0, 0, 0, 1]], 'a glyph that paints a shading')
  // A hairline is a point wide on the page, whatever the matrix.
  const hairline = onePagePdf('q /Pattern CS /P0 SCN 0.1 0 0 0.1 0 0 cm 0 w 1497 500 m 1497 1500 l S Q', { resources: '/Pattern << /P0 6 0 R >>' }, [solid])
  assert.deepEqual(unremovable(hairline), [[1, 0, 0, 0]], 'a pattern-stroked hairline beside the mark')
  // NULL when reading pattern cells spends the budget.
  // 160 pattern objects with the same bytes: each is a cell of its own.
  const cells = Array(160).fill(tiling([0, 0, 400, 200], 400, 200, ' '.repeat(8 << 20)))
  const heavy = onePagePdf(cells.map((_, i) => `/Pattern cs /P${i} scn ${band}`).join(' '), { resources: `/Pattern << ${cells.map((_, i) => `/P${i} ${6 + i} 0 R`).join(' ')} >>` }, cells)
  const started = Date.now()
  assert.equal(withDoc(heavy, (doc) => inspectPointer(doc, 0, [quadOf(PATTERN_MARK)])), 0, 'an inspection past the work budget returned a report')
  assert.ok(Date.now() - started < 10000, 'the inspection took more than 10 s')
  const light = onePagePdf(cells.slice(0, 3).map((_, i) => `/Pattern cs /P${i} scn ${band}`).join(' '), { resources: `/Pattern << ${cells.slice(0, 3).map((_, i) => `/P${i} ${6 + i} 0 R`).join(' ')} >>` }, cells.slice(0, 3))
  assert.notEqual(withDoc(light, (doc) => inspectPointer(doc, 0, [quadOf(PATTERN_MARK)])), 0, 'three such cells spent the budget')
}

// reads a path's box from its outline and its stroke, not from PDFium's
{
  const none = [0, 0, 0, 0]
  const solid = tiling([0, 0, 10, 10], 10, 10, '0 0 1 rg 0 0 9.75 10 re f')
  const patterned = (content) => onePagePdf(`q ${content} Q`, { resources: '/Pattern << /P0 6 0 R >>' }, [solid])
  // A negative line width pulls PDFium's box inward, off the fill it bounds.
  const inner = [255, 70, 295, 130]
  assert.deepEqual(unremovable(patterned('/Pattern cs /P0 scn -50 w 100 60 200 80 re B'), [inner]), [[1, 0, 0, 0]], 'a pattern fill stroked with a negative width')
  assert.equal(tryApply(patterned('/Pattern cs /P0 scn -50 w 100 60 200 80 re B'), inner).result, -1)
  const black = onePagePdf('0 g -50 w 100 60 200 80 re B BT /F1 12 Tf 20 20 Td (KEEP) Tj ET')
  assert.equal(under(black, inner).paths, 1, 'a black fill stroked with a negative width, under the region')
  const cut = tryApply(black, inner)
  assert.equal(cut.result, 1)
  assert.equal(under(cut.bytes, inner).paths, 0, 'a black fill stroked with a negative width was not cut by the apply')
  withDoc(cut.bytes, (doc) => assert.deepEqual(pixelAt(doc, 275, 100).slice(0, 3), [255, 255, 255], 'the fill is still painted under the mark'))
  // A sharp corner where a closed outline starts reaches far past its box.
  const tip = [20, 92, 48, 108]
  const spike = '10 w 60 100 m 260 121 l 260 79 l h S'
  assert.deepEqual(unremovable(patterned(`/Pattern CS /P0 SCN ${spike}`), [tip]), [[1, 0, 0, 0]], 'the mitred tip of a pattern-stroked triangle')
  assert.equal(tryApply(patterned(`/Pattern CS /P0 SCN ${spike}`), tip).result, -1)
  const blackSpike = onePagePdf(`0 G ${spike} BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`)
  assert.equal(under(blackSpike, tip).paths, 1, 'the mitred tip of a black triangle, under the region')
  const trimmed = tryApply(blackSpike, tip)
  assert.equal(trimmed.result, 1)
  assert.equal(under(trimmed.bytes, tip).paths, 0, 'the mitred tip was not cut by the apply')
  // The box is a first look, not the answer: a thin stroke beside the mark
  // does not count.
  assert.deepEqual(unremovable(patterned('/Pattern CS /P0 SCN 1 w 255 60 40 80 re S')), [none], 'a thin pattern-stroked box 4 pt beside the mark')
}

// reads a shading painted where its holder has no box
{
  // A form or a mask group without /BBox gives a shading painted in it an
  // empty rectangle, and PDFium still paints it. It is bounded by its own
  // geometry, or by nothing.
  const band = '20 50 360 100 re f'
  const boxless = onePagePdf('/Fm0 Do', { resources: '/XObject << /Fm0 7 0 R >>' }, [meshShading, streamObject('/Type /XObject /Subtype /Form /Resources << /Shading << /Sh0 6 0 R >> >>', '/Sh0 sh')])
  assert.deepEqual(unremovable(boxless, [PATTERN_MARK, BESIDE_MARK]), [[0, 1, 0, 0], [0, 0, 0, 0]], 'a mesh painted in a form without /BBox')
  const group = streamObject('/Type /XObject /Subtype /Form /Group << /S /Transparency /CS /DeviceGray >> /Resources << /Shading << /Sh0 7 0 R >> >>', '/Sh0 sh')
  const masked = onePagePdf(`q /GS0 gs 0 0 1 rg ${band} Q`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [group, meshShading])
  assert.deepEqual(unremovable(masked), [[0, 0, 1, 0]], 'a mesh painted in a mask group without /BBox')
  // A glyph's shading is bounded by the shading, not by the font's box,
  // which the file supplies and nothing clips to.
  const font = [
    '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 100 100] /FontMatrix [0.01 0 0 0.01 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 /Widths [100] /Resources << /Shading << /Sh0 << /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [100 0 0 100 150 0] /Function 8 0 R >> >> >> >>',
    streamObject('', '100 0 d0 /Sh0 sh', true),
    streamObject('/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]', '{ 2 copy add 2 div }', true),
  ]
  const glyph = onePagePdf('BT /T3 40 Tf 100 80 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, font)
  assert.deepEqual(unremovable(glyph), [[0, 0, 0, 1]], 'a glyph whose shading paints outside its font box')
  // Rendering modes that paint nothing reach nowhere.
  assert.deepEqual(unremovable(type3Page('3 Tr 100 80 Td (a) Tj')), [[0, 0, 0, 0]], 'an invisible out-of-box glyph')
}

// counts for each region of a call, and refuses for any
{
  const band = '20 50 360 100 re f'
  const source = patternPage(band, [hatchCell])
  const clear = [20, 160, 100, 195]
  assert.deepEqual(unremovable(source, [clear, PATTERN_MARK]), [[0, 0, 0, 0], [1, 0, 0, 0]], 'a carrier under the second of two regions')
  assert.equal(tryApplyMarks(source, [clear, PATTERN_MARK]).result, -1, 'an apply whose second mark covers a carrier went ahead')
  // An explicit mark over nothing and an own mark over the carrier.
  let outcome
  editPage(source, (doc, page) => {
    const own = addMark(page, PATTERN_MARK)
    setData(own, '{"v":1}')
    m._FPDFPage_CloseAnnot(own)
    const explicit = addMark(page, clear)
    outcome = applyPage(doc, page, [explicit], true)
    m._FPDFPage_CloseAnnot(explicit)
  })
  assert.equal(outcome.result, -1, 'an apply whose own mark covers a carrier went ahead')
  const twice = onePagePdf('q 1 0 0 1 160 70 cm /Fm0 Do Q q 1 0 0 1 200 100 cm /Fm0 Do Q', { resources: '/XObject << /Fm0 7 0 R >>' }, [
    hatchCell,
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 30 20] /Resources << /Pattern << /P0 6 0 R >> >>', '/Pattern cs /P0 scn 0 0 30 20 re f'),
  ])
  assert.deepEqual(unremovable(twice), [[2, 0, 0, 0]], 'a pattern fill in a form drawn twice under the mark')
}

// tells a fade from a picture in a mask group
{
  const none = [0, 0, 0, 0]
  const counts = [0, 0, 1, 0]
  const band = '20 50 360 100 re f'
  const resources = `/Shading << /Sh0 << ${axial} >> >> /Font << /F1 5 0 R >> /Pattern << /P0 7 0 R >> /XObject << /Fm0 8 0 R /Im0 9 0 R >> /ExtGState << /GS1 << /SMask << /Type /Mask /S /Luminosity /G 8 0 R >> >> >>`
  const group = (content, dict = '/BBox [0 0 400 200]') =>
    onePagePdf(`q /GS0 gs 0 0 1 rg ${band} Q`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [
      streamObject(`/Type /XObject /Subtype /Form ${dict} /Group << /S /Transparency /CS /DeviceGray >> /Resources << ${resources} >>`, content),
      hatchCell,
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200]', '0 0 400 200 re f'),
      streamObject('/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /BitsPerComponent 8', Buffer.alloc(64, 0x55)),
    ])
  for (const [name, content] of [
    ['a ramp under a text clip', 'q BT /F1 60 Tf 7 Tr 160 80 Td (AB) Tj ET /Sh0 sh Q'],
    ['a stroked rectangle', '1 G 4 w 160 70 80 60 re S'],
    ['a pattern-filled rectangle', '/Pattern cs /P0 scn 160 70 80 60 re f'],
    ['a sheared rectangle', 'q 1 0 0.5 1 0 0 cm 160 70 80 60 re f Q'],
    ['an object under a mask of its own', '/GS1 gs 160 70 80 60 re f'],
    ['a form', '/Fm0 Do'],
    ['an image', 'q 80 0 0 60 160 70 cm /Im0 Do Q'],
  ]) {
    assert.deepEqual(unremovable(group(content)), [counts], `a mask group holding ${name}`)
  }
  // An empty group holds nothing, so there is nothing to keep.
  assert.deepEqual(unremovable(group('')), [none], 'an empty mask group')
  assert.deepEqual(unremovable(group('q 100 40 200 120 re W n /Sh0 sh Q')), [none], 'a ramp under a rectangle smaller than the group box')
  // Clips a fade's writer adds without meaning a shape: a rectangle larger
  // than the group's box, and the box itself under a rotated matrix.
  assert.deepEqual(unremovable(group('q -50 -50 500 300 re W n /Sh0 sh Q')), [none], 'a ramp under a rectangle larger than the group box')
  assert.deepEqual(unremovable(group('/Sh0 sh', '/BBox [0 -400 400 400] /Matrix [0.8 0.6 -0.6 0.8 0 0]')), [none], 'a ramp in a group with a rotated matrix')
  // A group's names are read against its own resources.
  const nameless = onePagePdf(`q /GS0 gs 0 0 1 rg ${band} Q`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Group << /S /Transparency /CS /DeviceGray >>', '/Nowhere Do'),
  ])
  assert.deepEqual(unremovable(nameless), [counts], 'a mask group naming a resource nothing defines')
  // A group whose place on the page is not finite.
  const huge = `1${'0'.repeat(30)}.0`
  const adrift = onePagePdf(`q ${huge} 0 0 ${huge} 0 0 cm ${huge} 0 0 ${huge} 0 0 cm /GS0 gs 0 0 1 rg 0 0 1 1 re f Q`, { resources: '/ExtGState << /GS0 << /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 6 0 R >> >> >>' }, [
    maskGroup('1 g BT /F1 12 Tf 165 100 Td (MASKSECRET) Tj ET'),
  ])
  assert.ok(unremovablePointer(adrift) === 0 || unremovable(adrift)[0][2] > 0, 'a mask group set under a matrix that is not finite')
}

// counts a shading PDFium cannot load, and a picture by its own box
{
  const none = [0, 0, 0, 0]
  const band = '20 50 360 100 re f'
  const unloadable = onePagePdf(`q /Pattern cs /P0 scn ${band} Q`, { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading << /ShadingType 2 /Coords [0 0 400 0] >> >> >>' })
  assert.deepEqual(unremovable(unloadable), [[0, 1, 0, 0]], 'a shading pattern that does not load')
  const badBox = onePagePdf(`q /Pattern cs /P0 scn ${band} Q`, { resources: '/Pattern << /P0 << /Type /Pattern /PatternType 2 /Shading 6 0 R >> >>' }, [
    streamObject(
      '/ShadingType 4 /ColorSpace /DeviceRGB /BBox [0 0 1] /BitsPerCoordinate 8 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 400 0 200 0 1 0 1 0 1]',
      Buffer.from([0, 96, 77, 255, 0, 0, 0, 159, 77, 0, 255, 0, 0, 128, 179, 0, 0, 255]),
    ),
  ])
  assert.deepEqual(unremovable(badBox), [[0, 1, 0, 0]], 'a mesh whose /BBox is not four numbers')
  // A function-based shading paints only inside its domain. Its clip's box
  // meets the region and its domain does not: nothing of it is under the
  // mark, so it does not count, and the apply clips it.
  const elsewhere = onePagePdf('q 20 20 360 160 re W n /Sh0 sh Q', {
    resources: '/Shading << /Sh0 << /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [60 0 0 60 300 70] /Function 6 0 R >> >>',
  }, [streamObject('/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]', '{ 2 copy add 2 div }', true)])
  assert.deepEqual(unremovable(elsewhere), [none], 'a function-based shading whose domain lies beside the mark')
  assert.equal(tryApply(elsewhere).result, 1)
}

// counts drawing in a loupe's copy of the page
{
  // A loupe keeps a copy of its page. One made before an apply still holds
  // what lay under the mark, the drawing as well as the text, whatever part
  // of the page its lens shows.
  const star = '0 g 200 130 m 215 105 l 240 100 l 220 85 l 228 62 l 200 75 l 172 62 l 180 85 l 160 100 l 185 105 l h f'
  const loupeOn = (doc, page, content) => {
    const loupe = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
    const appearance = newAppearance(doc, [260, 150, 390, 195], content, false)
    withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(appearance, name, 0), 1))
    assert.equal(m._INK_Annot_SetAppearance(loupe, appearance), 1)
    m._FPDFPage_CloseAnnot(loupe)
  }
  const lensOnStar = 'q 0.3 0 0 0.3 260 150 cm /P0 Do Q'
  const lensElsewhere = 'q 1 0 0 1 260 150 cm /P0 Do Q'
  const copiedBefore = (content, lens) =>
    editPage(onePagePdf(`${content} BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`, { resources: `/Shading << /Sh0 << ${axial} >> >>` }), (doc, page) => {
      loupeOn(doc, page, lens)
      const mark = addMark(page, PATTERN_MARK)
      assert.equal(applyPage(doc, page, [mark], false).result, 1)
      m._FPDFPage_CloseAnnot(mark)
    })
  for (const [name, content, lens] of [
    ['a star, the lens on it', star, lensOnStar],
    ['a star, the lens elsewhere', star, lensElsewhere],
    ['an axial shading', 'q 160 70 80 60 re W n /Sh0 sh Q', lensOnStar],
  ]) {
    const after = under(copiedBefore(content, lens))
    assert.ok(after.paths > 0, `${name} in a page copy made before the apply: ${JSON.stringify(after)}`)
  }
  // A copy made after the apply holds the redacted page, its overlay
  // included, and reads clean.
  const copiedAfter = editPage(onePagePdf(`${star} BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`), (doc, page) => {
    const mark = addMark(page, PATTERN_MARK)
    const overlay = newAppearance(doc, [0, 0, 100, 80], '0 g 0 0 100 80 re f', false)
    assert.equal(m._INK_Redact_SetOverlay(doc, mark, overlay), 1)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
    loupeOn(doc, page, lensOnStar)
  })
  assert.deepEqual(zeros(under(copiedAfter)), [0, 0, 0, 0, 0, 0], `a page copy made after the apply: ${JSON.stringify(under(copiedAfter))}`)
}

// reads Type 3 text in every mode but the invisible one
{
  // PDFium and other readers paint Type 3 text with its fill colour whatever
  // the rendering mode, except mode 3.
  const wide = [
    '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 3000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 /Widths [100] >>',
    streamObject('', '100 0 0 0 100 100 d1 q 3000 0 0 1000 0 0 cm BI /IM true /W 8 /H 8 /BPC 1 /D [1 0] /F /AHx ID 8142241818244281> EI Q', true),
  ]
  const shown = (mode) => onePagePdf(`BT /T3 40 Tf ${mode} Tr 100 80 Td (a) Tj ET`, { resources: '/Font << /T3 6 0 R >>' }, wide)
  assert.deepEqual(unremovable(shown(7)), [[0, 0, 0, 1]], 'an out-of-box glyph shown in mode 7')
  assert.ok(under(shown(7)).textChars > 0, 'an out-of-box glyph shown in mode 7, in the inspector')
  assert.deepEqual(unremovable(shown(3)), [[0, 0, 0, 0]], 'an out-of-box glyph shown in mode 3')
  assert.equal(under(shown(3)).textChars, 0, 'an out-of-box glyph shown in mode 3, in the inspector')
  // A procedure whose paint has no bound reaches every mark its box misses.
  const procFont = (proc, resources) => [
    `<< /Type /Font /Subtype /Type3 /FontBBox [0 0 100 100] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [97 /a] >> /FirstChar 97 /LastChar 97 /Widths [100] /Resources << ${resources} >> >>`,
    streamObject('', proc, true),
    streamObject(`/Type /XObject /Subtype /Form /Resources << /Shading << /Sh0 << ${axial} >> >> >>`, '/Sh0 sh'),
  ]
  const far = (font) => onePagePdf('BT /T3 40 Tf 20 20 Td (a) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, font)
  assert.deepEqual(unremovable(far(procFont('100 0 d0 /Sh0 sh', `/Shading << /Sh0 << ${axial} >> >>`))), [[0, 0, 0, 1]], 'a glyph painting a shading with no box of its own')
  assert.deepEqual(unremovable(far(procFont('100 0 d0 /Fm0 Do', '/XObject << /Fm0 8 0 R >>'))), [[0, 0, 0, 1]], 'a glyph drawing a form without /BBox')
}

// refuses a ramp painted where its holder has no box
{
  // PDFium gives such a shading an empty rectangle and paints nothing; the
  // apply does not clip it; a reader that applies no clip paints all of it.
  const radial = '/ShadingType 3 /ColorSpace /DeviceRGB /Coords [200 100 0 200 100 30] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >>'
  const inForm = (box) =>
    onePagePdf('/Fm0 Do BT /F1 12 Tf 20 20 Td (KEEP) Tj ET', { resources: '/XObject << /Fm0 6 0 R >>' }, [
      streamObject(`/Type /XObject /Subtype /Form ${box} /Resources << /Shading << /Sh0 << ${radial} >> >> >>`, '/Sh0 sh'),
    ])
  assert.deepEqual(unremovable(inForm('')), [[0, 1, 0, 0]], 'a radial ramp in a form without /BBox')
  assert.equal(tryApply(inForm('')).result, -1)
  assert.deepEqual(unremovable(inForm('/BBox [160 70 240 130]')), [[0, 0, 0, 0]], 'a radial ramp in a form with a /BBox')
  const applied = tryApply(inForm('/BBox [160 70 240 130]'))
  assert.equal(applied.result, 1)
  assert.equal(under(applied.bytes).paths, 0, 'a ramp in a form with a /BBox left under the mark')
}

// keeps its arithmetic where a matrix is very small or very large
{
  const solid = tiling([0, 0, 10, 10], 10, 10, '0 0 1 rg 0 0 9.75 10 re f')
  // Written as reals: PDFium clamps an integer that long.
  const e27 = (n) => `${n}${'0'.repeat(26)}.0`
  const shrink = Array(5).fill('0.00001 0 0 0.00001 0 0 cm').join(' ')
  // Under a scale of 1e-25 a line at x 300 with a width of 300 on the page:
  // its stroke covers the mark, its outline lies beside it.
  const tiny = onePagePdf(`q /Pattern CS /P0 SCN ${shrink} ${e27(30)} w ${e27(30)} ${e27(6)} m ${e27(30)} ${e27(14)} l S Q`, { resources: '/Pattern << /P0 6 0 R >>' }, [solid])
  assert.ok(unremovablePointer(tiny) === 0 || unremovable(tiny)[0][0] > 0, 'a wide pattern stroke under a tiny matrix passed the gate')
  assert.equal(tryApply(tiny).result, -1)
  const tinyBlack = onePagePdf(`q 0 G ${shrink} ${e27(30)} w ${e27(30)} ${e27(6)} m ${e27(30)} ${e27(14)} l S Q BT /F1 12 Tf 20 20 Td (KEEP) Tj ET`)
  assert.ok(withDoc(tinyBlack, (doc) => inspectPointer(doc, 0, [quadOf(PATTERN_MARK)])) === 0 || under(tinyBlack).paths > 0, 'a wide black stroke under a tiny matrix was not seen')
  // A large matrix on a path away from the mark does not stop the apply.
  const large = onePagePdf(`${textContent} q 5000000000 0 0 5000000000 0 0 cm 0 G 0.0000000002 w 0.00000001 0.000000036 m 0.00000002 0.000000036 l S Q`)
  const applied = tryApply(large, SECRET_RECT)
  assert.equal(applied.result, 1, 'a path under a large matrix, away from the mark, stopped the apply')
  assert.equal(under(applied.bytes, SECRET_RECT).textChars, 0)
}

// closes the last gaps in the gate's checks
{
  const none = [0, 0, 0, 0]
  const band = '20 50 360 100 re f'
  const patterned = (content) => onePagePdf(`q ${content} Q`, { resources: '/Pattern << /P0 6 0 R >>' }, [hatchCell])
  // A square cap reaches half a line width past the end of its line.
  assert.deepEqual(unremovable(patterned('/Pattern CS /P0 SCN 2 J 20 w 100 100 m 145 100 l S')), [[1, 0, 0, 0]], 'the square cap of a pattern-stroked line')
  assert.deepEqual(unremovable(patterned('/Pattern CS /P0 SCN 0 J 20 w 100 100 m 145 100 l S')), [none], 'the butt end of a pattern-stroked line')
  // Rectangular clips do not make a shape of a fade.
  const fadeGroup = (content) => maskedPage(band, maskGroup(content, `/Shading << /Sh0 << ${axial} >> >>`))
  assert.deepEqual(unremovable(fadeGroup('q 100 40 200 120 re W n 150 20 200 160 re W n /Sh0 sh Q')), [none], 'a ramp under two crossing rectangular clips')
  assert.deepEqual(unremovable(fadeGroup('q 0.8 0.6 -0.6 0.8 100 -40 cm 100 60 80 40 re W n Q q 0.8 0.6 -0.6 0.8 100 -40 cm 100 60 80 40 re W n /Sh0 sh Q')), [none], 'a ramp under a small rotated rectangular clip')
  // Without regions the overlay tag is not trusted, in a page copy either.
  const copied = editPage(onePagePdf('0 g 200 130 m 240 100 l 200 70 l 160 100 l h f BT /F1 12 Tf 20 20 Td (KEEP) Tj ET'), (doc, page) => {
    const mark = addMark(page, PATTERN_MARK)
    const overlay = newAppearance(doc, [0, 0, 100, 80], '0 g 0 0 100 80 re f', false)
    assert.equal(m._INK_Redact_SetOverlay(doc, mark, overlay), 1)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
    const loupe = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
    const appearance = newAppearance(doc, [260, 150, 390, 195], 'q 0.3 0 0 0.3 260 150 cm /P0 Do Q', false)
    withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(appearance, name, 0), 1))
    assert.equal(m._INK_Annot_SetAppearance(loupe, appearance), 1)
    m._FPDFPage_CloseAnnot(loupe)
    const later = addMark(page, PATTERN_MARK)
    m._FPDFPage_CloseAnnot(later)
  })
  assert.deepEqual(zeros(under(copied)), [0, 0, 0, 0, 0, 0], 'the page and its copy with regions given')
  const candidate = withDoc(copied, (doc) => inspectPage(doc, 0, [])).find((c) => c.kind === 'redact-annotation')
  assert.ok(candidate && candidate.paths >= 2, `the overlay on the page and in its copy, without regions: ${JSON.stringify(candidate)}`)
}

// ends when patterns paint with patterns many times over
{
  // Eight patterns, each cell painting eight rectangles with the next: read
  // naively, eight to the eighth cells.
  const depth = 8
  const cells = Array.from({ length: depth }, (_, k) =>
    tiling(
      [0, 0, 400, 200],
      400,
      200,
      k + 1 < depth ? `/Pattern cs /Pn scn ${Array.from({ length: 8 }, (_, i) => `${i * 40} 0 40 200 re f`).join(' ')}` : '0 0 400 200 re f',
      '/PaintType 1',
      k + 1 < depth ? `/Pattern << /Pn ${7 + k} 0 R >>` : '',
    ),
  )
  const source = patternPage('20 50 360 100 re f', cells)
  const started = Date.now()
  withDoc(source, (doc) => inspectPointer(doc, 0, [quadOf(PATTERN_MARK)]))
  assert.ok(Date.now() - started < 5000, 'the inspection took more than 5 s')
}

// does not find what a cut left behind under a box: a known limit
{
  // A file another tool redacted by cutting a hole in a pattern fill and
  // drawing a black box into it. The pattern's cell still draws under the
  // box. The open check answers what is painted under a box, and nothing is:
  // it does not find the content of a pattern, a mask group or a shading
  // whose painted area was cut away from the box.
  const cut = patternPage('20 50 360 100 re 150 60 100 80 re f* 0 g 150 60 100 80 re f', [pageCell])
  const box = withDoc(cut, (doc) => inspectPage(doc, 0, [])).find((c) => c.kind === 'filled-box')
  assert.ok(box, 'no black box candidate')
  assert.deepEqual(zeros(box), [0, 0, 0, 0, 0, 0], `under a black box set into a cut fill: ${JSON.stringify(box)}`)
}

// leaves bitmap-font text to the text redactor
{
  // Each glyph is an image in a Type 3 procedure, as older TeX output draws
  // text. A glyph whose box meets the mark is removed with its text, and its
  // procedure with it when nothing else shows it, so it is not refused.
  const bitmap = (hex) => streamObject('', `1000 0 0 0 1000 1000 d1 q 1000 0 0 1000 0 0 cm BI /IM true /W 8 /H 8 /BPC 1 /D [1 0] /F /AHx ID ${hex}> EI Q`, true)
  const font = (procs, resources = '') =>
    `<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << ${procs} >> /Encoding << /Type /Encoding /Differences [97 /a /b] >> /FirstChar 97 /LastChar 98 /Widths [1000 1000] ${resources} >>`
  const page = (objects) => onePagePdf('BT /T3 40 Tf 180 80 Td (a) Tj ET BT /T3 40 Tf 300 80 Td (b) Tj ET BT /F1 12 Tf 20 20 Td (KEEP) Tj ET', { resources: '/Font << /T3 6 0 R >>' }, objects)
  const source = page([font('/a 7 0 R /b 8 0 R'), bitmap('8142241818244281'), bitmap('FF818181818181FF')])
  assert.deepEqual(unremovable(source, [PATTERN_MARK, [290, 70, 350, 130]]), [[0, 0, 0, 0], [0, 0, 0, 0]], 'bitmap-font text under a mark')
  const saved = inflateStreams(forceApply(source))
  assert.ok(inflateStreams(source).includes('8142241818244281'))
  assert.ok(!saved.includes('8142241818244281'), 'the image of the glyph under the mark is still in the file')
  assert.ok(!/(\(a\)|<61>)\s*Tj/.test(saved), 'the text under the mark is still shown')
  assert.ok(saved.includes('FF818181818181FF') && /(\(b\)|<62>)\s*Tj/.test(saved), 'the glyph beside the mark is gone')
  // The same for a glyph whose procedure paints with a pattern or a shading
  // and is shown only under the mark: the apply removes the procedure.
  for (const [name, proc, resources, extra, trace] of [
    ['a pattern', '1000 0 d0 /Pattern cs /P0 scn 0 0 987.5 1000 re f', '/Resources << /Pattern << /P0 9 0 R >> >>', tiling([0, 0, 10, 10], 10, 10, '0 G 1 w 0 0 m 9.75 10 l S'), '9.75 10 l'],
    ['a shading', '1000 0 d0 q 0 0 987.5 1000 re W n /Sh0 sh Q', '/Resources << /Shading << /Sh0 9 0 R >> >>', meshShading, '/BitsPerFlag'],
  ]) {
    const carrying = page([font('/a 7 0 R /b 8 0 R', resources), streamObject('', proc, true), bitmap('FF818181818181FF'), extra])
    assert.ok(inflateStreams(carrying).includes(trace) && inflateStreams(carrying).includes('987.5'))
    assert.deepEqual(unremovable(carrying), [[0, 0, 0, 0]], `a glyph painting with ${name} under a mark`)
    const after = inflateStreams(forceApply(carrying))
    assert.ok(!after.includes('987.5'), `the procedure painting with ${name} is still in the file`)
    assert.ok(!after.includes(trace), `${name} of a glyph shown only under the mark is still in the file`)
  }
}

// Other content defined once and painted through a reference: what an apply
// over it leaves in the saved file, and what the check then counts.
{
  const keep = 'BT /F1 12 Tf 20 20 Td (KEEP) Tj ET'
  const pixels = Buffer.alloc(16 * 16 * 3)
  for (let i = 0; i < 256; i++) pixels.set([(i % 16) * 16, Math.floor(i / 16) * 16, 128], i * 3)
  const type3 = [
    '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R /b 8 0 R >> /Encoding << /Type /Encoding /Differences [97 /a /b] >> /FirstChar 97 /LastChar 98 /Widths [1000 1000] >>',
    streamObject('', '1000 0 0 0 1000 1000 d1 111.5 222.5 m 900 100 l 500 933.5 l h f'),
    streamObject('', '1000 0 0 0 1000 1000 d1 100 100 800 800 re f'),
  ]
  // Each: a name, a page, what must be gone from the saved file, and what may stay.
  const cases = [
    ['a Type 3 glyph used only under the mark', onePagePdf(`BT /T3 40 Tf 180 80 Td (a) Tj ET BT /T3 40 Tf 300 80 Td (b) Tj ET ${keep}`, { resources: '/Font << /T3 6 0 R >>' }, type3), ['111.5 222.5'], []],
    // The glyph the page still shows beside the mark keeps its procedure.
    ['a Type 3 glyph used under the mark and beside it', onePagePdf(`BT /T3 40 Tf 205 80 Td (a) Tj ET BT /T3 40 Tf 300 80 Td (a) Tj ET ${keep}`, { resources: '/Font << /T3 6 0 R >>' }, type3), [], ['111.5 222.5']],
    // The use beside the mark still shows the form's text.
    [
      'a form painted under the mark and beside it',
      onePagePdf(`q 1 0 0 1 160 90 cm /Fm0 Do Q q 1 0 0 1 20 160 cm /Fm0 Do Q ${keep}`, { resources: '/XObject << /Fm0 6 0 R >>' }, [
        streamObject('/Type /XObject /Subtype /Form /BBox [0 0 100 30] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 12 Tf 5 10 Td (FORMSECRET) Tj ET'),
      ]),
      [],
      ['FORMSECRET'],
    ],
    [
      'a transparency group form',
      onePagePdf(`/Fm0 Do ${keep}`, { resources: '/XObject << /Fm0 6 0 R >>' }, [
        streamObject(
          '/Type /XObject /Subtype /Form /BBox [0 0 400 200] /Group << /S /Transparency /CS /DeviceRGB /I true >> /PieceInfo << /App << /Private (PRIVATESECRET) >> >> /Resources << /Font << /F1 5 0 R >> >>',
          'BT /F1 12 Tf 165 100 Td (GROUPSECRET) Tj 150 0 Td (GROUPKEEP) Tj ET',
        ),
      ]),
      ['GROUPSECRET', 'PRIVATESECRET'],
      ['GROUPKEEP'],
    ],
    [
      'text on a hidden layer',
      onePagePdf(`/OC /MC0 BDC BT /F1 12 Tf 165 100 Td (HIDDENSECRET) Tj ET EMC ${keep}`, { resources: '/Properties << /MC0 6 0 R >>', catalog: '/OCProperties << /OCGs [6 0 R] /D << /OFF [6 0 R] /Order [6 0 R] >> >>' }, [
        '<< /Type /OCG /Name (Hidden) >>',
      ]),
      ['HIDDENSECRET'],
      [],
    ],
    // The annotation beside the mark still shows the appearance.
    [
      'an appearance two annotations share',
      onePagePdf(keep, { page: '/Annots [6 0 R 7 0 R]' }, [
        '<< /Type /Annot /Subtype /Stamp /Rect [160 90 240 120] /AP << /N 8 0 R >> >>',
        '<< /Type /Annot /Subtype /Stamp /Rect [20 160 100 190] /AP << /N 8 0 R >> >>',
        streamObject('/Type /XObject /Subtype /Form /BBox [0 0 80 30] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 12 Tf 5 10 Td (APSECRET) Tj ET'),
      ]),
      [],
      ['APSECRET'],
    ],
    ['replacement text on a span', onePagePdf(`/Span << /ActualText (ACTUALSECRET) /Alt (ALTSECRET) >> BDC BT /F1 12 Tf 165 100 Td (xx) Tj ET EMC ${keep}`), ['ACTUALSECRET', 'ALTSECRET'], []],
    ['replacement text on a span partly under the mark', onePagePdf(`/Span << /ActualText (PARTSECRET) >> BDC BT /F1 12 Tf 230 100 Td (xxxxxxxx) Tj ET EMC ${keep}`), ['PARTSECRET'], []],
    [
      'replacement text in /Properties',
      onePagePdf(`/Span /MC0 BDC BT /F1 12 Tf 165 100 Td (xx) Tj ET EMC ${keep}`, { resources: '/Properties << /MC0 << /ActualText (PROPSECRET) /Alt (PROPALT) >> >>' }),
      ['PROPSECRET', 'PROPALT'],
      [],
    ],
    [
      'replacement text on a structure element',
      onePagePdf(`/P << /MCID 0 >> BDC BT /F1 12 Tf 165 100 Td (xx) Tj ET EMC ${keep}`, { catalog: '/StructTreeRoot 6 0 R /MarkInfo << /Marked true >>', page: '/StructParents 0' }, [
        '<< /Type /StructTreeRoot /K 7 0 R /ParentTree << /Nums [0 [7 0 R]] >> >>',
        '<< /Type /StructElem /S /P /P 6 0 R /Pg 3 0 R /K 0 /ActualText (STRUCTSECRET) /Alt (STRUCTALT) >>',
      ]),
      ['STRUCTSECRET', 'STRUCTALT'],
      [],
    ],
    [
      'a field value',
      onePagePdf(keep, { catalog: '/AcroForm << /Fields [6 0 R] >>', page: '/Annots [6 0 R]' }, [
        '<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (FIELDSECRET) /Rect [160 90 240 120] /DA (/F1 12 Tf 0 g) >>',
      ]),
      ['FIELDSECRET'],
      [],
    ],
    [
      'a note and its popup',
      onePagePdf(keep, { page: '/Annots [6 0 R 7 0 R]' }, [
        '<< /Type /Annot /Subtype /Text /Rect [160 90 180 110] /Contents (NOTESECRET) /Popup 7 0 R >>',
        '<< /Type /Annot /Subtype /Popup /Rect [300 150 380 190] /Parent 6 0 R /Contents (POPSECRET) >>',
      ]),
      ['NOTESECRET', 'POPSECRET'],
      [],
    ],
    [
      'an attached file',
      onePagePdf(keep, { page: '/Annots [6 0 R]' }, [
        '<< /Type /Annot /Subtype /FileAttachment /Rect [160 90 180 110] /FS << /Type /Filespec /F (a.txt) /EF << /F 7 0 R >> >> >>',
        streamObject('/Type /EmbeddedFile', 'ATTACHSECRET', true),
      ]),
      ['ATTACHSECRET'],
      [],
    ],
    [
      'an image with an alternate, metadata and private data',
      onePagePdf(`q 360 0 0 100 20 50 cm /Im0 Do Q ${keep}`, { resources: '/XObject << /Im0 6 0 R >>' }, [
        streamObject(
          '/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Alternates [<< /Image 7 0 R /DefaultForPrinting true >>] /Metadata 8 0 R /PieceInfo << /App << /Private (IMAGEPRIVATESECRET) >> >>',
          pixels,
        ),
        streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', Buffer.from('ALTIMAGESECRET'.repeat(60)).subarray(0, 768), true),
        streamObject('/Type /Metadata /Subtype /XML', '<x>IMAGEMETASECRET</x>', true),
      ]),
      ['ALTIMAGESECRET', 'IMAGEMETASECRET', 'IMAGEPRIVATESECRET'],
      [],
    ],
    [
      'a page thumbnail and private data',
      onePagePdf(`BT /F1 12 Tf 165 100 Td (xx) Tj ET ${keep}`, { page: '/Thumb 6 0 R /PieceInfo << /App << /Private (PAGEPRIVATESECRET) >> >>' }, [
        streamObject('/Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', Buffer.from('THUMBSECRET'.repeat(70)).subarray(0, 768), true),
      ]),
      ['THUMBSECRET', 'PAGEPRIVATESECRET'],
      [],
    ],
    ['text used as a clip', onePagePdf(`q BT /F1 40 Tf 7 Tr 160 80 Td (CLIP) Tj ET 0 0 1 rg 0 0 400 200 re f Q ${keep}`), ['CLIP'], []],
  ]
  const wrong = []
  for (const [name, source, gone, stays] of cases) {
    for (const trace of [...gone, ...stays]) assert.ok(shows(inflateStreams(source), trace), `${name}: the source lacks ${trace}`)
    const applied = editPage(source, (doc, page) => {
      const mark = addMark(page, PATTERN_MARK)
      assert.equal(applyPage(doc, page, [mark], false).result, 1)
      m._FPDFPage_CloseAnnot(mark)
    })
    const saved = inflateStreams(applied)
    const [after] = withDoc(applied, (doc) => inspectPage(doc, 0, [PATTERN_MARK]))
    for (const trace of gone) if (shows(saved, trace)) wrong.push(`${name}: ${trace} is still in the file`)
    for (const trace of stays) if (!shows(saved, trace)) wrong.push(`${name}: ${trace} is gone`)
    if (after.textChars + after.imagesNotBlank + after.paths + after.patternContent + after.annotationText + after.annotationImages) wrong.push(`${name}: ${JSON.stringify(after)}`)
    assert.ok(shows(saved, 'KEEP'), `${name}: the kept text is gone`)
  }
  assert.deepEqual(wrong, [], 'what an apply left behind')
}

// Leaves nothing in the saved file of a resource whose only use lay wholly
// under a mark: the apply removes the object, and the rewrite must not write
// the definition the page's /Resources named. For the kinds the apply does
// not refuse: an image, a form, a plain ramp.
{
  const keep = 'BT /F1 12 Tf 20 20 Td (KEEP) Tj ET'
  const secretPixels = Buffer.from('IMGSECRET'.repeat(86)).subarray(0, 16 * 16 * 3)
  const kinds = [
    [
      'an image',
      onePagePdf(`q 80 0 0 60 160 70 cm /Im0 Do Q ${keep}`, { resources: '/XObject << /Im0 6 0 R >>' }, [
        streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', secretPixels),
      ]),
      // The apply leaves a blanked copy of the image in place.
      ['IMG', 'SECRET'],
    ],
    [
      'a form holding text',
      onePagePdf(`/Fm0 Do ${keep}`, { resources: '/XObject << /Fm0 6 0 R >>' }, [
        streamObject('/Type /XObject /Subtype /Form /BBox [160 70 240 130] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 12 Tf 165 100 Td (FORMSECRET) Tj ET'),
      ]),
      // The apply leaves an emptied copy of the form in place.
      ['FORMSECRET'],
    ],
    [
      'an axial shading',
      onePagePdf(`q 160 70 80 60 re W n /Sh0 sh Q ${keep}`, { resources: '/Shading << /Sh0 6 0 R >>' }, [
        '<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 399.5 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >>',
      ]),
      ['399.5', '/ShadingType'],
    ],
    [
      'an image the page inherits from its parent',
      buildPdf([
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /Font << /F1 5 0 R >> /XObject << /Im0 6 0 R >> >> >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R >>',
        streamObject('', `q 80 0 0 60 160 70 cm /Im0 Do Q ${keep}`),
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
        streamObject('/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', secretPixels),
      ]),
      ['IMG', 'SECRET'],
    ],
  ]
  const left = []
  for (const [name, source, traces] of kinds) {
    assert.ok(inflateStreams(source).includes(traces[0]), `${name}: the source lacks ${traces[0]}`)
    const saved = inflateStreams(
      editPage(source, (doc, page) => {
        const mark = addMark(page, PATTERN_MARK)
        assert.equal(applyPage(doc, page, [mark], false).result, 1)
        m._FPDFPage_CloseAnnot(mark)
      }),
    )
    const found = traces.filter((trace) => shows(saved, trace))
    if (found.length) left.push(`${name}: ${found.join(', ')}`)
    assert.ok(shows(saved, 'KEEP'), `${name}: the kept text is gone`)
  }
  assert.deepEqual(left, [], 'resources left in the saved file after their only use was removed')
}

// a region too small to shrink still counts the path under it
{
  const source = onePagePdf('0 0 1 rg 50 50 100 100 re f')
  const [tiny] = withDoc(source, (doc) => inspectPage(doc, 0, [[70, 70, 70.015, 70.015]]))
  assert.equal(tiny.paths, 1, `path under a tiny region: ${JSON.stringify(tiny)}`)
}

// counts a glyph whose own box misses the region when its extracted box meets it
{
  // A Type3 glyph whose bounding box is a 0.2 pt square at its origin, outside
  // the region, while it advances 20 pt into it.
  const source = onePagePdf(
    'BT /T3 20 Tf 40 100 Td (A) Tj ET',
    { resources: '/Font << /F1 5 0 R /T3 6 0 R >>' },
    [
      '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [65 /a] >> /FirstChar 65 /LastChar 65 /Widths [1000] /Resources << >> >>',
      streamObject('', '1000 0 0 0 10 10 d1 0 0 10 10 re f'),
    ],
  )
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [SECRET_RECT]))
  assert.ok(region.textChars > 0, `a glyph that advances into the region: ${JSON.stringify(region)}`)
}

// shrinks a rotated thin region by 1 pt perpendicular to each edge
{
  // A 100 x 4 pt quad turned 45 degrees about (200, 100). A Type3 glyph's
  // ink lies outside it; the corner of its advance box reaches 0.5 pt into
  // the quad across its long edge, so it misses the quad shrunk by 1 pt.
  const s = Math.SQRT1_2
  const at = (u, v) => [200 + u * s - v * s, 100 + u * s + v * s]
  const quad = [...at(-50, 2), ...at(50, 2), ...at(-50, -2), ...at(50, -2)]
  const source = onePagePdf(
    `BT /T3 20 Tf 180 ${100 + 1.5 / s} Td (A) Tj ET`,
    { resources: '/Font << /F1 5 0 R /T3 6 0 R >>' },
    [
      '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /a 7 0 R >> /Encoding << /Type /Encoding /Differences [65 /a] >> /FirstChar 65 /LastChar 65 /Widths [1000] /Resources << >> >>',
      streamObject('', '1000 0 900 900 1000 1000 d1 900 900 100 100 re f'),
    ],
  )
  const [region] = withDoc(source, (doc) => inspectQuads(doc, 0, [quad]))
  assert.equal(region.textChars, 0, `a glyph box 0.5 pt into a rotated thin region: ${JSON.stringify(region)}`)
}

// counts a path that crosses the region where its dash pattern leaves a gap
{
  const source = onePagePdf('[2 200] 0 d 2 w 50 150 m 350 150 l S')
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [[100, 140, 200, 160]]))
  assert.equal(region.paths, 1, `a dashed line through the region: ${JSON.stringify(region)}`)
}

// inspects text inside a tiling pattern
{
  const source = onePagePdf(
    '/Pattern cs /P0 scn 50 50 200 100 re f',
    { resources: '/Pattern << /P0 6 0 R >>' },
    [
      streamObject(
        '/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 50 20] /XStep 50 /YStep 20 /Resources << /Font << /F1 5 0 R >> >>',
        'BT /F1 10 Tf 0 5 Td (TILE) Tj ET',
      ),
    ],
  )
  const region = [60, 60, 100, 100]
  const [before] = withDoc(source, (doc) => inspectPage(doc, 0, [region]))
  assert.ok(before.patternContent > 0, `pattern text under the region: ${JSON.stringify(before)}`)
  assert.equal(before.textChars, 0, 'pattern text counted as page text')
  // The fill does not paint beside itself, so its cell does not count there.
  const [away] = withDoc(source, (doc) => inspectPage(doc, 0, [[300, 60, 340, 100]]))
  assert.equal(away.patternContent, 0, `pattern text where the fill does not paint: ${JSON.stringify(away)}`)
}

// reads tiling patterns inside patterns and inside annotation appearances
{
  const tile = '/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 150 80] /XStep 150 /YStep 80'
  const source = onePagePdf(
    '/Pattern cs /P1 scn 50 50 200 100 re f',
    { resources: '/Pattern << /P1 7 0 R >>', page: '/Annots [8 0 R]' },
    [
      streamObject(`${tile} /Resources << /Font << /F1 5 0 R >> >>`, 'BT /F1 10 Tf 65 65 Td (TILE) Tj 65 0 Td (TILE) Tj ET'),
      streamObject(`${tile} /Resources << /Pattern << /P0 6 0 R >> >>`, '/Pattern cs /P0 scn 0 0 150 80 re f'),
      '<< /Type /Annot /Subtype /Square /Rect [260 50 360 150] /AP << /N 9 0 R >> >>',
      streamObject('/Type /XObject /Subtype /Form /BBox [260 50 360 150] /Resources << /Pattern << /P0 6 0 R >> >>', '/Pattern cs /P0 scn 260 50 100 100 re f'),
    ],
  )
  const [nested, annotation] = withDoc(source, (doc) =>
    inspectPage(doc, 0, [
      [60, 60, 100, 100],
      [280, 60, 320, 100],
    ]),
  )
  assert.ok(nested.patternContent > 0, `text in a pattern inside a pattern: ${JSON.stringify(nested)}`)
  assert.ok(annotation.patternContent > 0, `text in a pattern inside an appearance: ${JSON.stringify(annotation)}`)
}

// reads every appearance state of an annotation
{
  const form = (text) => streamObject('/Type /XObject /Subtype /Form /BBox [60 100 160 120] /Resources << /Font << /F1 5 0 R >> >>', `BT /F1 10 Tf 62 105 Td (${text}) Tj ET`)
  const source = onePagePdf(
    '',
    { page: '/Annots [6 0 R 7 0 R]' },
    [
      '<< /Type /Annot /Subtype /Square /Rect [60 100 160 120] /AP << /N 8 0 R /D 9 0 R >> >>',
      '<< /Type /Annot /Subtype /Widget /FT /Btn /Rect [60 100 160 120] /AS /Off /AP << /N << /Off 8 0 R /On 10 0 R >> >> >>',
      streamObject('/Type /XObject /Subtype /Form /BBox [60 100 160 120]', ''),
      form('DOWN'),
      form('ON'),
    ],
  )
  const [region] = withDoc(source, (doc) => inspectPage(doc, 0, [SECRET_RECT]))
  assert.ok(region.annotationText >= 6, `text in the down and on appearances: ${JSON.stringify(region)}`)
}

// counts only what a candidate box is painted over, and does not trust the overlay tag
{
  const background = onePagePdf(`1 g 0 0 400 200 re f 0 g ${textContent} 0 g 45 95 185 30 re f 0 g 245 95 120 30 re f BT /F1 20 Tf 250 100 Td (LATER) Tj ET`)
  const [white, black, early] = withDoc(background, (doc) => inspectPage(doc, 0, []))
  assert.deepEqual(
    [white.kind, white.textChars, white.paths],
    ['filled-box', 0, 0],
    `the white background has nothing painted before it: ${JSON.stringify(white)}`,
  )
  assert.ok(black.textChars > 0, `text painted before the black box: ${JSON.stringify(black)}`)
  assert.equal(black.paths, 1, 'the background under the black box')
  // KEEP-BETA lies under the third box and was painted before it; LATER was painted after it.
  const [laterOnly] = withDoc(onePagePdf(`0 g 245 95 120 30 re f BT /F1 20 Tf 250 100 Td (LATER) Tj ET`), (doc) => inspectPage(doc, 0, []))
  assert.equal(laterOnly.textChars, 0, `text painted after the box is not under it: ${JSON.stringify(laterOnly)}`)
  assert.ok(early.textChars > 0, 'KEEP-BETA under the third box')
  const hidden = onePagePdf(
    '/Fm0 Do',
    { resources: '/XObject << /Fm0 6 0 R >>' },
    [streamObject('/Type /XObject /Subtype /Form /BBox [0 0 400 200] /INKB_Overlay true /Resources << /Font << /F1 5 0 R >> >>', `${textContent} 0 g 45 95 185 30 re f`)],
  )
  withDoc(hidden, (doc) => {
    const [box] = inspectPage(doc, 0, [])
    assert.ok(box && box.textChars > 0, `text behind a box in a tagged form: ${JSON.stringify(box)}`)
    assert.equal(inspectPage(doc, 0, [SECRET_RECT])[0].textChars, 0, 'regions given: the tag is trusted')
  })
}

// ignores the overlay it tagged
const overlaySaved = editPage(onePagePdf(textContent), (doc, page) => {
  const mark = addMark(page, SECRET_RECT)
  const overlay = newAppearance(doc, [0, 0, 185, 30], '0 g 0 0 185 30 re f 1 g BT /F0 12 Tf 4 10 Td (REDACTED) Tj ET', false)
  withStrings(['F0', 'Helvetica'], (name, base) => assert.equal(m._INK_Appearance_AddFont(overlay, name, base), 1))
  assert.equal(m._INK_Redact_SetOverlay(doc, mark, overlay), 1)
  const square = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
  const notRedact = newAppearance(doc, [0, 0, 10, 10], '', false)
  assert.equal(m._INK_Redact_SetOverlay(doc, square, notRedact), 0, 'an overlay on a square')
  m._FPDFPage_CloseAnnot(square)
  assert.equal(applyPage(doc, page, [mark], false).result, 1)
  m._FPDFPage_CloseAnnot(mark)
})
{
  assert.equal(count(overlaySaved, '/INKB_Overlay true'), 1, 'the flattened overlay lost its tag')
  const [after] = withDoc(overlaySaved, (doc) => inspectPage(doc, 0, [SECRET_RECT]))
  assert.deepEqual(
    [after.textChars, after.paths, after.imagesNotBlank],
    [0, 0, 0],
    `the overlay counted as content: ${JSON.stringify(after)}`,
  )
  // Blanked to the same length, so the cross-reference offsets stay right.
  const tag = '/INKB_Overlay true'
  const untagged = Buffer.from(overlaySaved.toString('latin1').replace(tag, ' '.repeat(tag.length)), 'latin1')
  const [plainOverlay] = withDoc(untagged, (doc) => inspectPage(doc, 0, [SECRET_RECT]))
  assert.ok(plainOverlay.paths > 0 && plainOverlay.textChars > 0, `untagged, the overlay is content: ${JSON.stringify(plainOverlay)}`)
}

// finds a black box over text as a candidate
{
  const source = onePagePdf(`${textContent} 0 g 45 95 185 30 re f`)
  const candidates = withDoc(source, (doc) => inspectPage(doc, 0, []))
  assert.equal(candidates.length, 1, JSON.stringify(candidates))
  const [box] = candidates
  assert.equal(box.kind, 'filled-box')
  assert.deepEqual(box.rect, SECRET_RECT)
  assert.ok(box.textChars > 0, `text under the box: ${JSON.stringify(box)}`)
  assert.equal(box.paths, 0, 'the box counted itself')
}

// reads annotation appearances and a loupe's page copy
{
  const source = onePagePdf(textContent)
  const withAnnots = editPage(source, (doc, page) => {
    // A note over the region, its text in its appearance.
    const note = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
    const noteAp = newAppearance(doc, [60, 100, 120, 120], 'BT /F0 10 Tf 62 105 Td (NOTE) Tj ET', false)
    withStrings(['F0', 'Helvetica'], (name, base) => assert.equal(m._INK_Appearance_AddFont(noteAp, name, base), 1))
    assert.equal(m._INK_Annot_SetAppearance(note, noteAp), 1)
    m._FPDFPage_CloseAnnot(note)
    // A loupe away from the region, drawing the page at 0.375 of its size.
    const loupe = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
    const loupeAp = newAppearance(doc, [250, 0, 400, 75], 'q 0.375 0 0 0.375 250 0 cm /P0 Do Q', false)
    withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(loupeAp, name, 0), 1))
    assert.equal(m._INK_Annot_SetAppearance(loupe, loupeAp), 1)
    m._FPDFPage_CloseAnnot(loupe)
  })
  assert.equal(count(withAnnots, '/INKB_PageCopy true'), 1, 'the page copy is not tagged')
  withDoc(withAnnots, (doc) => {
    const [region] = inspectPage(doc, 0, [SECRET_RECT])
    // NOTE is 4 characters; the page copy holds SECRET-ALPHA in page coordinates.
    assert.ok(region.annotationText > 4, `annotation text under the region: ${JSON.stringify(region)}`)
    const [away] = inspectPage(doc, 0, [[0, 0, 40, 40]])
    assert.equal(away.annotationText, 0, `annotation text away from everything: ${JSON.stringify(away)}`)
  })
}

// reads a loupe's page copy in page space, and a fresh copy after an apply
for (const [label, mediaBox, rotate, at, region, lens] of [
  ['rotated', '[0 0 400 200]', 90, [50, 100], SECRET_RECT, [300, 0, 400, 50]],
  ['offset', '[100 100 500 300]', 0, [150, 200], [145, 195, 330, 225], [400, 100, 500, 150]],
]) {
  const source = onePagePdf(`BT /F1 20 Tf ${at[0]} ${at[1]} Td (SECRET-ALPHA) Tj ET`, { mediaBox, page: `/Rotate ${rotate}` })
  withDoc(source, (doc) => {
    const page = m._FPDF_LoadPage(doc, 0)
    const loupe = m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE)
    const lensAppearance = () => {
      const a = newAppearance(doc, lens, `q 0.25 0 0 0.25 ${lens[0]} ${lens[1]} cm /P0 Do Q`, false)
      withStrings(['P0'], (name) => assert.equal(m._INK_Appearance_AddPage(a, name, 0), 1))
      return a
    }
    assert.equal(m._INK_Annot_SetAppearance(loupe, lensAppearance()), 1)
    const [before] = inspectPage(doc, 0, [region])
    assert.ok(before.textChars > 0, `${label}: page text under the region: ${JSON.stringify(before)}`)
    assert.ok(before.annotationText > 0, `${label}: the loupe's page copy under the region: ${JSON.stringify(before)}`)
    const mark = addMark(page, region)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
    // A loupe drawn after the apply copies the redacted page.
    assert.equal(m._INK_Annot_SetAppearance(loupe, lensAppearance()), 1)
    const [after] = inspectPage(doc, 0, [region])
    assert.deepEqual([after.textChars, after.annotationText], [0, 0], `${label}: text left after the apply: ${JSON.stringify(after)}`)
    m._FPDFPage_CloseAnnot(loupe)
    m._FPDF_ClosePage(page)
  })
}

// sanitises metadata, attachments, scripts, thumbnails and private data
const sanitizeSource = buildPdf(
  [
    '<< /Type /Catalog /Pages 2 0 R /Metadata 6 0 R /Names << /EmbeddedFiles << /Names [(a.txt) 7 0 R] >> /JavaScript << /Names [(js) 9 0 R] >> >> /OpenAction 9 0 R /PieceInfo << /App << /Private (CATALOG-PRIVATE) >> >> /OCProperties << /OCGs [14 0 R] /D << /OFF [14 0 R] >> >> /AcroForm << /Fields [15 0 R] >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> /Thumb 10 0 R /PieceInfo << /App << /Private (PAGE-PRIVATE) >> >> /AA << /O 9 0 R >> /Annots [11 0 R] >>',
    streamObject('', textContent),
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
    streamObject('/Type /Metadata /Subtype /XML', '<x:xmpmeta xmlns:x="adobe:ns:meta/">XMP-SECRET</x:xmpmeta>', true),
    '<< /Type /Filespec /F (a.txt) /EF << /F 8 0 R >> >>',
    streamObject('/Type /EmbeddedFile', 'ATTACHED-SECRET', true),
    '<< /S /JavaScript /JS (app.alert\\(1\\)) >>',
    streamObject('/Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8', Buffer.from([0])),
    '<< /Type /Annot /Subtype /FileAttachment /Rect [0 0 20 20] /FS 12 0 R >>',
    '<< /Type /Filespec /F (b.txt) /EF << /F 13 0 R >> >>',
    streamObject('/Type /EmbeddedFile', 'ANNOT-SECRET', true),
    '<< /Type /OCG /Name (Layer) >>',
    '<< /FT /Sig /T (Signature) /V << /Type /Sig >> >>',
    '<< /Title (INFO-SECRET) >>',
  ],
  '/Info 16 0 R',
)
const sanitizedSaved = withDoc(sanitizeSource, (doc) => {
  assert.deepEqual(inspectFile(doc, sanitizeSource), {
    revisions: 1,
    info: true,
    xmp: true,
    attachments: 2,
    scripts: true,
    hiddenLayers: true,
    signed: true,
  })
  const out = m._malloc(8)
  assert.equal(m._INK_Sanitize(doc, out, out + 4), 1)
  const flags = m.HEAPU32[out / 4]
  const attachments = m.HEAPU32[out / 4 + 1]
  m._free(out)
  assert.equal(flags, 1 | 2 | 4 | 8 | 16, `sanitise flags ${flags}`)
  assert.equal(attachments, 2)
  return save(doc)
})
{
  const inflated = inflateStreams(sanitizedSaved)
  for (const text of ['/Info', '/EmbeddedFiles', '/JavaScript', '/Thumb', '/PieceInfo', '/Metadata', '/FileAttachment', '/OpenAction', '/AA']) {
    assert.ok(!inflated.includes(text), `the sanitised file holds ${text}`)
  }
  for (const text of ['ATTACHED-SECRET', 'ANNOT-SECRET', 'INFO-SECRET', 'XMP-SECRET', 'PRIVATE']) {
    assert.ok(!inflated.includes(text), `the sanitised file holds ${text}`)
  }
  assert.ok(streamTexts(sanitizedSaved).some((t) => t.includes('KEEP-BETA')), 'sanitising lost the page text')
  withDoc(sanitizedSaved, (doc) => {
    assert.deepEqual(inspectFile(doc, sanitizedSaved), {
      revisions: 1,
      info: false,
      xmp: false,
      attachments: 0,
      scripts: false,
      hiddenLayers: true,
      signed: true,
    })
  })
}

// sanitises a direct /Info, XFA, javascript: links and every embedded file, each counted once
{
  const source = buildPdf(
    [
      '<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles << /Names [(a.txt) 7 0 R] >> >> /AcroForm << /Fields [] /XFA 9 0 R >> >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> /Annots [10 0 R 11 0 R 12 0 R] >>',
      streamObject('', textContent),
      '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
      '<< >>',
      '<< /Type /Filespec /F (a.txt) /EF << /F 8 0 R >> >>',
      streamObject('/Type /EmbeddedFile', 'SHARED-SECRET', true),
      streamObject('', '<xdp:xdp>XFA-SECRET</xdp:xdp>', true),
      '<< /Type /Annot /Subtype /FileAttachment /Rect [0 0 20 20] /FS 7 0 R >>',
      '<< /Type /Annot /Subtype /Link /Rect [20 0 40 20] /A << /S /GoToE /T << /R /C /N (x) >> /F 13 0 R >> >>',
      '<< /Type /Annot /Subtype /Link /Rect [40 0 60 20] /A << /S /URI /URI (  JavaScript:app.alert\\(1\\)) >> >>',
      '<< /Type /Filespec /F (b.pdf) /EF << /F 14 0 R >> >>',
      streamObject('/Type /EmbeddedFile', 'GOTOE-SECRET', true),
    ],
    '/Info << /Title (DIRECT-INFO) >>',
  )
  const facts = { revisions: 1, info: true, xmp: false, attachments: 2, scripts: true, hiddenLayers: false, signed: false }
  const saved = withDoc(source, (doc) => {
    assert.deepEqual(inspectFile(doc, source), facts)
    const out = m._malloc(8)
    assert.equal(m._INK_Sanitize(doc, out, out + 4), 1)
    const flags = m.HEAPU32[out / 4]
    const attachments = m.HEAPU32[out / 4 + 1]
    m._free(out)
    assert.equal(flags, 1 | 2 | 4, `sanitise flags ${flags}`)
    assert.equal(attachments, 2, 'the shared file is one attachment')
    return save(doc)
  })
  const inflated = inflateStreams(saved)
  for (const text of ['/Info', 'DIRECT-INFO', '/XFA', 'XFA-SECRET', 'SHARED-SECRET', 'GOTOE-SECRET', 'JavaScript:', '/EF']) {
    assert.ok(!inflated.includes(text), `the sanitised file holds ${text}`)
  }
  withDoc(saved, (doc) => {
    assert.deepEqual(inspectFile(doc, saved), { ...facts, info: false, attachments: 0, scripts: false })
  })
}

// matches javascript: links as a browser does, counts unembedded files alike, and sanitises once
{
  const source = buildPdf(
    [
      '<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles << /Names [(ext.txt) 7 0 R (other.txt) 12 0 R] >> >> >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> /Annots [8 0 R 9 0 R 10 0 R 11 0 R 14 0 R] >>',
      streamObject('', textContent),
      '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
      '<< >>',
      '<< /Type /Filespec /F (ext.txt) >>',
      '<< /Type /Annot /Subtype /FileAttachment /Rect [0 0 20 20] /FS 7 0 R >>',
      '<< /Type /Annot /Subtype /Link /Rect [20 0 40 20] /A << /S /URI /URI (\\001 Jav\\tascript:x) >> >>',
      '<< /Type /Annot /Subtype /Link /Rect [40 0 60 20] /A << /S /URI /URI <FEFF006A006100760061007300630072006900700074003A0078> >> >>',
      '<< /Type /Annot /Subtype /Link /Rect [60 0 80 20] /A << /S /URI /URI (java\\nscript:x) >> >>',
      '<< /Type /Filespec /F (other.txt) >>',
      '<< /Title (ONCE-INFO) >>',
      '<< /Type /Annot /Subtype /Link /Rect [80 0 100 20] /A << /S /URI /URI <FFFE6A006100760061007300630072006900700074003A007800> >> >>',
    ],
    '/Info 13 0 R',
  )
  const sanitized = withDoc(source, (doc) => {
    const facts = inspectFile(doc, source)
    assert.deepEqual([facts.info, facts.attachments, facts.scripts], [true, 2, true], `file facts: ${JSON.stringify(facts)}`)
    const out = m._malloc(8)
    assert.equal(m._INK_Sanitize(doc, out, out + 4), 1)
    assert.deepEqual([m.HEAPU32[out / 4], m.HEAPU32[out / 4 + 1]], [1 | 2 | 4, 2], 'flags and attachments')
    assert.equal(m._INK_Sanitize(doc, out, out + 4), 1)
    assert.deepEqual([m.HEAPU32[out / 4], m.HEAPU32[out / 4 + 1]], [0, 0], 'a second sanitise finds nothing')
    m._free(out)
    return save(doc)
  })
  const inflated = inflateStreams(sanitized)
  assert.ok(!inflated.includes('/URI') && !inflated.includes('ONCE-INFO'), 'script links or /Info left')
}

// reports file facts
{
  const FPDF_INCREMENTAL = 1
  const first = onePagePdf(textContent)
  const second = withDoc(first, (doc) => {
    assert.equal(inspectFile(doc, first).revisions, 1)
    const page = m._FPDF_LoadPage(doc, 0)
    m._FPDFPage_CloseAnnot(m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE))
    m._FPDF_ClosePage(page)
    const writer = m._INK_Save(doc, FPDF_INCREMENTAL)
    assert.notEqual(writer, 0)
    const data = m._INK_WriterData(writer)
    const bytes = Buffer.from(m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
    m._INK_WriterFree(writer)
    return bytes
  })
  assert.equal(count(second, '%%EOF'), 2, 'the incremental save made no second revision')
  // A linearized file (hello_world.pdf through qpdf --linearize) has a second
  // %EOF after its first-page section, and is still one revision.
  const linearized = Buffer.from(
    'JVBERi0xLjcKJb/3ov4KMiAwIG9iago8PCAvTGluZWFyaXplZCAxIC9MIDEzNzcgL0ggWyA1OTAgMTIyIF0gL08gNSAvRSAxMTUyIC9OIDEgL1QgMTIxOSA+PgplbmRvYmoKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAKeHJlZgoyIDgKMDAwMDAwMDAxNSAwMDAwMCBuIAowMDAwMDAwNTQxIDAwMDAwIG4gCjAwMDAwMDA1OTAgMDAwMDAgbiAKMDAwMDAwMDcxMiAwMDAwMCBuIAowMDAwMDAwODQwIDAwMDAwIG4gCjAwMDAwMDA5MTIgMDAwMDAgbiAKMDAwMDAwMDk4MiAwMDAwMCBuIAowMDAwMDAxMTIxIDAwMDAwIG4gCnRyYWlsZXIgPDwgL1Jvb3QgMyAwIFIgL1NpemUgMTAgL1ByZXYgMTIxMSAgICAgICAgICAgICAgICAgIC9JRCBbPDU0NDQ1YWFlNzkwMTVkMGJiYTgyZjk3NGFiMGEwM2RiPjw1NDQ0NWFhZTc5MDE1ZDBiYmE4MmY5NzRhYjBhMDNkYj5dID4+CnN0YXJ0eHJlZgowCiUlRU9GCjMgMCBvYmoKPDwgL1BhZ2VzIDEgMCBSIC9UeXBlIC9DYXRhbG9nID4+CmVuZG9iago0IDAgb2JqCjw8IC9GaWx0ZXIgL0ZsYXRlRGVjb2RlIC9TIDM2IC9MZW5ndGggNDUgPj4Kc3RyZWFtCnicY2BgYGVgYPJjAALGHQxwAGUzAzELQhSkFowZGOQZ2A8ttTvAwAAAZbMEZAplbmRzdHJlYW0KZW5kb2JqCjUgMCBvYmoKPDwgL0NvbnRlbnRzIDggMCBSIC9NZWRpYUJveCA5IDAgUiAvUGFyZW50IDEgMCBSIC9SZXNvdXJjZXMgPDwgL0ZvbnQgPDwgL0YxIDYgMCBSIC9GMiA3IDAgUiA+PiA+PiAvVHlwZSAvUGFnZSA+PgplbmRvYmoKNiAwIG9iago8PCAvQmFzZUZvbnQgL1RpbWVzLVJvbWFuIC9TdWJ0eXBlIC9UeXBlMSAvVHlwZSAvRm9udCA+PgplbmRvYmoKNyAwIG9iago8PCAvQmFzZUZvbnQgL0hlbHZldGljYSAvU3VidHlwZSAvVHlwZTEgL1R5cGUgL0ZvbnQgPj4KZW5kb2JqCjggMCBvYmoKPDwgL0xlbmd0aCA2OSAvRmlsdGVyIC9GbGF0ZURlY29kZSA+PgpzdHJlYW0KeJxzCuEyMlAwNVAISeHSdzNUMDRSCEnj0vBIzcnJ11Eozy/KSVHUVAjJ4kIoMlIwNAMrcs/PT0mqTEVR5hrCBQBPIRUcZW5kc3RyZWFtCmVuZG9iago5IDAgb2JqClsgMCAwIDIwMCAyMDAgXQplbmRvYmoKMSAwIG9iago8PCAvQ291bnQgMSAvS2lkcyBbIDUgMCBSIF0gL1R5cGUgL1BhZ2VzID4+CmVuZG9iagp4cmVmCjAgMgowMDAwMDAwMDAwIDY1NTM1IGYgCjAwMDAwMDExNTIgMDAwMDAgbiAKdHJhaWxlciA8PCAvU2l6ZSAyIC9JRCBbPDU0NDQ1YWFlNzkwMTVkMGJiYTgyZjk3NGFiMGEwM2RiPjw1NDQ0NWFhZTc5MDE1ZDBiYmE4MmY5NzRhYjBhMDNkYj5dID4+CnN0YXJ0eHJlZgoyMTYKJSVFT0YK',
    'base64',
  )
  assert.equal(count(linearized, '%%EOF'), 2)
  const appended = withDoc(linearized, (doc) => {
    assert.equal(inspectFile(doc, linearized).revisions, 1, 'a linearized file is one revision')
    const page = m._FPDF_LoadPage(doc, 0)
    m._FPDFPage_CloseAnnot(m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE))
    m._FPDF_ClosePage(page)
    const writer = m._INK_Save(doc, FPDF_INCREMENTAL)
    assert.notEqual(writer, 0)
    const data = m._INK_WriterData(writer)
    const bytes = Buffer.from(m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
    m._INK_WriterFree(writer)
    return bytes
  })
  withDoc(appended, (doc) => {
    assert.equal(inspectFile(doc, appended).revisions, 2, 'a linearized file with an appended revision')
  })
  // Not linearized, whatever its first object says: a /Linearized key whose
  // /L is the file's own length, and a /LinearizedX key. Each gets one
  // appended update, so each is two revisions.
  const incremental = (bytes) =>
    withDoc(bytes, (doc) => {
      const page = m._FPDF_LoadPage(doc, 0)
      m._FPDFPage_CloseAnnot(m._FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE))
      m._FPDF_ClosePage(page)
      const writer = m._INK_Save(doc, FPDF_INCREMENTAL)
      const data = m._INK_WriterData(writer)
      const out = Buffer.from(m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
      m._INK_WriterFree(writer)
      return out
    })
  const spoofed = (key) => {
    const body = (length) =>
      buildPdf([
        `<< /Type /Catalog /Pages 2 0 R ${key} 1 /L ${String(length).padStart(10, '0')} /H [0 0] /O 3 /E 1 /N 1 /T 1 >>`,
        '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] >>',
      ])
    return body(body(0).length)
  }
  // A /Linearized first object whose /L is set, after a real update, to the
  // final length: its trailer's /Prev points back, so still two revisions.
  {
    const placeholder = '/L 0000000000'
    const original = buildPdf([
      `<< /Type /Catalog /Pages 2 0 R /Linearized 1 ${placeholder} >>`,
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] >>',
    ])
    const updated = incremental(original)
    const crafted = Buffer.from(updated.toString('latin1').replace(placeholder, `/L ${String(updated.length).padStart(10, '0')}`), 'latin1')
    withDoc(crafted, (doc) => {
      assert.equal(inspectFile(doc, crafted).revisions, 2, 'a crafted /L over an update')
    })
  }
  for (const key of ['/Linearized', '/LinearizedX']) {
    const updated = incremental(spoofed(key))
    withDoc(updated, (doc) => {
      assert.equal(inspectFile(doc, updated).revisions, 2, `a first object with ${key} and one update`)
    })
  }
  withDoc(second, (doc) => {
    assert.deepEqual(inspectFile(doc, second), {
      revisions: 2,
      info: false,
      xmp: false,
      attachments: 0,
      scripts: false,
      hiddenLayers: false,
      signed: false,
    })
  })
}

// Page rendering.

// The BGRA bytes of |bitmap|, |height| rows, top row first.
function bitmapBytes(bitmap, height) {
  const data = m._FPDFBitmap_GetBuffer(bitmap)
  return Buffer.from(m.HEAPU8.slice(data, data + m._FPDFBitmap_GetStride(bitmap) * height))
}

// Renders page |index| of |doc| at one pixel per point with FPDF_RenderPageBitmap
// and |flags| onto white.
function renderPixels(doc, index = 0, flags = FPDF_ANNOT) {
  const page = m._FPDF_LoadPage(doc, index)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page))
  const height = Math.round(m._FPDF_GetPageHeightF(page))
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff)
  m._FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, flags)
  const bytes = bitmapBytes(bitmap, height)
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return { bytes, width, height }
}

// The number of pixels that are not white between |bottom| and |top| points.
function inkInBand({ bytes, width, height }, bottom, top) {
  let n = 0
  for (let row = Math.max(height - Math.ceil(top), 0); row < height - Math.floor(bottom); row++) {
    for (let x = 0; x < width; x++) {
      const o = (row * width + x) * 4
      if (bytes[o] < 250 || bytes[o + 1] < 250 || bytes[o + 2] < 250) n++
    }
  }
  return n
}

// The standard fonts draw from PDFium's built-in faces with no font registered:
// one line each of Helvetica, Times, Courier, Symbol and ZapfDingbats.
const STANDARD_FONTS = ['Helvetica', 'Times-Roman', 'Courier', 'Symbol', 'ZapfDingbats']
const standardPdf = buildPdf([
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
  `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 260] /Contents 4 0 R /Resources << /Font << ${STANDARD_FONTS.map((_, i) => `/F${i} ${i + 5} 0 R`).join(' ')} >> >> >>`,
  streamObject('', STANDARD_FONTS.map((_, i) => `BT /F${i} 20 Tf 20 ${20 + i * 50} Td (abcABC) Tj ET`).join(' ')),
  ...STANDARD_FONTS.map((name) => `<< /Type /Font /Subtype /Type1 /BaseFont /${name} >>`),
])
withDoc(standardPdf, (doc) => {
  const pixels = renderPixels(doc)
  STANDARD_FONTS.forEach((name, i) => {
    assert.ok(inkInBand(pixels, 15 + i * 50, 45 + i * 50) > 0, `${name} drew nothing`)
  })
})

// INK_Render_Start, INK_Render_Continue and INK_Render_Close draw what
// FPDF_RenderPageBitmap draws, in slices, and can leave out annotations.

// Renders page |index| through the INK_Render_* calls, |slice| ms a call, onto
// white, with FPDF_RenderPageBitmap's arguments |args| (start_x, start_y,
// size_x, size_y, rotate) and |flags|, leaving out the annotations numbered in
// |skip| and those at the places in /Annots in |skipAt|. Returns the bytes and the statuses INK_Render_Continue returned.
function inkRender(doc, { index = 0, flags = FPDF_ANNOT, skip = [], skipAt = [], slice = 1000, args } = {}) {
  const page = m._FPDF_LoadPage(doc, index)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page))
  const height = Math.round(m._FPDF_GetPageHeightF(page))
  const [startX, startY, sizeX, sizeY, rotate] = args ?? [0, 0, width, height, 0]
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff)
  const skipPtr = skip.length ? m._malloc(skip.length * 4) : 0
  if (skipPtr) m.HEAPU32.set(skip, skipPtr / 4)
  const skipAtPtr = skipAt.length ? m._malloc(skipAt.length * 4) : 0
  skipAt.forEach((at, i) => m.setValue(skipAtPtr + i * 4, at, 'i32'))
  const r = m._INK_Render_Start(bitmap, page, startX, startY, sizeX, sizeY, rotate, flags, skipPtr, skip.length, skipAtPtr, skipAt.length)
  // Start reads the lists before it returns.
  if (skipPtr) m._free(skipPtr)
  if (skipAtPtr) m._free(skipAtPtr)
  assert.notEqual(r, 0, 'INK_Render_Start failed')
  const statuses = []
  for (;;) {
    const status = m._INK_Render_Continue(r, slice)
    statuses.push(status)
    if (status !== 0) break
    assert.ok(statuses.length < 1_000_000, 'the render does not end')
  }
  m._INK_Render_Close(r)
  const bytes = bitmapBytes(bitmap, height)
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return { bytes, width, height, statuses }
}

// FPDF_RenderPageBitmap with the same arguments as inkRender.
function referenceRender(doc, { index = 0, flags = FPDF_ANNOT, args } = {}) {
  const page = m._FPDF_LoadPage(doc, index)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page))
  const height = Math.round(m._FPDF_GetPageHeightF(page))
  const [startX, startY, sizeX, sizeY, rotate] = args ?? [0, 0, width, height, 0]
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xffffffff)
  m._FPDF_RenderPageBitmap(bitmap, page, startX, startY, sizeX, sizeY, rotate, flags)
  const bytes = bitmapBytes(bitmap, height)
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return bytes
}

// Whether two saves wrote the same bytes. A save writes a fresh random second
// /ID string each time, so the /ID arrays are left out of the comparison.
function sameSave(a, b) {
  const unid = (bytes) => bytes.toString('latin1').replace(/\/ID\s*\[\s*<[0-9A-Fa-f]*>\s*<[0-9A-Fa-f]*>\s*\]/g, '/ID[]')
  return unid(a) === unid(b)
}

function saveWith(doc, flags) {
  const writer = m._INK_Save(doc, flags)
  assert.notEqual(writer, 0, 'save failed')
  const data = m._INK_WriterData(writer)
  const bytes = Buffer.from(m.HEAPU8.slice(data, data + m._INK_WriterSize(writer)))
  m._INK_WriterFree(writer)
  return bytes
}

// A page of text and a grey box under a Square annotation, object 6, whose
// appearance (object 7) fills a red box. /F 4 is Print, which the render must
// put back as it was.
const squarePdf = onePagePdf(
  'BT /F1 20 Tf 20 150 Td (Rendered in slices) Tj ET 0.6 g 40 40 120 60 re f',
  { page: '/Annots [6 0 R]' },
  [
    '<< /Type /Annot /Subtype /Square /F 4 /Rect [100 20 300 120] /C [1 0 0] /AP << /N 7 0 R >> >>',
    streamObject('/Type /XObject /Subtype /Form /BBox [100 20 300 120]', '1 0 0 rg 100 20 200 100 re f'),
  ],
)
const SQUARE_OBJ = 6

withDoc(squarePdf, (doc) => {
  // The whole page, and a region that starts off the page and is drawn larger and turned.
  for (const args of [undefined, [-50, -30, 800, 400, 1]]) {
    const reference = referenceRender(doc, { args })
    const sliced = inkRender(doc, { args })
    assert.ok(sliced.bytes.equals(reference), `a render with ${JSON.stringify(args)} differs from FPDF_RenderPageBitmap`)
    assert.equal(sliced.statuses.at(-1), 1)
  }
})

// A page drawing one 2000 x 2000 image pauses when given no time, and ends
// with the bytes of a render at once.
const bigImage = Buffer.alloc(2000 * 2000)
for (let y = 0; y < 2000; y++) for (let x = 0; x < 2000; x++) bigImage[y * 2000 + x] = (x ^ y) & 0xff
const bigImagePdf = onePagePdf('q 400 0 0 400 0 0 cm /Im0 Do Q', { mediaBox: '[0 0 400 400]', resources: '/XObject << /Im0 6 0 R >>' }, [
  streamObject('/Type /XObject /Subtype /Image /Width 2000 /Height 2000 /ColorSpace /DeviceGray /BitsPerComponent 8', bigImage),
])
withDoc(bigImagePdf, (doc) => {
  const reference = referenceRender(doc)
  const sliced = inkRender(doc, { slice: 0 })
  assert.ok(sliced.statuses.includes(0), 'a render given no time never paused')
  assert.equal(sliced.statuses.at(-1), 1)
  assert.ok(sliced.bytes.equals(reference), 'a render in slices differs from a render at once')

  // Closed before it finished, a render leaves the page free: a fresh whole
  // render gives the reference bytes.
  const page = m._FPDF_LoadPage(doc, 0)
  const bitmap = m._FPDFBitmap_CreateEx(400, 400, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, 400, 400, 0xffffffff)
  const r = m._INK_Render_Start(bitmap, page, 0, 0, 400, 400, 0, FPDF_ANNOT, 0, 0, 0, 0)
  assert.notEqual(r, 0)
  assert.equal(m._INK_Render_Continue(r, 0), 0, 'the render ended in its first slice')
  m._INK_Render_Close(r)
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  assert.ok(inkRender(doc).bytes.equals(reference), 'a render after an unfinished one differs')
  assert.ok(referenceRender(doc).equals(reference), 'FPDF_RenderPageBitmap after an unfinished render differs')
})
m._INK_Render_Close(0)

// Leaving out the square draws the page as a render without annotations, and
// a save afterwards writes the bytes of a save of the file never rendered.
{
  const neverRendered = withDoc(squarePdf, (doc) => [saveWith(doc, NO_INCREMENTAL), saveWith(doc, 1)])
  withDoc(squarePdf, (doc) => {
    const withAnnots = referenceRender(doc)
    const withoutAnnots = referenceRender(doc, { flags: 0 })
    assert.ok(!withAnnots.equals(withoutAnnots), 'the square draws nothing')
    const skipped = inkRender(doc, { skip: [SQUARE_OBJ] })
    assert.ok(skipped.bytes.equals(withoutAnnots), 'the left-out square was drawn')
    // An unlisted number leaves the square in.
    assert.ok(inkRender(doc, { skip: [SQUARE_OBJ + 100] }).bytes.equals(withAnnots), 'an unlisted annotation was left out')
    assert.ok(referenceRender(doc).equals(withAnnots), 'the square stays hidden after the render')
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), neverRendered[0]), 'a render changed what a save writes')
    assert.ok(sameSave(saveWith(doc, 1), neverRendered[1]), 'a render changed what an incremental save writes')
  })
  // Saved while a render that leaves the square out is unfinished; the render
  // then ends without the square.
  withDoc(squarePdf, (doc) => {
    const withoutAnnots = referenceRender(doc, { flags: 0 })
    const page = m._FPDF_LoadPage(doc, 0)
    const bitmap = m._FPDFBitmap_CreateEx(400, 200, 4, 0, 0)
    m._FPDFBitmap_FillRect(bitmap, 0, 0, 400, 200, 0xffffffff)
    const skipPtr = m._malloc(4)
    m.HEAPU32[skipPtr / 4] = SQUARE_OBJ
    const r = m._INK_Render_Start(bitmap, page, 0, 0, 400, 200, 0, FPDF_ANNOT, skipPtr, 1, 0, 0)
    m._free(skipPtr)
    assert.notEqual(r, 0)
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), neverRendered[0]), 'a save during a render differs')
    let status
    while ((status = m._INK_Render_Continue(r, 1000)) === 0);
    assert.equal(status, 1)
    m._INK_Render_Close(r)
    assert.ok(bitmapBytes(bitmap, 200).equals(withoutAnnots), 'a render saved during drew the left-out square')
    m._FPDFBitmap_Destroy(bitmap)
    m._FPDF_ClosePage(page)
  })
}

// A stale handle touches no other render. R1 starts; FPDF_RenderPageBitmap on
// the same page ends it; R2 starts. R1 then fails and its Close leaves R2,
// which ends with the bytes of a render at once.
withDoc(bigImagePdf, (doc) => {
  const reference = referenceRender(doc)
  const page = m._FPDF_LoadPage(doc, 0)
  const bitmaps = [0, 1, 2].map(() => {
    const bitmap = m._FPDFBitmap_CreateEx(400, 400, 4, 0, 0)
    m._FPDFBitmap_FillRect(bitmap, 0, 0, 400, 400, 0xffffffff)
    return bitmap
  })
  const r1 = m._INK_Render_Start(bitmaps[0], page, 0, 0, 400, 400, 0, FPDF_ANNOT, 0, 0, 0, 0)
  assert.notEqual(r1, 0)
  m._FPDF_RenderPageBitmap(bitmaps[1], page, 0, 0, 400, 400, 0, FPDF_ANNOT)
  const r2 = m._INK_Render_Start(bitmaps[2], page, 0, 0, 400, 400, 0, FPDF_ANNOT, 0, 0, 0, 0)
  assert.notEqual(r2, 0, 'a render could not start after a stale one')
  assert.equal(m._INK_Render_Continue(r1, 1000), -1, 'a stale render continued')
  m._INK_Render_Close(r1)
  let status
  while ((status = m._INK_Render_Continue(r2, 1000)) === 0);
  assert.equal(status, 1, 'closing a stale render ended the current one')
  m._INK_Render_Close(r2)
  assert.ok(bitmapBytes(bitmaps[2], 400).equals(reference), 'the render after a stale one differs')
  bitmaps.forEach((bitmap) => m._FPDFBitmap_Destroy(bitmap))
  m._FPDF_ClosePage(page)
})

// /UserUnit: 2 when the page says 2, else 1.
for (const [entry, expected] of [
  ['/UserUnit 2', 2],
  ['', 1],
  ['/UserUnit -3', 1],
  ['/UserUnit 0', 1],
]) {
  withDoc(onePagePdf('', { page: entry }), (doc) => {
    const page = m._FPDF_LoadPage(doc, 0)
    assert.equal(m._INK_Page_UserUnit(page), expected, `INK_Page_UserUnit for "${entry}"`)
    m._FPDF_ClosePage(page)
  })
}

// Fallback fonts. INK_Fonts_PageNeeds says which keys a page's unembedded
// fonts need, INK_Fonts_TakeMisses which keys the font mapper asked for and
// found unregistered, and INK_Fonts_Register serves a font for a key.
const FONT_KEYS = { japanese: 1, simplifiedChinese: 2, traditionalChinese: 4, korean: 8, default: 16 }

// An unembedded Type 0 font with the given CIDSystemInfo ordering, as objects
// |first| (the font), |first| + 1 (its descendant) and |first| + 2 (the descriptor).
function type0Font(ordering, first) {
  return [
    `<< /Type /Font /Subtype /Type0 /BaseFont /Ink${ordering} /Encoding /Identity-H /DescendantFonts [${first + 1} 0 R] >>`,
    `<< /Type /Font /Subtype /CIDFontType0 /BaseFont /Ink${ordering} /CIDSystemInfo << /Registry (Adobe) /Ordering (${ordering}) /Supplement 0 >> /FontDescriptor ${first + 2} 0 R /DW 1000 >>`,
    `<< /Type /FontDescriptor /FontName /Ink${ordering} /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 880 /Descent -120 /CapHeight 700 /StemV 80 >>`,
  ]
}

// One 300 x 100 pt page showing |text| in font object 5 (|font| from object 5 on) at 40 pt, baseline 30.
function fontPagePdf(text, font, { pageExtra = '', resources = '', extra = [] } = {}) {
  return buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> ${resources} >> ${pageExtra} >>`,
    streamObject('', `BT /F1 40 Tf 20 30 Td ${text} Tj ET`),
    ...font,
    ...extra,
  ])
}

// Japan1 CIDs 843 to 846 are hiragana, which the built-in faces lack.
const kana = '<034B034C034D034E>'
const fontCases = [
  { name: 'Japan1', pdf: fontPagePdf(kana, type0Font('Japan1', 5)), needs: FONT_KEYS.japanese },
  { name: 'GB1', pdf: fontPagePdf(kana, type0Font('GB1', 5)), needs: FONT_KEYS.simplifiedChinese },
  { name: 'CNS1', pdf: fontPagePdf(kana, type0Font('CNS1', 5)), needs: FONT_KEYS.traditionalChinese },
  { name: 'Korea1', pdf: fontPagePdf(kana, type0Font('Korea1', 5)), needs: FONT_KEYS.korean },
  {
    name: 'Arial',
    pdf: fontPagePdf('(Arial) ', ['<< /Type /Font /Subtype /TrueType /BaseFont /Arial /Encoding /WinAnsiEncoding >>']),
    needs: 0,
  },
  {
    name: 'a simple font with Cyrillic /Differences',
    pdf: fontPagePdf('<C0C1C2C3>', [
      '<< /Type /Font /Subtype /TrueType /BaseFont /PTSans /FirstChar 192 /LastChar 195 /Widths [600 600 600 600] /FontDescriptor 6 0 R /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [192 /afii10017 /afii10018 /afii10019 /afii10020] >> >>',
      '<< /Type /FontDescriptor /FontName /PTSans /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>',
    ]),
    needs: FONT_KEYS.default,
  },
  {
    name: 'a simple font with Greek /Differences',
    pdf: fontPagePdf('(ABCD)', [
      '<< /Type /Font /Subtype /Type1 /BaseFont /InkGreek /FirstChar 65 /LastChar 68 /Widths [600 600 600 600] /FontDescriptor 6 0 R /Encoding << /Type /Encoding /Differences [65 /Alpha /Beta /Gamma /Delta] >> >>',
      '<< /Type /FontDescriptor /FontName /InkGreek /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>',
    ]),
    needs: FONT_KEYS.default,
  },
  {
    // The Symbol font keeps its Greek: PDFium draws it from the built-in Symbol face.
    name: 'SymbolMT with Greek /Differences',
    pdf: fontPagePdf('(ABCD)', [
      '<< /Type /Font /Subtype /TrueType /BaseFont /SymbolMT /FirstChar 65 /LastChar 68 /Widths [600 600 600 600] /FontDescriptor 6 0 R /Encoding << /Type /Encoding /Differences [65 /alpha /beta /gamma /delta] >> >>',
      '<< /Type /FontDescriptor /FontName /SymbolMT /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>',
    ]),
    needs: 0,
  },
  {
    name: 'Calibri',
    pdf: fontPagePdf('(Calibri) ', [
      '<< /Type /Font /Subtype /TrueType /BaseFont /Calibri /Encoding /WinAnsiEncoding /FontDescriptor 6 0 R >>',
      '<< /Type /FontDescriptor /FontName /Calibri /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>',
    ]),
    needs: 0,
  },
  // PDFium maps an unrecognised ordering as ANSI: no key draws it.
  { name: 'an unrecognised ordering', pdf: fontPagePdf(kana, type0Font('Klingon1', 5)), needs: 0 },
  {
    // The page's own font is Helvetica; the only unembedded CJK font is inside a form.
    name: 'a form XObject',
    pdf: buildPdf([
      '<< /Type /Catalog /Pages 2 0 R >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Contents 4 0 R /Resources << /XObject << /Fm0 5 0 R >> >> >>',
      streamObject('', '/Fm0 Do'),
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 300 100] /Resources << /Font << /F1 6 0 R >> >>', `BT /F1 40 Tf 20 30 Td ${kana} Tj ET`),
      ...type0Font('Korea1', 6),
    ]),
    needs: FONT_KEYS.korean,
  },
  {
    name: 'an annotation appearance',
    pdf: fontPagePdf('(page) ', ['<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'], {
      pageExtra: '/Annots [6 0 R]',
      extra: [
        '<< /Type /Annot /Subtype /FreeText /Rect [0 0 300 100] /DA (/F2 20 Tf) /AP << /N 7 0 R >> >>',
        streamObject('/Type /XObject /Subtype /Form /BBox [0 0 300 100] /Resources << /Font << /F2 8 0 R >> >>', `BT /F2 40 Tf 20 30 Td ${kana} Tj ET`),
        ...type0Font('GB1', 8),
      ],
    }),
    needs: FONT_KEYS.simplifiedChinese,
  },
]

for (const { name, pdf, needs } of fontCases) {
  withDoc(pdf, (doc) => assert.equal(m._INK_Fonts_PageNeeds(doc, 0), needs, `INK_Fonts_PageNeeds for ${name}`))
}

// The font the smoke check serves for Japanese: SMOKE_CJK_FONT names a
// Japanese-capable OpenType file. Without it any bytes stand in, and the
// check that glyphs draw is skipped.
const cjkFontPath = process.env.SMOKE_CJK_FONT
const cjkFont = cjkFontPath ? readFileSync(cjkFontPath) : Buffer.from('not a font')
if (!cjkFontPath) console.log('SMOKE_CJK_FONT is not set: the check that a registered font draws glyphs is skipped')

// Japan1 with nothing registered: one miss, then none. The text draws nothing.
const japan1 = fontCases[0].pdf
m._INK_Fonts_TakeMisses()
withDoc(japan1, (doc) => {
  assert.equal(inkInBand(renderPixels(doc), 15, 75), 0, 'the hiragana drew with no font registered')
})
assert.equal(m._INK_Fonts_TakeMisses(), FONT_KEYS.japanese, 'a Japan1 render missed no Japanese font')
assert.equal(m._INK_Fonts_TakeMisses(), 0, 'INK_Fonts_TakeMisses did not clear its log')

function registerFont(module, key, bytes) {
  const ptr = module._malloc(bytes.length)
  module.HEAPU8.set(bytes, ptr)
  const result = module._INK_Fonts_Register(key, ptr, bytes.length)
  // Register copies the bytes.
  module._free(ptr)
  return result
}

assert.equal(registerFont(m, 3, cjkFont), 0, 'a key of two bits was accepted')
assert.equal(registerFont(m, 32, cjkFont), 0, 'an unknown key was accepted')
assert.equal(m._INK_Fonts_Register(FONT_KEYS.japanese, 0, 0), 0, 'an empty font was accepted')
assert.equal(registerFont(m, FONT_KEYS.japanese, cjkFont), 1)
withDoc(japan1, (doc) => {
  const ink = inkInBand(renderPixels(doc), 15, 75)
  if (cjkFontPath) assert.ok(ink > 0, 'the registered Japanese font drew no glyphs')
})
assert.equal(m._INK_Fonts_TakeMisses(), 0, 'a registered key was logged as a miss')

// Runs |fn(module, doc)| on page 0 of |pdf| loaded in a fresh module, after
// registering |fonts|, a map from key to bytes.
async function withFreshModule(pdf, fonts, fn) {
  const fresh = await createModule()
  fresh._INK_Init()
  for (const [key, bytes] of fonts) assert.equal(registerFont(fresh, key, bytes), 1)
  const buf = fresh._malloc(pdf.length)
  fresh.HEAPU8.set(pdf, buf)
  const doc = fresh._FPDF_LoadMemDocument64(buf, pdf.length, 0)
  assert.notEqual(doc, 0)
  try {
    return fn(fresh, doc)
  } finally {
    fresh._FPDF_CloseDocument(doc)
    fresh._free(buf)
    fresh._INK_Destroy()
  }
}

// Page 0 of |doc| in |module|, 300 x 100 pt with annotations and widgets.
function freshRender(module, doc) {
  const page = module._FPDF_LoadPage(doc, 0)
  const bitmap = module._FPDFBitmap_CreateEx(300, 100, 4, 0, 0)
  module._FPDFBitmap_FillRect(bitmap, 0, 0, 300, 100, 0xffffffff)
  module._FPDF_RenderPageBitmap(bitmap, page, 0, 0, 300, 100, 0, FPDF_ANNOT | EPDF_RENDER_WIDGETS)
  const data = module._FPDFBitmap_GetBuffer(bitmap)
  const bytes = Buffer.from(module.HEAPU8.slice(data, data + 300 * 100 * 4))
  module._FPDFBitmap_Destroy(bitmap)
  module._FPDF_ClosePage(page)
  return { bytes, width: 300, height: 100 }
}

// Needs and misses agree: on a fresh module, with every key a page needs
// registered first, rendering the page (annotations included) misses nothing.
for (const { name, pdf } of fontCases) {
  const needs = withDoc(pdf, (doc) => m._INK_Fonts_PageNeeds(doc, 0))
  const fonts = Object.values(FONT_KEYS)
    .filter((key) => needs & key)
    .map((key) => [key, cjkFont])
  await withFreshModule(pdf, fonts, (fresh, doc) => {
    assert.equal(fresh._INK_Fonts_PageNeeds(doc, 0), needs)
    freshRender(fresh, doc)
    assert.equal(fresh._INK_Fonts_TakeMisses(), 0, `rendering ${name} missed a key INK_Fonts_PageNeeds left out`)
  })
}

// The default font: SMOKE_DEFAULT_FONT names an OpenType file with Cyrillic
// and Greek, such as Noto Sans. Without it any bytes stand in, and the check
// that glyphs draw is skipped.
const defaultFontPath = process.env.SMOKE_DEFAULT_FONT
const defaultFont = defaultFontPath ? readFileSync(defaultFontPath) : Buffer.from('not a font')
if (!defaultFontPath) console.log('SMOKE_DEFAULT_FONT is not set: the check that the default font draws Cyrillic and Greek is skipped')

// Simple fonts whose /Differences name Cyrillic or Greek letters ask for the
// default key: missed with nothing registered, where they draw nothing, and
// drawn from the registered default font.
for (const name of ['a simple font with Cyrillic /Differences', 'a simple font with Greek /Differences']) {
  const { pdf } = fontCases.find((c) => c.name === name)
  await withFreshModule(pdf, [], (fresh, doc) => {
    assert.equal(inkInBand(freshRender(fresh, doc), 15, 75), 0, `${name} drew with no font registered`)
    assert.equal(fresh._INK_Fonts_TakeMisses(), FONT_KEYS.default, `${name} did not miss the default font`)
  })
  await withFreshModule(pdf, [[FONT_KEYS.default, defaultFont]], (fresh, doc) => {
    const ink = inkInBand(freshRender(fresh, doc), 15, 75)
    if (defaultFontPath) assert.ok(ink > 0, `the default font drew no glyphs for ${name}`)
    assert.equal(fresh._INK_Fonts_TakeMisses(), 0, `${name} missed a registered default font`)
  })
}

// A WinAnsi font with a name PDFium does not know, and SymbolMT with Greek
// /Differences, draw the same bytes whether every key has a font or none does,
// and miss nothing: the mapper never asks for them.
for (const name of ['Calibri', 'SymbolMT with Greek /Differences']) {
  const { pdf } = fontCases.find((c) => c.name === name)
  const every = Object.values(FONT_KEYS).map((key) => [key, key === FONT_KEYS.default ? defaultFont : cjkFont])
  const draw = (fresh, doc) => {
    const { bytes } = freshRender(fresh, doc)
    assert.equal(fresh._INK_Fonts_TakeMisses(), 0, `${name} missed a key`)
    return bytes
  }
  const none = await withFreshModule(pdf, [], draw)
  const all = await withFreshModule(pdf, every, draw)
  assert.ok(inkInBand({ bytes: none, width: 300, height: 100 }, 15, 75) > 0, `${name} drew nothing`)
  assert.ok(none.equals(all), `registered fonts changed how ${name} draws`)
}

// INK_Page_Facts reads a page's box, rotation and user unit from its
// dictionary, without loading the page: the same values a loaded page reports.

// The six floats INK_Page_Facts writes for page |index|, or null when it returns 0.
function pageFacts(module, doc, index) {
  const out = module._malloc(24)
  const ok = module._INK_Page_Facts(doc, index, out)
  const facts = [...module.HEAPF32.slice(out / 4, out / 4 + 6)]
  module._free(out)
  return ok === 1 ? facts : null
}

// What a loaded page reports: FPDF_GetPageBoundingBox (an FS_RECTF: left, top,
// right, bottom), FPDFPage_GetRotation, INK_Page_UserUnit and the page's size.
function loadedFacts(module, doc, index) {
  const page = module._FPDF_LoadPage(doc, index)
  if (!page) return null
  const rect = module._malloc(16)
  assert.equal(module._FPDF_GetPageBoundingBox(page, rect), 1)
  const [left, top, right, bottom] = module.HEAPF32.slice(rect / 4, rect / 4 + 4)
  module._free(rect)
  const facts = {
    box: [left, bottom, right, top],
    rotation: module._FPDFPage_GetRotation(page) * 90,
    unit: module._INK_Page_UserUnit(page),
    width: module._FPDF_GetPageWidthF(page),
    height: module._FPDF_GetPageHeightF(page),
  }
  module._FPDF_ClosePage(page)
  return facts
}

// Asserts INK_Page_Facts agrees with a loaded page, field by field. The loaded
// width and height are the box's, swapped for a page turned 90 or 270 degrees.
function checkFacts(module, doc, index, label) {
  const facts = pageFacts(module, doc, index)
  const loaded = loadedFacts(module, doc, index)
  assert.ok(facts && loaded, `${label}: one of the two could not read the page`)
  const [left, bottom, right, top, rotation, unit] = facts
  assert.deepEqual([left, bottom, right, top], loaded.box, `${label}: box`)
  assert.equal(rotation, loaded.rotation, `${label}: rotation`)
  assert.equal(unit, loaded.unit, `${label}: user unit`)
  const turned = rotation === 90 || rotation === 270
  assert.equal(Math.fround(turned ? top - bottom : right - left), loaded.width, `${label}: width`)
  assert.equal(Math.fround(turned ? right - left : top - bottom), loaded.height, `${label}: height`)
}

{
  const pages = [
    ['a plain page', '/MediaBox [0 0 400 200]'],
    ['/Rotate 90', '/MediaBox [0 0 400 200] /Rotate 90'],
    ['/Rotate 180', '/MediaBox [0 0 400 200] /Rotate 180'],
    ['/Rotate 270', '/MediaBox [0 0 400 200] /Rotate 270'],
    ['/Rotate -90', '/MediaBox [0 0 400 200] /Rotate -90'],
    ['/Rotate 450', '/MediaBox [0 0 400 200] /Rotate 450'],
    ['/Rotate 45', '/MediaBox [0 0 400 200] /Rotate 45'],
    ['a CropBox inside the MediaBox', '/MediaBox [0 0 400 200] /CropBox [50 20 300 150]'],
    ['a CropBox larger than the MediaBox', '/MediaBox [0 0 400 200] /CropBox [-100 -100 1000 1000]'],
    ['no MediaBox of its own', ''],
    ['/UserUnit 2', '/MediaBox [0 0 400 200] /UserUnit 2'],
    ['a MediaBox with swapped corners', '/MediaBox [400 200 10 20]'],
  ]
  // Object 2 is the root of the page tree, with a MediaBox to inherit; object 3
  // a node with /Rotate 90 holding the last page; pages follow from object 4.
  const first = 4
  const inheritedRotate = first + pages.length
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    `<< /Type /Pages /MediaBox [0 0 500 300] /Kids [${pages.map((_, i) => `${first + i} 0 R`).join(' ')} 3 0 R] /Count ${pages.length + 1} >>`,
    `<< /Type /Pages /Parent 2 0 R /Rotate 90 /Kids [${inheritedRotate} 0 R] /Count 1 >>`,
    ...pages.map(([, entries]) => `<< /Type /Page /Parent 2 0 R ${entries} >>`),
    '<< /Type /Page /Parent 3 0 R /MediaBox [0 0 400 200] >>',
  ])
  withDoc(pdf, (doc) => {
    assert.equal(m._FPDF_GetPageCount(doc), pages.length + 1)
    pages.forEach(([label], i) => checkFacts(m, doc, i, label))
    checkFacts(m, doc, pages.length, 'an inherited /Rotate')
    assert.equal(pageFacts(m, doc, pages.length + 1), null, 'a page past the end')
    assert.equal(pageFacts(m, doc, -1), null, 'a negative index')
    assert.equal(m._INK_Page_Facts(doc, 0, 0), 0, 'a NULL out')
  })
  const out = m._malloc(24)
  assert.equal(m._INK_Page_Facts(0, 0, out), 0, 'a NULL document')
  m._free(out)
}

// INK_Page_Facts loads no font: reading every page of the Japan1 file before
// registering leaves no miss, and the font registered after it draws.
{
  const fresh = await createModule()
  fresh._INK_Init()
  const buf = fresh._malloc(japan1.length)
  fresh.HEAPU8.set(japan1, buf)
  const doc = fresh._FPDF_LoadMemDocument64(buf, japan1.length, 0)
  assert.notEqual(doc, 0)
  for (let i = 0; i < fresh._FPDF_GetPageCount(doc); i++) assert.ok(pageFacts(fresh, doc, i))
  assert.equal(fresh._INK_Fonts_TakeMisses(), 0, 'INK_Page_Facts loaded a font')
  if (cjkFontPath) {
    assert.equal(registerFont(fresh, FONT_KEYS.japanese, cjkFont), 1)
    assert.ok(inkInBand(freshRender(fresh, doc), 15, 75) > 0, 'a font registered after INK_Page_Facts drew nothing')
    assert.equal(fresh._INK_Fonts_TakeMisses(), 0, 'a font registered after INK_Page_Facts was missed')
  }
  fresh._FPDF_CloseDocument(doc)
  fresh._free(buf)
  fresh._INK_Destroy()
}

// INK_Text_*: a page's characters as PDFium's text page reports them.
const LETTER = [0, 0, 612, 792]
const letterBox = `[${LETTER.join(' ')}]`

// The text of page |index| of |doc| in |mod|, or null when INK_Text_Page
// returns NULL. geometry holds 15 floats per character (see text.h).
function pageText(doc, index, mod = m) {
  const t = mod._INK_Text_Page(doc, index)
  if (!t) return null
  const n = mod._INK_Text_Count(t)
  const bytes = (ptr, size) => Buffer.from(mod.HEAPU8.slice(ptr, ptr + n * size))
  const codesBytes = bytes(mod._INK_Text_Codes(t), 4)
  const geometryBytes = bytes(mod._INK_Text_Geometry(t), 60)
  const fontsBytes = bytes(mod._INK_Text_Fonts(t), 2)
  const result = {
    count: n,
    codes: Array.from({ length: n }, (_, i) => codesBytes.readUInt32LE(i * 4)),
    geometry: Array.from({ length: n }, (_, i) => Array.from({ length: 15 }, (_, k) => geometryBytes.readFloatLE((i * 15 + k) * 4))),
    flags: [...bytes(mod._INK_Text_Flags(t), 1)],
    fonts: Array.from({ length: n }, (_, i) => fontsBytes.readUInt16LE(i * 2)),
    fontTable: JSON.parse(cString(mod._INK_Text_FontTable(t), mod)),
    truncated: mod._INK_Text_Truncated(t),
  }
  mod._INK_Text_Free(t)
  return result
}

// The NUL-terminated UTF-8 string at |ptr| in |mod|.
function cString(ptr, mod = m) {
  assert.notEqual(ptr, 0, 'no string')
  let end = ptr
  while (mod.HEAPU8[end]) end++
  return Buffer.from(mod.HEAPU8.slice(ptr, end)).toString('utf8')
}

// The text of page 0 of |pdf|, read in |mod|.
function textOf(pdf, mod = m) {
  const buf = mod._malloc(pdf.length)
  mod.HEAPU8.set(pdf, buf)
  const doc = mod._FPDF_LoadMemDocument64(buf, pdf.length, 0)
  assert.notEqual(doc, 0, 'load failed')
  const text = pageText(doc, 0, mod)
  mod._FPDF_CloseDocument(doc)
  mod._free(buf)
  return text
}

const near = (actual, expected, what, tolerance = 0.01) =>
  assert.ok(Math.abs(actual - expected) <= tolerance, `${what}: ${actual}, expected ${expected}`)
const spell = (codes) => String.fromCodePoint(...codes)

{
  const hello = textOf(onePagePdf('BT /F1 12 Tf 72 700 Td (Hello) Tj ET', { mediaBox: letterBox }))
  assert.equal(hello.count, 5)
  assert.equal(spell(hello.codes), 'Hello')
  hello.geometry.forEach((g, i) => {
    near(g[9], 700, `origin y of character ${i}`)
    near(g[10], 12, `font size of character ${i}`)
    assert.ok(g[0] < g[2] && g[1] < g[3], `character ${i} has an empty box: ${g.slice(0, 4)}`)
    assert.ok(g[4] <= g[0] && g[5] <= g[1] && g[6] >= g[2] && g[7] >= g[3], `character ${i}'s loose box does not hold its box`)
    if (i > 0) assert.ok(g[8] > hello.geometry[i - 1][8], `origin x of character ${i} does not rise`)
  })
  near(hello.geometry[0][8], 72, 'origin x of H')
  assert.deepEqual(hello.flags, [0, 0, 0, 0, 0])
  assert.deepEqual(hello.fonts, [0, 0, 0, 0, 0])
  // PDFium takes a standard font's ascent and descent from the face it loads
  // for it, not from the font's metrics file (0.718 and -0.207 for Helvetica).
  assert.deepEqual(hello.fontTable, [
    {
      name: 'Helvetica',
      fixedPitch: false,
      serif: false,
      italic: false,
      bold: false,
      vertical: false,
      ascent: 0.944999993,
      descent: -0.224000007,
      descriptorAscent: null,
      descriptorDescent: null,
      standard: 'Helvetica',
    },
  ])
}

{
  // Separate Tj with a gap: PDFium puts a space it generated between the words.
  const words = textOf(onePagePdf('BT /F1 12 Tf 72 700 Td (Hello) Tj 80 0 Td (World) Tj ET', { mediaBox: letterBox }))
  assert.equal(spell(words.codes), 'Hello World')
  assert.equal(words.codes[5], 32)
  assert.equal(words.flags[5] & 1, 1, 'the space between the words is not marked generated')
  assert.ok(words.flags.every((f, i) => i === 5 || f === 0), `flags ${words.flags}`)
}

{
  const empty = textOf(onePagePdf('', { mediaBox: letterBox }))
  assert.equal(empty.count, 0, 'a page with no content has characters')
  assert.deepEqual(empty.fontTable, [])
  const opened = load(onePagePdf('', { mediaBox: letterBox }))
  assert.equal(m._INK_Text_Page(opened.doc, 99), 0, 'page 99 of a one-page file')
  assert.equal(m._INK_Text_Page(opened.doc, -1), 0, 'page -1')
  m._FPDF_CloseDocument(opened.doc)
  m._free(opened.buf)
  assert.equal(m._INK_Text_Page(0, 0), 0, 'a NULL document')
  m._INK_Text_Free(0)
  assert.equal(m._INK_Text_Truncated(0), 0, 'NULL is truncated')
  assert.equal(empty.truncated, 0, 'a page with no text is truncated')
}

{
  // A page of 1,001,000 characters gives the first 1,000,000 and says so; one
  // of exactly 1,000,000 gives them all. PDFium keeps at most 32,767
  // characters of one string, so the page shows strings of 1,000.
  const read = (strings) =>
    withDoc(onePagePdf(`BT /F1 1 Tf 0 0 Td ${strings.map((text) => `(${text}) Tj`).join(' ')} ET`, { mediaBox: letterBox }), (doc) => {
      const t = m._INK_Text_Page(doc, 0)
      assert.notEqual(t, 0)
      const count = m._INK_Text_Count(t)
      const codes = new Uint32Array(m.HEAPU8.slice(m._INK_Text_Codes(t), m._INK_Text_Codes(t) + count * 4).buffer)
      const truncated = m._INK_Text_Truncated(t)
      m._INK_Text_Free(t)
      return { count, codes, truncated }
    })
  const thousand = Array(1000).fill('a'.repeat(1000))
  const over = read([...thousand, 'b'.repeat(1000)])
  assert.equal(over.count, 1000000)
  assert.equal(over.truncated, 1)
  assert.ok(
    over.codes.every((c) => c === 97),
    'the characters kept are not the first ones',
  )
  const full = read(thousand)
  assert.equal(full.count, 1000000)
  assert.equal(full.truncated, 0)
}

{
  // Text inside a form XObject.
  const form = textOf(
    onePagePdf('q 1 0 0 1 100 100 cm /Fm0 Do Q', { mediaBox: letterBox, resources: '/XObject << /Fm0 6 0 R >>' }, [
      streamObject('/Type /XObject /Subtype /Form /BBox [0 0 200 50] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 10 Tf 5 5 Td (Inside) Tj ET'),
    ]),
  )
  assert.equal(spell(form.codes), 'Inside')
  near(form.geometry[0][8], 105, 'origin x of text in a form')
  near(form.geometry[0][9], 105, 'origin y of text in a form')
}

{
  // Text turned a quarter turn by its text matrix.
  const turned = textOf(onePagePdf('BT /F1 12 Tf 0 1 -1 0 300 300 Tm (Up) Tj ET', { mediaBox: letterBox }))
  assert.equal(spell(turned.codes), 'Up')
  for (const g of turned.geometry) {
    near(g[11], 0, 'matrix a')
    near(g[12], 1, 'matrix b')
    near(g[13], -1, 'matrix c')
    near(g[14], 0, 'matrix d')
    near(g[8], 300, 'origin x of turned text')
  }
  assert.ok(turned.geometry[1][9] > turned.geometry[0][9], 'turned text does not rise')
}

{
  // A Type 0 font written vertically. Japan1 CIDs 34 and 35 are A and B. A
  // fresh module, since the fonts check above registered bytes that are not a
  // font for Japanese, and PDFium's text page drops vertical text that has no
  // face.
  const fresh = await createModule()
  fresh._INK_Init()
  const [font, descendant, descriptor] = type0Font('Japan1', 5)
  const vertical = textOf(
    buildPdf([
      '<< /Type /Catalog /Pages 2 0 R >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>`,
      streamObject('', 'BT /F1 20 Tf 100 600 Td <00220023> Tj ET'),
      font.replace('/Identity-H', '/Identity-V'),
      descendant,
      descriptor,
    ]),
    fresh,
  )
  fresh._INK_Destroy()
  assert.equal(spell(vertical.codes), 'AB')
  assert.equal(vertical.fontTable.length, 1)
  assert.equal(vertical.fontTable[0].name, 'InkJapan1')
  assert.equal(vertical.fontTable[0].vertical, true, 'an /Identity-V font is not vertical')
  // The font descriptor's /Ascent 880 and /Descent -120.
  assert.equal(vertical.fontTable[0].ascent, 0.879999995)
  assert.equal(vertical.fontTable[0].descent, -0.119999997)
  // A Type 0 font's descriptor is its descendant's.
  assert.equal(vertical.fontTable[0].descriptorAscent, 0.879999995)
  assert.equal(vertical.fontTable[0].descriptorDescent, -0.119999997)
  assert.equal(vertical.fontTable[0].standard, null)
}

{
  // An embedded TrueType font whose descriptor gives a positive /Descent:
  // PDFium turns its own figure negative, the descriptor's is given as the
  // file has it. A font with a file of its own is no standard font, whatever
  // its name; an alias PDFium knows, such as TimesNewRoman, gives the
  // standard name.
  const ahem = readFileSync(fileURLToPath(new URL('../testing/resources/fonts/ahem/Ahem.ttf', import.meta.url)))
  const page = (font, extra = []) =>
    textOf(
      buildPdf([
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>`,
        streamObject('', 'BT /F1 12 Tf 72 700 Td (AB) Tj ET'),
        font,
        ...extra,
      ]),
    ).fontTable[0]
  const embedded = page('<< /Type /Font /Subtype /TrueType /BaseFont /Helvetica /FirstChar 65 /LastChar 66 /Widths [1000 1000] /FontDescriptor 6 0 R >>', [
    '<< /Type /FontDescriptor /FontName /Helvetica /Flags 32 /FontBBox [0 -200 1000 800] /ItalicAngle 0 /Ascent 800 /Descent 200 /CapHeight 800 /StemV 80 /FontFile2 7 0 R >>',
    streamObject('', ahem),
  ])
  assert.equal(embedded.descriptorAscent, 0.800000012)
  assert.equal(embedded.descriptorDescent, 0.200000003)
  assert.equal(embedded.descent, -0.200000003)
  assert.equal(embedded.standard, null)
  const bold = page('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>')
  assert.equal(bold.standard, 'Helvetica-Bold')
  assert.equal(bold.descriptorAscent, null)
  const times = page('<< /Type /Font /Subtype /Type1 /BaseFont /TimesNewRoman >>')
  assert.equal(times.standard, 'Times-Roman', 'an alias of a standard font')
  const notNumber = page('<< /Type /Font /Subtype /Type1 /BaseFont /Courier /FontDescriptor 6 0 R >>', [
    '<< /Type /FontDescriptor /FontName /Courier /Flags 32 /Ascent /Tall >>',
  ])
  assert.equal(notNumber.descriptorAscent, null)
  assert.equal(notNumber.descriptorDescent, null)
  assert.equal(notNumber.standard, 'Courier')
}

{
  // A /ToUnicode section is read to its end keyword whatever count it
  // announces, as PDF.js reads it. A malformed entry still drops its section.
  const mapped = (content, cmap) =>
    spell(
      textOf(
        buildPdf([
          '<< /Type /Catalog /Pages 2 0 R >>',
          '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
          `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>`,
          streamObject('', `BT /F1 12 Tf 72 700 Td ${content} Tj ET`),
          '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>',
          streamObject('', `/CIDInit /ProcSet findresource begin 12 dict begin begincmap 1 begincodespacerange <00> <FF> endcodespacerange\n${cmap}`),
        ]),
      ).codes,
    )
  const end = '\nendcmap CMapName currentdict /CMap defineresource pop end end'
  const ranges = (count, n) => `${count} beginbfrange ${['<41><41><0061>', '<42><42><0062>', '<43><43><0063>'].slice(0, n).join(' ')} endbfrange${end}`
  assert.equal(mapped('(ABCDE)', ranges(2, 2)), 'abCDE', 'a well-formed section')
  assert.equal(mapped('(ABCDE)', ranges(1, 3)), 'abcDE', 'one announced, three held')
  assert.equal(mapped('(ABCDE)', ranges(5, 2)), 'abCDE', 'five announced, two held')
  assert.equal(mapped('(ABCDE)', `1 beginbfchar <41> <0061> <42> <0062> endbfchar${end}`), 'abCDE', 'bfchar, one announced, two held')
  assert.equal(mapped('(ABCDE)', `3 beginbfchar <41> <0061> endbfchar${end}`), 'aBCDE', 'bfchar, three announced, one held')
  // A section that never ends is read to the end of the stream.
  assert.equal(mapped('(ABCDE)', '1 beginbfrange <41><41><0061> <42><42><0062>'), 'abCDE', 'a section with no end')
  assert.equal(mapped('(ABCDE)', '2 beginbfchar <41> <0061> <42>'), 'aBCDE', 'a bfchar section with no end')
  assert.equal(mapped('(ABCDE)', `5 beginbfrange <41><41><0061> <4G><42><0062> endbfrange${end}`), 'ABCDE', 'a malformed entry')
  // The case of PDF.js's bug 1388: ten ranges announced, one held, mapping the
  // space to U+B0A9. An A follows, since Helvetica's space has no outline and
  // the text page drops an object with no width.
  assert.equal(mapped('( A)', `10 beginbfrange <20> <20> <b0a9> endbfrange${end}`), '\u{b0a9}A')
}

{
  // /Rotate does not change the text: the same lines, a matrix-turned word and
  // a form XObject read in the same order with the same boxes, origins and
  // matrices on a page turned 0, 90, 180 and 270 degrees.
  const content = [
    'BT /F1 12 Tf 72 700 Td (First line) Tj 0 -20 Td (Second line) Tj 0 -20 Td (Third line) Tj ET',
    'BT /F1 12 Tf 0 1 -1 0 400 500 Tm (Up) Tj ET',
    'q 1 0 0 1 100 100 cm /Fm0 Do Q',
  ].join('\n')
  const form = streamObject('/Type /XObject /Subtype /Form /BBox [0 0 200 50] /Resources << /Font << /F1 5 0 R >> >>', 'BT /F1 10 Tf 5 5 Td (Inside) Tj ET')
  const [upright, ...turned] = [0, 90, 180, 270].map((rotate) =>
    textOf(onePagePdf(content, { mediaBox: letterBox, page: `/Rotate ${rotate}`, resources: '/XObject << /Fm0 6 0 R >>' }, [form])),
  )
  assert.ok(spell(upright.codes).startsWith('First line\r\nSecond line\r\nThird line'), JSON.stringify(spell(upright.codes)))
  turned.forEach((text, i) => assert.deepEqual(text, upright, `the text of a page turned ${(i + 1) * 90} degrees`))
}

// A letter page drawing |content| with a Type 3 font as /T3, object 5, whose
// /Differences are |differences| and whose every glyph name in |names| draws
// one procedure, object 6. |fontExtra| goes in the font dictionary, and
// |extra| objects follow from 7.
function type3Pdf(content, differences, names, { fontExtra = '', extra = [] } = {}) {
  return buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Contents 4 0 R /Resources << /Font << /T3 5 0 R >> >> >>`,
    streamObject('', content),
    `<< /Type /Font /Subtype /Type3 /FontBBox [0 0 600 700] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << ${names.map((n) => `/${n} 6 0 R`).join(' ')} >> /Encoding << /Type /Encoding /Differences [${differences}] >> /FirstChar 0 /LastChar 255 /Widths [${Array(256).fill(600).join(' ')}] /Resources << >> ${fontExtra} >>`,
    streamObject('', '600 0 0 0 600 700 d1 0 0 600 700 re f'),
    ...extra,
  ])
}

{
  // A Type 3 font has no ascent or descent.
  const text = textOf(type3Pdf('BT /T3 12 Tf 72 700 Td (A) Tj ET', '65 /A', ['A']))
  assert.equal(text.fontTable.length, 1)
  assert.equal(text.fontTable[0].ascent, null)
  assert.equal(text.fontTable[0].descent, null)
  assert.equal(text.fontTable[0].descriptorAscent, null)
  assert.equal(text.fontTable[0].descriptorDescent, null)
  assert.equal(text.fontTable[0].standard, null)
}

{
  // Type 3 text with no /ToUnicode reads its glyph names, and the character
  // code where a name means nothing.
  const adobe = textOf(
    type3Pdf(
      'BT /T3 12 Tf 72 700 Td (SECRET-ALPHA) Tj 100 0 Td (KEEP-BETA) Tj ET',
      '83 /S 69 /E 67 /C 82 /R 84 /T 45 /hyphen 65 /A 76 /L 80 /P 72 /H 75 /K 66 /B',
      ['S', 'E', 'C', 'R', 'T', 'hyphen', 'A', 'L', 'P', 'H', 'K', 'B'],
    ),
  )
  assert.equal(spell(adobe.codes), 'SECRET-ALPHA KEEP-BETA')
  assert.ok(
    adobe.flags.every((f) => (f & 4) === 0),
    `flags ${adobe.flags}`,
  )
  // Names that mean nothing: the codes, 65 (A) and 97 (a) on a WinAnsi base.
  assert.equal(spell(textOf(type3Pdf('BT /T3 12 Tf 72 700 Td (AA) Tj ET', '65 /C1', ['C1'])).codes), 'AA')
  assert.equal(
    spell(textOf(type3Pdf('BT /T3 12 Tf 72 700 Td (aa) Tj ET', '97 /a0', ['a0'], { fontExtra: '/Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [97 /a0] >>' })).codes),
    'aa',
  )
  // A uniXXXX name, an arbitrary code with a name that means nothing, a name
  // on a control code, and a control code whose name means nothing.
  const mixed = textOf(type3Pdf('BT /T3 12 Tf 72 700 Td <C8030201> Tj ET', '1 /g1 2 /A 3 /uni00E9 200 /g7', ['g1', 'A', 'uni00E9', 'g7']))
  assert.deepEqual(mixed.codes, [0xc8, 0xe9, 0x41, 0])
  assert.deepEqual(
    mixed.flags.map((f) => f & 4),
    [0, 0, 0, 4],
  )
  // A name whose value is no Unicode scalar, a surrogate or past U+10FFFF,
  // keeps flag 4 and code 0.
  const scalar = textOf(type3Pdf('BT /T3 12 Tf 72 700 Td (AB) Tj ET', '65 /uD800 66 /uFFFFFF', ['uD800', 'uFFFFFF']))
  assert.deepEqual(scalar.codes, [0, 0])
  assert.deepEqual(scalar.flags, [4, 4])
  // A /ToUnicode wins: code 65 reads Z.
  const cmap = [
    '/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CMapName /Ink def',
    '1 begincodespacerange <00> <FF> endcodespacerange',
    '1 beginbfchar <41> <005A> endbfchar',
    'endcmap CMapName currentdict /CMap defineresource pop end end',
  ].join('\n')
  const mapped = textOf(type3Pdf('BT /T3 12 Tf 72 700 Td (A) Tj ET', '65 /A', ['A'], { fontExtra: '/ToUnicode 7 0 R', extra: [streamObject('', cmap)] }))
  assert.equal(spell(mapped.codes), 'Z')
  assert.deepEqual(mapped.flags, [0])
}

// INK_Links_Page, INK_Outline and INK_Dest_Named: what the file says about a
// page's annotations, its links, the outline and named destinations.

// The JSON at |ptr|, freed with INK_Json_Free, or null for NULL.
function takeJson(ptr) {
  if (!ptr) return null
  const text = cString(ptr)
  m._INK_Json_Free(ptr)
  return JSON.parse(text)
}

const hex = (text) => Buffer.from(text, 'latin1').toString('hex')
// The shape keys of an explicit destination whose fit is a name and whose
// arguments, none a reference, are of |kinds|.
const shape = (kinds = [], pageKind = 'ref') => ({ argCount: kinds.length, argKinds: kinds, argIndirect: false, fitKind: 'name', pageKind })

function withPdf(pdf, fn) {
  const opened = load(pdf)
  try {
    return fn(opened.doc)
  } finally {
    m._FPDF_CloseDocument(opened.doc)
    m._free(opened.buf)
  }
}

function namedDest(doc, name) {
  const bytes = Buffer.from(name, 'latin1')
  const ptr = heapBytes(bytes)
  const result = takeJson(m._INK_Dest_Named(doc, ptr, bytes.length))
  m._free(ptr)
  return result
}

m._INK_Json_Free(0)

{
  // A hidden Square, a Link with an explicit destination and a Link with a URI
  // action, stored as a direct object.
  const pdf = onePagePdf(
    'BT /F1 12 Tf 72 700 Td (Hello) Tj ET',
    {
      mediaBox: letterBox,
      page: '/Annots [6 0 R 7 0 R << /Type /Annot /Subtype /Link /Rect [72 600 200 620] /A << /S /URI /URI (https://example.com/a) >> >>]',
    },
    ['<< /Type /Annot /Subtype /Square /Rect [10 10 50 50] /F 2 >>', '<< /Type /Annot /Subtype /Link /Rect [72 700 200 720] /Dest [3 0 R /XYZ 72 700 null] >>'],
  )
  withPdf(pdf, (doc) => {
    const { annots, links } = takeJson(m._INK_Links_Page(doc, 0))
    assert.deepEqual(annots, [
      { index: 0, subtype: 'Square', flags: 2, obj: 6, generation: 0, rectOk: true, hasQuads: false, quadsOk: true },
      { index: 1, subtype: 'Link', flags: 0, obj: 7, generation: 0, rectOk: true, hasQuads: false, quadsOk: true },
      { index: 2, subtype: 'Link', flags: 0, obj: 0, generation: 0, rectOk: true, hasQuads: false, quadsOk: true },
    ])
    assert.deepEqual(links, [
      { index: 1, rect: [72, 700, 200, 720], quads: null, dest: { page: 0, pageRef: [3, 0], fit: 'XYZ', args: [72, 700, null, null], ...shape(['number', 'number', 'null']) }, action: null, aa: null },
      { index: 2, rect: [72, 600, 200, 620], quads: null, dest: null, action: { s: 'URI', uriHex: hex('https://example.com/a'), uriKind: 'string' }, aa: null },
    ])
    assert.equal(Buffer.from(links[1].action.uriHex, 'hex').toString('latin1'), 'https://example.com/a')
    assert.equal(m._INK_Links_Page(doc, 1), 0, 'page 1 of a one-page file')
    assert.equal(m._INK_Links_Page(doc, -1), 0, 'page -1')
    assert.deepEqual(takeJson(m._INK_Outline(doc)), { items: [], truncated: false }, 'a file with no outline')
  })
  assert.equal(m._INK_Links_Page(0, 0), 0, 'a NULL document')
  assert.equal(m._INK_Outline(0), 0, 'a NULL document')
}

{
  // Every kind of target, on a page whose content stream cannot be decoded.
  const link = (rest) => `<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] ${rest} >>`
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> /Annots [${Array.from({ length: 13 }, (_, i) => `${i + 6} 0 R`).join(' ')} null] >>`,
    streamObject('/Filter /FlateDecode', 'BT /F1 12 Tf (not flate', true),
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
    link('/A << /S /URI /URI (https://example.com/b) >> /Dest [3 0 R /Fit]'),
    link('/A 5'),
    '<< /Type /Annot /Subtype /Link /Dest [3 0 R /Fit] >>',
    link('/A << /S /GoToR /F (other.pdf) /D (chapter) /NewWindow true >>'),
    link('/A << /S /Launch /F << /Type /Filespec /F (app.exe) >> >>'),
    link('/A << /S /Named /N /NextPage >>'),
    link("/A << /S /JavaScript /JS (app.launchURL\\('https://example.com/c'\\)) >>"),
    link('/A << /S /JavaScript /JS 19 0 R >>'),
    link('/AA << /U << /S /URI /URI (https://example.com/e) >> >>'),
    link('/Dest [1 0 R /Fit]'),
    link('/QuadPoints [0 10 10 10 0 0 10 0] /A << /S /SubmitForm >>'),
    link('/QuadPoints [1 2 3]'),
    link('/A << /S /GoTo /D /chapter-2 >>'),
    streamObject('', 'window.open("https://example.com/d")\n'),
  ])
  withPdf(pdf, (doc) => {
    const { annots, links } = takeJson(m._INK_Links_Page(doc, 0))
    assert.deepEqual(
      annots.map((a) => a.index),
      Array.from({ length: 13 }, (_, i) => i),
      'the null entry is listed',
    )
    assert.deepEqual(
      annots.map((a) => a.rectOk),
      annots.map((_, i) => i !== 2),
    )
    assert.deepEqual(
      annots.map((a) => a.quadsOk),
      annots.map((_, i) => i !== 11),
    )
    assert.equal(links.length, 13)
    const fit = (page, pageRef) => ({ page, pageRef, fit: 'Fit', args: [null, null, null, null], ...shape() })
    assert.deepEqual(links[0].action, { s: 'URI', uriHex: hex('https://example.com/b'), uriKind: 'string' }, '/A beside /Dest')
    assert.deepEqual(links[0].dest, fit(0, [3, 0]), '/Dest beside /A')
    assert.equal(links[1].action, null, '/A 5')
    assert.deepEqual(links[2].rect, [0, 0, 0, 0], 'no /Rect')
    assert.deepEqual(links[3].action, { s: 'GoToR', fileHex: hex('other.pdf'), fileKind: 'string', dest: { nameHex: hex('chapter') }, newWindow: true })
    assert.deepEqual(links[4].action, { s: 'Launch', fileHex: hex('app.exe'), fileKind: 'spec', dest: null, newWindow: null })
    assert.deepEqual(links[5].action, { s: 'Named', name: 'NextPage' })
    assert.deepEqual(links[6].action, { s: 'JavaScript', js: "app.launchURL('https://example.com/c')" })
    assert.deepEqual(links[7].action, { s: 'JavaScript', js: 'window.open("https://example.com/d")\n' })
    assert.equal(links[8].action, null)
    assert.deepEqual(links[8].aa, { s: 'URI', uriHex: hex('https://example.com/e'), uriKind: 'string' }, '/AA /U')
    assert.deepEqual(links[9].dest, fit(-1, [1, 0]), 'a destination on the catalog')
    assert.deepEqual(links[10].quads, [0, 10, 10, 10, 0, 0, 10, 0])
    assert.deepEqual(links[10].action, { s: 'SubmitForm' })
    assert.equal(links[11].quads, null, 'malformed /QuadPoints')
    assert.deepEqual(links[12].action, { s: 'GoTo', dest: { nameHex: hex('chapter-2') } })
  })
}

{
  // A script is cut where what it writes inside its JSON string, UTF-8 with
  // escapes, would pass 4,096 bytes, at a character's end, and the scripts of
  // one page's links write at most 1,048,576 such bytes in all.
  const PAGE_SCRIPTS = 1048576
  // The bytes a script writes inside its JSON string, as json.h escapes it.
  const written = (text) =>
    [...text].reduce((sum, ch) => {
      const c = ch.codePointAt(0)
      if (ch === '"' || ch === '\\') return sum + 2
      if (c < 0x20 || c === 0x7f) return sum + 6
      return sum + Buffer.byteLength(ch)
    }, 0)
  const scriptPage = (annots, objects) =>
    buildPdf([
      '<< /Type /Catalog /Pages 2 0 R >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [${annots.map((n) => `${n} 0 R`).join(' ')}] >>`,
      ...objects,
    ])
  const readLinks = (pdf) =>
    withPdf(pdf, (doc) => {
      const ptr = m._INK_Links_Page(doc, 0)
      const text = cString(ptr)
      m._INK_Json_Free(ptr)
      assert.ok(Buffer.byteLength(text) < 16 * 1024 * 1024, `${Buffer.byteLength(text)} bytes of JSON`)
      return JSON.parse(text).links
    })
  const js = (script) => `<< /S /JavaScript /JS ${script} 0 R >>`
  const link = (rest) => `<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] ${rest} >>`

  // A 1 MB script on 2,000 links, after one of 'a' and 3,000 é (0xE9 in
  // PDFDocEncoding, two bytes in UTF-8): that one is cut after 2,047 é, at
  // 4,095 bytes; 255 more fill the page's budget but for one byte, one 'a';
  // the rest are empty.
  {
    const links = readLinks(
      scriptPage(
        Array.from({ length: 2001 }, (_, i) => i + 6),
        [
          streamObject('', 'a'.repeat(1 << 20)),
          streamObject('', Buffer.concat([Buffer.from('a'), Buffer.alloc(3000, 0xe9)])),
          link(`/A ${js(5)}`),
          ...Array.from({ length: 2000 }, () => link(`/A ${js(4)}`)),
        ],
      ),
    )
    assert.equal(links.length, 2001)
    assert.deepEqual(links[0].action, { s: 'JavaScript', js: `a${'é'.repeat(2047)}`, scriptTruncated: true })
    const expected = (i) => (i <= 255 ? 'a'.repeat(4096) : i === 256 ? 'a' : '')
    links.slice(1).forEach((l, k) => assert.deepEqual(l.action, { s: 'JavaScript', js: expected(k + 1), scriptTruncated: true }, `link ${k + 1}`))
    assert.equal(
      links.reduce((sum, l) => sum + written(l.action.js), 0),
      PAGE_SCRIPTS,
    )
  }

  // 2,000 links whose /A and /AA scripts are 4,096 bytes of \x01, which JSON
  // writes as \u0001: each is cut at 682 characters (4,092 bytes), and the
  // page's budget runs out after 256 of them and 170 characters of the next.
  {
    const links = readLinks(
      scriptPage(
        Array.from({ length: 2000 }, (_, i) => i + 5),
        [streamObject('', Buffer.alloc(4096, 1)), ...Array.from({ length: 2000 }, () => link(`/A ${js(4)} /AA << /U ${js(4)} >>`))],
      ),
    )
    const scripts = links.flatMap((l) => [l.action, l.aa])
    assert.equal(scripts.length, 4000)
    assert.ok(
      scripts.every((a) => a.scriptTruncated === true && /^\x01*$/.test(a.js)),
      'a script was not cut or holds more than \\x01',
    )
    assert.deepEqual(
      scripts.slice(0, 258).map((a) => a.js.length),
      [...Array(256).fill(682), 170, 0],
    )
    assert.ok(scripts.slice(257).every((a) => a.js === ''))
    const total = scripts.reduce((sum, a) => sum + written(a.js), 0)
    assert.ok(total <= PAGE_SCRIPTS, `${total} bytes of scripts`)
  }

  // A script of 4,096 quotes is cut at 2,048, each written as \"; 4,096 bytes
  // of plain text exactly are not cut.
  {
    const links = readLinks(
      scriptPage([4, 5], [link(`/A ${js(6)}`), link(`/A ${js(7)}`), streamObject('', '"'.repeat(4096)), streamObject('', 'a'.repeat(4096))]),
    )
    assert.deepEqual(links[0].action, { s: 'JavaScript', js: '"'.repeat(2048), scriptTruncated: true })
    assert.deepEqual(links[1].action, { s: 'JavaScript', js: 'a'.repeat(4096) })
  }
}

{
  // INK_Links_Page does not parse the page's content, so it loads no font.
  const fresh = await createModule()
  fresh._INK_Init()
  const pdf = fontPagePdf(kana, type0Font('Japan1', 5), { pageExtra: '/Annots [8 0 R]', extra: ['<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] /Dest [3 0 R /Fit] >>'] })
  const buf = fresh._malloc(pdf.length)
  fresh.HEAPU8.set(pdf, buf)
  const doc = fresh._FPDF_LoadMemDocument64(buf, pdf.length, 0)
  assert.notEqual(doc, 0)
  const ptr = fresh._INK_Links_Page(doc, 0)
  assert.equal(JSON.parse(cString(ptr, fresh)).links.length, 1)
  fresh._INK_Json_Free(ptr)
  assert.equal(fresh._INK_Fonts_TakeMisses(), 0, 'INK_Links_Page loaded a font')
  fresh._FPDF_CloseDocument(doc)
  fresh._free(buf)
  fresh._INK_Destroy()
}

{
  // An outline whose second item's /Next points back at the first.
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/Outlines 6 0 R' }, [
    '<< /Type /Outlines /First 7 0 R /Last 8 0 R /Count 3 >>',
    '<< /Title (Cover) /Parent 6 0 R /Next 8 0 R /First 9 0 R /Last 9 0 R /Count -1 /F 2 /Dest [3 0 R /XYZ 0 792 null] >>',
    '<< /Title (Second) /Parent 6 0 R /Prev 7 0 R /Next 7 0 R >>',
    '<< /Title (Child) /Parent 7 0 R >>',
  ])
  withPdf(pdf, (doc) => {
    const outline = takeJson(m._INK_Outline(doc))
    assert.deepEqual(
      outline.items.map((item) => [item.depth, item.title]),
      [
        [0, 'Cover'],
        [1, 'Child'],
        [0, 'Second'],
      ],
    )
    assert.deepEqual(outline.items[0], { depth: 0, title: 'Cover', flags: 2, count: -1, dest: { page: 0, pageRef: [3, 0], fit: 'XYZ', args: [0, 792, null, null], ...shape(['number', 'number', 'null']) }, action: null })
    assert.equal(outline.truncated, true, 'a loop does not truncate')
  })
}

{
  // An outline 200 deep.
  const items = Array.from({ length: 200 }, (_, i) => `<< /Title (L${i}) ${i < 199 ? `/First ${i + 8} 0 R` : ''} >>`)
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/Outlines 6 0 R' }, ['<< /Type /Outlines /First 7 0 R >>', ...items])
  withPdf(pdf, (doc) => {
    const outline = takeJson(m._INK_Outline(doc))
    assert.equal(outline.items.length, 64)
    assert.deepEqual(
      outline.items.map((item) => item.depth),
      Array.from({ length: 64 }, (_, i) => i),
    )
    assert.equal(outline.truncated, true, 'depth does not truncate')
  })
}

{
  // Named destinations in /Names /Dests and in the catalog's /Dests, and an
  // outline with escaped and UTF-16 titles.
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/Outlines 6 0 R /Names << /Dests 9 0 R >> /Dests << /old [3 0 R /Fit] >>' }, [
    '<< /Type /Outlines /First 7 0 R /Last 8 0 R /Count 2 >>',
    '<< /Title (Say "hi"\\n\\\\ back\\t) /Parent 6 0 R /Next 8 0 R /F 3 /Count -2 /Dest (chapter-2) >>',
    '<< /Title <FEFF00E90074D83DDE00> /Parent 6 0 R /Prev 7 0 R /A << /S /URI /URI (https://example.com/f) >> >>',
    '<< /Names [(chapter-2) [3 0 R /FitH 500]] >>',
  ])
  withPdf(pdf, (doc) => {
    assert.deepEqual(takeJson(m._INK_Outline(doc)), {
      items: [
        { depth: 0, title: 'Say "hi"\n\\ back\t', flags: 3, count: -2, dest: { nameHex: '636861707465722d32' }, action: null },
        { depth: 0, title: 'ét\u{1f600}', flags: 0, count: 0, dest: null, action: { s: 'URI', uriHex: hex('https://example.com/f'), uriKind: 'string' } },
      ],
      truncated: false,
    })
    assert.deepEqual(namedDest(doc, 'chapter-2'), { page: 0, pageRef: [3, 0], fit: 'FitH', args: [500, null, null, null], ...shape(['number']) })
    assert.deepEqual(namedDest(doc, 'old'), { page: 0, pageRef: [3, 0], fit: 'Fit', args: [null, null, null, null], ...shape() })
    assert.equal(namedDest(doc, 'missing'), null)
  })
}

{
  // Page numbers in local destinations that are not pages of this file are
  // -1; a remote go-to's page number is the other file's and stays. A stream
  // in /Annots is not an annotation, and the entry after it keeps its index.
  const link = (rest) => `<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] ${rest} >>`
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [6 0 R 7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R] >>`,
    '<< >>',
    '<< >>',
    link('/Dest [999 /Fit]'),
    link('/Dest [-5 /Fit]'),
    link('/Dest [0 /Fit]'),
    link('/A << /S /GoTo /D [999 /Fit] >>'),
    link('/A << /S /GoToR /F (other.pdf) /D [7 /Fit] >>'),
    streamObject('/Type /Annot /Subtype /Link /Rect [0 0 10 10]', ''),
    link('/Dest [0 /XYZ 0.1 null 1.5]'),
  ])
  withPdf(pdf, (doc) => {
    const { annots, links } = takeJson(m._INK_Links_Page(doc, 0))
    assert.deepEqual(
      annots.map((a) => a.index),
      [0, 1, 2, 3, 4, 6],
      'the stream entry is listed, or the entry after it lost its index',
    )
    assert.deepEqual(
      links.map((l) => l.dest?.page ?? l.action.dest.page),
      [-1, -1, 0, -1, 7, 0],
    )
    // Floats are written so that they read back as the same float.
    assert.deepEqual(links[5].dest.args.map((v) => (v === null ? null : Math.fround(v))), [Math.fround(0.1), null, 1.5, null])
    assert.equal(links[5].dest.args[0], Number(Math.fround(0.1).toPrecision(9)))
  })
}

{
  // Facts a reader needs to treat a link as PDF.js does: whether /QuadPoints is
  // there, a generation other than 0, a /Dest that is no destination beside
  // /AA, whether /F was a string or a file specification, a /URI written as a
  // name, and the reference a destination's page entry is.
  const link = (rest) => `<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] ${rest} >>`
  const body = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 9 3 R 10 0 R 11 0 R] >>`,
    '<< /Type /Annot /Subtype /Highlight /Rect [0 0 10 10] >>',
    '<< /Type /Annot /Subtype /Highlight /Rect [0 0 10 10] /QuadPoints 5 >>',
    link('/Dest 5 /AA << /U << /S /URI /URI (https://example.com/g) >> >>'),
    link('/Dest null /AA << /U << /S /URI /URI (https://example.com/g) >> >>'),
    link('/A << /S /GoToR /F << /Type /Filespec /UF (spec.pdf) >> /D [3 /Fit] >>'),
    link('/A << /S /URI /URI /https:#2F#2Fexample.com#2Fh >>'),
    link('/A << /S /URI /URI 7 >>'),
    link('/Dest [3 0 R /Fit]'),
  ])
  // Object 9 written with generation 3, in its header and in the xref table.
  const lines = body.toString('latin1').replace('\n9 0 obj\n', '\n9 3 obj\n').split('\n')
  const entry = lines.indexOf('0000000000 65535 f ') + 9
  lines[entry] = lines[entry].replace(' 00000 n ', ' 00003 n ')
  const pdf = Buffer.from(lines.join('\n'), 'latin1')
  withPdf(pdf, (doc) => {
    const { annots, links } = takeJson(m._INK_Links_Page(doc, 0))
    assert.deepEqual(
      annots.map((a) => a.hasQuads),
      [false, true, false, false, false, false, false, false],
    )
    assert.deepEqual(
      annots.map((a) => a.quadsOk),
      [true, false, true, true, true, true, true, true],
    )
    assert.deepEqual(
      annots.map((a) => [a.obj, a.generation]),
      [4, 5, 6, 7, 8, 9, 10, 11].map((n) => [n, n === 9 ? 3 : 0]),
    )
    assert.equal(links[0].dest, null)
    assert.equal(links[0].destInvalid, true, '/Dest 5')
    assert.equal(links[1].destInvalid, true, '/Dest null')
    assert.equal(links[5].destInvalid, undefined, 'a good /Dest')
    assert.equal(links[3].destInvalid, undefined, 'no /Dest')
    assert.deepEqual(links[0].aa, { s: 'URI', uriHex: hex('https://example.com/g'), uriKind: 'string' })
    assert.deepEqual(links[2].action, { s: 'GoToR', fileHex: hex('spec.pdf'), fileKind: 'spec', dest: { page: 3, pageRef: null, fit: 'Fit', args: [null, null, null, null], ...shape([], 'integer') }, newWindow: null })
    assert.deepEqual(links[3].action, { s: 'URI', uriHex: hex('https://example.com/h'), uriKind: 'name' })
    assert.deepEqual(links[4].action, { s: 'URI', uriHex: '', uriKind: null })
    assert.deepEqual(links[5].dest, { page: 0, pageRef: [3, 0], fit: 'Fit', args: [null, null, null, null], ...shape() })
  })
}

{
  // An explicit destination's shape as the file wrote it, on a link's /Dest,
  // in a GoTo action, in the outline and through a named destination. Object
  // 6 is the number 7; object 7 a destination of a million entries.
  const dests = [
    ['[3 0 R /XYZ]', { argCount: 0, argKinds: [] }],
    ['[3 0 R /XYZ 1]', { argCount: 1, argKinds: ['number'] }],
    ['[3 0 R /XYZ null null null]', { argCount: 3, argKinds: ['null', 'null', 'null'] }],
    ['[3 0 R /Fit null]', { argCount: 1, argKinds: ['null'] }],
    ['[3 0 R /FitR 1 2 3 4 5]', { argCount: 5, argKinds: ['number', 'number', 'number', 'number', 'number'] }],
    ['[3 0 R /XYZ (left) 2]', { argCount: 2, argKinds: ['other', 'number'] }],
    ['[3 0 R /XYZ 6 0 R 2]', { argCount: 2, argKinds: ['number', 'number'], argIndirect: true }],
    ['[1.5 /Fit]', { pageKind: 'other' }],
    ['[/P /Fit]', { pageKind: 'other' }],
    ['[0 /Fit]', { pageKind: 'integer' }],
    ['[2.0 /Fit]', { pageKind: 'integer' }],
    ['[6 0 R /Fit]', { pageKind: 'other' }],
    ['[3 0 R (Fit)]', { fitKind: 'string' }],
    ['[3 0 R 5]', { fitKind: 'other' }],
    ['[3 0 R]', { argCount: 0, argKinds: [], fitKind: 'missing' }],
    ['[]', { argCount: 0, argKinds: [], fitKind: 'missing', pageKind: 'missing' }],
    ['7 0 R', { argCount: 999998, argKinds: Array(8).fill('number') }],
  ]
  const expected = dests.map(([, facts]) => ({ argCount: 0, argKinds: [], argIndirect: false, fitKind: 'name', pageKind: 'ref', ...facts }))
  const shapeOf = (dest) => {
    const { argCount, argKinds, argIndirect, fitKind, pageKind } = dest
    return { argCount, argKinds, argIndirect, fitKind, pageKind }
  }
  const link = (rest) => `<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] ${rest} >>`
  const first = 8
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /Outlines ${first + 2 * dests.length} 0 R /Dests << ${dests.map(([d], i) => `/d${i} ${d}`).join(' ')} >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [${dests.flatMap((_, i) => [`${first + 2 * i} 0 R`, `${first + 2 * i + 1} 0 R`]).join(' ')}] >>`,
    '<< >>',
    '<< >>',
    '7',
    `[3 0 R /XYZ ${'1 '.repeat(999998)}]`,
    ...dests.flatMap(([d]) => [link(`/Dest ${d}`), link(`/A << /S /GoTo /D ${d} >>`)]),
    `<< /Type /Outlines /First ${first + 2 * dests.length + 1} 0 R >>`,
    ...dests.map(([d], i) => `<< /Title (T${i}) /Dest ${d} ${i + 1 < dests.length ? `/Next ${first + 2 * dests.length + 2 + i} 0 R` : ''} >>`),
  ])
  withPdf(pdf, (doc) => {
    const { links } = takeJson(m._INK_Links_Page(doc, 0))
    const outline = takeJson(m._INK_Outline(doc))
    dests.forEach(([d], i) => {
      assert.deepEqual(shapeOf(links[2 * i].dest), expected[i], `/Dest ${d}`)
      assert.deepEqual(shapeOf(links[2 * i + 1].action.dest), expected[i], `GoTo ${d}`)
      assert.deepEqual(shapeOf(outline.items[i].dest), expected[i], `outline ${d}`)
      assert.deepEqual(shapeOf(namedDest(doc, `d${i}`)), expected[i], `named ${d}`)
    })
    // The values that were there before keep theirs.
    assert.deepEqual(links[0].dest.args, [null, null, null, null])
    assert.equal(links[0].dest.fit, 'XYZ')
    assert.deepEqual(links[2 * 4].dest.args, [1, 2, 3, 4])
  })
}

{
  // INK_Dest_Named with no name.
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/Dests << /old [3 0 R /Fit] >>' })
  withPdf(pdf, (doc) => {
    const ptr = heapBytes(Buffer.from('old', 'latin1'))
    assert.equal(takeJson(m._INK_Dest_Named(doc, ptr, 0)), null, 'len 0')
    assert.equal(takeJson(m._INK_Dest_Named(doc, ptr, -1)), null, 'a negative len')
    assert.equal(takeJson(m._INK_Dest_Named(doc, 0, 3)), null, 'a NULL name')
    assert.equal(cString(m._INK_Dest_Named(doc, 0, 0)), 'null', 'the JSON text null')
    assert.deepEqual(takeJson(m._INK_Dest_Named(doc, ptr, 3)), { page: 0, pageRef: [3, 0], fit: 'Fit', args: [null, null, null, null], ...shape() })
    m._free(ptr)
  })
  assert.equal(m._INK_Dest_Named(0, 0, 0), 0, 'a NULL document')
}

{
  // An outline of 100,001 items stops at 100,000.
  const total = 100001
  const items = Array.from({ length: total }, (_, i) => `<< /Title (I${i}) ${i + 1 < total ? `/Next ${i + 8} 0 R` : ''} >>`)
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/Outlines 6 0 R' }, ['<< /Type /Outlines /First 7 0 R >>', ...items])
  withPdf(pdf, (doc) => {
    const outline = takeJson(m._INK_Outline(doc))
    assert.equal(outline.items.length, 100000)
    assert.equal(outline.items.at(-1).title, 'I99999')
    assert.equal(outline.truncated, true, 'the item cap does not truncate')
  })
}

// INK_Annots_Page, INK_Form, INK_Scripts, INK_Doc_Facts, INK_Struct_Order and
// INK_Annot_Draw: what the file says about a page's annotations, its form, its
// scripts and itself, the order its structure gives annotations, and one
// annotation drawn alone. None of them writes to the document.

// A float[6] matrix in the heap. The caller frees it.
function heapMatrix(values) {
  const ptr = m._malloc(24)
  values.forEach((v, i) => m.setValue(ptr + i * 4, v, 'float'))
  return ptr
}

// Draws annotation |index| of page |pageIndex| alone onto a transparent bitmap
// of the page's size, in |state| (null: the one /AS selects; a Buffer: the
// name's bytes; a string: its UTF-8). Returns the result and the bytes.
function drawAnnot(doc, pageIndex, index, state) {
  const page = m._FPDF_LoadPage(doc, pageIndex)
  assert.notEqual(page, 0)
  const width = Math.round(m._FPDF_GetPageWidthF(page))
  const height = Math.round(m._FPDF_GetPageHeightF(page))
  const bitmap = m._FPDFBitmap_CreateEx(width, height, 4, 0, 0)
  m._FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0)
  const matrix = heapMatrix([width / m._FPDF_GetPageWidthF(page), 0, 0, height / m._FPDF_GetPageHeightF(page), 0, 0])
  let drew
  if (state === null) {
    drew = m._INK_Annot_Draw(bitmap, page, index, 0, matrix)
  } else if (Buffer.isBuffer(state)) {
    // A state as the name's own bytes.
    const name = heapBytes(Buffer.concat([state, Buffer.from([0])]))
    drew = m._INK_Annot_Draw(bitmap, page, index, name, matrix)
    m._free(name)
  } else {
    drew = withStrings([state], (name) => m._INK_Annot_Draw(bitmap, page, index, name, matrix))
  }
  const bytes = bitmapBytes(bitmap, height)
  m._free(matrix)
  m._FPDFBitmap_Destroy(bitmap)
  m._FPDF_ClosePage(page)
  return { drew, bytes }
}

const inked = (bytes) => bytes.some((b) => b !== 0)

{
  // form-fields.pdf, from the monorepo's fixtures: every field kind, 23
  // widgets, 20 of them on page 0, and page 1 turned 90 degrees.
  const formFields = readFileSync(new URL('./testdata/form-fields.pdf', import.meta.url))
  const KINDS = {
    agree: 'checkbox',
    auto: 'text',
    city: 'combo',
    code: 'text',
    country: 'combo',
    email: 'text',
    'header.ref': 'text',
    interests: 'list',
    internal: 'text',
    locked: 'text',
    name: 'text',
    notes: 'text',
    pin: 'text',
    plan: 'radio',
    reset: 'button',
    rotated: 'text',
    shared: 'radio',
    sig: 'signature',
    sig2: 'signature',
    styled: 'text',
  }
  withPdf(formFields, (doc) => {
    const unread = [saveWith(doc, NO_INCREMENTAL), saveWith(doc, 1)]

    const form = takeJson(m._INK_Form(doc))
    assert.equal(form.kind, 'acroform')
    assert.equal(form.truncated, false)
    assert.deepEqual(Object.fromEntries(form.fields.map((f) => [f.name, f.kind])), KINDS)
    assert.equal(form.fields.length, Object.keys(KINDS).length, 'a field is listed twice')
    assert.equal(form.fields.flatMap((f) => f.widgets).length, 23)
    const field = (name) => form.fields.find((f) => f.name === name)

    const { annots, tabs, truncated } = takeJson(m._INK_Annots_Page(doc, 0))
    assert.equal(truncated, false)
    assert.equal(tabs, 'R')
    assert.equal(annots.length, 20)
    assert.deepEqual(
      annots.map((a) => [a.index, a.subtype]),
      annots.map((_, i) => [i, 'Widget']),
    )
    // Every widget on page 0 is the annotation at its place in /Annots.
    const onPage0 = form.fields.flatMap((f) => f.widgets).filter((w) => w.page === 0)
    assert.equal(onPage0.length, 20)
    for (const w of onPage0) assert.equal(annots[w.annotIndex].objectNumber, w.objectNumber)
    // shared's second widget sits after country's in /Annots, apart from its first.
    assert.equal(field('shared').widgets[1].annotIndex, field('country').widgets[0].annotIndex + 1)
    assert.notEqual(field('shared').widgets[0].annotIndex + 1, field('shared').widgets[1].annotIndex)
    // header.ref's second widget is on the turned page, turned back by /MK /R.
    assert.deepEqual([field('header.ref').widgets[1].page, field('header.ref').widgets[1].rotation], [1, 90])
    assert.equal(takeJson(m._INK_Annots_Page(doc, 1)).annots.length, 3)
    assert.equal(field('email').tooltip, 'Email address')
    assert.equal(field('pin').maxLength, 4)
    assert.deepEqual([field('country').valueKind, field('country').value], ['string', 'gb'])
    assert.deepEqual(field('country').options[0], { exportValue: 'gb', displayValue: 'United Kingdom' })
    assert.deepEqual([field('plan').valueKind, field('plan').value, field('plan').defaultValue], ['name', 'monthly', 'yearly'])
    assert.deepEqual(
      field('plan').widgets.map((w) => w.onState),
      ['monthly', 'yearly'],
    )
    assert.deepEqual(field('reset').widgets[0].action, { s: 'ResetForm', fields: [], refs: [], flags: 0 })
    assert.equal(field('styled').widgets[0].da, '/F1 10 Tf 0 0 1 rg')

    // The checkbox drawn off and on gives different pixels; with no state it
    // draws what /AS selects, Off.
    const agree = field('agree').widgets[0]
    assert.ok(agree.states.includes('Off') && agree.states.includes(agree.onState), JSON.stringify(agree.states))
    const off = drawAnnot(doc, 0, agree.annotIndex, 'Off')
    const on = drawAnnot(doc, 0, agree.annotIndex, agree.onState)
    assert.deepEqual([off.drew, on.drew], [1, 1])
    assert.ok(inked(on.bytes), 'the checked box drew nothing')
    assert.ok(!off.bytes.equals(on.bytes), 'Off and on draw the same')
    assert.ok(drawAnnot(doc, 0, agree.annotIndex, null).bytes.equals(off.bytes), 'no state is not /AS')
    assert.equal(drawAnnot(doc, 0, agree.annotIndex, 'Missing').drew, 0, 'a state /N lacks')
    assert.equal(drawAnnot(doc, 0, 99, null).drew, 0, 'no annotation at 99')

    // Drawn alone, a widget lands where the page with its widgets draws it:
    // laid over the page drawn without widgets, it gives the pixels of the page
    // drawn with them, to within 1 level, on the turned page too. On page 0 its
    // pixels fill its /Rect's box.
    for (const w of [field('name').widgets[0], field('plan').widgets[1], field('header.ref').widgets[1], field('rotated').widgets[0]]) {
      const page = m._FPDF_LoadPage(doc, w.page)
      const width = Math.round(m._FPDF_GetPageWidthF(page))
      const height = Math.round(m._FPDF_GetPageHeightF(page))
      m._FPDF_ClosePage(page)
      const alone = drawAnnot(doc, w.page, w.annotIndex, null)
      assert.equal(alone.drew, 1)
      const full = renderPixels(doc, w.page, FPDF_ANNOT | EPDF_RENDER_WIDGETS).bytes
      const bare = renderPixels(doc, w.page, FPDF_ANNOT).bytes
      const box = [width, height, -1, -1]
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          if (alone.bytes[(y * width + x) * 4 + 3] === 0) continue
          box[0] = Math.min(box[0], x)
          box[1] = Math.min(box[1], y)
          box[2] = Math.max(box[2], x)
          box[3] = Math.max(box[3], y)
        }
      }
      assert.ok(box[2] >= box[0], `widget ${w.objectNumber} drew nothing`)
      let worst = 0
      for (let y = box[1]; y <= box[3]; y++) {
        for (let x = box[0]; x <= box[2]; x++) {
          const i = (y * width + x) * 4
          const a = alone.bytes[i + 3] / 255
          for (let c = 0; c < 3; c++) worst = Math.max(worst, Math.abs(Math.round(alone.bytes[i + c] * a + bare[i + c] * (1 - a)) - full[i + c]))
        }
      }
      assert.ok(worst <= 1, `widget ${w.objectNumber} drawn alone is ${worst} levels off the page's render`)
      if (w.page === 0) {
        const [l, b, r, t] = w.rect
        assert.deepEqual(box, [Math.floor(l), height - Math.ceil(t), Math.ceil(r) - 1, height - Math.floor(b) - 1], `widget ${w.objectNumber} is not in its /Rect`)
      }
    }

    // A draw fails while a render holds the page.
    {
      const page = m._FPDF_LoadPage(doc, 0)
      const bitmap = m._FPDFBitmap_CreateEx(612, 792, 4, 0, 0)
      const r = m._INK_Render_Start(bitmap, page, 0, 0, 612, 792, 0, FPDF_ANNOT, 0, 0, 0, 0)
      assert.notEqual(r, 0)
      assert.equal(m._INK_Annot_Draw(bitmap, page, agree.annotIndex, 0, 0), 0, 'a draw during a render')
      m._INK_Render_Close(r)
      assert.equal(m._INK_Annot_Draw(bitmap, page, agree.annotIndex, 0, 0), 1, 'a draw after the render closed')
      assert.equal(m._INK_Annot_Draw(0, page, agree.annotIndex, 0, 0), 0, 'a NULL bitmap')
      m._FPDFBitmap_Destroy(bitmap)
      m._FPDF_ClosePage(page)
    }

    assert.deepEqual(takeJson(m._INK_Scripts(doc)), { document: [], documentActions: {}, pageActions: [], truncated: false })
    assert.deepEqual(takeJson(m._INK_Doc_Facts(doc)), { title: 'Inkbar fixture', author: 'Inkbar', encrypted: false, openedAs: 'none' })
    assert.deepEqual(takeJson(m._INK_Struct_Order(doc, 0)), { order: [], truncated: false })

    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread[0]), 'a read or a draw changed what a save writes')
    assert.ok(sameSave(saveWith(doc, 1), unread[1]), 'a read or a draw changed what an incremental save writes')
  })
  for (const call of ['_INK_Annots_Page', '_INK_Struct_Order']) {
    assert.equal(m[call](0, 0), 0, `${call} of a NULL document`)
    withPdf(onePagePdf(''), (doc) => assert.equal(m[call](doc, 1), 0, `${call} of page 1 of a one-page file`))
  }
  for (const call of ['_INK_Form', '_INK_Scripts', '_INK_Doc_Facts']) assert.equal(m[call](0), 0, `${call} of a NULL document`)
}

{
  // Annotations whose dictionaries are wrong: /Rect missing, a colour of five
  // components and /Contents in no encoding, /IRT naming itself and a UTF-16
  // string with an unpaired surrogate, an /InkList entry that is not a number
  // and one that is not an array, /Rect with a string in it, and a direct
  // dictionary. No AcroForm.
  const pdf = onePagePdf('', { mediaBox: letterBox, page: '/Annots [6 0 R 7 0 R 8 0 R 9 0 R << /Subtype /Square /Rect [0 0 10 10] /F 2 >> 10 0 R]' }, [
    '<< /Type /Annot /Subtype /Square /C [1 0 0] >>',
    '<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /C [1 0 0 0 0] /Contents (a\\177b) /Name /Comment /T <FEFF00E9> /CA 0.5 >>',
    '<< /Type /Annot /Subtype /Text /Rect [30 30 10 10] /IRT 8 0 R /Contents <FEFFD800> /M (D:20261125) >>',
    '<< /Type /Annot /Subtype /Ink /Rect [0 0 (x) 10] /InkList [[1 2 /x 4] 5] /BS << /W 2 /S /D >> /Border [0 0 3] >>',
    `<< /Type /Annot /Subtype /Ink /Rect [0 0 10 10] /InkList [[${'1 '.repeat(600000)}] [${'2 '.repeat(600000)}]] >>`,
  ])
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const { annots, truncated } = takeJson(m._INK_Annots_Page(doc, 0))
    assert.equal(truncated, false)
    assert.deepEqual(
      annots.map((a) => [a.index, a.objectNumber]),
      [
        [0, 6],
        [1, 7],
        [2, 8],
        [3, 9],
        [4, undefined],
        [5, 10],
      ],
    )
    assert.deepEqual([annots[0].rect, annots[0].rectOk], [null, false], '/Rect missing')
    assert.deepEqual(annots[1].color, [1, 0, 0, 0, 0])
    assert.deepEqual(annots[1].contents, { bytes: hex('a\x7fb') })
    assert.deepEqual([annots[1].author, annots[1].icon, annots[1].opacity], ['é', 'Comment', 0.5])
    assert.deepEqual([annots[2].inReplyTo, annots[2].contents, annots[2].modified], [[8, 0], { bytes: 'feffd800' }, 'D:20261125'])
    assert.deepEqual([annots[2].rect, annots[2].rectOk], [[30, 30, 10, 10], true], 'corners as written')
    assert.deepEqual([annots[3].rect, annots[3].rectOk], [[0, 0, null, 10], false])
    assert.deepEqual(annots[3].inkLists, [[1, 2, null, 4], null])
    assert.deepEqual([annots[3].borderStyle, annots[3].border], [{ width: 2, style: 'D' }, [0, 0, 3]])
    assert.deepEqual([annots[4].flags, annots[4].hasAppearance, annots[4].appearance], [2, false, null])
    // /InkList is read up to 1,000,000 numbers in all.
    assert.deepEqual(
      annots[5].inkLists.map((path) => path.length),
      [600000, 400000],
    )
    assert.equal(annots[5].truncated, true)
    assert.equal(annots[4].truncated, undefined)
    const form = takeJson(m._INK_Form(doc))
    assert.deepEqual(form, { kind: 'none', needAppearances: false, fields: [], groups: [], calculationOrder: [], calculationOrderRefs: [], fieldTreeDepth: 0, fieldTreeRevisits: false, tooLarge: false, truncated: false })
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'a report changed what a save writes')
  })
}

{
  // A field tree whose /Kids loop, each node listing the other twice (walked
  // without care, the work doubles at each level), a field whose widget no
  // page holds, a field with no widget, a widget with no /T and no parent, and
  // a widget /Fields does not reach. /CO names a field, a page and a number.
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 8 0 R 10 0 R 11 0 R] /CO [8 0 R 3 0 R 12] /DA (/Helv 0 Tf 0 g) >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [11 0 R 12 0 R] >>`,
    '<< >>',
    '<< >>',
    '<< /T (a) /Kids [7 0 R 7 0 R] >>',
    '<< /T (b) /Parent 6 0 R /Kids [6 0 R 6 0 R] >>',
    '<< /FT /Tx /T (lost) /Kids [9 0 R] >>',
    '<< /Type /Annot /Subtype /Widget /Parent 8 0 R /Rect [0 0 10 10] >>',
    '<< /FT /Tx /T (unplaced) >>',
    '<< /Type /Annot /Subtype /Widget /FT /Tx /Rect [0 0 10 10] >>',
    '<< /Type /Annot /Subtype /Widget /FT /Btn /T (stray) /Rect [20 20 30 30] >>',
  ])
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const started = Date.now()
    const form = takeJson(m._INK_Form(doc))
    assert.ok(Date.now() - started < 5000, `the looping tree took ${Date.now() - started} ms`)
    assert.equal(form.fieldTreeRevisits, true)
    assert.equal(form.tooLarge, false)
    assert.equal(form.truncated, false)
    const field = (name) => form.fields.find((f) => f.name === name)
    assert.deepEqual(
      form.fields.map((f) => f.name).sort(),
      ['lost', 'stray', 'unplaced'],
    )
    assert.deepEqual(
      field('lost').widgets.map((w) => [w.objectNumber, w.page, w.annotIndex]),
      [[9, -1, -1]],
    )
    assert.deepEqual(field('unplaced').widgets, [])
    assert.deepEqual([field('stray').origin, field('stray').widgets[0].page, field('stray').widgets[0].annotIndex], ['recovered', 0, 1])
    assert.equal(field('lost').origin, 'acroform')
    assert.deepEqual(form.calculationOrder, [form.fields.indexOf(field('lost')), -1, -1])
    assert.deepEqual(form.calculationOrderRefs, [[8, 0], [3, 0], null])
    assert.equal(form.da, '/Helv 0 Tf 0 g')
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'the form report changed what a save writes')
  })
}

{
  // 50,001 fields: none is read, and the report says so at once. 200,001
  // annotations on a page: the report stops within 16 MB and 200,000 entries.
  const fields = 50001
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [${Array.from({ length: fields }, (_, i) => `${i + 4} 0 R`).join(' ')}] >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [${'<< /Subtype /Square /Rect [0 0 1 1] >> '.repeat(200001)}] >>`,
    ...Array.from({ length: fields }, (_, i) => `<< /FT /Tx /T (f${i}) >>`),
  ])
  withPdf(pdf, (doc) => {
    const started = Date.now()
    const form = takeJson(m._INK_Form(doc))
    assert.ok(Date.now() - started < 5000, `50,001 fields took ${Date.now() - started} ms`)
    assert.deepEqual([form.fields.length, form.tooLarge, form.truncated], [0, true, false], 'refused, not cut short')
    const ptr = m._INK_Annots_Page(doc, 0)
    const text = cString(ptr)
    m._INK_Json_Free(ptr)
    assert.ok(Buffer.byteLength(text) <= 16 * 1024 * 1024, `${Buffer.byteLength(text)} bytes of JSON`)
    const { annots, truncated } = JSON.parse(text)
    assert.equal(truncated, true)
    assert.ok(annots.length > 0 && annots.length <= 200000, `${annots.length} annotations`)
    assert.deepEqual(
      annots.slice(0, 2).map((a) => a.index),
      [0, 1],
    )
  })
}

{
  // 1,000 form fields and 50,001 widget annotations with no /FT, which the
  // form does not load: they do not count towards the limit.
  const fields = 1000
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [${Array.from({ length: fields }, (_, i) => `${i + 4} 0 R`).join(' ')}] >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [${'<< /Subtype /Widget /Rect [0 0 1 1] >> '.repeat(50001)}] >>`,
    ...Array.from({ length: fields }, (_, i) => `<< /FT /Tx /T (f${i}) >>`),
  ])
  withPdf(pdf, (doc) => {
    const form = takeJson(m._INK_Form(doc))
    assert.deepEqual([form.fields.length, form.tooLarge, form.truncated], [fields, false, false])
  })
}

{
  // 20,000 dictionaries that all share one /Kids array, which lists them all,
  // as the form's fields and as the /Names /JavaScript tree. Each array is
  // walked once.
  const n = 20000
  const refs = Array.from({ length: n }, (_, i) => `${i + 5} 0 R`).join(' ')
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [${refs}] >> /Names << /JavaScript << /Kids 4 0 R >> >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} >>`,
    `[${refs}]`,
    ...Array.from({ length: n }, (_, i) => `<< /T (f${i}) /Kids 4 0 R /Names [(s${i}) << /S /JavaScript /JS (x) >>] >>`),
  ])
  withPdf(pdf, (doc) => {
    let started = Date.now()
    const form = takeJson(m._INK_Form(doc))
    assert.ok(Date.now() - started < 2000, `the shared /Kids took ${Date.now() - started} ms`)
    assert.equal(form.fieldTreeRevisits, true)
    started = Date.now()
    const scripts = takeJson(m._INK_Scripts(doc))
    assert.ok(Date.now() - started < 2000, `the shared name tree took ${Date.now() - started} ms`)
    assert.deepEqual([scripts.document.length, scripts.document[0], scripts.truncated], [n, { name: 's0', source: 'x' }, false])
  })
}

{
  // 1,000 JavaScript actions whose /JS is one 3 MB stream, as the open
  // action's chain and as a widget's /A chain, and 2,000 widgets of one
  // field sharing a /MK /BC of 1,000,000 numbers: each report stops within
  // 16 MB and says so.
  const chain = `<< /S /JavaScript /JS 6 0 R /Next [${'<< /S /JavaScript /JS 6 0 R >> '.repeat(999)}] >>`
  const widgets = 2000
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /OpenAction ${chain} /AcroForm << /Fields [8 0 R 9 0 R] >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} >>`,
    '<< >>',
    '<< >>',
    streamObject('', `/* ${'x'.repeat(3 * 1024 * 1024)} */`, true),
    `[${'1 '.repeat(1000000)}]`,
    `<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (script) /Rect [0 0 10 10] /A ${chain} >>`,
    `<< /FT /Tx /T (wide) /Kids [${Array.from({ length: widgets }, (_, i) => `${i + 10} 0 R`).join(' ')}] >>`,
    ...Array.from({ length: widgets }, () => '<< /Type /Annot /Subtype /Widget /Parent 9 0 R /Rect [0 0 1 1] /MK << /BC 7 0 R >> >>'),
  ])
  withPdf(pdf, (doc) => {
    for (const call of ['_INK_Scripts', '_INK_Form']) {
      const ptr = m[call](doc)
      assert.notEqual(ptr, 0, call)
      const text = cString(ptr)
      m._INK_Json_Free(ptr)
      assert.ok(Buffer.byteLength(text) <= 16 * 1024 * 1024, `${call}: ${Buffer.byteLength(text)} bytes`)
      assert.equal(JSON.parse(text).truncated, true, `${call} was not cut`)
    }
  })
}

{
  // A field 40 levels down, below PDFium's walk of 33 levels, whose widget is
  // on the page; only the top and the bottom have /T. The page sweep climbs
  // from the widget and walks down again, so the field is the bottom
  // dictionary (object 45), not its widget (46): the fields and object numbers
  // EPDFForm_LoadModel gives for this file in the released v8165.8 module.
  const levels = 40
  const nodes = Array.from(
    { length: levels },
    (_, i) => `<< ${i === 0 || i === levels - 1 ? `/T (n${i})` : ''} ${i > 0 ? `/Parent ${i + 5} 0 R` : ''} ${i + 1 < levels ? `/Kids [${i + 7} 0 R]` : '/FT /Tx /Kids [46 0 R]'} >>`,
  )
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [46 0 R] >>',
    '<< >>',
    '<< >>',
    ...nodes,
    '<< /Type /Annot /Subtype /Widget /Parent 45 0 R /Rect [0 0 10 10] >>',
  ])
  withPdf(pdf, (doc) => {
    const form = takeJson(m._INK_Form(doc))
    assert.deepEqual(
      form.fields.map((f) => [f.name, f.objectNumber, f.widgets.map((w) => w.objectNumber)]),
      [['n0.n39', 45, [46]]],
    )
  })
}

{
  // Script reading is bounded by work, not only by bytes. A widget under 1,000
  // /Parent levels whose /AA are one dictionary of 1,000 keys, each the same
  // chain of 999 Named actions and one empty script; and a catalog /AA of
  // 200,000 keys, each that chain. Each shared object
  // is read once a report.
  // Objects |first| on: |count| - 1 Named actions, each naming the next, and
  // an empty script.
  const chain = (first, count) =>
    Array.from({ length: count }, (_, i) => (i + 1 < count ? `<< /S /Named /N /NextPage /Next ${first + i + 1} 0 R >>` : '<< /S /JavaScript /JS () >>'))
  const levels = 1000
  const keys = 200000
  // 6: the /AA of 1,000 keys; 7 to 1006: the chain; 1007 to 2006: the levels;
  // 2007: the widget.
  const pdf = buildPdf([
    `<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [2007 0 R] >> /AA << ${Array.from({ length: keys }, (_, i) => `/k${i} 7 0 R`).join(' ')} >> >>`,
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [2007 0 R] >>',
    '<< >>',
    '<< >>',
    `<< ${Array.from({ length: 1000 }, (_, i) => `/k${i} 7 0 R`).join(' ')} >>`,
    ...chain(7, 1000),
    ...Array.from({ length: levels }, (_, i) => `<< /AA 6 0 R ${i + 1 < levels ? `/Parent ${i + 1008} 0 R` : ''} >>`),
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (deep) /Rect [0 0 10 10] /Parent 1007 0 R /AA 6 0 R >>',
  ])
  withPdf(pdf, (doc) => {
    let started = Date.now()
    const form = takeJson(m._INK_Form(doc))
    assert.ok(Date.now() - started < 2000, `the shared /AA took ${Date.now() - started} ms`)
    assert.equal(Object.keys(form.fields[0].widgets[0].scripts).length, 1000)
    started = Date.now()
    const ptr = m._INK_Scripts(doc)
    const text = cString(ptr)
    m._INK_Json_Free(ptr)
    assert.ok(Date.now() - started < 2000, `200,000 /AA keys took ${Date.now() - started} ms`)
    assert.ok(Buffer.byteLength(text) <= 16 * 1024 * 1024, `${Buffer.byteLength(text)} bytes of JSON`)
    const scripts = JSON.parse(text)
    assert.equal(Object.keys(scripts.documentActions).length, keys)
    assert.deepEqual(scripts.documentActions.k0, [''])
  })
}

{
  // A field whose five widgets share one /AA of 1,000 keys, each holding a
  // script, at each of 1,000 /Parent levels: about 5,000,000 units of work,
  // past the allowance of 4,000,000. The report stops reading and says so.
  const levels = 1000
  // 6: the /AA; 7: the field; 8 to 12: the widgets; 13 on: the levels.
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R] >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>',
    '<< >>',
    '<< >>',
    `<< ${Array.from({ length: 1000 }, (_, i) => `/k${i} << /S /JavaScript /JS (s${i}) >>`).join(' ')} >>`,
    '<< /FT /Tx /T (five) /Kids [8 0 R 9 0 R 10 0 R 11 0 R 12 0 R] /AA 6 0 R /Parent 13 0 R >>',
    ...Array.from({ length: 5 }, () => '<< /Type /Annot /Subtype /Widget /Parent 7 0 R /Rect [0 0 10 10] /AA 6 0 R >>'),
    ...Array.from({ length: levels }, (_, i) => `<< /AA 6 0 R ${i + 1 < levels ? `/Parent ${i + 14} 0 R` : ''} >>`),
  ])
  withPdf(pdf, (doc) => {
    const started = Date.now()
    const form = takeJson(m._INK_Form(doc))
    assert.ok(Date.now() - started < 2000, `five widgets under 1,000 levels took ${Date.now() - started} ms`)
    assert.deepEqual([form.fields[0].truncated, form.truncated], [true, true], 'the work allowance ran out unmarked')
  })
}

{
  // 130 Flate /JS streams, each 4 MB of text from a few KB, under one /AA of
  // the catalog and of a widget: a report holds at most 16 MB of decoded text,
  // so it returns, cut.
  const bomb = deflateSync(Buffer.alloc(4 * 1024 * 1024, 'x'))
  const count = 130
  const stream = Buffer.concat([Buffer.from(`<< /Length ${bomb.length} /Filter /FlateDecode >>\nstream\n`), bomb, Buffer.from('\nendstream')])
  // 6: the /AA; 7: the widget; 8 on: the streams.
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R] >> /AA 6 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>',
    '<< >>',
    '<< >>',
    `<< ${Array.from({ length: count }, (_, i) => `/k${i} << /S /JavaScript /JS ${i + 8} 0 R >>`).join(' ')} >>`,
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (bomb) /Rect [0 0 10 10] /AA 6 0 R >>',
    ...Array.from({ length: count }, () => stream),
  ])
  withPdf(pdf, (doc) => {
    for (const call of ['_INK_Scripts', '_INK_Form']) {
      const ptr = m[call](doc)
      assert.notEqual(ptr, 0, call)
      const text = cString(ptr)
      m._INK_Json_Free(ptr)
      assert.equal(JSON.parse(text).truncated, true, `${call} was not cut`)
    }
  })
}

{
  // One /JS stream of 5 MB, cut at 4 MB, read first through a parent's /AA
  // under field a (a's own /K is nearer) and then through field b's own
  // chain: b's source is cut too, and says so.
  const big = deflateSync(Buffer.alloc(5 * 1024 * 1024, 'y'))
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R 8 0 R] >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>',
    '<< >>',
    '<< >>',
    Buffer.concat([Buffer.from(`<< /Length ${big.length} /Filter /FlateDecode >>\nstream\n`), big, Buffer.from('\nendstream')]),
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (a) /Rect [0 0 10 10] /Parent 9 0 R /AA << /K << /S /JavaScript /JS (small) >> >> >>',
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (b) /Rect [0 20 10 30] /AA << /K << /S /JavaScript /JS 6 0 R >> >> >>',
    '<< /AA << /K << /S /JavaScript /JS 6 0 R >> >> >>',
  ])
  withPdf(pdf, (doc) => {
    const form = takeJson(m._INK_Form(doc))
    const widget = (name) => form.fields.find((f) => f.name === name).widgets[0]
    assert.deepEqual(widget('a').scripts, { K: ['small'] })
    // Cut at 4 MB, less the 5 characters of 'small' the decoded allowance
    // already holds.
    assert.equal(widget('b').scripts.K[0].length, 4 * 1024 * 1024 - 5)
    assert.equal(widget('b').truncated, true, 'a cut /JS reached a second time lost its mark')
  })
}

// A zlib stream that inflates to |piece| |times| over, built a piece at a
// time so the whole is never held.
function repeatedDeflate(piece, times) {
  const block = deflateRawSync(piece, { finishFlush: constants.Z_FULL_FLUSH })
  let a = 1n
  let b = 0n
  for (const byte of piece) {
    a = (a + BigInt(byte)) % 65521n
    b = (b + a) % 65521n
  }
  // Adler-32 of the piece repeated: each repeat adds the piece's sums, and
  // its length times the sum so far.
  let allA = 1n
  let allB = 0n
  for (let i = 0; i < times; i++) {
    allB = (allB + b + BigInt(piece.length) * (allA - 1n)) % 65521n
    allA = (allA + a - 1n) % 65521n
  }
  const adler = Buffer.alloc(4)
  adler.writeUInt32BE(Number((allB << 16n) | allA))
  return Buffer.concat([Buffer.from([0x78, 0x9c]), ...Array.from({ length: times }, () => block), Buffer.from([0x03, 0x00]), adler])
}
// A catalog whose /AA and a widget whose /AA hold the /JS |streams|, objects
// 8 on.
const scriptsPdf = (streams) =>
  buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R] >> /AA 6 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>',
    '<< >>',
    '<< >>',
    `<< ${streams.map((_, i) => `/k${i} << /S /JavaScript /JS ${i + 8} 0 R >>`).join(' ')} >>`,
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (f) /Rect [0 0 10 10] /AA 6 0 R >>',
    ...streams,
  ])
// Both reports of |pdf|, each checked to return within |within| ms.
function scriptReports(pdf, within, what) {
  return withPdf(pdf, (doc) =>
    ['_INK_Scripts', '_INK_Form'].map((call) => {
      const started = Date.now()
      const ptr = m[call](doc)
      const took = Date.now() - started
      assert.notEqual(ptr, 0, call)
      const report = takeJson(ptr)
      assert.ok(took < within, `${call} took ${took} ms over ${what}`)
      return report
    }),
  )
}

{
  // One /JS stream of 1 MB that inflates to 1,008 MB: only its first 16 MB
  // are inflated, and the source is cut.
  const bomb = repeatedDeflate(Buffer.alloc(16 * 1024 * 1024, 'a'), 63)
  assert.ok(bomb.length < 1100000, `${bomb.length} bytes stored`)
  const [scripts, form] = scriptReports(scriptsPdf([streamObject('/Filter /FlateDecode', bomb, true)]), 1000, 'a stream of 1,008 MB')
  assert.equal(scripts.documentActions.k0[0], 'a'.repeat(4 * 1024 * 1024))
  assert.equal(scripts.truncated, true, 'the cut source is not marked')
  const widget = form.fields[0].widgets[0]
  assert.equal(widget.scripts.k0[0].length, 4 * 1024 * 1024)
  assert.deepEqual([widget.truncated, form.truncated], [true, true], 'the cut source is not marked in the form')
}

{
  // 300 /JS streams that each inflate to 64 MB and leave no text: UTF-16 with
  // a language escape that never closes. What is decoded is charged, whatever
  // text it leaves, so the report stops decoding after 64 MB.
  const piece = Buffer.alloc(1024 * 1024)
  for (let i = 1; i < piece.length; i += 2) piece[i] = 0x41
  const silent = Buffer.concat([Buffer.from([0x78, 0x9c]), deflateRawSync(Buffer.from([0xfe, 0xff, 0x00, 0x1b]), { finishFlush: constants.Z_FULL_FLUSH }), repeatedDeflate(piece, 64).subarray(2)])
  const stream = streamObject('/Filter /FlateDecode', silent, true)
  const [scripts, form] = scriptReports(scriptsPdf(Array.from({ length: 300 }, () => stream)), 2000, '300 streams of 64 MB')
  assert.equal(Object.keys(scripts.documentActions).length, 300)
  assert.ok(Object.values(scripts.documentActions).every((sources) => sources.length === 1 && sources[0] === ''), 'a silent stream left text')
  assert.equal(scripts.truncated, true, 'the decode allowance ran out unmarked')
  assert.deepEqual([form.fields[0].widgets[0].truncated, form.truncated], [true, true], 'the decode allowance ran out unmarked in the form')
}

{
  // A script of 30,000 characters that Flate cannot shrink below 16 KB.
  let seed = 1
  const script = Array.from({ length: 30000 }, () => {
    seed ^= seed << 13
    seed ^= seed >>> 17
    seed ^= seed << 5
    return String.fromCharCode(48 + ((seed >>> 0) % 64))
  }).join('')
  const packed = deflateSync(Buffer.from(script, 'latin1'))
  assert.ok(packed.length * 1032 > 16 * 1024 * 1024 + 3, `${packed.length} bytes stored`)
  // PNG "up" rows of 100 bytes, as /Predictor 12 reads them.
  const rows = (text) => Buffer.concat(Array.from({ length: text.length / 100 }, (_, i) => Buffer.from(`\0${text.slice(i * 100, i * 100 + 100)}`, 'latin1')))
  const small = 'app.alert("predicted");'.padEnd(100) + ' '.repeat(100)
  const [scripts, form] = scriptReports(
    scriptsPdf([
      // k0: one FlateDecode, no /DecodeParms: inflated in full, not cut.
      streamObject('/Filter /FlateDecode', packed, true),
      // k1: no filter.
      streamObject('', Buffer.from(script, 'latin1'), true),
      // k2 and k3: a predictor and a chain, small enough that no decoding
      // of them can pass the cap: PDFium decodes them.
      streamObject('/Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 100 >>', deflateSync(rows(small)), true),
      streamObject('/Filter [/ASCIIHexDecode /FlateDecode]', Buffer.from(`${deflateSync(Buffer.from('chained()')).toString('hex')}>`), true),
      // k4 and k5: /DecodeParms PDFium reads as no predictor: inflated in
      // full like k0.
      streamObject('/Filter /FlateDecode /DecodeParms << >>', packed, true),
      streamObject('/Filter /FlateDecode /DecodeParms << /Predictor 1 >>', packed, true),
    ]),
    2000,
    'six ordinary scripts',
  )
  assert.deepEqual(scripts.documentActions, { k0: [script], k1: [script], k2: [small], k3: ['chained()'], k4: [script], k5: [script] })
  assert.equal(scripts.truncated, false, 'an ordinary script was marked cut')
  assert.deepEqual(form.fields[0].widgets[0].scripts, scripts.documentActions)
  assert.deepEqual([form.fields[0].widgets[0].truncated, form.truncated], [undefined, false])
  // The same predictor and chain over stored bytes that could decode past the
  // cap (stored size times 1,032 for Flate): not decoded, empty and cut.
  for (const stream of [
    streamObject('/Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 100 >>', deflateSync(rows(script)), true),
    streamObject('/Filter [/ASCIIHexDecode /FlateDecode]', Buffer.from(`${packed.toString('hex')}>`), true),
  ]) {
    const [cutScripts, cutForm] = scriptReports(scriptsPdf([stream]), 2000, 'a stream PDFium would decode whole')
    assert.deepEqual([cutScripts.documentActions, cutScripts.truncated], [{ k0: [''] }, true])
    assert.deepEqual([cutForm.fields[0].widgets[0].scripts, cutForm.fields[0].widgets[0].truncated, cutForm.truncated], [{ k0: [''] }, true, true])
  }
}

{
  // 1,000 /JS streams of 15 KB stored that inflate to a "0" and 15 MB of
  // spaces and then go through ASCIIHex, which leaves one byte. A stream
  // PDFium decodes is charged what its stages could produce, not what they
  // leave, so the report decodes two and stops.
  const packed = deflateSync(Buffer.concat([Buffer.from('0'), Buffer.alloc(15 * 1024 * 1024, ' ')]), { level: 9 })
  assert.ok(packed.length * 1032 <= 16 * 1024 * 1024 + 3, `${packed.length} bytes stored`)
  const stream = streamObject('/Filter [/FlateDecode /ASCIIHexDecode]', packed, true)
  const [scripts, form] = scriptReports(scriptsPdf(Array.from({ length: 1000 }, () => stream)), 2000, '1,000 streams of 15 MB that leave a byte')
  const sources = Object.values(scripts.documentActions).map(([source]) => source)
  assert.equal(sources.length, 1000)
  assert.equal(sources[0].length, 1, 'the first stream was not decoded')
  assert.ok(sources.filter((source) => source !== '').length <= 4, 'more streams were decoded than the allowance covers')
  assert.equal(scripts.truncated, true, 'the decode allowance ran out unmarked')
  assert.deepEqual([form.fields[0].widgets[0].truncated, form.truncated], [true, true], 'the decode allowance ran out unmarked in the form')
}

{
  // The same 15 MB of spaces under one Flate and a few hundred ASCIIHex
  // filters, each a stage that could hold 15.8 MB. The stored length and the
  // number of stages are picked so the stages' sum is a small number modulo
  // 2^32, the size of the module's size_t: the charge stops at the allowance
  // and cannot wrap, so none of 1,000 such streams is decoded.
  const packed = deflateSync(Buffer.concat([Buffer.from('0'), Buffer.alloc(15 * 1024 * 1024, ' ')]), { level: 9 })
  let stored = 0
  let stages = 0
  for (let length = packed.length; !stages && length <= 16256; length++) {
    for (let n = 257; n <= 1200; n++) {
      if ((length * 1032 * n) % 2 ** 32 < 30000) {
        stored = length
        stages = n
        break
      }
    }
  }
  assert.ok(stages, 'no chain wraps the sum')
  // Bytes after the end of the zlib data are not read.
  const padded = Buffer.concat([packed, Buffer.alloc(stored - packed.length)])
  const stream = streamObject(`/Filter [/FlateDecode${' /ASCIIHexDecode'.repeat(stages - 1)}]`, padded, true)
  const [scripts, form] = scriptReports(scriptsPdf(Array.from({ length: 1000 }, () => stream)), 2000, `1,000 streams of ${stages} stages`)
  const sources = Object.values(scripts.documentActions).map(([source]) => source)
  assert.equal(sources.length, 1000)
  assert.ok(sources.every((source) => source === ''), 'a stream past the allowance was decoded')
  assert.equal(scripts.truncated, true, 'the refused streams are not marked')
  assert.deepEqual([form.fields[0].widgets[0].truncated, form.truncated], [true, true], 'the refused streams are not marked in the form')
}

{
  // An encrypted file (RC4, 40 bits, empty user password) with a Flate /JS
  // stream: the bytes inflated are the decrypted ones.
  const rc4 = (key, data) => {
    const s = Uint8Array.from({ length: 256 }, (_, i) => i)
    for (let i = 0, j = 0; i < 256; i++) {
      j = (j + s[i] + key[i % key.length]) & 255
      ;[s[i], s[j]] = [s[j], s[i]]
    }
    const out = Buffer.alloc(data.length)
    for (let n = 0, i = 0, j = 0; n < data.length; n++) {
      i = (i + 1) & 255
      j = (j + s[i]) & 255
      ;[s[i], s[j]] = [s[j], s[i]]
      out[n] = data[n] ^ s[(s[i] + s[j]) & 255]
    }
    return out
  }
  const md5 = (...parts) => createHash('md5').update(Buffer.concat(parts)).digest()
  const pad = Buffer.from('28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a', 'hex')
  const id = Buffer.alloc(16, 7)
  const owner = rc4(md5(pad).subarray(0, 5), pad)
  const key = md5(pad, owner, Buffer.from([0xfc, 0xff, 0xff, 0xff]), id).subarray(0, 5)
  const user = rc4(key, pad)
  const source = 'app.alert("locked");\n'.repeat(500)
  // Object 4's key: the file key, then the object and generation numbers.
  const stored = rc4(md5(key, Buffer.from([4, 0, 0, 0, 0])).subarray(0, 10), deflateSync(Buffer.from(source)))
  const pdf = buildPdf(
    [
      '<< /Type /Catalog /Pages 2 0 R /OpenAction << /S /JavaScript /JS 4 0 R >> >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>',
      streamObject('/Filter /FlateDecode', stored, true),
      `<< /Filter /Standard /V 1 /R 2 /O <${owner.toString('hex')}> /U <${user.toString('hex')}> /P -4 >>`,
    ],
    `/Encrypt 5 0 R /ID [<${id.toString('hex')}> <${id.toString('hex')}>]`,
  )
  withPdf(pdf, (doc) => {
    assert.equal(takeJson(m._INK_Doc_Facts(doc)).encrypted, true)
    const scripts = takeJson(m._INK_Scripts(doc))
    assert.deepEqual([scripts.documentActions.OpenAction, scripts.truncated], [[source], false])
  })
}

{
  // A field tree 1,005 levels deep: the walk stops at 1,000; PDFium loads 33
  // levels, so the text field at the bottom is not a field.
  const levels = 1005
  const nodes = Array.from({ length: levels }, (_, i) => `<< /T (n${i}) ${i > 0 ? `/Parent ${i + 5} 0 R` : ''} ${i + 1 < levels ? `/Kids [${i + 7} 0 R]` : '/FT /Tx'} >>`)
  const pdf = onePagePdf('', { mediaBox: letterBox, catalog: '/AcroForm << /Fields [6 0 R] >>' }, nodes)
  withPdf(pdf, (doc) => {
    const form = takeJson(m._INK_Form(doc))
    assert.deepEqual([form.fieldTreeDepth, form.truncated, form.fields.length], [1000, true, 0])
  })
}

{
  // Widget facts: /MK, /Q, /TU, /PMD, /DA inherited, scripts by event from the
  // widget and its field, the widget's own /A, a URI and a Named action.
  const js = (source) => `<< /S /JavaScript /JS (${source}) >>`
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 8 0 R 9 0 R] /DA (/Helv 0 Tf 0 g) >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [7 0 R 8 0 R 9 0 R] >>`,
    '<< >>',
    '<< >>',
    `<< /FT /Tx /T (total) /Q 1 /DA (/Helv 9 Tf 1 0 0 rg) /AA << /C ${js('calc()')} /K ${js('parentK()')} >> /Kids [7 0 R] >>`,
    `<< /Type /Annot /Subtype /Widget /Parent 6 0 R /Rect [0 0 100 20] /F 4 /AA << /K ${js('ownK()')} /Fo << /S /JavaScript /JS (focus\\(\\)) /Next [${js('then()')} << /S /URI /URI (x) >>] >> /U << /S /URI /URI (x) >> >> /A ${js('click()')} /MK << /BC [1 0 0] /BG [0 0 1 0 0] /R 90 /CA (OK) >> /TU (tip) /PMD << >> >>`,
    '<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (go) /Rect [0 30 100 50] /A << /S /URI /URI (https://example.com/a) >> >>',
    '<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (print) /Rect [0 60 100 80] /A << /S /Named /N /Print >> >>',
  ])
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const form = takeJson(m._INK_Form(doc))
    const field = (name) => form.fields.find((f) => f.name === name)
    const w = field('total').widgets[0]
    assert.deepEqual(w.scripts, { A: ['click()'], C: ['calc()'], Fo: ['focus()', 'then()'], K: ['ownK()'] })
    assert.deepEqual(w.action, { s: 'JavaScript' })
    assert.deepEqual([w.borderColor, w.backgroundColor, w.rotation, w.caption], [[1, 0, 0], [0, 0, 1, 0, 0], 90, 'OK'])
    assert.deepEqual([w.q, w.tooltip, w.da, w.pmd, w.flags], [1, 'tip', '/Helv 9 Tf 1 0 0 rg', true, 4])
    assert.deepEqual(field('go').widgets[0].action, { s: 'URI', uriHex: hex('https://example.com/a') })
    assert.deepEqual(field('go').widgets[0].scripts, {})
    assert.deepEqual(field('print').widgets[0].action, { s: 'Named', name: 'Print' })
    assert.equal(field('go').kind, 'button')
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'the form report changed what a save writes')
  })
}

{
  // Toggles whose appearances are odd: a checkbox with no Off appearance, an
  // on-state with a space (/Yes#20No) and one with a byte past ASCII
  // (/On#E9), a text widget with no appearance and an open popup with none,
  // whose parent note has text for it.
  // Drawing them writes nothing to the file.
  const box = (x) => `/Rect [${x} 10 ${x + 20} 30]`
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R 8 0 R 9 0 R] /DA (/Helv 12 Tf 0 g) >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Annots [6 0 R 7 0 R 8 0 R 9 0 R 10 0 R] >>`,
    '<< >>',
    '<< >>',
    `<< /Type /Annot /Subtype /Widget /FT /Btn /T (noOff) ${box(10)} /AS /Yes /AP << /N << /Yes 11 0 R >> >> >>`,
    `<< /Type /Annot /Subtype /Widget /FT /Btn /T (spaced) ${box(40)} /AS /Off /AP << /N << /Off 12 0 R /Yes#20No 11 0 R >> >> >>`,
    `<< /Type /Annot /Subtype /Widget /FT /Btn /T (accent) ${box(70)} /AS /Off /AP << /N << /Off 12 0 R /On#E9 11 0 R >> >> >>`,
    '<< /Type /Annot /Subtype /Widget /FT /Tx /T (bare) /Rect [100 10 200 30] /V (hello) >>',
    '<< /Type /Annot /Subtype /Popup /Rect [0 50 100 90] /Open true /Parent 13 0 R >>',
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 20 20]', '0 0 1 rg 0 0 20 20 re f'),
    streamObject('/Type /XObject /Subtype /Form /BBox [0 0 20 20]', '1 0 0 rg 0 0 20 20 re f'),
    '<< /Type /Annot /Subtype /Text /Rect [0 90 20 100] /Contents (A note) /T (Sam) /Popup 10 0 R >>',
  ])
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const form = takeJson(m._INK_Form(doc))
    const widget = (name) => form.fields.find((f) => f.name === name).widgets[0]
    assert.deepEqual([widget('noOff').states, widget('noOff').onState], [['Yes'], 'Yes'])
    assert.equal(drawAnnot(doc, 0, 0, 'Off').drew, 0, 'Off where there is none')
    assert.ok(inked(drawAnnot(doc, 0, 0, null).bytes), 'the checked box with no Off drew nothing')
    // The on-state name round-trips into the draw as the report writes it.
    assert.equal(widget('spaced').onState, 'Yes No')
    const spacedOff = drawAnnot(doc, 0, 1, 'Off')
    const spacedOn = drawAnnot(doc, 0, 1, widget('spaced').onState)
    assert.deepEqual([spacedOff.drew, spacedOn.drew], [1, 1])
    assert.ok(!spacedOff.bytes.equals(spacedOn.bytes), '/Yes#20No draws as Off')
    // A name's bytes past ASCII are code points U+0080 to U+00FF: the draw
    // takes them back as those bytes, not as their UTF-8.
    assert.equal(widget('accent').onState, 'Oné')
    const accentOn = drawAnnot(doc, 0, 2, Buffer.from(widget('accent').onState, 'latin1'))
    assert.equal(accentOn.drew, 1)
    assert.ok(inked(accentOn.bytes), 'the on-state with a byte past ASCII drew nothing')
    assert.equal(drawAnnot(doc, 0, 2, widget('accent').onState).drew, 0, 'a state given as UTF-8')
    assert.equal(widget('bare').hasAppearance, false)
    drawAnnot(doc, 0, 3, null)
    assert.equal(drawAnnot(doc, 0, 4, null).drew, 0, 'an open popup')
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'a draw changed what a save writes')
  })
}

{
  // The document's scripts: a name tree whose first script's /Next names
  // itself and whose second is a stream, an open action with a /Next, /AA on
  // the catalog and on the page.
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /Names << /JavaScript 6 0 R >> /OpenAction << /S /JavaScript /JS (open\\(\\)) /Next << /S /JavaScript /JS (next\\(\\)) >> >> /AA << /WC << /S /JavaScript /JS (bye\\(\\)) >> /WS << /S /URI /URI (x) >> >> >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /AA << /O << /S /JavaScript /JS (opened\\(\\)) >> /C << /S /URI /URI (x) >> >> >>`,
    '<< >>',
    '<< >>',
    '<< /Kids [9 0 R] >>',
    '<< /S /JavaScript /JS (var a = 1;) /Next 7 0 R >>',
    streamObject('', 'var b = "two";'),
    '<< /Names [(init) 7 0 R <FEFF00E9> << /S /JavaScript /JS 8 0 R >>] >>',
  ])
  withPdf(pdf, (doc) => {
    assert.deepEqual(takeJson(m._INK_Scripts(doc)), {
      document: [
        { name: 'init', source: 'var a = 1;' },
        { name: 'é', source: 'var b = "two";' },
      ],
      documentActions: { OpenAction: ['open()', 'next()'], WC: ['bye()'] },
      pageActions: [{ page: 0, scripts: { O: ['opened()'] } }],
      truncated: false,
    })
  })
}

{
  // Structure order: the tree reaches annotation 8, then 7 through a nested
  // element (whose /K also points back up), then 6; 5 is not an annotation of
  // the page, and 6 is reached twice.
  const pdf = buildPdf([
    '<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 9 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    `<< /Type /Page /Parent 2 0 R /MediaBox ${letterBox} /Annots [6 0 R 7 0 R 8 0 R] /Tabs /S >>`,
    '<< >>',
    '<< >>',
    '<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] >>',
    '<< /Type /Annot /Subtype /Link /Rect [0 20 10 30] >>',
    '<< /Type /Annot /Subtype /Link /Rect [0 40 10 50] >>',
    '<< /Type /StructTreeRoot /K 10 0 R >>',
    '<< /Type /StructElem /S /Document /K [<< /Type /OBJR /Obj 8 0 R >> 11 0 R << /Type /OBJR /Obj 6 0 R >> << /Type /OBJR /Obj 5 0 R >>] >>',
    '<< /Type /StructElem /S /P /K [5 << /Type /OBJR /Obj 7 0 R >> 10 0 R << /Type /OBJR /Obj 6 0 R >>] >>',
  ])
  withPdf(pdf, (doc) => {
    assert.deepEqual(takeJson(m._INK_Struct_Order(doc, 0)), { order: [8, 7, 6], truncated: false })
    assert.equal(takeJson(m._INK_Annots_Page(doc, 0)).tabs, 'S')
  })
}

{
  // /Info's title and author, and an encrypted file opened with its user and
  // its owner password.
  withPdf(onePagePdf('', { trailer: '/Info 6 0 R' }, ['<< /Title (Facts) /Author <FEFF00C9> /Subject 5 >>']), (doc) => {
    assert.deepEqual(takeJson(m._INK_Doc_Facts(doc)), { title: 'Facts', author: 'É', encrypted: false, openedAs: 'none' })
  })
  withPdf(onePagePdf('', { trailer: '/Info << /Title <FEFF00> >>' }), (doc) => {
    assert.deepEqual(takeJson(m._INK_Doc_Facts(doc)), { title: { bytes: 'feff00' }, encrypted: false, openedAs: 'none' }, 'a direct /Info')
  })
  const encrypted = readFileSync(new URL('../testing/resources/encrypted.pdf', import.meta.url))
  for (const [password, openedAs] of [
    ['1234', 'user'],
    ['5678', 'owner'],
  ]) {
    const buf = heapBytes(encrypted)
    const doc = withStrings([password], (pw) => m._FPDF_LoadMemDocument64(buf, encrypted.length, pw))
    assert.notEqual(doc, 0, `encrypted.pdf did not open with ${password}`)
    assert.deepEqual(takeJson(m._INK_Doc_Facts(doc)), { title: 'Title', encrypted: true, openedAs }, `opened with ${password}`)
    m._FPDF_CloseDocument(doc)
    m._free(buf)
  }
}

{
  // A direct annotation is left out of a render by its place in /Annots.
  const pdf = onePagePdf('', {
    page: '/Annots [<< /Type /Annot /Subtype /Square /Rect [100 20 300 120] /AP << /N 6 0 R >> >>]',
  }, [streamObject('/Type /XObject /Subtype /Form /BBox [100 20 300 120]', '1 0 0 rg 100 20 200 100 re f')])
  const neverRendered = withPdf(pdf, (doc) => saveWith(doc, NO_INCREMENTAL))
  withPdf(pdf, (doc) => {
    const withAnnots = referenceRender(doc)
    const withoutAnnots = referenceRender(doc, { flags: 0 })
    assert.ok(!withAnnots.equals(withoutAnnots), 'the direct square draws nothing')
    assert.ok(inkRender(doc, { skipAt: [0] }).bytes.equals(withoutAnnots), 'the square left out by place was drawn')
    assert.ok(inkRender(doc, { skipAt: [1] }).bytes.equals(withAnnots), 'a place past /Annots left the square out')
    assert.ok(referenceRender(doc).equals(withAnnots), 'the square stays hidden after the render')
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), neverRendered), 'a render by place changed what a save writes')
  })
}

{
  // Facts PDF.js reads that the reports carry as the file has them: a choice
  // field's /I (here disagreeing with /V, inherited from a parent, with wrong
  // entries, and not an array), /V and /DV present as null against absent, a
  // name inside a /V array, a Launch and a GoToR action's /F, border dashes
  // (valid, from /Border, invalid, all zeros, not arrays) and reals written
  // as the shortest decimal that reads back as PDFium's float.
  const widget = (rest) => `<< /Type /Annot /Subtype /Widget /Rect [10 10 60 30] ${rest} >>`
  const square = (rest) => `<< /Type /Annot /Subtype /Square /Rect [0 0 10 10] ${rest} >>`
  const pdf = onePagePdf(
    '',
    { catalog: '/AcroForm 6 0 R', page: '/Annots [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 13 0 R 15 0 R 16 0 R 17 0 R 18 0 R 19 0 R 20 0 R 21 0 R]' },
    [
      '<< /Fields [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 13 0 R 14 0 R] >>',
      widget('/FT /Ch /Ff 2097152 /T (list) /Opt [(a) (b) (c)] /V (a) /I [1 2]'),
      widget('/FT /Tx /T (vnull) /V null /DV null'),
      widget('/FT /Tx /T (vnone)'),
      widget('/FT /Ch /Ff 2097152 /T (varr) /Opt [(x) (y)] /V [/Nm (str) 3] /DV [/D1] /I [(x) /n 2.5 -1]'),
      widget('/FT /Btn /Ff 65536 /T (launch) /A << /S /Launch /F (app.exe) >>'),
      widget('/FT /Btn /Ff 65536 /T (gotor) /A << /S /GoToR /F << /Type /Filespec /F (other.pdf) /UF <FEFF00E9> >> /D [0 /Fit] >>'),
      widget('/FT /Btn /Ff 65536 /T (oddfile) /I 3 /A << /S /Launch /F 5 >>'),
      '<< /T (group) /FT /Ch /V (p) /I [0] /Kids [15 0 R] >>',
      widget('/Parent 14 0 R /T (kid)'),
      square('/BS << /W 0.4 /S /D /D [3 2] >> /C [0.1 0.4 0.7] /CA 0.4'),
      square('/Border [0 0 1 [4 2]]'),
      square('/BS << /D [-1 (x) 0] >> /Border [0 0 1 5]'),
      square('/BS << /D [0 0] >> /Border [0 0 1 [] 9]'),
      square('/BS << /D 7 >> /Border [0 0 1 null]'),
      // One array of 1,000,001 numbers as both dashes: the first is cut at
      // 1,000,000, the second by the report's budget, and the report is
      // still JSON.
      square('/BS << /D 22 0 R >> /Border [0 0 1 22 0 R]'),
      `[${'1 '.repeat(1000001)}]`,
    ],
  )
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const failures = []
    const check = (label, fn) => {
      try {
        fn()
      } catch (error) {
        failures.push(`${label}: ${error.message.split('\n')[0]}`)
      }
    }
    const form = takeJson(m._INK_Form(doc))
    const field = (name) => form.fields.find((f) => f.name === name)
    const { annots, truncated } = takeJson(m._INK_Annots_Page(doc, 0))
    const annot = (i) => annots.find((a) => a.index === i)

    check('/I disagreeing with /V', () => assert.deepEqual([field('list').value, field('list').selectedIndices], ['a', [1, 2]]))
    check('/I inherited from the parent', () => assert.deepEqual([field('group.kid').value, field('group.kid').selectedIndices], ['p', [0]]))
    check('/I with wrong entries', () => assert.deepEqual(field('varr').selectedIndices, [null, null, 2.5, -1]))
    check('/I not an array', () => assert.equal(field('oddfile').selectedIndices, null))
    check('no /I', () => assert.ok(!('selectedIndices' in field('vnull'))))
    check('/V null', () => assert.deepEqual([field('vnull').valueKind, field('vnull').valueNull, field('vnull').defaultValueNull], ['none', true, true]))
    check('no /V', () => assert.deepEqual([field('vnone').valueKind, 'valueNull' in field('vnone'), 'defaultValueNull' in field('vnone')], ['none', false, false]))
    check('a name in a /V array', () => assert.deepEqual([field('varr').valueKind, field('varr').value, field('varr').defaultValue], ['array', [{ name: 'Nm' }, 'str', null], [{ name: 'D1' }]]))
    check('Launch /F', () => assert.deepEqual(field('launch').widgets[0].action, { s: 'Launch', fileKind: 'string', fileHex: hex('app.exe') }))
    check('GoToR /F', () => assert.deepEqual(field('gotor').widgets[0].action, { s: 'GoToR', fileKind: 'spec', fileSpecFHex: hex('other.pdf'), fileSpecUFHex: 'feff00e9' }))
    check('Launch /F of another type', () => assert.deepEqual(field('oddfile').widgets[0].action, { s: 'Launch', fileKind: 'other' }))
    check('/BS /D', () => assert.deepEqual(annot(8).borderStyle, { width: 0.4, style: 'D', dash: [3, 2] }))
    check('/Border dash', () => assert.deepEqual([annot(9).border, annot(9).borderDash], [[0, 0, 1], [4, 2]]))
    check('an invalid dash', () => assert.deepEqual([annot(10).borderStyle.dash, annot(10).borderDash], [[-1, null, 0], null]))
    check('an all-zero dash and an empty one', () => assert.deepEqual([annot(11).borderStyle.dash, annot(11).borderDash], [[0, 0], []]))
    check('dashes that are not arrays', () => assert.deepEqual([annot(12).borderStyle.dash, annot(12).borderDash], [null, null]))
    check('a long shared dash', () => {
      assert.equal(annot(13).borderStyle.dash.length, 1000000)
      assert.ok(annot(13).borderDash.length < 1000000, 'the budget did not cut the second dash')
      assert.deepEqual([annot(13).truncated, truncated], [true, true])
    })
    check('reals written short', () => assert.deepEqual([annot(8).color, annot(8).opacity], [[0.1, 0.4, 0.7], 0.4]))
    // Absent when the file has none.
    check('no dash', () => assert.ok(!('borderDash' in annot(0)) && !('borderStyle' in annot(0))))
    assert.deepEqual(failures, [], failures.join('\n'))
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'a report changed what a save writes')
  })
}

{
  // How numbers are written, read from the report's text, which parsed values
  // cannot show: whole reals without an exponent or a point, minus zero, a
  // denormal, an unsigned integer past INT_MAX written exactly, a large real,
  // a colour component of n/255, and 0.4 and 0.1 as written.
  const tiny = `0.${'0'.repeat(44)}14`
  const pdf = onePagePdf('', { page: '/Annots [6 0 R]' }, [
    `<< /Type /Annot /Subtype /Square /Rect [100.0 0 600.0 10.5] /C [-0.0 ${tiny} 3000000000 3000000000.5 0.9490196 0.4 0.1] /CA 0.1 >>`,
  ])
  withPdf(pdf, (doc) => {
    const ptr = m._INK_Annots_Page(doc, 0)
    const text = cString(ptr)
    m._INK_Json_Free(ptr)
    assert.ok(text.includes('"rect":[100,0,600,10.5]'), text)
    assert.ok(text.includes('"color":[-0,1e-45,3000000000,3e+09,0.9490196,0.4,0.1]'), text)
    assert.ok(text.includes('"opacity":0.1'), text)
    const { annots } = JSON.parse(text)
    assert.ok(Object.is(annots[0].color[0], -0), 'minus zero')
    assert.deepEqual(
      annots[0].color.slice(1).map((v) => Math.fround(v)),
      [1e-45, 3000000000, 3000000000.5, 0.9490196, 0.4, 0.1].map((v) => Math.fround(v)),
      'each reads back as the float',
    )
  })
}

{
  // Field dictionaries that are no field's own: a three-level tree (group, sub,
  // terminals a and b, listed b first) with a script on sub, /MK and /P on
  // group and /CO naming group; a radio field's /Opt; /I on a parent and on a
  // widget kid; a tree whose /Kids loop back to its root.
  const widget = (rest) => `<< /Type /Annot /Subtype /Widget /Rect [10 10 60 30] ${rest} >>`
  const pdf = onePagePdf(
    '',
    { catalog: '/AcroForm 6 0 R', page: '/Annots [9 0 R 10 0 R 12 0 R 13 0 R 15 0 R 17 0 R 18 0 R 21 0 R]' },
    [
      '<< /Fields [7 0 R 11 0 R 14 0 R 16 0 R 19 0 R] /CO [7 0 R 9 0 R] >>',
      '<< /T (group) /Kids [8 0 R] /MK << /BC [1 0 0] /BG [0 1 0] /R 90 >> /P 3 0 R >>',
      '<< /T (sub) /Parent 7 0 R /Kids [10 0 R 9 0 R] /AA << /K << /S /JavaScript /JS (subK) >> >> >>',
      widget('/T (a) /FT /Tx /Parent 8 0 R'),
      widget('/T (b) /FT /Tx /Parent 8 0 R'),
      '<< /T (r) /FT /Btn /Ff 49152 /Opt [(Alpha) <FEFF00E9> 5] /Kids [12 0 R 13 0 R] >>',
      widget('/Parent 11 0 R'),
      widget('/Parent 11 0 R'),
      '<< /T (lp) /I [1] /Kids [15 0 R] >>',
      widget('/T (list) /FT /Ch /Opt [(x) (y)] /Parent 14 0 R'),
      '<< /T (list2) /FT /Ch /Opt [(x) (y)] /I [0] /Kids [17 0 R 18 0 R] >>',
      widget('/Parent 16 0 R /I [1]'),
      widget('/Parent 16 0 R'),
      '<< /T (loop) /Kids [20 0 R] >>',
      '<< /T (inner) /Parent 19 0 R /Kids [19 0 R 21 0 R] >>',
      widget('/T (w) /FT /Tx /Parent 20 0 R'),
    ],
  )
  withPdf(pdf, (doc) => {
    const unread = saveWith(doc, NO_INCREMENTAL)
    const failures = []
    const check = (label, fn) => {
      try {
        fn()
      } catch (error) {
        failures.push(`${label}: ${error.message.split('\n')[0]}`)
      }
    }
    const form = takeJson(m._INK_Form(doc))
    const field = (name) => form.fields.find((f) => f.name === name)
    const group = (name) => form.groups?.find((g) => g.name === name)
    const kid = (n, hasKids = false) => ({ ref: [n, 0], dictionary: true, hasT: true, hasKids })

    check('the groups, in walk order', () =>
      assert.deepEqual(
        form.groups.map((g) => [g.name, g.objectNumber, g.walkIndex]),
        [
          ['group', 7, 0],
          ['group.sub', 8, 1],
          ['lp', 14, 7],
          ['loop', 19, 12],
          ['loop.inner', 20, 13],
        ],
      ),
    )
    check('group', () =>
      assert.deepEqual(group('group'), {
        name: 'group',
        objectNumber: 7,
        generation: 0,
        walkIndex: 0,
        kids: [kid(8, true)],
        borderColor: [1, 0, 0],
        backgroundColor: [0, 1, 0],
        rotation: 90,
        page: 0,
        scripts: {},
      }),
    )
    check('sub: kids in file order and its script', () =>
      assert.deepEqual([group('group.sub').kids, group('group.sub').scripts], [[kid(10), kid(9)], { K: ['subK'] }]),
    )
    check('the walk order rebuilt', () => {
      const entries = [...form.groups.map((g) => [g.walkIndex, g.name]), ...form.fields.filter((f) => 'walkIndex' in f).map((f) => [f.walkIndex, f.name])]
      assert.deepEqual(
        entries.sort((x, y) => x[0] - y[0]).map((e) => e[1]),
        ['group', 'group.sub', 'group.sub.b', 'group.sub.a', 'r', 'lp', 'lp.list', 'list2', 'loop', 'loop.inner', 'loop.inner.w'],
      )
    })
    check('/CO names the group by reference', () => assert.deepEqual([form.calculationOrder[0], form.calculationOrderRefs[0]], [-1, [group('group').objectNumber, 0]]))
    check('a radio field\'s /Opt', () => assert.deepEqual(field('r').opt, ['Alpha', 'é', null]))
    check('/I on the parent', () => assert.deepEqual([field('lp.list').selectedIndices, field('lp.list').selectedIndicesOwn], [[1], false]))
    check('/I on the field and on a widget kid', () => {
      const list2 = field('list2')
      assert.deepEqual([list2.selectedIndices, list2.selectedIndicesOwn, list2.widgets[0].selectedIndices, 'selectedIndices' in list2.widgets[1]], [[0], true, [1], false])
    })
    check('the loop ends', () => assert.deepEqual(group('loop.inner')?.kids, [kid(19, true), kid(21)]))
    assert.deepEqual(failures, [], failures.join('\n'))
    assert.equal(form.truncated, false)
    assert.ok(sameSave(saveWith(doc, NO_INCREMENTAL), unread), 'a report changed what a save writes')
  })
}

m._INK_Destroy()
// SMOKE_OUT, SMOKE_FORM_OUT and SMOKE_IMAGE_OUT name files to receive the saved
// documents, for qpdf --check: the appearance document, the text box, border
// and reply one, the enlarged images one, SMOKE_REDACT_OUT the redacted one,
// SMOKE_OVERLAY_OUT the one applied with a tagged overlay and SMOKE_SANITIZE_OUT
// the sanitised one.
if (process.env.SMOKE_OUT) writeFileSync(process.env.SMOKE_OUT, saved)
if (process.env.SMOKE_FORM_OUT) writeFileSync(process.env.SMOKE_FORM_OUT, replySaved, 'latin1')
if (process.env.SMOKE_IMAGE_OUT) writeFileSync(process.env.SMOKE_IMAGE_OUT, imagesSaved)
if (process.env.SMOKE_REDACT_OUT) writeFileSync(process.env.SMOKE_REDACT_OUT, redactSaved)
if (process.env.SMOKE_OVERLAY_OUT) writeFileSync(process.env.SMOKE_OVERLAY_OUT, overlaySaved)
if (process.env.SMOKE_SANITIZE_OUT) writeFileSync(process.env.SMOKE_SANITIZE_OUT, sanitizedSaved)
console.log('SMOKE_OK')
