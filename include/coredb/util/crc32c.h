#pragma once

#include <cstddef>
#include <cstdint>

namespace coredb::util {

// CRC-32C (Castagnoli), software table implementation (RFC 3720 polynomial
// 0x1EDC6F41 reflected = 0x82F63B78). Used for both segment column
// checksums and WAL record checksums so a single implementation is the one
// thing to audit for correctness.
//
// This is intentionally a plain table-driven implementation rather than the
// hardware CRC32C instruction (SSE4.2 on x86, CRC32 extension on arm64): it
// is a few times slower but architecture-portable and easy to verify against
// a reference vector, which matters more here than raw checksum speed.
uint32_t Crc32c(const void* data, size_t length);

// Incremental variant: fold `data` into a checksum already in progress.
// Crc32c(data, len) == Crc32cExtend(0, data, len).
uint32_t Crc32cExtend(uint32_t partial_crc, const void* data, size_t length);

}  // namespace coredb::util
