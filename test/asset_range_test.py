#!/usr/bin/env python3
"""Compile only the range loader/test seam, no player or graphics context.

Tests real file/memory miniz reads, raw-vs-full-load CRC behavior, the exact
import callback against a small V8 call-interface double, and worker JS parity.
The existing import_bounds_test.py separately exercises real V8 carts once a
new native host is built. This test does not claim that full-host integration.
"""
import ctypes as C
import io
import json
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = r'''
#include "cart_host.h"
#include "miniz.h"
#include <stdlib.h>
#include <stdio.h>
FILE* _wc_log_file; long _wc_log_bytes;
void* test_open(const char* path) {
    wc_host_t* h = calloc(1, sizeof(*h));
    h->memory_size = 65536; h->memory = calloc(1, h->memory_size);
    if (wc_archive_open(h, path)) { free(h->memory); free(h); return NULL; }
    return h;
}
void* test_open_memory(const unsigned char* data, size_t size) {
    wc_host_t* h = calloc(1, sizeof(*h));
    h->memory_size = 65536; h->memory = calloc(1, h->memory_size);
    if (wc_archive_open_memory(h, data, size)) { wc_archive_close(h); free(h->memory); free(h); return NULL; }
    return h;
}
void test_close(wc_host_t* h) { wc_archive_close(h); free(h->memory); free(h); }
unsigned char* test_memory(wc_host_t* h) { return h->memory; }
static mz_file_read_func previous_read;
static void* previous_opaque;
static size_t total_read, max_read;
static size_t counted_read(void* unused, mz_uint64 ofs, void* dest, size_t n) {
    total_read += n; if (n > max_read) max_read = n;
    return previous_read(previous_opaque, ofs, dest, n);
}
int test_counted_range(wc_host_t* h, const char* p, unsigned offset, unsigned char* out, unsigned n) {
    mz_zip_archive* z = h->archive;
    previous_read = z->m_pRead; previous_opaque = z->m_pIO_opaque;
    total_read = max_read = 0; z->m_pRead = counted_read;
    int r = wc_archive_read_asset_range(h, p, offset, out, n);
    z->m_pRead = previous_read; return r;
}
size_t test_total_read(void) { return total_read; }
size_t test_max_read(void) { return max_read; }
'''
STUB = r'''
extern "C" {
#include "cart_host.h"
}
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
static int trapped, refreshes, result;
static wc_host_t* _current_host;
static void* g_isolate;
static int ctx() { return 0; }
static void refresh_memory(wc_host_t*) { ++refreshes; }
static void trap_cart(wc_host_t* h, const char*, ...) { trapped = 1; h->trapped = true; }
namespace v8 {
struct Value { uint32_t x; Value* operator->() { return this; }
    Value Uint32Value(int) { return *this; } uint32_t FromJust() { return x; } };
struct Integer { static int New(void*, int x) { return x; } };
struct Return { void Set(int x) { result = x; } };
template<class T> struct FunctionCallbackInfo {
    mutable Value a[5]; Value& operator[](unsigned i) const { return a[i]; }
    Return GetReturnValue() const { return {}; }
};
}
'''

def function(source, needle):
    start = source.index(needle)
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build(tmp):
    src = (ROOT/'src/cart_host.cpp').read_text()
    assert 'v8str("wc_read_asset_range"), make_fn(v8_wc_read_asset_range)' in src
    (tmp/'bridge.c').write_text(BRIDGE)
    # The bounds function and callback are executed unchanged, not reimplemented.
    (tmp/'callback.cpp').write_text(STUB + '\n' +
        function(src, 'extern "C" bool wc_cart_range_ok(') + '\n' +
        function(src, 'static void v8_wc_read_asset_range(') + r'''
extern "C" int test_import(wc_host_t* h, uint32_t p, uint32_t l, uint32_t o, uint32_t d, uint32_t n) {
    _current_host = h; trapped = refreshes = 0; result = -999;
    v8::FunctionCallbackInfo<v8::Value> a{{{p},{l},{o},{d},{n}}};
    v8_wc_read_asset_range(a);
    return trapped ? -999 : result;
}
extern "C" int test_refreshed(void) { return refreshes; }
''')
    inc = ['-I'+str(ROOT/'src'), '-I'+str(ROOT/'include'), '-I'+str(ROOT/'deps')]
    objs=[]
    for name, path in [('bridge', tmp/'bridge.c'), ('asset', ROOT/'src/asset_loader.c'),
                       ('miniz', ROOT/'deps/miniz.c'), ('json', ROOT/'deps/cJSON.c')]:
        obj=tmp/(name+'.o'); objs.append(str(obj))
        subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DMINIZ_NO_TIME','-fPIC',*inc,'-c',str(path),'-o',str(obj)], check=True)
    obj=tmp/'callback.o'; objs.append(str(obj))
    subprocess.run(['c++','-std=c++17','-fPIC',*inc,'-c',str(tmp/'callback.cpp'),'-o',str(obj)],check=True)
    lib=tmp/'range.so'
    subprocess.run(['c++','-shared',*objs,'-o',str(lib)],check=True)
    d=C.CDLL(str(lib))
    for f in ['test_open']:
        getattr(d,f).argtypes=[C.c_char_p]; getattr(d,f).restype=C.c_void_p
    d.test_open_memory.argtypes=[C.c_void_p,C.c_size_t]; d.test_open_memory.restype=C.c_void_p
    d.test_close.argtypes=[C.c_void_p]
    d.test_memory.argtypes=[C.c_void_p]; d.test_memory.restype=C.POINTER(C.c_ubyte)
    for f in ['wc_archive_load_asset','wc_archive_read_asset_range','test_counted_range']:
        getattr(d,f).argtypes=([C.c_void_p,C.c_char_p,C.c_void_p,C.c_uint32] if f=='wc_archive_load_asset' else
                              [C.c_void_p,C.c_char_p,C.c_uint32,C.c_void_p,C.c_uint32])
        getattr(d,f).restype=C.c_int32
    d.test_import.argtypes=[C.c_void_p]+[C.c_uint32]*5;d.test_import.restype=C.c_int32
    d.test_total_read.restype=C.c_size_t;d.test_max_read.restype=C.c_size_t
    return d


