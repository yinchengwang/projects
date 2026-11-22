#ifndef DB_MODALITIES_RELATIONAL_REL_OPENER_H
#define DB_MODALITIES_RELATIONAL_REL_OPENER_H

#include "spi/table_opener.h"

/*
 * 关系模态的表打开器（catalog → heap → StorageAM）。
 *
 * 定义在 `modalities/relational/rel_opener.c`。
 */
extern const TableOpener rel_table_opener;

#endif /* DB_MODALITIES_RELATIONAL_REL_OPENER_H */
