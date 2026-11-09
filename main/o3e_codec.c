#include "o3e_codec.h"
#include "o3e_db.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "cJSON.h"
#include "esp_heap_caps.h"
#else
#include <cjson/cJSON.h>
#endif

/* Longest "not found in <list>" message; the longest list name is 26 chars. */
#define ENUM_MISS_MAX 64

/* Codec trees go to PSRAM; CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL would put
 * these small allocations in internal RAM. */
static void *node_calloc(size_t n, size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
#endif
    return calloc(n, size);   /* boards without PSRAM keep working */
}

static void *node_realloc(void *ptr, size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
#endif
    return realloc(ptr, size);
}

static char *node_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = node_calloc(1, n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Compile: JSON codec description -> node tree                         */

static const struct {
    const char *name;
    o3e_kind_t  kind;
} KINDS[] = {
    { "RawCodec",       O3E_K_RAW },
    { "O3EInt",         O3E_K_INT },
    { "O3EInt8",        O3E_K_INT },
    { "O3EInt16",       O3E_K_INT },
    { "O3EInt32",       O3E_K_INT },
    { "O3EInt64",       O3E_K_INT },
    { "O3EFloat32",     O3E_K_FLOAT32 },
    { "O3EByteVal",     O3E_K_BYTEVAL },
    { "O3EBool",        O3E_K_BOOL },
    { "O3EUtf8",        O3E_K_UTF8 },
    { "O3ESoftVers",    O3E_K_SOFTVERS },
    { "O3EMacAddr",     O3E_K_MACADDR },
    { "O3EIp4Addr",     O3E_K_IP4ADDR },
    { "O3ESdate",       O3E_K_SDATE },
    { "O3EDateTime",    O3E_K_DATETIME },
    { "O3EStime",       O3E_K_STIME },
    { "O3EUtc",         O3E_K_UTC },
    { "O3EEnum",        O3E_K_ENUM },
    { "O3EList",        O3E_K_LIST },
    { "O3EArray",       O3E_K_ARRAY },
    { "O3EComplexType", O3E_K_COMPLEX },
    { "O3ESwitch",      O3E_K_SWITCH },
    { "O3EcosPhi",      O3E_K_COSPHI },
};

static o3e_kind_t kind_of(const char *name)
{
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (strcmp(KINDS[i].name, name) == 0) {
            return KINDS[i].kind;
        }
    }
    return O3E_K_UNKNOWN;
}

static char *dup_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0]) {
        return NULL;
    }
    return node_strdup(v->valuestring);
}

static double num_or(const cJSON *o, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool bool_or(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0;
    }
    return dflt;
}

static bool add_kid(o3e_node_t *n, o3e_node_t *kid)
{
    o3e_node_t **k = node_realloc(n->kids, sizeof(*k) * (n->n_kids + 1));
    if (!k) {
        o3e_codec_free(kid);
        return false;
    }
    n->kids = k;
    n->kids[n->n_kids++] = kid;
    return true;
}

