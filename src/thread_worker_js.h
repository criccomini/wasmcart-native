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
let zipFd = null;
function zipEntry(name) {
  const idx = cfg.zipIndex;
  return idx && Object.hasOwn(idx, name) ? idx[name] : null;
}
function locate(path) {
  if (cfg.assetsRoot) { const e = zipEntry(cfg.assetsRoot + path); if (e) return e; }
  return zipEntry(path) || zipEntry('assets/' + path);
}
function readEntry(e) {
  if (zipFd === null) zipFd = fs.openSync(cfg.wascPath, 'r');
  const hdr = Buffer.alloc(30);
  fs.readSync(zipFd, hdr, 0, 30, e.ofs);
  if (hdr.readUInt32LE(0) !== 0x04034b50) return null;
  const dataOfs = e.ofs + 30 + hdr.readUInt16LE(26) + hdr.readUInt16LE(28);
  const comp = Buffer.alloc(e.csize);
  fs.readSync(zipFd, comp, 0, e.csize, dataOfs);
  if (e.method === 0) return comp;
  if (e.method === 8) return zlib.inflateRawSync(comp);
  return null;
}
function assetSize(pathPtr, pathLen) {
  const path = readStr('wc_asset_size', pathPtr, pathLen);
  if (path === '_filelist.txt') return cfg.fileList === null ? -1 : Buffer.byteLength(cfg.fileList);
  const e = locate(path);
  return e ? e.usize : -1;
}
function loadAsset(pathPtr, pathLen, destPtr, maxSize) {
  const path = readStr('wc_load_asset', pathPtr, pathLen);
  // The whole of dest, as on the main thread: maxSize is the cart's word for
  // how big its buffer is.
  destPtr = need('wc_load_asset', destPtr, maxSize);
  maxSize >>>= 0;
  let data;
  if (path === '_filelist.txt') {
    if (cfg.fileList === null) return -1;
    data = Buffer.from(cfg.fileList);
    if (data.length > maxSize) return -1;
  } else {
    const e = locate(path);
    if (!e) return -1;
    try { data = readEntry(e); } catch (err) { log(`asset ${path}: ${err.message}`); return -1; }
    if (!data) return -1;
    if (data.length > maxSize) data = data.subarray(0, maxSize);
  }
  if (destPtr + data.length > memory.buffer.byteLength) return -1;
  u8().set(data, destPtr);
  return data.length;
}

// ---- WASI (preview1) as threaded wasi-libc uses it ----
const EBADF = 8, EINVAL = 28, ENOTSUP = 58;
const sleepCell = new Int32Array(new SharedArrayBuffer(4));
// The cart's clocks, read as the main thread reads them (cart_host.cpp,
// wasi_now_ns): REALTIME (id 0) is the wall clock, the rest count from the
// origin the main thread took when it built the cart's imports, on uv_hrtime,
// which is what process.hrtime.bigint() reads. A deadline computed on one
// thread then means the same instant on another.
const clockNs = (id) => id === 0
  ? BigInt(Math.round((performance.timeOrigin + performance.now()) * 1e6))
  : process.hrtime.bigint() - cfg.clockOriginNs;
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

try {
  const instance = new WebAssembly.Instance(wasmModule, imports);
  instance.exports.wasi_thread_start(tid, startArg);
} catch (err) {
  if (err instanceof ProcExit) log(`thread called ${err.message}; thread ended`);
  else log(`thread trapped: ${err && err.stack ? err.stack : err}`);
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
