/*
 * Flattened device tree parser (DTB format version 16/17).
 *
 * Layout (Devicetree Specification v0.4, chapter 5):
 *   header -> memory reservation block -> structure block -> strings block
 *
 * The structure block is a stream of big-endian 32-bit tokens:
 *   FDT_BEGIN_NODE name\0 (padded to 4)
 *   FDT_PROP len nameoff value (padded to 4)
 *   FDT_END_NODE, FDT_NOP, FDT_END
 */
#include "fdt.h"
#include "kstring.h"

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x1
#define FDT_END_NODE    0x2
#define FDT_PROP        0x3
#define FDT_NOP         0x4
#define FDT_END         0x9

#define MAX_DEPTH       16

static const uint8_t *blob;
static const uint8_t *dt_struct;
static const char    *dt_strings;
static uint32_t       struct_size;
static uint32_t       total_size;

uint32_t fdt_be32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

uint64_t fdt_read_cells(const void *p, int cells)
{
    const uint8_t *b = p;
    uint64_t v = 0;
    for (int i = 0; i < cells; i++)
        v = (v << 32) | fdt_be32(b + 4 * i);
    return v;
}

static inline uint32_t align4(uint32_t x)
{
    return (x + 3) & ~3u;
}

static inline uint32_t token(uint32_t off)
{
    return fdt_be32(dt_struct + off);
}

int fdt_init(const void *p)
{
    const uint8_t *b = p;

    blob = 0;
    if (!b || fdt_be32(b) != FDT_MAGIC)
        return -1;

    uint32_t totalsize   = fdt_be32(b + 4);
    uint32_t off_struct  = fdt_be32(b + 8);
    uint32_t off_strings = fdt_be32(b + 12);
    uint32_t version     = fdt_be32(b + 20);
    uint32_t size_struct = fdt_be32(b + 36);

    if (version < 16 || off_struct >= totalsize || off_strings >= totalsize ||
        off_struct + size_struct > totalsize)
        return -1;

    blob = b;
    dt_struct = b + off_struct;
    dt_strings = (const char *)(b + off_strings);
    struct_size = size_struct;
    total_size = totalsize;
    return 0;
}

int fdt_valid(void)             { return blob != 0; }
const void *fdt_blob(void)      { return blob; }
uint32_t fdt_total_size(void)   { return blob ? total_size : 0; }

/* Offset of the first property token after a BEGIN_NODE token. */
static uint32_t node_props_start(int node_off)
{
    const char *name = (const char *)(dt_struct + node_off + 4);
    return align4((uint32_t)node_off + 4 + (uint32_t)strlen(name) + 1);
}

const void *fdt_getprop(const struct fdt_node *node, const char *pname, int *len)
{
    if (!blob)
        return 0;

    uint32_t off = node_props_start(node->off);
    while (off < struct_size) {
        uint32_t t = token(off);
        if (t == FDT_NOP) {
            off += 4;
            continue;
        }
        if (t != FDT_PROP)
            break;              /* child node or end of this node */
        uint32_t plen = token(off + 4);
        uint32_t nameoff = token(off + 8);
        if (strcmp(dt_strings + nameoff, pname) == 0) {
            if (len)
                *len = (int)plen;
            return dt_struct + off + 12;
        }
        off = align4(off + 12 + plen);
    }
    return 0;
}

const char *fdt_getprop_str(const struct fdt_node *node, const char *pname)
{
    int len;
    const char *s = fdt_getprop(node, pname, &len);
    return (s && len > 0 && s[len - 1] == '\0') ? s : 0;
}

int fdt_getprop_u32(const struct fdt_node *node, const char *pname, uint32_t *out)
{
    int len;
    const void *p = fdt_getprop(node, pname, &len);
    if (!p || len < 4)
        return -1;
    *out = fdt_be32(p);
    return 0;
}

int fdt_is_compatible(const struct fdt_node *node, const char *compat)
{
    int len;
    const char *s = fdt_getprop(node, "compatible", &len);
    if (!s)
        return 0;
    /* "compatible" is a list of NUL-separated strings */
    for (const char *end = s + len; s < end; s += strlen(s) + 1)
        if (strcmp(s, compat) == 0)
            return 1;
    return 0;
}

int fdt_walk(fdt_walk_fn fn, void *ctx)
{
    /* cells[d] = #address-cells/#size-cells that children of depth d use */
    int ac[MAX_DEPTH + 1], sc[MAX_DEPTH + 1];
    int depth = -1;
    uint32_t off = 0;

    if (!blob)
        return -1;

    while (off < struct_size) {
        uint32_t t = token(off);
        switch (t) {
        case FDT_BEGIN_NODE: {
            depth++;
            if (depth > MAX_DEPTH)
                return -1;

            struct fdt_node n;
            n.off = (int)off;
            n.name = (const char *)(dt_struct + off + 4);
            n.depth = depth;
            /* Spec defaults when a parent omits the properties: 2 and 1 */
            n.parent_ac = depth ? ac[depth - 1] : 2;
            n.parent_sc = depth ? sc[depth - 1] : 1;

            uint32_t v;
            ac[depth] = fdt_getprop_u32(&n, "#address-cells", &v) == 0 ? (int)v : 2;
            sc[depth] = fdt_getprop_u32(&n, "#size-cells", &v) == 0 ? (int)v : 1;

            int r = fn(&n, ctx);
            if (r)
                return r;
            off = node_props_start(n.off);
            break;
        }
        case FDT_END_NODE:
            depth--;
            off += 4;
            break;
        case FDT_PROP:
            off = align4(off + 12 + token(off + 4));
            break;
        case FDT_NOP:
            off += 4;
            break;
        case FDT_END:
            return 0;
        default:
            return -1;          /* corrupt blob */
        }
    }
    return -1;
}

struct find_ctx {
    const char      *compat;
    uint32_t         phandle;
    struct fdt_node *out;
};

static int find_compat_cb(const struct fdt_node *n, void *p)
{
    struct find_ctx *c = p;
    if (fdt_is_compatible(n, c->compat)) {
        *c->out = *n;
        return 1;
    }
    return 0;
}

int fdt_find_compatible(const char *compat, struct fdt_node *out)
{
    struct find_ctx c = { compat, 0, out };
    return fdt_walk(find_compat_cb, &c) == 1 ? 0 : -1;
}

static int find_phandle_cb(const struct fdt_node *n, void *p)
{
    struct find_ctx *c = p;
    uint32_t ph;
    if (fdt_getprop_u32(n, "phandle", &ph) == 0 && ph == c->phandle) {
        *c->out = *n;
        return 1;
    }
    return 0;
}

int fdt_find_phandle(uint32_t phandle, struct fdt_node *out)
{
    struct find_ctx c = { 0, phandle, out };
    return fdt_walk(find_phandle_cb, &c) == 1 ? 0 : -1;
}

int fdt_get_reg(const struct fdt_node *node, int index, uint64_t *addr, uint64_t *size)
{
    int len;
    const uint8_t *p = fdt_getprop(node, "reg", &len);
    int ac = node->parent_ac, sc = node->parent_sc;
    int entry = 4 * (ac + sc);

    if (!p || entry == 0 || (index + 1) * entry > len || ac > 2 || sc > 2)
        return -1;
    p += index * entry;
    *addr = fdt_read_cells(p, ac);
    if (size)
        *size = sc ? fdt_read_cells(p + 4 * ac, sc) : 0;
    return 0;
}