o3e_node_t *o3e_codec_compile_cjson(const cJSON *jn)
{
    if (!cJSON_IsObject(jn)) {
        return NULL;
    }
    const cJSON *jcodec = cJSON_GetObjectItemCaseSensitive(jn, "codec");
    if (!cJSON_IsString(jcodec)) {
        return NULL;
    }

    o3e_node_t *n = node_calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->kind = kind_of(jcodec->valuestring);
    n->len = (uint16_t)num_or(jn, "len", 0);
    n->id = dup_str(jn, "id");
    n->scale = 1.0;

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(jn, "args");
    if (cJSON_IsObject(args)) {
        n->unit = dup_str(args, "unit");
        n->list_name = dup_str(args, "listStr");
        n->scale = num_or(args, "scale", 1.0);
        n->decimals = (uint8_t)num_or(args, "decimals", n->kind == O3E_K_FLOAT32 ? 2 : 0);
        n->signd = bool_or(args, "signed", false);
        n->array_len = (uint16_t)num_or(args, "arrayLength", 0);

        const cJSON *acc = cJSON_GetObjectItemCaseSensitive(args, "acc");
        if (cJSON_IsString(acc) && acc->valuestring) {
            n->acc = strcmp(acc->valuestring, "rw") == 0 ? O3E_ACC_RW
                   : strcmp(acc->valuestring, "ro") == 0 ? O3E_ACC_RO
                   : O3E_ACC_NONE;
        }

        const cJSON *tf = cJSON_GetObjectItemCaseSensitive(args, "timeformat");
        if (cJSON_IsString(tf) && tf->valuestring) {
            n->ts_format = strcmp(tf->valuestring, "ts") == 0;
        }

        const cJSON *subs = cJSON_GetObjectItemCaseSensitive(args, "subTypes");
        if (cJSON_IsArray(subs)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, subs) {
                o3e_node_t *kid = o3e_codec_compile_cjson(it);
                if (!kid || !add_kid(n, kid)) {
                    o3e_codec_free(n);
                    return NULL;
                }
            }
        }

        /* O3ESwitch: a 1-byte discriminator selects one of several equally
         * sized branches. Branch keys arrive as an object keyed by the
         * stringified discriminator value. */
        const cJSON *cases = cJSON_GetObjectItemCaseSensitive(args, "cases");
        if (cJSON_IsObject(cases)) {
            const cJSON *it;
            cJSON_ArrayForEach(it, cases) {
                o3e_node_t *kid = o3e_codec_compile_cjson(it);
                if (!kid || !add_kid(n, kid)) {
                    o3e_codec_free(n);
                    return NULL;
                }
                int32_t *cv = node_realloc(n->case_vals, sizeof(int32_t) * n->n_kids);
                bool *cd = node_realloc(n->case_default, sizeof(bool) * n->n_kids);
                if (!cv || !cd) {
                    free(cv);
                    free(cd);
                    o3e_codec_free(n);
                    return NULL;
                }
                n->case_vals = cv;
                n->case_default = cd;
                n->case_vals[n->n_kids - 1] = it->string ? (int32_t)strtol(it->string, NULL, 10) : 0;
                n->case_default[n->n_kids - 1] = false;
            }
        }
        const cJSON *dflt = cJSON_GetObjectItemCaseSensitive(args, "default");
        if (cJSON_IsObject(dflt)) {
            o3e_node_t *kid = o3e_codec_compile_cjson(dflt);
            if (!kid || !add_kid(n, kid)) {
                o3e_codec_free(n);
                return NULL;
            }
            int32_t *cv = node_realloc(n->case_vals, sizeof(int32_t) * n->n_kids);
            bool *cd = node_realloc(n->case_default, sizeof(bool) * n->n_kids);
            if (!cv || !cd) {
                free(cv);
                free(cd);
                o3e_codec_free(n);
                return NULL;
            }
            n->case_vals = cv;
            n->case_default = cd;
            n->case_vals[n->n_kids - 1] = 0;
            n->case_default[n->n_kids - 1] = true;
        }
    } else if (n->kind == O3E_K_FLOAT32) {
        n->decimals = 2;
    }

    return n;
}

o3e_node_t *o3e_codec_raw(uint16_t len, const char *id)
{
    o3e_node_t *n = node_calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->kind = O3E_K_RAW;
    n->len = len;
    n->scale = 1.0;
    n->acc = O3E_ACC_RO;   /* writing bytes we cannot interpret is not offered */
    n->id = id ? node_strdup(id) : NULL;
    return n;
}

o3e_node_t *o3e_codec_compile(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return NULL;
    }
    o3e_node_t *n = o3e_codec_compile_cjson(root);
    cJSON_Delete(root);
    return n;
}

void o3e_codec_free(o3e_node_t *n)
{
    if (!n) {
        return;
    }
    for (uint16_t i = 0; i < n->n_kids; i++) {
        o3e_codec_free(n->kids[i]);
    }
    free(n->kids);
    free(n->case_vals);
    free(n->case_default);
    free(n->id);
    free(n->unit);
    free(n->list_name);
    free(n);
}

/* ------------------------------------------------------------------ */
/* Decode                                                               */

/* open3e slices Python bytes objects, which silently yield fewer bytes when
 * the payload is short rather than raising.  Clamping here reproduces that
 * while keeping the C port inside its buffer. */
static uint64_t read_uint(const uint8_t *d, size_t avail, size_t width)
{
    uint64_t v = 0;
    size_t n = width < avail ? width : avail;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)d[i] << (8 * i);
    }
    return v;
}

static int64_t read_int(const uint8_t *d, size_t avail, size_t width, bool signd)
{
    uint64_t v = read_uint(d, avail, width);
    size_t n = width < avail ? width : avail;
    if (signd && n > 0 && n < 8) {
        uint64_t sign_bit = 1ULL << (8 * n - 1);
        if (v & sign_bit) {
            v |= ~((1ULL << (8 * n)) - 1);
        }
    }
    return (int64_t)v;
}

/* CPython's round(x, n) rounds the exact value of the double half-to-even.
 * glibc's printf does the same, so formatting and reading back reproduces it
 * without reimplementing correct decimal rounding. */
