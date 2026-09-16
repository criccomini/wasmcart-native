// asset_loader.c — .wasc ZIP archive reading and manifest parsing

#include "cart_host.h"

#include "../deps/miniz.h"
#include "../deps/cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wc_log.h"

// ─── Archive open/close ────────────────────────────────────────────────────

static int read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(size);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    fclose(f);
    *out = data;
    *out_len = (size_t)size;
    return 0;
}

int wc_archive_open(wc_host_t* host, const char* path) {
    size_t len = strlen(path);
    bool is_wasc = (len > 5 && strcmp(path + len - 5, ".wasc") == 0);

    if (!is_wasc) {
        // Bare .wasm file
        if (read_file(path, &host->wasm_bytes, &host->wasm_bytes_len) != 0) return -1;

        // Default manifest
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
        return 0;
    }

    // .wasc ZIP archive — open from file (no need to load entire ZIP into RAM)
    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_file(zip, path, 0)) {
        wc_log("wasmcart: failed to open ZIP: %s\n", path);
        free(zip);
        return -1;
    }
    host->archive = zip;

    // Read manifest.json
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) {
            wc_parse_manifest(host, json, json_size);
            free(json);
        }
    } else {
        // No manifest — use defaults
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        strncpy(host->manifest.name, basename, sizeof(host->manifest.name) - 1);
        host->manifest.abi = WC_ABI_VERSION;
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);
        host->manifest.players = 1;
    }

    // Read cart.wasm (or manifest entry)
    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) {
        wc_log("wasmcart: entry '%s' not found in archive\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        host->archive = NULL;
        return -1;
    }

    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    if (!host->wasm_bytes) {
        wc_log("wasmcart: failed to extract %s\n", entry);
        mz_zip_reader_end(zip);
        free(zip);
        host->archive = NULL;
        return -1;
    }

    return 0;
}

int wc_archive_open_memory(wc_host_t* host, const uint8_t* data, size_t len) {
    mz_zip_archive* zip = calloc(1, sizeof(mz_zip_archive));
    if (!mz_zip_reader_init_mem(zip, data, len, 0)) {
        free(zip);
        return -1;
    }
    host->archive = zip;

    // Same flow as above for manifest + wasm extraction
    int idx = mz_zip_reader_locate_file(zip, "manifest.json", NULL, 0);
    if (idx >= 0) {
        size_t json_size;
        char* json = mz_zip_reader_extract_to_heap(zip, idx, &json_size, 0);
        if (json) { wc_parse_manifest(host, json, json_size); free(json); }
    }

    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";
    idx = mz_zip_reader_locate_file(zip, entry, NULL, 0);
    if (idx < 0) return -1;
    host->wasm_bytes = mz_zip_reader_extract_to_heap(zip, idx, &host->wasm_bytes_len, 0);
    return host->wasm_bytes ? 0 : -1;
}

void wc_archive_close(wc_host_t* host) {
    if (host->file_list) {
        free(host->file_list);
        host->file_list = NULL;
        host->file_list_len = 0;
    }
    if (host->archive) {
        mz_zip_reader_end((mz_zip_archive*)host->archive);
        free(host->archive);
        host->archive = NULL;
    }
    if (host->wasm_bytes) {
        free(host->wasm_bytes);
        host->wasm_bytes = NULL;
        host->wasm_bytes_len = 0;
    }
}

// ─── Asset loading ─────────────────────────────────────────────────────────

/*
 * Resolve a cart-relative asset path to a ZIP entry index.
 *
 * A cart says "roms/game.prg"; the archive may hold it bare, under the
 * manifest's own asset root ("app/roms/game.prg"), or under the legacy
 * "assets/" the packer uses when it generates a manifest itself. Try the
 * declared root first so a cart that ships both spellings gets its own.
 */
static int locate_asset(mz_zip_archive* zip, const wc_manifest_t* man, const char* path) {
    char buf[512];
    if (man->assets[0]) {
        snprintf(buf, sizeof(buf), "%s%s", man->assets, path);
        int idx = mz_zip_reader_locate_file(zip, buf, NULL, 0);
        if (idx >= 0) return idx;
    }
    int idx = mz_zip_reader_locate_file(zip, path, NULL, 0);
    if (idx >= 0) return idx;
    snprintf(buf, sizeof(buf), "assets/%s", path);
    return mz_zip_reader_locate_file(zip, buf, NULL, 0);
}

