// wgpu_bridge.cjs - WebGPU carts in wasmcart-native (SPEC.md, "WebGPU").
//
// The native host already runs every cart inside an embedded Node, so WebGPU
// reuses the reference host's code rather than reimplementing it in C++:
// wasmcart's src/wgpu/host.js runs the emdawnwebgpu glue for the cart, and
// Dawn comes from native-dawn's dawn.node addon. All of it sits in a `wgpu/`
// directory next to the executable (CMake copies it there when built with
// WASMCART_WGPU); this file is the small interface cart_host.cpp calls.
//
// Asynchronous steps return a job ({ done, error, value }); C++ pumps Node's
// event loop until `done`.
'use strict';

const path = require('node:path');

module.exports = function createBridge(dir) {
  const dawn = require(path.join(dir, 'dawn.node'));
  const host = require(path.join(dir, 'host.js'));
  let gpu = null;
  const sessions = new Map();

  const job = fn => {
    const j = { done: false, error: null, value: null };
    Promise.resolve().then(fn).then(v => { j.value = v; }, e => { j.error = String(e?.message || e); })
      .finally(() => { j.done = true; });
    return j;
  };

  return {
    isWgpuImportName: host.isWgpuImportName,

    importsWgpu(module) {
      return host.importsWgpu(WebAssembly.Module.imports(module));
    },

    trap: host.gpuImportTrap,

    // Ask for an adapter and build the cart's session. value: the glue's env
    // functions. A failure's message is the reason the host gives the cart.
    prepare(id, module, width, height) {
      return job(async () => {
        if (process.env.WASMCART_NO_WGPU === '1') throw new Error('WebGPU is disabled (WASMCART_NO_WGPU=1)');
        gpu ??= dawn.create([]);
        // WASMCART_WGPU_POWER=low-power|high-performance picks the GPU on a
        // two-GPU machine (wasmcart docs/webgpu.md); the choice is logged so a
        // caller can assert it.
        const adapterOptions = host.wgpuAdapterOptions();
        const adapter = await gpu.requestAdapter(adapterOptions);
        if (!adapter) throw new Error('no WebGPU adapter is available (check the GPU driver)');
        const a = host.describeAdapter(adapter, adapterOptions);
        process.stderr.write(`wasmcart-run: WebGPU on ${a.device || a.description} (${a.vendor}, ${a.featureLevel}${a.powerPreference ? ', ' + a.powerPreference : ''})\n`);
        const session = await host.createWgpuSession({
          moduleImports: WebAssembly.Module.imports(module), gpu, adapter, width, height,
          globals: dawn.globals,
          log: msg => process.stderr.write(msg + '\n'),
        });
        sessions.set(id, { session, surface: null, size: [0, 0] });
        return session.env;
      });
    },

    attach(id, instance, memory) {
      return job(() => sessions.get(id).session.attach(instance, memory));
    },

    begin(id) {
      sessions.get(id)?.session.beginFrame();
    },

    // A window to present into. kind: xlib | wayland | win32 | metal-layer.
    // vsync false (--uncapped) presents without waiting for the display:
    // FIFO under Xwayland measured ~0.65 s per frame.
    attachWindow(id, kind, display, handle, vsync = true) {
      const s = sessions.get(id);
      s.surface?.destroy();
      s.surface = new dawn.NativeSurface(s.session.device, { kind, display, handle, xid: handle });
      s.size = [0, 0];
      s.presentModes = vsync ? ['fifo'] : ['mailbox', 'immediate', 'fifo'];
      return true;
    },

    // Draw the cart's frame into the window, letterboxed into (x, y, w, h)
    // of a winW x winH surface, and present it.
    present(id, x, y, w, h, winW, winH) {
      const s = sessions.get(id);
      if (!s?.surface) return false;
      if (s.size[0] !== winW || s.size[1] !== winH) {
        // The first present mode the surface supports (configure checks).
        let err;
        for (const presentMode of s.presentModes) {
          try {
            s.surface.configure({ width: winW, height: winH, format: gpu.getPreferredCanvasFormat(), usage: 0x10, presentMode });
            err = null;
            break;
          } catch (e) { err = e; }
        }
        if (err) throw err;
        s.size = [winW, winH];
      }
      const texture = s.surface.getCurrentTexture();
      const drew = s.session.drawTo({ getCurrentTexture: () => texture }, { x, y, w, h });
      s.surface.present();
      return drew;
    },

    // The cart's last frame as RGBA. value: { width, height, data }.
    read(id) {
      return job(() => sessions.get(id).session.readFrame());
    },

    // A job: settles once the device is gone and the cart's pending callbacks
    // were answered. The host pumps it before tearing Node down.
    destroy(id) {
      const s = sessions.get(id);
      sessions.delete(id);
      return job(async () => {
        if (!s) return;
        try { s.surface?.destroy(); } catch {}
        await s.session.destroy();
        // Dawn rejects the cart's still-pending maps a couple of event-loop
        // turns AFTER device.lost (measured: 2 turns, every run). A promise
        // left unsettled when Node is torn down aborts the process, so give
        // it a bounded margin; each turn is a fraction of a millisecond.
        for (let i = 0; i < 20; i++) await new Promise(resolve => setImmediate(resolve));
        // The last cart gone: drop the instance too, so the host can collect
        // every Dawn object now, while Dawn is alive, instead of leaving them
        // to node::FreeEnvironment, which a player runs from a static
        // destructor at exit, after Dawn's own state may already be gone.
        if (sessions.size === 0) gpu = null;
      });
    },
  };
};