static double round_decimals(double v, int decimals)
{
    if (decimals <= 0 || !isfinite(v)) {
        return v;
    }
    /* Beyond 2^53 a double has no fractional part left, so rounding is the
     * identity -- and that is also where "%.*f" would need 300+ characters.
     * Skipping those keeps this off the deep end of a recursive decode. */
    if (v <= -9007199254740992.0 || v >= 9007199254740992.0) {
        return v;
    }
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%.*f", decimals, v);
    return strtod(tmp, NULL);
}

/* Append to a bounded buffer, tracking the offset safely.
 *
 * The obvious `o += snprintf(buf + o, sizeof(buf) - o, ...)` is wrong: on
 * truncation snprintf returns what it *would* have written, so `o` runs past
 * the end and the next call's `sizeof(buf) - o` underflows into a huge size.
 * A datapoint definition longer than expected would then write out of bounds. */
static void app_fmt(char *buf, size_t cap, size_t *o, const char *fmt, ...)
{
    if (*o >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *o, cap - *o, fmt, ap);
    va_end(ap);
    *o = (n < 0) ? cap : (*o + (size_t)n > cap ? cap : *o + (size_t)n);
}

static void emit_hex(const uint8_t *d, size_t n, const o3e_sink_t *sink, void *ctx)
{
    char stack[128];
    char *buf = n * 2 + 1 <= sizeof(stack) ? stack : malloc(n * 2 + 1);
    if (!buf) {
        sink->val_str(ctx, "");
        return;
    }
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        buf[i * 2] = HEX[d[i] >> 4];
        buf[i * 2 + 1] = HEX[d[i] & 0x0f];
    }
    buf[n * 2] = '\0';
    sink->val_str(ctx, buf);
    if (buf != stack) {
        free(buf);
    }
}

/* {"ID": v, "Text": "..."} -- open3e's shape for both O3EEnum and the
 * discriminator of O3ESwitch. */
static void emit_enum_pair(const char *list_name, int64_t val,
                           const o3e_sink_t *sink, void *ctx)
{
    sink->key(ctx, "ID");
    sink->val_i64(ctx, val);
    sink->key(ctx, "Text");

    const char *txt = o3e_db_enum(list_name, (int32_t)val);
    if (txt) {
        sink->val_str(ctx, txt);
    } else {
        char miss[ENUM_MISS_MAX];
        snprintf(miss, sizeof(miss), "not found in %s", list_name ? list_name : "");
        sink->val_str(ctx, miss);
    }
}

static bool decode_node(const o3e_node_t *n, const uint8_t *d, size_t avail,
                        const o3e_sink_t *sink, void *ctx);

/* O3EList: a Count field followed by that many repetitions of the complex
 * sub-type, with the remainder of the buffer left as padding.  The declared
 * length is the capacity, not the content length. */
static bool decode_list(const o3e_node_t *n, const uint8_t *d, size_t avail,
                        const o3e_sink_t *sink, void *ctx)
{
    size_t idx = 0;
    int64_t count = 0;

    sink->obj_begin(ctx);
    for (uint16_t i = 0; i < n->n_kids; i++) {
        const o3e_node_t *sub = n->kids[i];
        size_t rem = idx < avail ? avail - idx : 0;

        if (sub->id && strcasecmp(sub->id, "count") == 0) {
            count = read_int(d + idx, rem, sub->len, sub->signd);
            sink->key(ctx, sub->id);
            sink->val_i64(ctx, count);
            idx += sub->len;
        } else if (sub->kind == O3E_K_COMPLEX) {
            sink->key(ctx, sub->id ? sub->id : "");
            sink->arr_begin(ctx);
            for (int64_t k = 0; k < count; k++) {
                rem = idx < avail ? avail - idx : 0;
                if (sub->len && idx + sub->len > avail) {
                    break;   /* truncated response; emit what is there */
                }
                decode_node(sub, d + idx, rem, sink, ctx);
                idx += sub->len;
            }
            sink->arr_end(ctx);
        } else {
            sink->key(ctx, sub->id ? sub->id : "");
            decode_node(sub, d + idx, rem, sink, ctx);
            idx += sub->len;
        }
    }
    sink->obj_end(ctx);
    return true;
}

