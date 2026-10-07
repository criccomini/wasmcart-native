// thread_worker_js.h -- JavaScript for WASI threads (wasi.thread-spawn).
//
// A spawned cart thread is a node worker_thread: its own V8 isolate on its own
// native thread, instantiating the SAME compiled module against the SAME shared
// WebAssembly.Memory and calling exports.wasi_thread_start(tid, start_arg).
// That is what the JS host does (CartHost.js + cartWorker.js), and it is the
// only route where V8 itself shares the compiled code and the memory between
// isolates: there is no public C++ API for either.
//
// Two scripts live here:
//
//   WC_THREADS_SPAWNER_JS  evaluated once on the main isolate. Returns a factory
//                          that builds the spawn/shutdown pair for one cart.
//   WC_THREAD_WORKER_JS    the worker body (run with { eval: true }). It also
//                          spawns NESTED threads itself, from inside the worker,
//                          so a thread that creates a thread never has to wait
//                          on the main thread -- which may be parked in a futex
//                          wait for exactly that thread.
//
// Nothing a worker does depends on the main thread's event loop: output goes
// through fs.writeSync, assets are read straight from the .wasc with fs, and
// errors are reported by the worker itself. The main thread only pumps its loop
// once per frame, and may be blocked in memory.atomic.wait between those.
//
// Tids come from one SharedArrayBuffer counter shared by every thread, so
// nested spawns stay unique without a round trip.

#ifndef WC_THREAD_WORKER_JS_H
#define WC_THREAD_WORKER_JS_H

static const char WC_THREAD_WORKER_JS[] = R"WCJS(
'use strict';
const { workerData, Worker } = require('worker_threads');
const fs = require('fs');
const zlib = require('zlib');
const crypto = require('crypto');

const { module: wasmModule, memory, tid, startArg, tidCounter, workerSrc, cfg } = workerData;

function log(text) {
  try { fs.writeSync(2, `wasmcart [cart t${tid}]: ${text}\n`); } catch {}
}

const u8 = () => new Uint8Array(memory.buffer);
const dv = () => new DataView(memory.buffer);

// A pointer outside the cart's memory traps the thread, as one handed to an
// import traps the main thread (cart_host.cpp, wc_cart_range_ok): a
// RangeError naming the import, thrown back into the cart before the import
// has read or written anything. DataView accessors and TypedArray.set would
// throw by themselves, but only when they got there, partway through, and
// without the import's name; slice and copyWithin would quietly clamp. So
// every range an import touches is checked here first. wasm hands i32s over
// signed, hence the >>> 0; a length the shim computed (a count times a size)
// is already a non-negative Number and is taken as it is.
function need(name, ptr, len) {
  ptr >>>= 0;
  if (len < 0) len >>>= 0;
  const size = memory.buffer.byteLength;
  if (ptr + len > size)
    throw new RangeError(`${name}: ${len} bytes at ${ptr} are outside the cart's memory (${size} bytes)`);
  return ptr;
}
const readStr = (name, ptr, len) => {
  ptr = need(name, ptr, len);
  return Buffer.from(u8().slice(ptr, ptr + (len >>> 0))).toString('utf8');
};

// ---- assets: read the .wasc ourselves, same lookup rules as asset_loader.c ----
//
// An asset goes straight into the cart's memory, as on the main thread
// (asset_loader.c): a stored entry is read into it a chunk at a time, a
// deflated one inflated into it a chunk of input at a time, and its CRC is
// checked on the way. A worker never holds the entry, compressed or not, in
// a buffer of its own; a 600 MiB asset costs the cart's 600 MiB and a few
// more, where a whole-entry read and inflate cost two or three times that
// and could end the game on its memory limit. The return values are the
// main thread's too: the asset's size, or -1 when it's missing, bigger than
// dest, too big for an int32, damaged, or not stored or deflated.
//
// The archive's index is one SharedArrayBuffer that every thread shares
// (build_asset_index in cart_host.cpp lays it out), not a copy per worker.
// A name is looked up in it as the main thread looks it up: the path's
// bytes up to a NUL and at most 511 of them (v8_wc_load_asset), under the
// manifest's asset root, then bare, then under "assets/" (locate_asset),
// each ASCII case-folded and binary-searched as miniz searches.
const ai = cfg.assetIndex;
const aiNums = ai ? new Float64Array(ai.buffer, 0, ai.count * 3) : null;
const aiRecs = ai ? new Uint32Array(ai.buffer, ai.recs, ai.count * 5) : null;
const aiNames = ai ? new Uint8Array(ai.buffer, ai.names, ai.namesLen) : null;
const fileList = ai && ai.listLen >= 0 ? new Uint8Array(ai.buffer, ai.list, ai.listLen) : null;
const AI_DIR = 1 << 16;
const PATH_MAX = 511;
const NONE = Buffer.alloc(0), ASSETS = Buffer.from('assets/'), FILELIST = Buffer.from('_filelist.txt');
const root = Buffer.from(cfg.assetsRoot || '');