def archive(descriptor=False, zip64=False):
    class Forward(io.BytesIO):
        def seek(self, *args): raise io.UnsupportedOperation('forward-only')
    b=Forward() if descriptor else io.BytesIO()
    old_limit=zipfile.ZIP64_LIMIT
    if zip64: zipfile.ZIP64_LIMIT=128
    with zipfile.ZipFile(b,'w') as z:
        z.writestr('manifest.json',json.dumps({'name':'range','abi':4,'assets':'data/'}))
        z.writestr('cart.wasm',b'\0asm\1\0\0\0')
        for name,data,method in [
            ('data/a.bin',bytes(range(256))*1024,0), ('a.bin',b'bare-hidden',0),
            ('assets/a.bin',b'legacy-hidden',0), ('bare.bin',b'bare',0),
            ('assets/legacy.bin',b'legacy',0), ('data/empty.bin',b'',0),
            ('data/deflated.bin',b'zip'*100,8), ('data/bzip.bin',b'bzip'*100,12),
            ('data/directory/',b'',0), ('data/parts/area1.zip',b'area-one',0),
            ('data/parts/area2.zip',b'area-two',0)]:
            z.writestr(name,data,compress_type=method)
    zipfile.ZIP64_LIMIT=old_limit
    return b.getvalue()


def entries(raw):
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        return {i.filename:i for i in z.infolist()}


def patch(raw, name, local=None, central=None, data=False):
    b=bytearray(raw); e=entries(raw)[name]; at=e.header_offset
    if local:
        off,fmt,value=local;struct.pack_into(fmt,b,at+off,value)
    if central:
        cd=b.index(b'PK\1\2'); rawname=name.encode()
        while b[cd+46:cd+46+len(rawname)]!=rawname:
            cd += 46+sum(struct.unpack_from('<HHH',b,cd+28))
        off,fmt,value=central;struct.pack_into(fmt,b,cd+off,value)
    if data:
        start=at+30+sum(struct.unpack_from('<HH',b,at+26));b[start+17]^=0x80
    return bytes(b)


def worker_test(tmp, fixtures, cases):
    # Execute the actual embedded worker asset functions with its real fs/ZIP
    # index representation; no worker lifecycle/GL scaffold is needed.
    (tmp/'worker-cases.json').write_text(json.dumps({'fixtures':fixtures,'cases':cases}))
    js=r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const [header,input]=process.argv.slice(2), packet=JSON.parse(fs.readFileSync(input));
