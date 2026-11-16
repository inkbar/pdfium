// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Edward Coleridge Smith
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import createModule from './out/inkbar-pdfium.mjs'

const pdfPath = process.argv[2]
assert.ok(pdfPath, 'usage: node inkbar/smoke.mjs <one-page.pdf>')

const exportsUrl = new URL('./out/exports.txt', import.meta.url)
const names = readFileSync(fileURLToPath(exportsUrl), 'utf8').split('\n').filter(Boolean)
assert.equal(names.length, 23)

const m = await createModule()
for (const name of names) {
  assert.equal(typeof m[`_${name}`], 'function', `missing export ${name}`)
}
assert.equal(m._FPDF_GetMetaText, undefined)

m._INK_Init()
const bytes = readFileSync(pdfPath)
const buf = m._malloc(bytes.length)
m.HEAPU8.set(bytes, buf)
const doc = m._FPDF_LoadMemDocument64(buf, bytes.length, 0)
assert.notEqual(doc, 0, `load failed, error ${m._FPDF_GetLastError()}`)
assert.equal(m._FPDF_GetPageCount(doc), 1)

const writer = m._INK_Save(doc, 2)
assert.notEqual(writer, 0)
assert.ok(m._INK_WriterSize(writer) > 0)
const head = Buffer.from(m.HEAPU8.subarray(m._INK_WriterData(writer), m._INK_WriterData(writer) + 5)).toString('latin1')
assert.equal(head, '%PDF-')

m._INK_WriterFree(writer)
m._FPDF_CloseDocument(doc)
m._free(buf)
m._INK_Destroy()
console.log('SMOKE_OK')
