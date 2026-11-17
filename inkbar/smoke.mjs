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
assert.equal(names.length, 71)

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

// A 2x2 RGBA PNG with one transparent pixel, so the image carries an /SMask.
function tinyPng() {
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
  const ihdr = Buffer.from([0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0])
  const rows = Buffer.from([0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0, 0, 255])
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(rows)),
    chunk('IEND', Buffer.alloc(0)),
  ])
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

m._INK_Destroy()
// SMOKE_OUT names a file to receive the saved document, for qpdf --check.
if (process.env.SMOKE_OUT) writeFileSync(process.env.SMOKE_OUT, saved)
console.log('SMOKE_OK')