const all=Array.from(fs.readFileSync(header,'utf8').matchAll(/R"WCJS\(([\s\S]*?)\)WCJS"/g),m=>m[1]).join('');
const prefix=all.slice(0,all.indexOf('// ---- WASI'));
for(const f of packet.fixtures) {
 const names=f.entries.map(e=>Buffer.from(e.name.toLowerCase())).sort(Buffer.compare);
 const rows=names.map(n=>f.entries.find(e=>e.name.toLowerCase()===n.toString()));
 const namesLen=names.reduce((s,b)=>s+b.length,0),count=rows.length,recs=count*24,nameAt=recs+count*20;
 const buffer=new SharedArrayBuffer(nameAt+namesLen),nums=new Float64Array(buffer,0,count*3),rec=new Uint32Array(buffer,recs,count*5),str=new Uint8Array(buffer,nameAt,namesLen);
 let off=0; rows.forEach((e,i)=>{nums.set([e.ofs,e.csize,e.usize],i*3);rec.set([off,names[i].length,e.crc,e.method,e.flags|(e.dir?65536:0)],i*5);str.set(names[i],off);off+=names[i].length;});
 const memory=new WebAssembly.Memory({initial:1}),u8=new Uint8Array(memory.buffer);
 const cfg={assetIndex:{buffer,count,recs,names:nameAt,namesLen,listLen:-1},assetsRoot:'data/',wascPath:f.path};
 const context={Buffer,performance,SharedArrayBuffer,WebAssembly,Uint8Array,DataView,Float64Array,Uint32Array,Int32Array,console,
  require:n=>n==='worker_threads'?{workerData:{memory,cfg}}:require(n)};
 vm.createContext(context);vm.runInContext(prefix+'\nglobalThis.range=readAssetRange;globalThis.load=loadAsset;',context);
 for(const c of packet.cases) {
  u8.fill(0xa5);const p=Buffer.from(c.path);u8.set(p,100);
  const r=context.range(100,p.length,c.offset,2000,c.length),expected=c.expected[f.name]??c.expected.base;
  assert.strictEqual(r,expected,`${f.name}/${c.path}/${c.offset}/${c.length}`);
  if(r>=0 && c.bytes && ['base','zip64','descriptor'].includes(f.name)) assert.deepStrictEqual(Array.from(u8.slice(2000,2000+r)),c.bytes.slice(0,r));
  if(r<0) assert(u8.slice(2000,2000+Math.min(c.length,100)).every(x=>x===0xa5));
 }
 u8.set(Buffer.from('a.bin'),100);
 for(const args of [[65534,5,0,2000,1],[100,5,0,65535,2],[100,5,0,0xffffff00,0x200]]) {
  assert.throws(()=>context.range(...args),/wc_read_asset_range.*outside/);
 }
 const rawOK=['base','corrupt-data','zip64','descriptor'].includes(f.name);
 assert.strictEqual(context.range(100,5,17,100,4),rawOK?4:-1);
 if(rawOK)assert.deepStrictEqual(Array.from(u8.slice(100,104)),[f.name==='corrupt-data'?145:17,18,19,20]);
 assert.strictEqual(context.range(100,0,0,65536,0),-1);
 u8.set(Buffer.from('a.bin'),100);
 const full=context.load(100,5,2000,65536-2000);assert.strictEqual(full,-1); // too small, old contract
 }