function find(key) {
  let l = 0, h = ai.count - 1;
  while (l <= h) {
    const m = (l + h) >>> 1, at = aiRecs[m * 5], len = aiRecs[m * 5 + 1];
    let d = 0;
    for (let j = 0, k = Math.min(len, key.length); j < k && !d; j++) d = aiNames[at + j] - key[j];
    if (!d) d = len - key.length;
    if (!d) return m;
    if (d < 0) l = m + 1; else h = m - 1;
  }
  return -1;
}
function lookup(prefix, path) {
  let key = Buffer.concat([prefix, path]);
  if (key.length > PATH_MAX) key = key.subarray(0, PATH_MAX);
  for (let i = 0; i < key.length; i++) if (key[i] >= 65 && key[i] <= 90) key[i] += 32;
  const m = find(key);
  if (m < 0) return null;
  const flags = aiRecs[m * 5 + 4];
  return { ofs: aiNums[m * 3], csize: aiNums[m * 3 + 1], usize: aiNums[m * 3 + 2],
           crc: aiRecs[m * 5 + 2], method: aiRecs[m * 5 + 3], flags: flags & 0xffff,
           dir: (flags & AI_DIR) !== 0 };
}
function locate(path) {
  if (root.length) { const e = lookup(root, path); if (e) return e; }
  return lookup(NONE, path) || lookup(ASSETS, path);
}
// A cart's path as the main thread takes it: a copy, since the cart may
// change its memory meanwhile.
function cartPath(name, ptr, len) {
  ptr = need(name, ptr, len);
  let b = u8().subarray(ptr, ptr + Math.min(len >>> 0, PATH_MAX));
  const nul = b.indexOf(0);
  if (nul >= 0) b = b.subarray(0, nul);
  return Buffer.from(b);
}

let zipFd = null;
let zipSize = 0;
const INT32_MAX = 0x7fffffff;
const READ_CHUNK = 4 << 20;     // a stored entry's reads, straight into dest
const INFLATE_IN = 1 << 20;     // a deflated entry's compressed input, per read
// Encrypted, strongly encrypted, a patch: miniz refuses all three.
const UNSUPPORTED_FLAGS = 0x1 | 0x40 | 0x20;

let crc32 = zlib.crc32;
if (typeof crc32 !== 'function') {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c;
  }
  crc32 = (buf, crc = 0) => {
    let c = ~crc;
    for (let i = 0; i < buf.length; i++) c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return ~c >>> 0;
  };
}

// fs.readSync until len bytes are in buf at off, or the file ends.
function readFully(buf, off, len, pos) {
  while (len > 0) {
    const n = fs.readSync(zipFd, buf, off, len, pos);
    if (n <= 0) return false;
    off += n; len -= n; pos += n;
  }
  return true;
}

// Where an entry's data starts, from its local header (whose name and extra
// field lengths can differ from the central directory's).
function dataOffset(e) {
  if (zipFd === null) {
    zipFd = fs.openSync(cfg.wascPath, 'r');
    zipSize = fs.fstatSync(zipFd).size;
  }
  const hdr = Buffer.alloc(30);
  if (!readFully(hdr, 0, 30, e.ofs) || hdr.readUInt32LE(0) !== 0x04034b50) return -1;
  const ofs = e.ofs + 30 + hdr.readUInt16LE(26) + hdr.readUInt16LE(28);
  return ofs + e.csize > zipSize ? -1 : ofs;
}

// A stored entry: read into out in chunks, the CRC taken as each one lands.
function readStored(pos, out) {
  let crc = 0;
  for (let off = 0; off < out.length;) {
    const n = Math.min(READ_CHUNK, out.length - off);
    if (!readFully(out, off, n, pos + off)) return { why: 'the archive ends inside it' };
    crc = crc32(out.subarray(off, off + n), crc);
    off += n;
  }
  return { crc };
}