static bool decode_node(const o3e_node_t *n, const uint8_t *d, size_t avail,
                        const o3e_sink_t *sink, void *ctx)
{
    /* Sized against the database: the longest formatted value is 24 characters
     * (software version, date-time), so this is four times the real worst
     * case. It is per recursion level, and decoding recurses through nested
     * types, so the size matters more than it looks. */
    char buf[96];

    switch (n->kind) {
    case O3E_K_RAW:
    case O3E_K_UNKNOWN:
        emit_hex(d, n->len < avail ? n->len : avail, sink, ctx);
        return true;

    case O3E_K_INT: {
        /* An unsigned 64-bit datapoint (energy counters, DID 3230/3231) can
         * have the top bit set, so it must not travel through int64_t on the
         * way to a double -- that would wrap it negative. */
        double raw = n->signd ? (double)read_int(d, avail, n->len, true)
                              : (double)read_uint(d, avail, n->len);
        sink->val_double(ctx, round_decimals(raw / n->scale, n->decimals));
        return true;
    }

    case O3E_K_FLOAT32: {
        uint32_t bits = (uint32_t)read_uint(d, avail, 4);
        float f;
        memcpy(&f, &bits, sizeof(f));
        /* open3e's O3EFloat32 has no scale, so the default of 1.0 leaves its
         * datapoints untouched; E3onCAN's version divides, which is how the
         * E380 reports cumulated energy in kWh. */
        sink->val_double(ctx, round_decimals((double)f / n->scale, n->decimals));
        return true;
    }

    case O3E_K_COSPHI: {
        /* E3onCAN: the value is the second byte, negated when the first is
         * 0x04, then scaled. */
        double v = avail >= 2 ? (double)d[1] : 0.0;
        if (avail >= 1 && d[0] == 0x04) {
            v = -v;
        }
        sink->val_double(ctx, round_decimals(v / n->scale, n->decimals));
        return true;
    }

    case O3E_K_BYTEVAL:
        sink->val_i64(ctx, (int64_t)read_uint(d, avail, n->len));
        return true;

    case O3E_K_BOOL:
        sink->val_str(ctx, (avail && d[0]) ? "on" : "off");
        return true;

    case O3E_K_UTF8: {
        size_t n_av = n->len < avail ? n->len : avail;
        char *s = malloc(n_av + 1);
        if (!s) {
            sink->val_str(ctx, "");
            return true;
        }
        /* open3e decodes UTF-8 then strips NULs; the bytes pass through
         * unchanged, so copying while dropping 0x00 is equivalent. */
        size_t o = 0;
        for (size_t i = 0; i < n_av; i++) {
            if (d[i] != 0x00) {
                s[o++] = (char)d[i];
            }
        }
        s[o] = '\0';
        sink->val_str(ctx, s);
        free(s);
        return true;
    }

    case O3E_K_SOFTVERS: {
        size_t o = 0;
        for (size_t i = 0; i + 1 < (size_t)n->len && i + 1 < avail; i += 2) {
            app_fmt(buf, sizeof(buf), &o, "%s%u",
                    i ? "." : "", (unsigned)read_uint(d + i, avail - i, 2));
        }
        sink->val_str(ctx, buf);
        return true;
    }

    case O3E_K_MACADDR: {
        size_t o = 0;
        for (size_t i = 0; i < 6 && i < avail; i++) {
            app_fmt(buf, sizeof(buf), &o, "%s%02X", i ? "-" : "", d[i]);
        }
        sink->val_str(ctx, buf);
        return true;
    }

    case O3E_K_IP4ADDR: {
        size_t o = 0;
        for (size_t i = 0; i < n->len && i < avail; i++) {
            app_fmt(buf, sizeof(buf), &o, "%s%03u", i ? "." : "", d[i]);
        }
        sink->val_str(ctx, buf);
        return true;
    }

    case O3E_K_SDATE:
        if (avail >= 3) {
            snprintf(buf, sizeof(buf), "%02u.%02u.%u", d[0], d[1], 2000u + d[2]);
        } else {
            buf[0] = '\0';
        }
        sink->val_str(ctx, buf);
        return true;

    case O3E_K_STIME: {
        size_t o = 0;
        for (size_t i = 0; i < n->len && i < avail; i++) {
            app_fmt(buf, sizeof(buf), &o, "%s%02u", i ? ":" : "", d[i]);
        }
        sink->val_str(ctx, buf);
        return true;
    }

    case O3E_K_UTC: {
        /* open3e uses datetime.fromtimestamp(), i.e. local time. The device
         * timezone is configurable; see app_config's tz setting. */
        time_t ts = (time_t)read_uint(d, avail, 4);
        struct tm tmv;
        localtime_r(&ts, &tmv);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        sink->val_str(ctx, buf);
        return true;
    }

    case O3E_K_DATETIME: {
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_isdst = -1;
        if (n->ts_format) {
            time_t ts = (time_t)read_uint(d, avail, 6);
            localtime_r(&ts, &tmv);
        } else if (avail >= 8) {
            /* "VM" layout: year hi/lo, month, day, weekday (unused), h, m, s */
            tmv.tm_year = d[0] * 100 + d[1] - 1900;
            tmv.tm_mon  = d[2] - 1;
            tmv.tm_mday = d[3];
            tmv.tm_hour = d[5];
            tmv.tm_min  = d[6];
            tmv.tm_sec  = d[7];
        }
        struct tm norm = tmv;
        time_t epoch = mktime(&norm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);

        sink->obj_begin(ctx);
        sink->key(ctx, "DateTime");
        sink->val_str(ctx, buf);
        sink->key(ctx, "Timestamp");
        sink->val_i64(ctx, (int64_t)epoch * 1000);
        sink->obj_end(ctx);
        return true;
    }

    case O3E_K_ENUM: {
        int64_t v = (int64_t)read_uint(d, avail, n->len);
        sink->obj_begin(ctx);
        emit_enum_pair(n->list_name, v, sink, ctx);
        sink->obj_end(ctx);
        return true;
    }

    case O3E_K_COMPLEX: {
        size_t idx = 0;
        sink->obj_begin(ctx);
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *sub = n->kids[i];
            size_t rem = idx < avail ? avail - idx : 0;
            sink->key(ctx, sub->id ? sub->id : "");
            decode_node(sub, d + (idx < avail ? idx : avail), rem, sink, ctx);
            idx += sub->len;
        }
        sink->obj_end(ctx);
        return true;
    }

    case O3E_K_LIST:
        return decode_list(n, d, avail, sink, ctx);

    case O3E_K_ARRAY: {
        size_t idx = 0;
        sink->obj_begin(ctx);
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *sub = n->kids[i];
            sink->key(ctx, sub->id ? sub->id : "");
            sink->arr_begin(ctx);
            for (uint16_t k = 0; k < n->array_len; k++) {
                size_t rem = idx < avail ? avail - idx : 0;
                decode_node(sub, d + (idx < avail ? idx : avail), rem, sink, ctx);
                idx += sub->len;
            }
            sink->arr_end(ctx);
        }
        sink->obj_end(ctx);
        return true;
    }

    case O3E_K_SWITCH: {
        int64_t v = avail ? d[0] : 0;
        const o3e_node_t *branch = NULL;
        for (uint16_t i = 0; i < n->n_kids; i++) {
            if (!n->case_default[i] && n->case_vals[i] == (int32_t)v) {
                branch = n->kids[i];
                break;
            }
        }
        if (!branch) {
            for (uint16_t i = 0; i < n->n_kids; i++) {
                if (n->case_default[i]) {
                    branch = n->kids[i];
                    break;
                }
            }
        }

        sink->obj_begin(ctx);
        emit_enum_pair(n->list_name, v, sink, ctx);
        if (!branch) {
            snprintf(buf, sizeof(buf), "no matching case for ID %lld", (long long)v);
            sink->key(ctx, "Error");
            sink->val_str(ctx, buf);
        } else if (branch->kind == O3E_K_COMPLEX) {
            /* open3e merges a complex branch into the parent object rather
             * than nesting it, so the sub-fields sit next to ID/Text. */
            size_t idx = 1;
            for (uint16_t i = 0; i < branch->n_kids; i++) {
                const o3e_node_t *sub = branch->kids[i];
                size_t rem = idx < avail ? avail - idx : 0;
                sink->key(ctx, sub->id ? sub->id : "");
                decode_node(sub, d + (idx < avail ? idx : avail), rem, sink, ctx);
                idx += sub->len;
            }
        } else {
            size_t rem = avail > 1 ? avail - 1 : 0;
            sink->key(ctx, branch->id ? branch->id : "");
            decode_node(branch, d + (avail ? 1 : 0), rem, sink, ctx);
        }
        sink->obj_end(ctx);
        return true;
    }
    }
    return false;
}

