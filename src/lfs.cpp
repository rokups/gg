// Git LFS clean and smudge filters, so paths marked `filter=lfs` compare,
// commit and check out as Git with git-lfs installed does. Objects live in
// the repository's local LFS store; downloading missing objects is left to
// git-lfs.
#include "repository.hpp"

#include <git2/errors.h>
#include <git2/sys/errors.h>
#include <git2/sys/filter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace gg::detail {
namespace {

constexpr std::string_view kPointerVersion =
    "version https://git-lfs.github.com/spec/v1";
// git-lfs never treats larger content as a pointer.
constexpr std::size_t kMaxPointerSize = 1024;

class Sha256 {
 public:
  void update(const unsigned char* data, std::size_t size) {
    total_ += size;
    while (size > 0) {
      const std::size_t take = std::min(size, block_.size() - used_);
      std::memcpy(block_.data() + used_, data, take);
      used_ += take;
      data += take;
      size -= take;
      if (used_ == block_.size()) {
        compress(block_.data());
        used_ = 0;
      }
    }
  }

  std::string hex() {
    const std::uint64_t bits = total_ * 8;
    const unsigned char pad = 0x80;
    update(&pad, 1);
    const unsigned char zero = 0;
    while (used_ != 56) update(&zero, 1);
    std::array<unsigned char, 8> length{};
    for (int index = 0; index < 8; ++index) {
      length[static_cast<std::size_t>(index)] =
          static_cast<unsigned char>(bits >> (56 - index * 8));
    }
    update(length.data(), length.size());
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    for (const std::uint32_t word : state_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        result.push_back(kDigits[(word >> shift) & 0xf]);
      }
    }
    return result;
  }

 private:
  static std::uint32_t rotate(std::uint32_t value, int count) {
    return (value >> count) | (value << (32 - count));
  }

  void compress(const unsigned char* chunk) {
    static constexpr std::array<std::uint32_t, 64> kRounds{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] = (std::uint32_t{chunk[index * 4]} << 24) |
                     (std::uint32_t{chunk[index * 4 + 1]} << 16) |
                     (std::uint32_t{chunk[index * 4 + 2]} << 8) |
                     std::uint32_t{chunk[index * 4 + 3]};
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const std::uint32_t low = rotate(words[index - 15], 7) ^
                                rotate(words[index - 15], 18) ^
                                (words[index - 15] >> 3);
      const std::uint32_t high = rotate(words[index - 2], 17) ^
                                 rotate(words[index - 2], 19) ^
                                 (words[index - 2] >> 10);
      words[index] = words[index - 16] + low + words[index - 7] + high;
    }
    std::array<std::uint32_t, 8> value = state_;
    for (std::size_t index = 0; index < 64; ++index) {
      const std::uint32_t sum1 =
          rotate(value[4], 6) ^ rotate(value[4], 11) ^ rotate(value[4], 25);
      const std::uint32_t choice =
          (value[4] & value[5]) ^ (~value[4] & value[6]);
      const std::uint32_t first =
          value[7] + sum1 + choice + kRounds[index] + words[index];
      const std::uint32_t sum0 =
          rotate(value[0], 2) ^ rotate(value[0], 13) ^ rotate(value[0], 22);
      const std::uint32_t majority =
          (value[0] & value[1]) ^ (value[0] & value[2]) ^ (value[1] & value[2]);
      const std::uint32_t second = sum0 + majority;
      value[7] = value[6];
      value[6] = value[5];
      value[5] = value[4];
      value[4] = value[3] + first;
      value[3] = value[2];
      value[2] = value[1];
      value[1] = value[0];
      value[0] = first + second;
    }
    for (std::size_t index = 0; index < 8; ++index) state_[index] += value[index];
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                      0xa54ff53a, 0x510e527f, 0x9b05688c,
                                      0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t total_ = 0;
};

struct Pointer {
  std::string oid;
  std::uint64_t size = 0;
};

std::string format_pointer(const Pointer& pointer) {
  return std::string(kPointerVersion) + "\noid sha256:" + pointer.oid +
         "\nsize " + std::to_string(pointer.size) + "\n";
}