// A deflated entry: inflated into out with zlib's own handle, which takes
// the output buffer and its window per call, so the output lands in the
// cart's memory and nowhere else. The stream has to end exactly at the
// entry's size: once out is full, anything more it gives goes into a
// one-byte probe, and that is an error, as one that ends short is.
//
// One handle per thread, reset for each entry. A stream made per load
// and closed would leave its close for a tick, and a cart thread never
// lets its event loop run: each would stay, with its 16 KiB output
// buffer, until the thread ended. One that failed is closed and dropped.
let inflater = null;
function takeInflater() {
  if (inflater) { inflater.h.reset(); inflater.failed = null; return inflater; }
  const z = zlib.createInflateRaw();
  const h = z._handle, state = z._writeState;
  if (!h || typeof h.writeSync !== 'function' || typeof h.reset !== 'function' || !state) return null;
  const inf = { z, h, state, failed: null };
  h.onerror = (message) => { inf.failed = String(message || 'inflate failed'); };
  return (inflater = inf);
}
function inflateTo(pos, csize, out) {
  const inf = takeInflater();
  if (!inf) return null;
  const { h, state } = inf;
  const input = Buffer.allocUnsafe(Math.max(1, Math.min(csize, INFLATE_IN)));
  const probe = Buffer.alloc(1);
  let crc = 0, done = 0, ok = false;
  // One call: input (or none) against what's left of out, else the probe.
  const step = (flush, inBuf, inOfs, inLen) => {
    const full = done === out.length;
    const dst = full ? probe : out, dstOfs = full ? 0 : done, room = full ? 1 : out.length - done;
    h.writeSync(flush, inBuf, inOfs, inLen, dst, dstOfs, room);
    if (inf.failed) return -1;
    const made = room - state[0];
    if (made && full) { inf.failed = 'more data than its size'; return -1; }
    if (made) { crc = crc32(out.subarray(done, done + made), crc); done += made; }
    return inBuf ? inLen - state[1] : made;   // consumed (or, with no input, made)
  };
  try {
    for (let left = csize; left > 0;) {
      const n = Math.min(left, input.length);
      if (!readFully(input, 0, n, pos)) return { why: 'the archive ends inside it' };
      pos += n; left -= n;
      for (let at = 0; at < n;) {
        const before = done;
        const used = step(zlib.constants.Z_NO_FLUSH, input, at, n - at);
        if (used < 0) return { why: inf.failed };
        at += used;
        if (!used && done === before) break;   // the stream has ended
      }
    }
    // All of the input is in. Z_FINISH says so: zlib reports a stream that
    // hasn't ended as "unexpected end of file".
    if (step(zlib.constants.Z_FINISH, null, 0, 0) < 0) return { why: inf.failed };
    ok = true;
    if (done !== out.length) return { why: `inflated to ${done} bytes, not ${out.length}` };
    return { crc };
  } finally {
    if (!ok) {
      try { h.close(); } catch {}
      inflater = null;
    }
  }
}

// Fallback for a node without zlib's handle: the whole entry at once.
function inflateWhole(pos, csize, out) {
  const comp = Buffer.allocUnsafe(csize);
  if (!readFully(comp, 0, csize, pos)) return { why: 'the archive ends inside it' };
  const data = zlib.inflateRawSync(comp);
  if (data.length !== out.length) return { why: `inflated to ${data.length} bytes, not ${out.length}` };
  out.set(data);
  return { crc: crc32(out) };
}

let saidWhole = false;
function extract(e, out) {
  if (e.dir || !e.csize) return null;   // as miniz: nothing to read, nothing written
  if (e.flags & UNSUPPORTED_FLAGS) return 'encrypted';
  if (e.method !== 0 && e.method !== 8) return `compression method ${e.method}`;
  const pos = dataOffset(e);
  if (pos < 0) return 'bad local header';
  let r = e.method === 0 ? readStored(pos, out) : inflateTo(pos, e.csize, out);
  if (r === null) {
    if (!saidWhole) { log("this node's zlib has no handle to inflate into; inflating whole entries"); saidWhole = true; }
    r = inflateWhole(pos, e.csize, out);
  }
  if (r.why) return r.why;
  if (r.crc !== e.crc) return 'CRC mismatch';
  return null;
}

