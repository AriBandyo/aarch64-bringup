/*
 * Read-only flattened device tree (DTB) parser.
 *
 * Enough to discover hardware at boot: walk nodes, read properties, match
 * "compatible" strings, follow phandles and decode "reg" using the parent's
 * #address-cells / #size-cells.
 */
#ifndef FDT_H
#define FDT_H

#include <stdint.h>

struct fdt_node {
    int         off;            /* offset of FDT_BEGIN_NODE in struct block */
    const char *name;           /* unit name, e.g. "pl011@9000000" */
    int         depth;          /* root = 0 */
    int         parent_ac;      /* parent's #address-cells */
    int         parent_sc;      /* parent's #size-cells */
};

/* Return non-zero from the callback to stop the walk (that value is returned). */
typedef int (*fdt_walk_fn)(const struct fdt_node *node, void *ctx);

int  fdt_init(const void *blob);        /* 0 if a valid DTB is at blob */
int  fdt_valid(void);
const void *fdt_blob(void);
uint32_t fdt_total_size(void);

int  fdt_walk(fdt_walk_fn fn, void *ctx);

const void *fdt_getprop(const struct fdt_node *node, const char *name, int *len);
const char *fdt_getprop_str(const struct fdt_node *node, const char *name);
int  fdt_getprop_u32(const struct fdt_node *node, const char *name, uint32_t *out);

/* True if the node's "compatible" string list contains compat. */
int  fdt_is_compatible(const struct fdt_node *node, const char *compat);

/* First node matching compat (searching in tree order). 0 if found. */
int  fdt_find_compatible(const char *compat, struct fdt_node *out);
int  fdt_find_phandle(uint32_t phandle, struct fdt_node *out);

/* Decode reg entry `index` using the parent's cell sizes. 0 on success. */
int  fdt_get_reg(const struct fdt_node *node, int index, uint64_t *addr, uint64_t *size);

/* Big-endian cell readers (DTB data is big-endian, possibly unaligned). */
uint32_t fdt_be32(const void *p);
uint64_t fdt_read_cells(const void *p, int cells);

#endif