console.log('worker range/parity/bounds checks passed');
'''
    (tmp/'worker-test.js').write_text(js)
    subprocess.run([shutil.which('node') or 'node',str(tmp/'worker-test.js'),str(ROOT/'src/thread_worker_js.h'),str(tmp/'worker-cases.json')],check=True)


def main():
    with tempfile.TemporaryDirectory(prefix='asset-range-test-') as name:
        tmp=Path(name);d=build(tmp);raw=archive(); fixtures=[]
        variants={'base':raw,'zip64':archive(zip64=True),'descriptor':archive(descriptor=True),
          'corrupt-data':patch(raw,'data/a.bin',data=True),
          'header-method':patch(raw,'data/a.bin',local=(8,'<H',8)),
          'header-flags':patch(raw,'data/a.bin',local=(6,'<H',1)),
          'encrypted':patch(raw,'data/a.bin',local=(6,'<H',1),central=(8,'<H',1)),
          'patch-flag':patch(raw,'data/a.bin',local=(6,'<H',32),central=(8,'<H',32)),
          'bad-signature':patch(raw,'data/a.bin',local=(0,'<I',0)),
          'bad-extra':patch(raw,'data/a.bin',local=(28,'<H',65535)),
          'unequal-size':patch(raw,'data/a.bin',central=(24,'<I',5))}
        # Bad extra shifts data but can remain in archive: choose all-FFFF lengths
        # on a small entry, ensuring the computed payload exceeds archive bounds.
        variants['bad-extra']=patch(patch(raw,'data/a.bin',local=(28,'<H',65535)), 'data/a.bin',local=(26,'<H',65535))
        cases=[]
        def add(path,off,n,want,data=None):
            exp={'base':want}
            if path.lower()=='a.bin':
                for v in variants:
                    if v not in ('base','corrupt-data','zip64','descriptor'):exp[v]=-1
            cases.append({'path':path,'offset':off,'length':n,'expected':exp,**({'bytes':list(data)} if data is not None else {})})
        add('a.bin',17,4,4,bytes(range(17,21)))
        add('A.BIN',262142,12,2,bytes([254,255]));add('a.bin',262144,0,0);add('a.bin',262144,99,0)
        add('a.bin',262145,0,-1);add('a.bin',0xffffffff,2,-1)
        add('bare.bin',0,99,4,b'bare');add('legacy.bin',0,99,6,b'legacy')
        add('parts/area1.zip',0,8,8,b'area-one');add('parts/area2.zip',0,8,8,b'area-two')
        add('missing',0,3,-1);add('_filelist.txt',0,3,-1);add('directory/',0,0,-1)
        add('empty.bin',0,3,0,b'');add('deflated.bin',0,3,-1);add('bzip.bin',0,3,-1)
        checks=0
        for v,b in variants.items():
            path=tmp/(v+'.wasc');path.write_bytes(b)
            infos=entries(b)
            fixtures.append({'name':v,'path':str(path),'entries':[{'name':e.filename,'ofs':e.header_offset,'csize':e.compress_size,'usize':e.file_size,'crc':e.CRC,'method':e.compress_type,'flags':e.flag_bits,'dir':e.is_dir()} for e in infos.values()]})
            for mode in ['file','memory']:
                owner=C.create_string_buffer(b)
                h=d.test_open(str(path).encode()) if mode=='file' else d.test_open_memory(owner,len(b))
                assert h,(v,mode)
                try:
                    for c in cases:
                        out=(C.c_ubyte*max(1,c['length']))(*([0xa5]*max(1,c['length'])))
                        r=d.wc_archive_read_asset_range(h,c['path'].encode(),c['offset'],out,c['length'])
                        assert r==c['expected'].get(v,c['expected']['base']),(v,mode,c,r)
                        if r>=0 and 'bytes'in c and v in ('base','zip64','descriptor'):assert bytes(out[:r])==bytes(c['bytes'][:r])
                        if r<0:assert all(x==0xa5 for x in out)
                        checks+=1
                    mem=d.test_memory(h)
                    for i in range(65536):mem[i]=0xa5
                    for i,x in enumerate(b'a.bin'):mem[100+i]=x
                    for args in [(65534,5,0,2000,1),(100,5,0,65535,2),(100,5,0,0xffffff00,0x200),(0xffffffff,2,0,2000,1),(100,5,262144,65535,2),(100,5,0,0,0xffffffff)]:
                        assert d.test_import(h,*args)==-999 and d.test_refreshed()==1
                        assert mem[2000]==0xa5
                        checks+=1
                    assert d.test_import(h,100,5,17,65536,0)==(0 if v in ('base','corrupt-data','zip64','descriptor') else -1)
                    if v=='base':
                        assert d.test_import(h,100,5,17,100,4)==4 and bytes(mem[100:104])==bytes(range(17,21))
                        out=(C.c_ubyte*16)()
                        assert d.test_counted_range(h,b'a.bin',1234,out,16)==16
                        assert d.test_total_read()==46 and d.test_max_read()==30 # no whole asset I/O
                        for i,x in enumerate(b'a.bin\0ignored'):mem[100+i]=x
                        assert d.test_import(h,100,13,17,2000,4)==4
                    if v=='corrupt-data':
                        out=(C.c_ubyte*4)()
                        assert d.wc_archive_read_asset_range(h,b'a.bin',17,out,4)==4 and bytes(out)==bytes([145,18,19,20])
                    full=(C.c_ubyte*262144)()
                    if v in ('base','corrupt-data','zip64','descriptor'):
                        assert d.wc_archive_read_asset_range(h,b'a.bin',0,full,262144)==262144
                        assert full[17]==(145 if v=='corrupt-data' else 17)
                        assert d.wc_archive_load_asset(h,b'a.bin',full,262144)==(-1 if v=='corrupt-data' else 262144)
                        assert d.wc_archive_load_asset(h,b'a.bin',full,262143)==-1
                    listing=(C.c_ubyte*4096)()
                    assert d.wc_archive_load_asset(h,b'_filelist.txt',listing,4096)>0
                    assert d.wc_archive_load_asset(h,b'directory/',listing,4096)==0
                    compressed=(C.c_ubyte*300)()
                    assert d.wc_archive_load_asset(h,b'deflated.bin',compressed,300)==300
                finally:d.test_close(h)
        worker_test(tmp,fixtures,cases)
        print(json.dumps({'status':'PASS','native_range_and_import_cases':checks,'archive_variants':len(variants),'modes':['file','memory'],'bounded_io':'30-byte header + requested payload','full_host_v8_build':'not run; callback call-interface double plus existing real-cart bounds test extension'},sort_keys=True))

if __name__=='__main__': main()