function assetSize(pathPtr, pathLen) {
  const path = cartPath('wc_asset_size', pathPtr, pathLen);
  if (!ai) return -1;
  if (path.equals(FILELIST)) return fileList === null ? -1 : fileList.length;
  const e = locate(path);
  return e && e.usize <= INT32_MAX ? e.usize : -1;
}
function loadAsset(pathPtr, pathLen, destPtr, maxSize) {
  const path = cartPath('wc_load_asset', pathPtr, pathLen);
  // The whole of dest, as on the main thread: maxSize is the cart's word for
  // how big its buffer is.
  destPtr = need('wc_load_asset', destPtr, maxSize);
  maxSize >>>= 0;
  if (!ai) return -1;
  if (path.equals(FILELIST)) {
    if (fileList === null || fileList.length > maxSize) return -1;
    u8().set(fileList, destPtr);
    return fileList.length;
  }
  const e = locate(path);
  if (!e || e.usize > INT32_MAX) return -1;
  if (e.usize > maxSize) {
    log(`asset ${path}: ${e.usize} bytes, more than dest's ${maxSize}`);
    return -1;
  }
  let why;
  try {
    why = extract(e, Buffer.from(memory.buffer, destPtr, e.usize));
  } catch (err) {
    why = err && err.message ? err.message : String(err);
  }
  if (why) { log(`asset ${path}: ${why}`); return -1; }
  return e.usize;
}

)WCJS"
// Two pieces: MSVC takes no single string literal over 16 KB.
R"WCJS(
// ---- WASI (preview1) as threaded wasi-libc uses it ----
const EBADF = 8, EINVAL = 28, ENOTSUP = 58;
const sleepCell = new Int32Array(new SharedArrayBuffer(4));
// The cart's clocks, read as the main thread reads them (cart_host.cpp,
// wasi_now_ns). MONOTONIC and the CPU-time clocks count from the origin the
// main thread took when it built the cart's imports, on uv_hrtime, which is
// what process.hrtime.bigint() reads, so a deadline on one of them means the
// same instant on every thread. REALTIME (id 0) is the wall clock, read live
// as the main thread's timespec_get is: performance.timeOrigin is the wall
// clock when this worker started, and timeOrigin + now() alone would never
// see the wall clock stepped (a Pi with no RTC that syncs over the network
// after a cart started). Date.now() sees steps but only to the millisecond,
// so it moves the offset only when the two disagree by more than that.
let wallOffsetMs = 0;
function realtimeNs() {
  const t = performance.timeOrigin + performance.now();
  const drift = Date.now() - (t + wallOffsetMs);
  if (drift > 1 || drift < -1) wallOffsetMs = Date.now() - t;
  return BigInt(Math.round((t + wallOffsetMs) * 1e6));
}
const clockNs = (id) => id === 0 ? realtimeNs() : process.hrtime.bigint() - cfg.clockOriginNs;
class ProcExit extends Error {}

function fdStat(fd, ptr) {
  if ((fd >>> 0) > 2) return EBADF;
  ptr = need('fd_fdstat_get', ptr, 24);
  const v = dv();
  for (let i = 0; i < 24; i++) v.setUint8(ptr + i, 0);
  v.setUint8(ptr, 2);                                   // filetype: character device
  v.setUint16(ptr + 2, fd === 0 ? 0 : 1, true);         // fdflags: append for out/err
  v.setBigUint64(ptr + 8, 0xffffffffffffffffn, true);   // rights base
  v.setBigUint64(ptr + 16, 0xffffffffffffffffn, true);  // rights inheriting
  return 0;
}
// environ_sizes_get, args_sizes_get: both counts are zero, and are written.
const sizesZero = (name) => (countPtr, sizePtr) => {
  countPtr = need(name, countPtr, 4);
  sizePtr = need(name, sizePtr, 4);
  const v = dv();
  v.setUint32(countPtr, 0, true);
  v.setUint32(sizePtr, 0, true);
  return 0;
};