std::optional<Pointer> parse_pointer(std::string_view text) {
  if (text.size() > kMaxPointerSize || !text.starts_with(kPointerVersion) ||
      text.size() == kPointerVersion.size() ||
      text[kPointerVersion.size()] != '\n') {
    return std::nullopt;
  }
  Pointer pointer;
  bool has_size = false;
  std::size_t position = kPointerVersion.size() + 1;
  while (position < text.size()) {
    const std::size_t end = text.find('\n', position);
    if (end == std::string_view::npos) return std::nullopt;
    const std::string_view line = text.substr(position, end - position);
    position = end + 1;
    if (line.starts_with("oid sha256:")) {
      pointer.oid = std::string(line.substr(11));
      if (pointer.oid.size() != 64 ||
          pointer.oid.find_first_not_of("0123456789abcdef") !=
              std::string::npos) {
        return std::nullopt;
      }
    } else if (line.starts_with("size ")) {
      const std::string_view digits = line.substr(5);
      const auto [end_digits, error] = std::from_chars(
          digits.data(), digits.data() + digits.size(), pointer.size);
      if (error != std::errc{} || end_digits != digits.data() + digits.size()) {
        return std::nullopt;
      }
      has_size = true;
    } else if (!line.starts_with("ext-")) {
      return std::nullopt;
    }
  }
  if (pointer.oid.empty() || !has_size) return std::nullopt;
  return pointer;
}

std::filesystem::path lfs_directory(const git_filter_source* source) {
  return std::filesystem::path(
             git_repository_commondir(git_filter_source_repo(source))) /
         "lfs";
}

std::filesystem::path object_path(const std::filesystem::path& lfs,
                                  const std::string& oid) {
  return lfs / "objects" / oid.substr(0, 2) / oid.substr(2, 2) / oid;
}

bool has_object(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         std::filesystem::file_size(path, error) == size && !error;
}

int filter_error(const char* message) {
  git_error_set_str(GIT_ERROR_FILTER, message);
  return -1;
}

// Content arrives in chunks. Small content is held in case it already is a
// pointer; everything is hashed and copied to a temporary file so the object
// can be stored without reading the file twice.
struct CleanStream {
  git_writestream base{};
  git_writestream* next = nullptr;
  std::filesystem::path lfs;
  std::string prefix;
  Sha256 hash;
  std::uint64_t size = 0;
  std::filesystem::path temporary;
  std::ofstream spill;

  int write(const char* data, std::size_t length) {
    hash.update(reinterpret_cast<const unsigned char*>(data), length);
    size += length;
    if (prefix.size() <= kMaxPointerSize) prefix.append(data, length);
    if (!spill.is_open()) {
      std::error_code error;
      std::filesystem::create_directories(lfs / "tmp", error);
      static std::atomic<std::uint64_t> counter{0};
      temporary = lfs / "tmp" /
                  ("gg-clean-" + std::to_string(std::random_device{}()) + "-" +
                   std::to_string(++counter));
      spill.open(temporary, std::ios::binary | std::ios::trunc);
      if (!spill) return filter_error("create temporary Git LFS object");
      // Anything held before the file opened is the prefix itself.
      spill.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    } else {
      spill.write(data, static_cast<std::streamsize>(length));
    }
    return spill ? 0 : filter_error("write temporary Git LFS object");
  }

  void discard() {
    if (spill.is_open()) spill.close();
    if (!temporary.empty()) {
      std::error_code error;
      std::filesystem::remove(temporary, error);
      temporary.clear();
    }
  }

  int close() {
    if (spill.is_open()) spill.close();
    std::string output;
    if (size == 0) {
      // git-lfs keeps empty files empty.
    } else if (size <= kMaxPointerSize && parse_pointer(prefix).has_value()) {
      output = prefix;  // Already a pointer, such as an unfetched object.
    } else {
      const Pointer pointer{hash.hex(), size};
      const std::filesystem::path target = object_path(lfs, pointer.oid);
      if (!has_object(target, pointer.size)) {
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        std::filesystem::rename(temporary, target, error);
        if (error) {
          discard();
          return filter_error("store Git LFS object");
        }
        temporary.clear();
      }
      output = format_pointer(pointer);
    }
    discard();
    const int result = next->write(next, output.data(), output.size());
    return result < 0 ? result : next->close(next);
  }
};

