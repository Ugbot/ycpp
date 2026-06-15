// test.js — drive ycpp_cli (or ybolt_cli) against the real Yjs JS library.
//
// Each test:
//   1. Creates a Y.Doc in Node, makes edits.
//   2. Emits updateV1 bytes (Y.encodeStateAsUpdate).
//   3. Pipes bytes to the CLI's stdin via child_process.spawnSync.
//   4. Reads CLI stdout — either re-emitted updateV1 (apply-and-emit) or
//      a queried map value / dumped text.
//   5. Asserts the result matches expectation.
//
// Usage:
//   node test.js [path/to/cli/binary]
//
// Defaults to the bundled extern/ycpp/build/msvc/Release/ycpp_cli.exe on
// Windows or build/release/ycpp_cli on POSIX.

import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import process from 'node:process';

import * as Y from 'yjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..');

function pickCliPath() {
    if (process.argv[2]) return resolve(process.argv[2]);
    const candidates = [
        join(repoRoot, 'build', 'msvc',  'tools', 'yjs_interop', 'Release', 'ycpp_cli.exe'),
        join(repoRoot, 'build', 'msvc',  'tools', 'yjs_interop', 'Release', 'ycpp_cli'),
        join(repoRoot, 'build', 'release', 'tools', 'yjs_interop', 'ycpp_cli'),
        join(repoRoot, 'build', 'release', 'tools', 'yjs_interop', 'ycpp_cli.exe'),
    ];
    for (const c of candidates) if (existsSync(c)) return c;
    throw new Error(`could not find ycpp_cli — pass path as first arg (looked in: ${candidates.join(', ')})`);
}

const CLI = pickCliPath();
console.log(`using cli: ${CLI}`);

function runCli(args, stdin) {
    const r = spawnSync(CLI, args, { input: stdin, stdio: ['pipe', 'pipe', 'pipe'] });
    return { stdout: r.stdout, stderr: r.stderr.toString('utf8'), code: r.status };
}

function bytesEq(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
    return true;
}

let pass = 0, fail = 0;
const failures = [];
function check(name, cond, detail = '') {
    if (cond) {
        ++pass;
        console.log(`  ok    ${name}`);
    } else {
        ++fail;
        failures.push(`${name}${detail ? '  (' + detail + ')' : ''}`);
        console.log(`  FAIL  ${name}${detail ? '  (' + detail + ')' : ''}`);
    }
}

// ----- Test 1: Y.Map with string values --------------------------------------
{
    console.log('\n[1] Y.Map string values');
    const ydoc = new Y.Doc();
    ydoc.clientID = 0x42;
    const ymap = ydoc.getMap('doc');
    ymap.set('title',  'Hello, world');
    ymap.set('author', 'alice');
    const updateV1 = Y.encodeStateAsUpdate(ydoc);

    // 1a: ycpp can apply the update at all (no kPendingReference / kUnsupportedFormat).
    const a = runCli(['apply-and-emit'], updateV1);
    check('apply-and-emit exits 0',  a.code === 0,
          `exit=${a.code} stderr=${a.stderr.trim()}`);

    // 1b: ycpp's re-emitted update applied back to a fresh JS Y.Doc reproduces
    //     the same Y.Map state.
    if (a.code === 0 && a.stdout.length > 0) {
        const ydoc2 = new Y.Doc();
        try {
            Y.applyUpdate(ydoc2, a.stdout);
            const m = ydoc2.getMap('doc');
            check('round-trip: title key',
                  m.get('title')  === 'Hello, world',
                  `got=${JSON.stringify(m.get('title'))}`);
            check('round-trip: author key',
                  m.get('author') === 'alice',
                  `got=${JSON.stringify(m.get('author'))}`);
        } catch (e) {
            check('round-trip applies cleanly to fresh Y.Doc', false, e.message);
        }
    }
}

// ----- Test 2: Y.Text small append -------------------------------------------
{
    console.log('\n[2] Y.Text short append');
    const ydoc = new Y.Doc();
    ydoc.clientID = 0x99;
    const ytext = ydoc.getText('body');
    ytext.insert(0, 'hi');
    const updateV1 = Y.encodeStateAsUpdate(ydoc);

    const a = runCli(['dump-text', 'body'], updateV1);
    check('dump-text exits 0', a.code === 0,
          `exit=${a.code} stderr=${a.stderr.trim()}`);
    const got = a.stdout.toString('utf8');
    check('dump-text byte-for-byte equals "hi"', got === 'hi',
          `got=${JSON.stringify(got)}`);
}

