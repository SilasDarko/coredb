#include "coredb/wal/wal_reader.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "coredb/util/crc32c.h"

namespace coredb::wal {

WalReadResult ReadAll(const std::string& path) {
  WalReadResult result;
  std::error_code size_ec;
  const uint64_t file_size = std::filesystem::file_size(path, size_ec);
  std::ifstream in(path, std::ios::binary);
  if (!in) return result;  // no WAL file yet is not an error: nothing to recover

  while (true) {
    const auto record_start = static_cast<uint64_t>(in.tellg());
    uint32_t total_len = 0;
    in.read(reinterpret_cast<char*>(&total_len), sizeof(total_len));
    if (in.gcount() == 0) {
      break;  // clean end of file, exactly on a record boundary
    }
    if (static_cast<size_t>(in.gcount()) < sizeof(total_len)) {
      result.truncated_tail = true;  // fewer than 4 bytes of a length header
      break;
    }

    // A corrupted or malicious length header can claim an enormous record
    // size; check it against the file's actual remaining bytes *before*
    // allocating, rather than after a multi-gigabyte allocation attempt.
    if (!size_ec && record_start + sizeof(total_len) + total_len > file_size) {
      result.truncated_tail = true;
      break;
    }

    std::vector<uint8_t> payload(total_len);
    in.read(reinterpret_cast<char*>(payload.data()), total_len);
    if (static_cast<uint32_t>(in.gcount()) < total_len) {
      result.truncated_tail = true;  // declared length runs past EOF
      break;
    }

    if (total_len < sizeof(uint32_t)) {
      result.corrupt_record = true;  // too short to even hold a checksum
      break;
    }
    uint32_t stored_checksum = 0;
    std::memcpy(&stored_checksum, payload.data(), sizeof(stored_checksum));
    const uint8_t* body_ptr = payload.data() + sizeof(stored_checksum);
    const size_t body_len = payload.size() - sizeof(stored_checksum);
    const uint32_t actual_checksum = util::Crc32c(body_ptr, body_len);
    if (actual_checksum != stored_checksum) {
      result.corrupt_record = true;
      break;
    }

    std::vector<uint8_t> body(body_ptr, body_ptr + body_len);
    WalRecord record;
    if (!DecodeBody(body, &record)) {
      result.corrupt_record = true;  // checksum matched but the body itself doesn't parse
      break;
    }

    result.last_valid_lsn = record.lsn;
    result.bytes_read += sizeof(total_len) + total_len;
    result.records.push_back(std::move(record));
  }

  return result;
}

}  // namespace coredb::wal