bool o3e_codec_decode(const o3e_node_t *n, const uint8_t *data, size_t len,
                      const o3e_sink_t *sink, void *ctx)
{
    if (!n || !sink) {
        return false;
    }
    return decode_node(n, data, len, sink, ctx);
}

char *o3e_codec_decode_json(const o3e_node_t *n, const uint8_t *data, size_t len)
{
    o3e_buf_t out;
    o3e_json_ctx_t jc;
    o3e_buf_init(&out);
    o3e_json_ctx_init(&jc, &out);

    if (!o3e_codec_decode(n, data, len, &o3e_json_sink, &jc) || out.oom) {
        o3e_buf_free(&out);
        return NULL;
    }
    if (!out.buf) {
        o3e_buf_adds(&out, "");
    }
    return out.buf;   /* ownership passes to the caller */
}

/* ------------------------------------------------------------------ */
/* Encode                                                              */

/* open3e implements encode() for only part of its codecs; the rest raise
 * "not implemented yet".  Reporting those as read-only is honest: inventing an
 * encoding for, say, a software version field and writing it to a heat pump
 * would be worse than refusing.
 *
 * o3e_codec_encodable() below is a *conservative hint* for the web UI, not the
 * decision: it answers "could any value of this datapoint be written", while
 * the real answer also depends on the value (see o3e_codec_encode). */
