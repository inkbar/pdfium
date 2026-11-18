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
assert.equal(names.length, 83)

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
  const applied = editPage(source, (doc, page) => {
    const mark = addMark(page, region)
    assert.equal(applyPage(doc, page, [mark], false).result, 1)
    m._FPDFPage_CloseAnnot(mark)
  })
  // The apply does not enter patterns; the check still sees the text.
  const [after] = withDoc(applied, (doc) => inspectPage(doc, 0, [region]))
  assert.ok(after.patternContent > 0, 'the check lost the pattern text after the apply')
}

// reads tiling patterns inside patterns and inside annotation appearances
{
  const tile = '/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 50 20] /XStep 50 /YStep 20'
  const source = onePagePdf(
    '/Pattern cs /P1 scn 50 50 200 100 re f',
    { resources: '/Pattern << /P1 7 0 R >>', page: '/Annots [8 0 R]' },
    [
      streamObject(`${tile} /Resources << /Font << /F1 5 0 R >> >>`, 'BT /F1 10 Tf 0 5 Td (TILE) Tj ET'),
      streamObject(`${tile} /Resources << /Pattern << /P0 6 0 R >> >>`, '/Pattern cs /P0 scn 0 0 50 20 re f'),
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
