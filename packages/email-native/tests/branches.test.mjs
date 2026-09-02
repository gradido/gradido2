/*
 * The branch markers, tested without an extraction.
 *
 * These decide WHICH of the 810 documents ships, so a defect here is a mail with
 * the wrong sentence in it -- and the one case with no coverage in the templates
 * is the three-way @if/@elif/@else, which only emailChangeSupport uses.
 */
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { scanBranches, selectBranches } from '../tools/branches.mjs'

/** An in-memory file tree, so nothing here touches the disk. */
const io = (files) => ({
  readFile: (f) => {
    if (!(f in files)) throw new Error(`no such file: ${f}`)
    return files[f]
  },
  resolve: (from, to) => to, // flat namespace; the real one resolves relative paths
})

const lines = (s) => s.split('\n').filter((l) => l.trim()).join('\n')

test('@if without @else has two cases: the block and no block', () => {
  const src = ['before', '<!--@if logoUrl-->', 'LOGO', '<!--@endif-->', 'after'].join('\n')
  assert.deepEqual(scanBranches('a', io({ a: src })), [{ id: 'logoUrl', cases: 2 }])
  assert.equal(lines(selectBranches(src, { logoUrl: 0 })), 'before\nLOGO\nafter')
  assert.equal(lines(selectBranches(src, { logoUrl: 1 })), 'before\nafter')
})

test('@if/@else has two cases and picks one of them', () => {
  const src = ['<!--@if minutes-->', 'WITH', '<!--@else-->', 'WITHOUT', '<!--@endif-->'].join('\n')
  assert.deepEqual(scanBranches('a', io({ a: src })), [{ id: 'minutes', cases: 2 }])
  assert.equal(lines(selectBranches(src, { minutes: 0 })), 'WITH')
  assert.equal(lines(selectBranches(src, { minutes: 1 })), 'WITHOUT')
})

test('@if/@elif/@else has three cases, one per alternative', () => {
  const src = [
    '<!--@if typoCorrection-->',
    'TYPO',
    '<!--@elif takeBack-->',
    'TAKEBACK',
    '<!--@else-->',
    'NORMAL',
    '<!--@endif-->',
  ].join('\n')
  assert.deepEqual(scanBranches('a', io({ a: src })), [{ id: 'typoCorrection', cases: 3 }])
  assert.equal(lines(selectBranches(src, { typoCorrection: 0 })), 'TYPO')
  assert.equal(lines(selectBranches(src, { typoCorrection: 1 })), 'TAKEBACK')
  assert.equal(lines(selectBranches(src, { typoCorrection: 2 })), 'NORMAL')
})

test('@elif without @else keeps the implicit empty case', () => {
  const src = ['<!--@if a-->', 'A', '<!--@elif b-->', 'B', '<!--@endif-->'].join('\n')
  assert.deepEqual(scanBranches('a', io({ a: src })), [{ id: 'a', cases: 3 }])
  assert.equal(lines(selectBranches(src, { a: 0 })), 'A')
  assert.equal(lines(selectBranches(src, { a: 1 })), 'B')
  assert.equal(lines(selectBranches(src, { a: 2 })), '')
})

test('a missing choice is case 0, which is what an unset variant means', () => {
  const src = ['<!--@if x-->', 'FIRST', '<!--@else-->', 'SECOND', '<!--@endif-->'].join('\n')
  assert.equal(lines(selectBranches(src, {})), 'FIRST')
})

test('nesting selects on every enclosing branch at once', () => {
  const src = [
    '<!--@if outer-->',
    'O',
    '<!--@if inner-->',
    'OI',
    '<!--@else-->',
    'OX',
    '<!--@endif-->',
    '<!--@else-->',
    'X',
    '<!--@endif-->',
  ].join('\n')
  assert.deepEqual(scanBranches('a', io({ a: src })), [
    { id: 'outer', cases: 2 },
    { id: 'inner', cases: 2 },
  ])
  assert.equal(lines(selectBranches(src, { outer: 0, inner: 0 })), 'O\nOI')
  assert.equal(lines(selectBranches(src, { outer: 0, inner: 1 })), 'O\nOX')
  // The inner choice cannot resurrect a block the outer one dropped.
  assert.equal(lines(selectBranches(src, { outer: 1, inner: 0 })), 'X')
  assert.equal(lines(selectBranches(src, { outer: 1, inner: 1 })), 'X')
})

test('branches inside an include count, in document order', () => {
  const files = {
    tpl: ['<!--@if first-->', 'F', '<!--@endif-->', '<mj-include path="inc" />'].join('\n'),
    inc: ['<!--@if second-->', 'S', '<!--@else-->', 'T', '<!--@endif-->'].join('\n'),
  }
  assert.deepEqual(scanBranches('tpl', io(files)), [
    { id: 'first', cases: 2 },
    { id: 'second', cases: 2 },
  ])
})

test('an include is scanned once, however often it is included', () => {
  const files = {
    tpl: ['<mj-include path="inc" />', '<mj-include path="inc" />'].join('\n'),
    inc: ['<!--@if x-->', 'X', '<!--@endif-->'].join('\n'),
  }
  assert.deepEqual(scanBranches('tpl', io(files)), [{ id: 'x', cases: 2 }])
})

test('markers must stand alone on their line', () => {
  // Anything else is text, and text is content -- silently treating it as a
  // marker would drop a block nobody meant to make conditional.
  const src = '<mj-text><!--@if x-->inline<!--@endif--></mj-text>'
  assert.deepEqual(scanBranches('a', io({ a: src })), [])
  assert.equal(selectBranches(src, {}), src)
})

test('unbalanced markers are an error, not a guess', () => {
  const cases = [
    [['<!--@if x-->', 'A'].join('\n'), /unclosed/],
    [['A', '<!--@endif-->'].join('\n'), /without an open/],
    [['<!--@else-->'].join('\n'), /without an open/],
    [['<!--@if-->', '<!--@endif-->'].join('\n'), /without a variable/],
    [['<!--@if x-->', '<!--@else-->', '<!--@elif y-->', '<!--@endif-->'].join('\n'), /after/],
  ]
  for (const [src, re] of cases) assert.throws(() => scanBranches('a', io({ a: src })), re)
})