static bool kind_encodable(o3e_kind_t k)
{
    switch (k) {
    case O3E_K_RAW:
    case O3E_K_INT:
    case O3E_K_FLOAT32:
    case O3E_K_BYTEVAL:
    case O3E_K_BOOL:
    case O3E_K_STIME:
    case O3E_K_ENUM:
    case O3E_K_COMPLEX:
    case O3E_K_LIST:
    case O3E_K_SWITCH:
        return true;
    default:
        return false;
    }
}

bool o3e_codec_encodable(const o3e_node_t *n)
{
    if (!n || !kind_encodable(n->kind)) {
        return false;
    }
    for (uint16_t i = 0; i < n->n_kids; i++) {
        if (!o3e_codec_encodable(n->kids[i])) {
            return false;
        }
    }
    return true;
}

#define ENC_FAIL(...) do { \
        if (err && err_sz) { snprintf(err, err_sz, __VA_ARGS__); } \
        return false; \
    } while (0)

static void put_le(uint8_t *out, uint64_t v, size_t width)
{
    for (size_t i = 0; i < width; i++) {
        out[i] = (uint8_t)(v >> (8 * i));
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool encode_node(const o3e_node_t *n, const cJSON *v,
                        uint8_t *out, size_t out_sz, char *err, size_t err_sz);

/* O3EEnum accepts the {"ID":n,"Text":"..."} object it decodes to, a bare text,
 * or -- as an addition for the web UI's select control -- the numeric ID. */
static bool encode_enum(const o3e_node_t *n, const cJSON *v,
                        uint8_t *out, size_t out_sz, char *err, size_t err_sz)
{
    const cJSON *text = NULL;
    if (cJSON_IsObject(v)) {
        /* Text wins when present. open3e resolves purely by text, so a value
         * that decoded to "not found in <list>" must fail to encode rather
         * than quietly writing back the raw ID it came from. */
        text = cJSON_GetObjectItemCaseSensitive(v, "Text");
        if (!cJSON_IsString(text)) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(v, "ID");
            if (cJSON_IsNumber(id)) {
                put_le(out, (uint64_t)(int64_t)id->valuedouble, n->len);
                return true;
            }
        }
    } else if (cJSON_IsNumber(v)) {
        put_le(out, (uint64_t)(int64_t)v->valuedouble, n->len);
        return true;
    } else if (cJSON_IsString(v)) {
        text = v;
    }

    if (!cJSON_IsString(text) || !text->valuestring) {
        ENC_FAIL("%s: expected an enum value, name or {\"ID\":...}",
                 n->id ? n->id : "?");
    }
    size_t count = o3e_db_enum_count(n->list_name);
    for (size_t i = 0; i < count; i++) {
        int32_t val;
        const char *txt;
        if (o3e_db_enum_at(n->list_name, i, &val, &txt) &&
            strcasecmp(txt, text->valuestring) == 0) {
            put_le(out, (uint64_t)val, n->len);
            return true;
        }
    }
    ENC_FAIL("%s: '%s' is not in enum list %s", n->id ? n->id : "?",
             text->valuestring, n->list_name ? n->list_name : "?");
}

static bool encode_node(const o3e_node_t *n, const cJSON *v,
                        uint8_t *out, size_t out_sz, char *err, size_t err_sz)
{
    if (out_sz < n->len) {
        ENC_FAIL("%s: buffer too small", n->id ? n->id : "?");
    }
    memset(out, 0, n->len);

    switch (n->kind) {
    case O3E_K_RAW: {
        if (!cJSON_IsString(v) || !v->valuestring) {
            ENC_FAIL("%s: expected a hex string", n->id ? n->id : "?");
        }
        size_t sl = strlen(v->valuestring);
        if (sl != (size_t)n->len * 2) {
            ENC_FAIL("%s: expected %u hex characters, got %zu",
                     n->id ? n->id : "?", n->len * 2, sl);
        }
        for (size_t i = 0; i < n->len; i++) {
            int hi = hexval(v->valuestring[i * 2]);
            int lo = hexval(v->valuestring[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                ENC_FAIL("%s: not a hex string", n->id ? n->id : "?");
            }
            out[i] = (uint8_t)(hi << 4 | lo);
        }
        return true;
    }

    case O3E_K_INT: {
        if (!cJSON_IsNumber(v)) {
            ENC_FAIL("%s: expected a number", n->id ? n->id : "?");
        }
        /* Mirrors open3e: round(value * scale), then little-endian.
         * Python does this in arbitrary precision, so an unsigned 64-bit
         * energy counter can legitimately scale past INT64_MAX -- rounding
         * through int64_t there would silently produce INT64_MIN. */
        double scaled = v->valuedouble * n->scale;
        double rounded = round(scaled);
        uint64_t raw;
        if (n->signd) {
            if (rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0) {
                ENC_FAIL("%s: %g is outside the range this field can hold",
                         n->id ? n->id : "?", v->valuedouble);
            }
            raw = (uint64_t)(int64_t)rounded;
        } else {
            if (rounded < 0.0 || rounded >= 18446744073709551616.0) {
                ENC_FAIL("%s: %g is outside the range this field can hold",
                         n->id ? n->id : "?", v->valuedouble);
            }
            raw = (uint64_t)rounded;
        }
        if (n->len < 8) {
            int64_t sraw = (int64_t)raw;
            int64_t lo = n->signd ? -(1LL << (8 * n->len - 1)) : 0;
            int64_t hi = n->signd ? (1LL << (8 * n->len - 1)) - 1
                                  : (int64_t)((1ULL << (8 * n->len)) - 1);
            if (sraw < lo || sraw > hi) {
                ENC_FAIL("%s: %g is outside the range this field can hold",
                         n->id ? n->id : "?", v->valuedouble);
            }
        }
        put_le(out, raw, n->len);
        return true;
    }

    case O3E_K_FLOAT32: {
        if (!cJSON_IsNumber(v)) {
            ENC_FAIL("%s: expected a number", n->id ? n->id : "?");
        }
        float f = (float)v->valuedouble;
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        put_le(out, bits, 4);
        return true;
    }

    case O3E_K_BYTEVAL: {
        if (!cJSON_IsNumber(v)) {
            ENC_FAIL("%s: expected a number", n->id ? n->id : "?");
        }
        if (v->valuedouble < 0) {
            ENC_FAIL("%s: value must not be negative", n->id ? n->id : "?");
        }
        put_le(out, (uint64_t)v->valuedouble, n->len);
        return true;
    }

    case O3E_K_BOOL:
        if (cJSON_IsBool(v)) {
            out[0] = cJSON_IsTrue(v) ? 1 : 0;
        } else if (cJSON_IsString(v) && v->valuestring) {
            out[0] = strcasecmp(v->valuestring, "on") == 0 ? 1 : 0;
        } else if (cJSON_IsNumber(v)) {
            out[0] = v->valuedouble != 0 ? 1 : 0;
        } else {
            ENC_FAIL("%s: expected \"on\"/\"off\"", n->id ? n->id : "?");
        }
        return true;

    case O3E_K_STIME: {
        if (!cJSON_IsString(v) || !v->valuestring) {
            ENC_FAIL("%s: expected \"hh:mm:ss\"", n->id ? n->id : "?");
        }
        const char *p = v->valuestring;
        for (uint16_t i = 0; i < n->len; i++) {
            char *end;
            long part = strtol(p, &end, 10);
            if (end == p || part < 0 || part > 255) {
                ENC_FAIL("%s: '%s' is not a colon-separated time",
                         n->id ? n->id : "?", v->valuestring);
            }
            out[i] = (uint8_t)part;
            p = (*end == ':') ? end + 1 : end;
        }
        return true;
    }

    case O3E_K_ENUM:
        return encode_enum(n, v, out, out_sz, err, err_sz);

    case O3E_K_COMPLEX: {
        if (!cJSON_IsObject(v)) {
            ENC_FAIL("%s: expected an object", n->id ? n->id : "?");
        }
        size_t off = 0;
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *sub = n->kids[i];
            const cJSON *sv = cJSON_GetObjectItemCaseSensitive(v, sub->id ? sub->id : "");
            if (!sv) {
                ENC_FAIL("missing field \"%s\"", sub->id ? sub->id : "?");
            }
            if (!encode_node(sub, sv, out + off, n->len - off, err, err_sz)) {
                return false;
            }
            off += sub->len;
        }
        return true;
    }

    case O3E_K_LIST: {
        if (!cJSON_IsObject(v)) {
            ENC_FAIL("%s: expected an object with Count and a list",
                     n->id ? n->id : "?");
        }
        /* open3e asserts a list has exactly two fields, so the three-field
         * DTC histories (Count + GrandTotal + ListEntries, DIDs 264 and 266)
         * cannot be written by it either. Refusing here keeps the two
         * implementations in step; those datapoints are read-only anyway. */
        if (n->n_kids != 2) {
            ENC_FAIL("%s: open3e cannot write lists with %u fields",
                     n->id ? n->id : "?", n->n_kids);
        }
        size_t off = 0;
        int64_t count = 0;
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *sub = n->kids[i];
            const cJSON *sv = cJSON_GetObjectItemCaseSensitive(v, sub->id ? sub->id : "");
            if (!sv) {
                ENC_FAIL("missing field \"%s\"", sub->id ? sub->id : "?");
            }
            if (sub->id && strcasecmp(sub->id, "count") == 0) {
                if (!cJSON_IsNumber(sv)) {
                    ENC_FAIL("Count must be a number");
                }
                count = (int64_t)sv->valuedouble;
                put_le(out + off, (uint64_t)count, sub->len);
                off += sub->len;
            } else if (sub->kind == O3E_K_COMPLEX) {
                if (!cJSON_IsArray(sv)) {
                    ENC_FAIL("%s must be an array", sub->id ? sub->id : "?");
                }
                if (cJSON_GetArraySize(sv) != count) {
                    ENC_FAIL("Count is %lld but %s has %d entries",
                             (long long)count, sub->id ? sub->id : "?",
                             cJSON_GetArraySize(sv));
                }
                const cJSON *it;
                cJSON_ArrayForEach(it, sv) {
                    if (off + sub->len > n->len) {
                        ENC_FAIL("%s: too many entries for this datapoint",
                                 n->id ? n->id : "?");
                    }
                    if (!encode_node(sub, it, out + off, n->len - off, err, err_sz)) {
                        return false;
                    }
                    off += sub->len;
                }
            } else {
                if (!encode_node(sub, sv, out + off, n->len - off, err, err_sz)) {
                    return false;
                }
                off += sub->len;
            }
        }
        /* Remaining bytes stay zero, matching open3e's explicit padding. */
        return true;
    }

    case O3E_K_SWITCH: {
        if (!cJSON_IsObject(v)) {
            ENC_FAIL("%s: expected an object with an ID", n->id ? n->id : "?");
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(v, "ID");
        if (!cJSON_IsNumber(id)) {
            ENC_FAIL("%s: ID selects the variant and must be a number",
                     n->id ? n->id : "?");
        }
        int32_t sel = (int32_t)id->valuedouble;
        const o3e_node_t *branch = NULL;
        for (uint16_t i = 0; i < n->n_kids; i++) {
            if (!n->case_default[i] && n->case_vals[i] == sel) {
                branch = n->kids[i];
                break;
            }
        }
        if (!branch) {
            for (uint16_t i = 0; i < n->n_kids; i++) {
                if (n->case_default[i]) {
                    branch = n->kids[i];
                }
            }
        }
        if (!branch) {
            ENC_FAIL("%s: no variant for ID %d", n->id ? n->id : "?", (int)sel);
        }
        out[0] = (uint8_t)sel;
        /* Decoding merges a complex branch into the parent object, so encoding
         * reads its fields from there too. */
        const cJSON *bv = (branch->kind == O3E_K_COMPLEX)
                        ? v
                        : cJSON_GetObjectItemCaseSensitive(v, branch->id ? branch->id : "");
        if (!bv) {
            ENC_FAIL("missing field \"%s\"", branch->id ? branch->id : "?");
        }
        return encode_node(branch, bv, out + 1, out_sz - 1, err, err_sz);
    }

    default:
        ENC_FAIL("%s: open3e cannot encode %s fields",
                 n->id ? n->id : "?", "this type");
    }
}

bool o3e_codec_encode(const o3e_node_t *n, const cJSON *value,
                      uint8_t *out, size_t out_sz, char *err, size_t err_sz)
{
    if (err && err_sz) {
        err[0] = '\0';
    }
    if (!n || !value) {
        ENC_FAIL("nothing to encode");
    }
    /* No static gate here on purpose: upstream decides per value, not per type.
     * A list whose element codec cannot be encoded still encodes fine when the
     * count is zero, and an enum encodes only when the text maps to a known
     * value. encode_node() fails at the exact field that cannot be written,
     * which also gives the web UI a message worth showing. */
    return encode_node(n, value, out, out_sz, err, err_sz);
}