const wasi = {
  fd_write(fd, iovs, iovsLen, nwrittenPtr) {
    // Everything checked before anything is written, as on the main thread:
    // the iovec list, nwritten, then each buffer as its iovec is read (once:
    // what's checked is what's copied).
    iovsLen >>>= 0;
    iovs = need('fd_write', iovs, iovsLen * 8);
    nwrittenPtr = need('fd_write', nwrittenPtr, 4);
    const v = dv();
    let total = 0;
    const chunks = [];
    for (let i = 0; i < iovsLen; i++) {
      const p = v.getUint32(iovs + i * 8, true);
      const l = v.getUint32(iovs + i * 8 + 4, true);
      need('fd_write', p, l);
      if (l) chunks.push(Buffer.from(u8().slice(p, p + l)));
      total += l;
    }
    if ((fd === 1 || fd === 2) && chunks.length) {
      try { fs.writeSync(fd, Buffer.concat(chunks)); } catch {}
    }
    v.setUint32(nwrittenPtr, total, true);
    return 0;
  },
  fd_read(fd, iovs, iovsLen, nreadPtr) {
    nreadPtr = need('fd_read', nreadPtr, 4);
    dv().setUint32(nreadPtr, 0, true);
    return 0;
  },
  fd_close() { return 0; },
  fd_seek() { return 0; },
  fd_fdstat_get(fd, ptr) { return fdStat(fd, ptr); },
  fd_fdstat_set_flags(fd) { return (fd >>> 0) > 2 ? EBADF : 0; },
  fd_filestat_get(fd, ptr) {
    if ((fd >>> 0) > 2) return EBADF;
    ptr = need('fd_filestat_get', ptr, 64);
    const v = dv();
    for (let i = 0; i < 64; i++) v.setUint8(ptr + i, 0);
    v.setUint8(ptr + 16, 2);                            // filetype: character device
    return 0;
  },
  fd_prestat_get() { return EBADF; },                   // no preopens: no filesystem
  fd_prestat_dir_name() { return EBADF; },
  path_open() { return EBADF; },
  path_filestat_get() { return EBADF; },
  environ_sizes_get: sizesZero('environ_sizes_get'),
  environ_get() { return 0; },
  args_sizes_get: sizesZero('args_sizes_get'),
  args_get() { return 0; },
  clock_time_get(id, precision, resultPtr) {
    resultPtr = need('clock_time_get', resultPtr, 8);
    dv().setBigUint64(resultPtr, clockNs(id), true);
    return 0;
  },
  clock_res_get(id, resultPtr) {
    resultPtr = need('clock_res_get', resultPtr, 8);
    dv().setBigUint64(resultPtr, 1000n, true);
    return 0;
  },
  random_get(ptr, len) {
    len >>>= 0;
    ptr = need('random_get', ptr, len);
    const tmp = Buffer.alloc(len);
    crypto.randomFillSync(tmp);
    u8().set(tmp, ptr);
    return 0;
  },
  sched_yield() { return 0; },
  proc_exit(code) { throw new ProcExit(`proc_exit(${code})`); },
  // Clock subscriptions sleep (that is nanosleep); fd subscriptions report ready.
  poll_oneoff(inPtr, outPtr, nsubs, neventsPtr) {
    nsubs >>>= 0;
    if (nsubs === 0) return EINVAL;
    inPtr = need('poll_oneoff', inPtr, nsubs * 48);
    outPtr = need('poll_oneoff', outPtr, nsubs * 32);
    neventsPtr = need('poll_oneoff', neventsPtr, 4);
    const v = dv();
    let wake = null;
    for (let i = 0; i < nsubs; i++) {
      const s = inPtr + i * 48;
      if (v.getUint8(s + 8) !== 0) continue;
      let t = v.getBigUint64(s + 24, true);
      if (v.getUint16(s + 40, true) & 1) {                 // abstime, on the clock it names
        const now = clockNs(v.getUint32(s + 16, true));
        t = t > now ? t - now : 0n;
      }
      if (wake === null || t < wake) wake = t;
    }
    if (wake !== null && wake > 0n) Atomics.wait(sleepCell, 0, 0, Number(wake) / 1e6);
    let n = 0;
    for (let i = 0; i < nsubs; i++) {
      const s = inPtr + i * 48, e = outPtr + n * 32;
      const type = v.getUint8(s + 8);
      for (let k = 0; k < 32; k++) v.setUint8(e + k, 0);
      v.setBigUint64(e, v.getBigUint64(s, true), true);   // userdata
      v.setUint8(e + 10, type);
      if (type !== 0) v.setUint16(e + 8, ENOTSUP, true);
      n++;
    }
    v.setUint32(neventsPtr, n, true);
    return 0;
  },
};

// ---- nested spawn: from right here, never through the main thread ----
function spawn(arg) {
  const newTid = Atomics.add(tidCounter, 0, 1);
  if (newTid <= 0 || newTid > 0x1fffffff) return -1;
  try {
    new Worker(workerSrc, { eval: true, workerData: { ...workerData, tid: newTid, startArg: arg } });
    return newTid;
  } catch (err) {
    log(`thread-spawn failed: ${err.message}`);
    return -1;
  }
}

