#ifndef RD_HOST_H
#define RD_HOST_H
/* Host-side arenas for the MiniWeb HTML engine (kernel/dom.h, css.h, layout.h, render.h).
 * The kernel gets these from memmap.h as fixed physical addresses; a host test just needs plain
 * arrays and the same macro names. Include this BEFORE the engine headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef int i32;
#define IO_H            /* stop the engine headers from pulling in the kernel's port-I/O header */

#define RD_NODE_MAX   16384
#define RD_ATTR_MAX   24576
#define RD_POOL_SIZE  0x40000
#define RD_CSS_RULE_MAX 3072
#define RD_CSS_COMP_MAX 6144
#define RD_CSS_DECL_MAX 12288
#define RD_ITEM_MAX   24576
#define RD_FRAG_MAX   1024
#define RD_SCRATCH_SZ 0x60000
static _Alignas(16) unsigned char g_rd_nodes[RD_NODE_MAX * 32];
static _Alignas(16) unsigned char g_rd_attrs[RD_ATTR_MAX * 8];
static _Alignas(16) unsigned char g_rd_pool[RD_POOL_SIZE];
static _Alignas(16) unsigned char g_rd_rules[RD_CSS_RULE_MAX * 32];
static _Alignas(16) unsigned char g_rd_comps[RD_CSS_COMP_MAX * 40];
static _Alignas(16) unsigned char g_rd_decls[RD_CSS_DECL_MAX * 12];
static _Alignas(16) unsigned char g_rd_items[RD_ITEM_MAX * 32];
static _Alignas(16) unsigned char g_rd_frags[RD_FRAG_MAX * 24];
static _Alignas(16) unsigned char g_rd_scratch[RD_SCRATCH_SZ];
#define RD_NODES ((rd_node_t *)g_rd_nodes)
#define RD_ATTRS ((rd_attr_t *)g_rd_attrs)
#define RD_POOL  ((u8 *)g_rd_pool)
#define RD_CSS_RULES ((css_rule_t *)g_rd_rules)
#define RD_CSS_COMPS ((css_comp_t *)g_rd_comps)
#define RD_CSS_DECLS ((css_decl_t *)g_rd_decls)
#define RD_ITEMS   ((rd_item_t *)g_rd_items)
#define RD_FRAGS   ((rd_frag_t *)g_rd_frags)
#define RD_SCRATCH ((u8 *)g_rd_scratch)
#endif