/*
 * The virtual "_filelist.txt": every asset path, newline separated, so a cart
 * can enumerate what it shipped with. The ABI has no directory call, so this
 * is how a ROM picker or a bezel picker finds its choices.
 *
 * Names are emitted ONCE and with the manifest's asset root stripped, i.e.
 * exactly the spelling a cart passes back to wc_load_asset. The JS host does
 * the same; a cart must not see a different list depending on which host ran
 * it. manifest.json and the wasm entry are not assets and are left out.
 *
 * Built on demand and cached: most carts never ask.
 */
static const char* build_file_list(wc_host_t* host, uint32_t* out_len) {
    if (host->file_list) { *out_len = host->file_list_len; return host->file_list; }
    if (!host->archive) return NULL;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    const char* root = host->manifest.assets;
    size_t rootlen = root[0] ? strlen(root) : 0;
    const char* entry = host->manifest.entry[0] ? host->manifest.entry : "cart.wasm";

    mz_uint n = mz_zip_reader_get_num_files(zip);
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap);
    if (!buf) return NULL;

    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(zip, i)) continue;
        const char* name = st.m_filename;
        if (strcmp(name, "manifest.json") == 0 || strcmp(name, entry) == 0) continue;
        if (rootlen && strncmp(name, root, rootlen) == 0) name += rootlen;
        else if (strncmp(name, "assets/", 7) == 0) name += 7;
        if (!name[0]) continue;

        size_t nl = strlen(name);
        if (len + nl + 2 > cap) {
            while (len + nl + 2 > cap) cap *= 2;
            char* grown = realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        if (len) buf[len++] = '\n';
        memcpy(buf + len, name, nl);
        len += nl;
    }
    buf[len] = 0;
    host->file_list = buf;
    host->file_list_len = (uint32_t)len;
    *out_len = host->file_list_len;
    return host->file_list;
}

int32_t wc_archive_asset_size(wc_host_t* host, const char* path) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    if (strcmp(path, "_filelist.txt") == 0) {
        uint32_t flen = 0;
        return build_file_list(host, &flen) ? (int32_t)flen : -1;
    }

    int idx = locate_asset(zip, &host->manifest, path);
    if (idx < 0) return -1;

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;
    return (int32_t)stat.m_uncomp_size;
}

int32_t wc_archive_load_asset(wc_host_t* host, const char* path, uint8_t* dest, uint32_t max_size) {
    if (!host->archive) return -1;
    mz_zip_archive* zip = (mz_zip_archive*)host->archive;

    if (strcmp(path, "_filelist.txt") == 0) {
        uint32_t flen = 0;
        const char* list = build_file_list(host, &flen);
        if (!list) return -1;
        if (flen > max_size) return -1;
        memcpy(dest, list, flen);
        return (int32_t)flen;
    }

    int idx = locate_asset(zip, &host->manifest, path);
    if (idx < 0) {
        static int _miss = 0;
        if (_miss < 3) { wc_log( "wasmcart: asset not found: %s\n", path); _miss++; }
        return -1;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) return -1;

    uint32_t read_size = (uint32_t)stat.m_uncomp_size;
    if (read_size > max_size) {
        wc_log( "wasmcart: asset %s: size %u > max %u, truncating\n", path, read_size, max_size);
        read_size = max_size;
    }

    if (!mz_zip_reader_extract_to_mem(zip, idx, dest, read_size, 0)) {
        wc_log( "wasmcart: asset %s: extract failed\n", path);
        return -1;
    }
    static int _load = 0;
    if (_load < 5) { wc_log( "wasmcart: loaded asset %s (%u bytes, max_size=%u, uncomp=%u)\n", path, read_size, max_size, (uint32_t)stat.m_uncomp_size); _load++; }
    return (int32_t)read_size;
}

// ─── Manifest parsing ──────────────────────────────────────────────────────

