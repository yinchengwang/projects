#ifndef DB_COMMON_CRC_H
#define DB_COMMON_CRC_H

#include "types.h"

/* crc32 (IEEE 802.3, poly 0xEDB88320)，用于 WAL 记录 */
u32 crc32(const void *buf, size_t n);

/*
 * crc32 的分段版本。
 *
 * 种子约定：初始传 0xFFFFFFFFu，最后取反 —— 和 crc32 一致。
 *   crc32(a) == ~crc32_update(0xFFFFFFFFu, a)
 * 但 crc32(a)+crc32(b) != crc32(a+b)，要算拼接必须用分段版。
 */
u32 crc32_update(u32 crc, const void *buf, size_t n);

/* crc16 (CCITT)，用于页校验和。
 * crc16_update 支持分段累加，初始种子传 0xFFFF：
 *   u16 c = 0xFFFFu;
 *   c = crc16_update(c, a, n1);
 *   c = crc16_update(c, b, n2);   // 等价于对 a+b 拼接后一次算
 * 页校验和需要跳过 H_CKSUM 字段，分段算比拷 4KB 便宜。
 */
u16 crc16(const void *buf, size_t n);
u16 crc16_update(u16 crc, const void *buf, size_t n);

#endif /* DB_COMMON_CRC_H */
