# Stored asset range import

`env.wc_read_asset_range(path_ptr, path_len, offset, dest_ptr, length)` takes
five Wasm `i32` words interpreted as unsigned and returns an `i32` byte count,
or `-1` on lookup, format, offset, or read failure. It is additive: existing
cartridges and `wc_load_asset` keep their existing behavior and ABI version.
New engines importing it require a host that registers this additional import.

The host checks both complete supplied Wasm ranges before reading the path,
looking up the asset, or writing any destination bytes. Invalid ranges trap as
other pointer-taking imports do. A valid path is copied before writing, so it
may alias the destination. Path handling is the existing first-NUL/511-byte,
manifest-root then bare then `assets/`, case-insensitive ZIP lookup policy.
This is archive lookup, never arbitrary filesystem path access.

Only ordinary unencrypted STORED outer ZIP entries up to `INT32_MAX` bytes
are supported. Deflated entries, directories and virtual `_filelist.txt` fail;
there is no whole-asset fallback. Valid ZIP64 metadata and data descriptors are
supported. The local header method and flags must agree with the central
metadata. Header, offset and payload bounds use checked subtraction.

`offset == size` returns zero; `offset > size` fails, even for zero length.
A request crossing EOF returns the remaining bytes. Metadata is checked before
a zero-byte success. A failed physical read may have written partial output.

Range reads do **not** check the outer entry CRC, even when reading the whole
entry. `wc_load_asset` still verifies it. A nested ZIP consumer must retain its
own entry CRC and resource hash verification. The new call reads a 30-byte local
header then exactly the requested available payload, directly into Wasm memory;
it does not allocate or extract the whole asset. This remains synchronous I/O:
a caller can request a large range and this import alone promises no frame-time
bound, background execution, or incremental decode/upload.

The native main host and its WASI worker adapter implement this contract.
Browser-host sources are external to these native/Defold checkouts; registering
an equivalent import there is a coordinated-release prerequisite. An existing
resident asset byte array can supply a checked bounded copy without changing
its old full-load implementation.

Run `python3 test/asset_range_test.py` for real miniz file/memory and worker
behavior tests. Its import-call test executes the actual callback/bounds source
against a small call-interface double. After building the native host, also run
`python3 test/import_bounds_test.py <wasmcart-run>` for real V8 cart traps; the
range path, destination-wrap and exact-end controls are included there.