// A pointer arrives from the object database and is replaced by the stored
// content when the local store has it; otherwise the pointer is kept, as
// git-lfs does when it cannot download.
struct SmudgeStream {
  git_writestream base{};
  git_writestream* next = nullptr;
  std::filesystem::path lfs;
  std::string content;
  bool passthrough = false;

  int write(const char* data, std::size_t length) {
    if (passthrough) return next->write(next, data, length);
    content.append(data, length);
    if (content.size() > kMaxPointerSize) {
      passthrough = true;
      const int result = next->write(next, content.data(), content.size());
      content.clear();
      return result;
    }
    return 0;
  }

  int close() {
    if (!passthrough) {
      const std::optional<Pointer> pointer = parse_pointer(content);
      const std::filesystem::path stored =
          pointer.has_value() ? object_path(lfs, pointer->oid)
                              : std::filesystem::path();
      if (pointer.has_value() && has_object(stored, pointer->size)) {
        std::ifstream input(stored, std::ios::binary);
        std::array<char, 64 * 1024> buffer{};
        while (input) {
          input.read(buffer.data(), buffer.size());
          const std::streamsize read = input.gcount();
          if (read <= 0) break;
          const int result =
              next->write(next, buffer.data(), static_cast<std::size_t>(read));
          if (result < 0) return result;
        }
        if (input.bad()) return filter_error("read Git LFS object");
      } else {
        const int result = next->write(next, content.data(), content.size());
        if (result < 0) return result;
      }
    }
    return next->close(next);
  }
};

template <typename Stream>
int stream_write(git_writestream* stream, const char* data, std::size_t length) {
  return reinterpret_cast<Stream*>(stream)->write(data, length);
}

template <typename Stream>
int stream_close(git_writestream* stream) {
  return reinterpret_cast<Stream*>(stream)->close();
}

void clean_free(git_writestream* stream) {
  auto* clean = reinterpret_cast<CleanStream*>(stream);
  clean->discard();
  delete clean;
}

void smudge_free(git_writestream* stream) {
  delete reinterpret_cast<SmudgeStream*>(stream);
}

int lfs_stream(git_writestream** out, git_filter*, void**,
               const git_filter_source* source, git_writestream* next) {
  if (git_filter_source_mode(source) == GIT_FILTER_CLEAN) {
    auto* stream = new CleanStream();
    stream->base = {stream_write<CleanStream>, stream_close<CleanStream>,
                    clean_free};
    stream->next = next;
    stream->lfs = lfs_directory(source);
    *out = &stream->base;
  } else {
    auto* stream = new SmudgeStream();
    stream->base = {stream_write<SmudgeStream>, stream_close<SmudgeStream>,
                    smudge_free};
    stream->next = next;
    stream->lfs = lfs_directory(source);
    *out = &stream->base;
  }
  return 0;
}

git_filter make_lfs_filter() {
  git_filter filter{};
  git_filter_init(&filter, GIT_FILTER_VERSION);
  filter.attributes = "filter=lfs";
  filter.stream = lfs_stream;
  return filter;
}

}  // namespace

std::string lfs_pointer_for_test(std::string_view content) {
  Sha256 hash;
  hash.update(reinterpret_cast<const unsigned char*>(content.data()),
              content.size());
  return format_pointer({hash.hex(), content.size()});
}

int register_lfs_filter() {
  // After the clean filters it follows on checkout, and before them when
  // committing, as Git orders a driver filter around CRLF and ident.
  static git_filter filter = make_lfs_filter();
  const int result = git_filter_register("lfs", &filter, 200);
  if (result == GIT_EEXISTS) {
    git_error_clear();
    return 0;
  }
  return result;
}

}  // namespace gg::detail