int wc_parse_manifest(wc_host_t* host, const char* json, size_t len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        wc_log( "wasmcart: failed to parse manifest.json\n");
        return -1;
    }

    cJSON* item;

    item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(item))
        strncpy(host->manifest.name, item->valuestring, sizeof(host->manifest.name) - 1);

    item = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(item))
        strncpy(host->manifest.version, item->valuestring, sizeof(host->manifest.version) - 1);

    item = cJSON_GetObjectItem(root, "abi");
    host->manifest.abi = cJSON_IsNumber(item) ? item->valueint : 2;

    item = cJSON_GetObjectItem(root, "entry");
    if (cJSON_IsString(item))
        strncpy(host->manifest.entry, item->valuestring, sizeof(host->manifest.entry) - 1);
    else
        strncpy(host->manifest.entry, "cart.wasm", sizeof(host->manifest.entry) - 1);

    /*
     * Asset root. The packer writes assets under whatever prefix the cart's
     * manifest declares ("app/" for the emulator carts), so resolving against
     * a hardcoded "assets/" finds nothing. Normalize to a single trailing
     * slash so the lookups can concatenate blindly.
     */
    item = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        strncpy(host->manifest.assets, item->valuestring, sizeof(host->manifest.assets) - 2);
        size_t n = strlen(host->manifest.assets);
        if (n && host->manifest.assets[n - 1] != '/') {
            host->manifest.assets[n] = '/';
            host->manifest.assets[n + 1] = 0;
        }
    }

    item = cJSON_GetObjectItem(root, "players");
    host->manifest.players = cJSON_IsNumber(item) ? item->valueint : 1;

    item = cJSON_GetObjectItem(root, "pointer");
    host->manifest.pointer = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "keyboard");
    host->manifest.keyboard = cJSON_IsTrue(item);

    cJSON* controls = cJSON_GetObjectItem(root, "controls");
    if (cJSON_IsArray(controls)) {
        // Presentation hint (SPEC: Manifest > Fields). Unknown tokens are
        // ignored by rule, so new tokens never break old hosts.
        static const struct { const char* token; uint32_t bit; } ctrl_map[] = {
            {"dpad", WC_CTRL_DPAD}, {"a", WC_CTRL_A}, {"b", WC_CTRL_B},
            {"x", WC_CTRL_X}, {"y", WC_CTRL_Y}, {"l", WC_CTRL_L},
            {"r", WC_CTRL_R}, {"start", WC_CTRL_START},
            {"select", WC_CTRL_SELECT}, {"left_stick", WC_CTRL_LSTICK},
            {"right_stick", WC_CTRL_RSTICK}, {"left_trigger", WC_CTRL_LTRIG},
            {"right_trigger", WC_CTRL_RTRIG}, {"l3", WC_CTRL_L3},
            {"r3", WC_CTRL_R3},
        };
        host->manifest.controls = 0;
        host->manifest.controls_set = true;
        cJSON* tok;
        cJSON_ArrayForEach(tok, controls) {
            if (!cJSON_IsString(tok)) continue;
            for (size_t i = 0; i < sizeof(ctrl_map) / sizeof(ctrl_map[0]); i++) {
                if (strcmp(tok->valuestring, ctrl_map[i].token) == 0) {
                    host->manifest.controls |= ctrl_map[i].bit;
                    break;
                }
            }
        }
    }

    cJSON* net = cJSON_GetObjectItem(root, "net");
    if (cJSON_IsObject(net)) {
        // The presence of `net` at all is half the gate: wc_peer_open refuses
        // without it even if the cart set WC_FLAG_NET_PEER.
        host->manifest.has_net = true;

        // net.domains is the current spelling; net.websocket is the superseded
        // one, still read so manifests written before the wc_peer_* merge keep
        // working. Same precedence as the JS host.
        cJSON* domains = cJSON_GetObjectItem(net, "domains");
        if (!cJSON_IsArray(domains)) domains = cJSON_GetObjectItem(net, "websocket");
        if (cJSON_IsArray(domains)) {
            host->manifest.websocket = true;
            host->manifest.ws_domain_count = 0;
            cJSON* domain;
            cJSON_ArrayForEach(domain, domains) {
                if (cJSON_IsString(domain) && host->manifest.ws_domain_count < 8) {
                    strncpy(host->manifest.ws_domains[host->manifest.ws_domain_count],
                            domain->valuestring, 255);
                    host->manifest.ws_domains[host->manifest.ws_domain_count][255] = 0;
                    host->manifest.ws_domain_count++;
                }
            }
        }
        item = cJSON_GetObjectItem(net, "data-channel");
        host->manifest.data_channel = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);
    return 0;
}