// ---- import object: the same module/name set the main thread gets ----
const notOnWorker = (name) => () => {
  throw new Error(`${name}() is main-thread only; called from cart thread ${tid}`);
};
const imports = {};
for (const imp of WebAssembly.Module.imports(wasmModule)) {
  const ns = (imports[imp.module] ||= {});
  let val;
  if (imp.kind === 'memory') val = memory;
  else if (imp.kind === 'global') val = new WebAssembly.Global({ value: 'i32', mutable: true }, 0);
  else if (imp.kind === 'table') val = new WebAssembly.Table({ element: 'anyfunc', initial: 0 });
  else if (imp.module === 'wasi' && imp.name === 'thread-spawn') val = spawn;
  else if (imp.module === 'wasi_snapshot_preview1' || imp.module === 'wasi_unstable')
    val = wasi[imp.name] || (() => 0);
  else if (imp.module === 'gl') val = notOnWorker(imp.name);
  else if (imp.module === 'env') {
    const n = imp.name;
    if (n === 'wc_log') val = (p, l) => log(readStr('wc_log', p, l));
    else if (n === 'wc_asset_size') val = assetSize;
    else if (n === 'wc_load_asset') val = loadAsset;
    else if (n === 'wc_debug_mark' || n === 'wc_frame_yield') val = () => {};
    else if (n === 'emscripten_memcpy_js') val = (d, s, c) => {
      s = need('emscripten_memcpy_js', s, c); d = need('emscripten_memcpy_js', d, c);
      u8().copyWithin(d, s, s + (c >>> 0));
    };
    else if (n.startsWith('wc_') || /^gl[A-Z]/.test(n) || n.startsWith('emscripten_gl')) val = notOnWorker(n);
    else val = () => 0;
  } else val = () => 0;
  ns[imp.name] = val;
}

// A thread that traps or calls proc_exit ends the whole cart, as either does
// on the main thread: WASI threads have no way to end just the one thread,
// and the cart's other threads may be waiting on it or be halfway through
// its save. The main thread can't be reached from here, so the shared word
// says which thread ended the cart and how; the host reads it before each
// frame and before each save (cart_host.cpp, note_thread_end).
const ended = new Int32Array(cfg.endedBuffer);
try {
  const instance = new WebAssembly.Instance(wasmModule, imports);
  instance.exports.wasi_thread_start(tid, startArg);
} catch (err) {
  if (err instanceof ProcExit) log(`thread called ${err.message}`);
  else log(`thread trapped: ${err && err.stack ? err.stack : err}`);
  Atomics.compareExchange(ended, 0, 0, (tid << 2) | (err instanceof ProcExit ? 2 : 1));
}
if (zipFd !== null) { try { fs.closeSync(zipFd); } catch {} }
)WCJS";

// Evaluated on the main isolate. (module, memory, cfg, workerSrc) -> { spawn, shutdown, count }
static const char WC_THREADS_SPAWNER_JS[] = R"WCJS(
(function (module, memory, cfg, workerSrc) {
  'use strict';
  const { Worker } = __wc_require('worker_threads');
  const fs = __wc_require('fs');
  const tidCounter = new Int32Array(new SharedArrayBuffer(4));
  tidCounter[0] = 1;                   // tids start at 1; 0 is never a spawned thread
  const ended = new Int32Array(cfg.endedBuffer);   // see the worker's end
  const workers = new Set();
  function spawn(startArg) {
    const tid = Atomics.add(tidCounter, 0, 1);
    if (tid <= 0 || tid > 0x1fffffff) return -1;
    try {
      const w = new Worker(workerSrc, {
        eval: true,
        workerData: { module, memory, tid, startArg, tidCounter, workerSrc, cfg },
      });
      workers.add(w);
      w.on('error', (err) => {
        try { fs.writeSync(2, `wasmcart: cart thread ${tid} failed: ${err && err.message}\n`); } catch {}
        Atomics.compareExchange(ended, 0, 0, (tid << 2) | 1);   // as a trap would
      });
      w.on('exit', () => workers.delete(w));
      return tid;
    } catch (err) {
      try { fs.writeSync(2, `wasmcart: thread-spawn failed: ${err && err.message}\n`); } catch {}
      return -1;
    }
  }
  function shutdown() {
    for (const w of workers) { try { w.terminate(); } catch {} }
  }
  return { spawn, shutdown, count: () => workers.size };
})
)WCJS";

#endif // WC_THREAD_WORKER_JS_H
