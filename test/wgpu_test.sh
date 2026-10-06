#!/bin/sh
# WebGPU carts in wasmcart-run (SPEC.md, "WebGPU").
#
# Needs a build configured with WASMCART_WGPU_JS_DIR and NATIVE_DAWN_DIR (so
# build/wgpu/ exists) and a WebGPU adapter. Runs headless on SDL's offscreen
# driver: the cart renders into its host-owned texture and --shot reads it.
#
# Fixtures (test/wgpu/*.wasc) are copies of wasmcart's test/fixtures, whose C
# sources describe them:
#   wgpucart     host device, "#canvas" surface, a compute result read back with
#                mapAsync (the background turns (42,0,255)), an error scope,
#                32 MB of memory growth
#   dualgpu      imports gl AND WebGPU; WebGPU draws green
#   dualgpu_bad  calls glClear whatever the host selected; must trap
#   gpuapi2/3    gpu_api values the cart does not back; refused
#   wgpufake     imports a WebGPU function the glue lacks; refused
#   wasicart     wgpucart built with wasi-sdk (wasip1-threads, two workers)
#
# Every run must also EXIT cleanly: teardown with GPU work in flight used to
# abort or segfault in node::FreeEnvironment.
#
# Run: sh test/wgpu_test.sh [path/to/wasmcart-run]
HERE="$(cd "$(dirname "$0")" && pwd)"
RUN="${1:-$HERE/../build/wasmcart-run}"
FX="$HERE/wgpu"
OUT="${TMPDIR:-/tmp}/wasmcart-wgpu-test.$$"
mkdir -p "$OUT"
fail=0
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy

if [ ! -d "$(dirname "$RUN")/wgpu" ]; then
  echo "*** FAIL $(dirname "$RUN")/wgpu is missing: configure with WASMCART_WGPU_JS_DIR and NATIVE_DAWN_DIR"
  exit 1
fi

# run CART FRAME NAME [env...]: wasmcart-run with --shot FRAME, stopped by SIGINT
# once the shot exists (or the player exits on its own). Leaves
# $OUT/NAME.log, $OUT/NAME.ppm (if any) and $OUT/NAME.status.
run() {
  cart=$1; frame=$2; name=$3; shift 3
  rm -f "$OUT/$name.ppm"
  env "$@" "$RUN" "$FX/$cart.wasc" --fixed-step 16 --shot "$frame" "$OUT/$name.ppm" > "$OUT/$name.log" 2>&1 &
  pid=$!
  i=0
  while [ $i -lt 120 ] && kill -0 $pid 2>/dev/null && [ ! -f "$OUT/$name.ppm" ]; do sleep 0.25; i=$((i + 1)); done
  sleep 0.3
  kill -INT $pid 2>/dev/null
  wait $pid
  echo $? > "$OUT/$name.status"
}

# pixel NAME X Y -> "r,g,b" from a binary PPM (P6, maxval 255)
pixel() {
  node -e '
    const b = require("fs").readFileSync(process.argv[1]); let o = 0, t = [];
    while (t.length < 4) { let e = o; while (b[e] > 32) e++; t.push(b.slice(o, e).toString()); o = e + 1; }
    const w = +t[1], x = +process.argv[2], y = +process.argv[3];
    console.log([...b.slice(o + (y * w + x) * 3, o + (y * w + x) * 3 + 3)].join(","));
  ' "$OUT/$1.ppm" "$2" "$3"
}

check() {  # check WHAT GOT WANT
  if [ "$2" = "$3" ]; then echo "  ok    $1"; else echo "*** FAIL $1: got '$2', want '$3'"; fail=1; fi
}
has() {  # has WHAT NAME PATTERN
  if grep -q "$3" "$OUT/$2.log"; then echo "  ok    $1"; else echo "*** FAIL $1: no '$3' in $OUT/$2.log"; fail=1; fi
}

run wgpucart 8 wgpucart
check "webgpu cart: triangle"                           "$(pixel wgpucart 128 110 2>/dev/null)" "255,128,64"
check "webgpu cart: compute result reached the cart"   "$(pixel wgpucart 2 2 2>/dev/null)" "42,0,255"
check "webgpu cart: clean exit"                        "$(cat "$OUT/wgpucart.status")" "0"

run dualgpu 3 dual
check "dual cart: WebGPU selected, green"              "$(pixel dual 64 48 2>/dev/null)" "0,255,0"
check "dual cart: clean exit"                          "$(cat "$OUT/dual.status")" "0"

run wasicart 30 wasi
check "wasi-sdk threaded cart: triangle"                 "$(pixel wasi 128 110 2>/dev/null)" "255,128,64"
check "wasi-sdk threaded cart: compute result"           "$(pixel wasi 2 2 2>/dev/null)" "42,0,255"
has   "wasi-sdk threaded cart: its workers run as threads" wasi "threads run as node workers"
check "wasi-sdk threaded cart: clean exit"               "$(cat "$OUT/wasi.status")" "0"

run dualgpu_bad 3 bad
has   "dual cart calling GL on WebGPU traps, naming the call" bad "called glClear, but this host selected WebGPU"

run wgpucart 3 nowgpu WASMCART_NO_WGPU=1
has   "WebGPU-only cart refused without WebGPU, with the reason" nowgpu "this cart is a WebGPU cart, but this host cannot provide WebGPU: WebGPU is disabled"

run dualgpu 3 dualgl WASMCART_NO_WGPU=1
has   "dual cart falls back to GL without WebGPU" dualgl "the cart also imports GL, so it runs on GL"

run gpuapi2 2 api2
has   "gpu_api 2 without WebGPU imports refused" api2 "declares gpu_api 2 (WebGPU) but imports no WebGPU functions"
run gpuapi3 2 api3
has   "gpu_api 3 refused" api3 "declares gpu_api 3, which this host does not support"
run wgpufake 2 fake
has   "unknown WebGPU function refused by name" fake "does not provide: wgpuDeviceDoesNotExistYet"

# Teardown under load, repeated: one exit in four used to crash.
crashes=0
k=0
while [ $k -lt 8 ]; do
  run wgpucart 4 loop
  [ "$(cat "$OUT/loop.status")" = "0" ] || crashes=$((crashes + 1))
  k=$((k + 1))
done
check "8 more runs exit cleanly" "$crashes" "0"

rm -rf "$OUT"
[ $fail -eq 0 ] && echo "all wgpu checks passed"
exit $fail