// ----- Test 3a: Y.Text length-N items (multi-char insert + append) ----------
{
    console.log('\n[3a] Y.Text length-N items (multi-char insert + sequential append)');
    const ydoc = new Y.Doc();
    ydoc.clientID = 0x77;
    const ytext = ydoc.getText('body');
    ytext.insert(0, 'Hello, ');     // length=7 in UTF-16
    ytext.insert(7, 'world!');      // length=6, anchored after 'Hello, '
    const updateV1 = Y.encodeStateAsUpdate(ydoc);

    // The second insert's origin_left should be (0x77, 6) — END of the
    // first length-7 item. ycpp must compute length=7 from "Hello, " to
    // accept the second item's origin without kPendingReference.
    const r = runCli(['dump-text', 'body'], updateV1);
    check('dump-text exits 0 on multi-append',
          r.code === 0,
          `exit=${r.code} stderr=${r.stderr.trim()}`);
    if (r.code === 0) {
        const got = r.stdout.toString('utf8');
        check('dump-text matches "Hello, world!"',
              got === 'Hello, world!',
              `got=${JSON.stringify(got)}`);
    }

    // Verify round-trip is byte-correct via apply-and-emit.
    const re = runCli(['apply-and-emit'], updateV1);
    check('apply-and-emit length-N exits 0',
          re.code === 0,
          `exit=${re.code} stderr=${re.stderr.trim()}`);
    if (re.code === 0) {
        const fresh = new Y.Doc();
        try {
            Y.applyUpdate(fresh, re.stdout);
            check('Yjs JS sees the same text after ycpp round-trip',
                  fresh.getText('body').toString() === 'Hello, world!',
                  `got=${JSON.stringify(fresh.getText('body').toString())}`);
        } catch (e) {
            check('apply ycpp output to fresh Y.Doc', false, e.message);
        }
    }
}

// ----- Test 3b: Y.Text concurrent mid-string edits (split needed) -----------
{
    console.log('\n[3b] Y.Text concurrent mid-string edit (interior origin → split)');
    const alice = new Y.Doc(); alice.clientID = 1;
    const bob   = new Y.Doc(); bob  .clientID = 2;

    alice.getText('body').insert(0, 'Hello world');   // alice owns it as length=11
    const a_to_b = Y.encodeStateAsUpdate(alice);
    Y.applyUpdate(bob, a_to_b);

    // Bob inserts ", reader" between 'Hello' and ' world' — at index 5.
    bob.getText('body').insert(5, ', reader');
    const expected = bob.getText('body').toString();  // Yjs canonical answer

    // What ycpp sees: alice's "Hello world" (length=11) + bob's ", reader"
    // with origin_left = (1, 4) which is INTERIOR to alice's item.
    const merged = Y.encodeStateAsUpdate(bob);
    const r = runCli(['dump-text', 'body'], merged);
    check('dump-text on mid-string edit exits 0',
          r.code === 0,
          `exit=${r.code} stderr=${r.stderr.trim()}`);
    if (r.code === 0) {
        const got = r.stdout.toString('utf8');
        check(`dump-text matches Yjs canonical "${expected}"`,
              got === expected,
              `got=${JSON.stringify(got)} expected=${JSON.stringify(expected)}`);
    }
}

// ----- Test 3: Y.Text concurrent edits converge ------------------------------
{
    console.log('\n[3] Y.Text concurrent appends (Yjs JS → ycpp)');
    const alice = new Y.Doc(); alice.clientID = 1;
    const bob   = new Y.Doc(); bob  .clientID = 2;
    alice.getText('body').insert(0, 'A');
    bob  .getText('body').insert(0, 'B');
    // Merge in Yjs first to get the canonical converged update.
    const a_to_b = Y.encodeStateAsUpdate(alice);
    const b_to_a = Y.encodeStateAsUpdate(bob);
    Y.applyUpdate(alice, b_to_a);
    Y.applyUpdate(bob,   a_to_b);
    const ref = alice.getText('body').toString();
    const merged = Y.encodeStateAsUpdate(alice);

    const r = runCli(['dump-text', 'body'], merged);
    check('dump-text exits 0 on merged update', r.code === 0,
          `exit=${r.code} stderr=${r.stderr.trim()}`);
    if (r.code === 0) {
        const got = r.stdout.toString('utf8');
        check(`dump-text matches Yjs canonical "${ref}"`,
              got === ref,
              `got=${JSON.stringify(got)} expected=${JSON.stringify(ref)}`);
    }
}

console.log('\n----------------------------------------');
console.log(`results: ${pass} passed, ${fail} failed`);
if (fail > 0) {
    console.log('failures:');
    for (const f of failures) console.log('  - ' + f);
    process.exit(1);
}
