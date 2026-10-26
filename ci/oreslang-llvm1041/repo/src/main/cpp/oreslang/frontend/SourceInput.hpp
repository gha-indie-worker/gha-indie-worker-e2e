#pragma once
#include <array>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <cstdint>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#endif

#if defined(__unix__) || defined(__APPLE__)
// Descriptor metadata is checked both before and after consuming bytes.
// This detects ordinary in-place replacement, truncation and extension
// during compilation. It is a best-effort stability check, not a guarantee
// of an atomic snapshot against hostile concurrent writers.
inline bool ores_source_stat_stable(const struct stat& before, const struct stat& after) {
#if defined(__APPLE__)
  const bool same_times =
      before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
      before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
      before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
      before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
  const bool same_times =
      before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
      before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
      before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
      before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
  return S_ISREG(after.st_mode) &&
      before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      before.st_size == after.st_size && same_times;
}
#endif

struct SourceIdentity {
  std::uint64_t device = 0, inode = 0;
  bool valid = false;
};

// Check the opened descriptor, so a path replacement cannot turn a regular
// source into a blocking FIFO between admission and reading. Regular symlinks
// remain supported. The byte budget also applies if a file grows during reads.
inline std::string read_regular_source(const char* path, std::size_t limit,
                                       const char* overflow, SourceIdentity* identity = nullptr) {
  std::array<char, 8192> buffer{};
  std::string text;
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error(std::string("cannot open source: ") + path);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot inspect source file");
  if (!S_ISREG(info.st_mode)) throw std::runtime_error("source must be a regular file");
  if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > limit)
    throw std::runtime_error(overflow);
  for (;;) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("failed reading source file");
    }
    if (count == 0) break;
    if (static_cast<std::size_t>(count) > limit - text.size())
      throw std::runtime_error(overflow);
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  struct stat after{};
  if (::fstat(fd, &after) != 0 || !ores_source_stat_stable(info, after) ||
      static_cast<std::uintmax_t>(after.st_size) != text.size())
    throw std::runtime_error("source changed during read");
  if (identity) *identity = {static_cast<std::uint64_t>(info.st_dev),
                             static_cast<std::uint64_t>(info.st_ino), true};
#else
  if (identity) *identity = {};
  // Portable admission; the descriptor-level nonblocking guarantee is POSIX.
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("source must be a regular file");
  std::ifstream source(path, std::ios::binary);
  if (!source) throw std::runtime_error(std::string("cannot open source: ") + path);
  while (source) {
    source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = source.gcount();
    if (count <= 0) break;
    if (static_cast<std::size_t>(count) > limit - text.size())
      throw std::runtime_error(overflow);
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (source.bad() || (source.fail() && !source.eof()))
    throw std::runtime_error("failed reading source file");
#endif
  return text;
}

// Reject malformed Unicode (overlong encodings, surrogates, truncated input and
// code points above U+10FFFF) and binary NUL before native parser admission.
inline void require_valid_source_utf8(std::string_view source) {
    for (std::size_t i = 0; i < source.size();) {
        const auto lead = static_cast<unsigned char>(source[i]);
        if (lead == 0) throw std::runtime_error("native source contains NUL byte");
        if (lead <= 0x7f) { ++i; continue; }

        std::size_t continuation_count = 0;
        unsigned char min_first = 0x80;
        unsigned char max_first = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            if (lead == 0xe0) min_first = 0xa0;
            if (lead == 0xed) max_first = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            if (lead == 0xf0) min_first = 0x90;
            if (lead == 0xf4) max_first = 0x8f;
        } else {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        if (source.size() - i <= continuation_count) {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte = static_cast<unsigned char>(source[i + j]);
            if (byte < (j == 1 ? min_first : 0x80) ||
                byte > (j == 1 ? max_first : 0xbf)) {
                throw std::runtime_error("native source is not valid UTF-8");
            }
        }
        i += continuation_count + 1;
    }
}
