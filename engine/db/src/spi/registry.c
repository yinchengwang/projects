/*
 * registry.c — 虚拟表注册表（v0.9）
 *
 * 设计说明见 spi/registry.h。这里只讲实现上的关键点。
 */

#include "spi/registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_NAME_MAX 64

typedef struct {
    char       name[REG_NAME_MAX];
    StorageAM *am;
} RegEntry;

struct SpiRegistry {
    RegEntry *entries;
    u32       n;
    u32       cap;
};

int spi_registry_create(SpiRegistry **out)
{
    if (!out)
        return E_INVAL;

    SpiRegistry *r = calloc(1, sizeof(*r));
    if (!r)
        return E_IO;

    *out = r;
    return OK;
}

void spi_registry_destroy(SpiRegistry *r)
{
    if (!r)
        return;
    free(r->entries);
    free(r);
}

int spi_registry_add(SpiRegistry *r, const char *name, StorageAM *am)
{
    if (!r || !name || !am || !name[0])
        return E_INVAL;

    /* 重名直接报错 —— 静默覆盖会让人查不到自己刚注册的那张表 */
    if (spi_registry_find(r, name))
        return E_INVAL;

    if (r->n >= r->cap) {
        u32 ncap = r->cap ? r->cap * 2 : 8;
        RegEntry *ne = realloc(r->entries, (size_t)ncap * sizeof(RegEntry));
        if (!ne)
            return E_IO;
        r->entries = ne;
        r->cap = ncap;
    }

    RegEntry *e = &r->entries[r->n];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->am = am;
    r->n++;
    return OK;
}

StorageAM *spi_registry_find(const SpiRegistry *r, const char *name)
{
    if (!r || !name)
        return NULL;

    for (u32 i = 0; i < r->n; i++) {
        if (strcmp(r->entries[i].name, name) == 0)
            return r->entries[i].am;
    }
    return NULL;
}

u32 spi_registry_count(const SpiRegistry *r)
{
    return r ? r->n : 0;
}
