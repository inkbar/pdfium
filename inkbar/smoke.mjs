// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
import assert from 'node:assert/strict'
import { readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { deflateSync, inflateSync } from 'node:zlib'
import createModule from './out/inkbar-pdfium.mjs'

const pdfPath = process.argv[2]
assert.ok(pdfPath, 'usage: node inkbar/smoke.mjs <one-page.pdf>')

const exportsUrl = new URL('./out/exports.txt', import.meta.url)
const names = readFileSync(fileURLToPath(exportsUrl), 'utf8').split('\n').filter(Boolean)
assert.equal(names.length, 78)

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

m._INK_Destroy()
// SMOKE_OUT, SMOKE_FORM_OUT and SMOKE_IMAGE_OUT name files to receive the saved
// documents, for qpdf --check: the appearance document, the text box, border
// and reply one, and the enlarged images one.
if (process.env.SMOKE_OUT) writeFileSync(process.env.SMOKE_OUT, saved)
if (process.env.SMOKE_FORM_OUT) writeFileSync(process.env.SMOKE_FORM_OUT, replySaved, 'latin1')
if (process.env.SMOKE_IMAGE_OUT) writeFileSync(process.env.SMOKE_IMAGE_OUT, imagesSaved)
console.log('SMOKE_OK')
