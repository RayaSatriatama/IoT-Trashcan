#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum SeekMode : uint8_t { SeekSet = 0U, SeekCur = 1U, SeekEnd = 2U };

inline constexpr const char* FILE_READ = "r";
inline constexpr const char* FILE_WRITE = "w";
inline constexpr const char* FILE_APPEND = "a";

namespace fs {

class FS;

class File final {
 public:
  File() noexcept = default;
  File(FS* owner, std::string path, const char* mode) noexcept;

  explicit operator bool() const noexcept { return owner_ != nullptr; }
  std::size_t size() const noexcept;
  int available() const noexcept;
  int read(uint8_t* destination, std::size_t length) noexcept;
  std::size_t write(const uint8_t* source, std::size_t length) noexcept;
  bool flush() noexcept;
  void close() noexcept { owner_ = nullptr; }
  bool seek(std::size_t position, SeekMode mode) noexcept;

 private:
  FS* owner_{nullptr};
  std::string path_{};
  std::size_t cursor_{0U};
};

class FS final {
 public:
  struct Faults final {
    bool fail_open{false};
    std::string fail_open_path{};
    bool fail_rename{false};
    bool fail_flush{false};
    std::size_t short_read_once{0U};
    std::size_t short_write_once{0U};
    std::size_t short_write_skip{0U};
    std::size_t short_write_after{0U};
  };

  File open(const char* path, const char* mode) noexcept;
  bool exists(const char* path) const noexcept;
  bool mkdir(const char* path) noexcept;
  bool rename(const char* source, const char* destination) noexcept;
  bool remove(const char* path) noexcept;

  void append_raw(const char* path, const uint8_t* bytes,
                  std::size_t length) noexcept;
  void clear_faults() noexcept { faults_ = Faults{}; }
  void fail_next_open() noexcept { faults_.fail_open = true; }
  void fail_next_open_for(const char* path) noexcept {
    faults_.fail_open = true;
    faults_.fail_open_path = path == nullptr ? std::string{} : path;
  }
  void fail_next_rename() noexcept { faults_.fail_rename = true; }
  void fail_next_flush() noexcept { faults_.fail_flush = true; }
  void short_read_once(std::size_t maximum) noexcept {
    faults_.short_read_once = maximum;
  }
  void short_write_once(std::size_t maximum) noexcept {
    faults_.short_write_once = maximum;
  }
  void short_write_after(std::size_t skipped_writes,
                         std::size_t maximum) noexcept {
    faults_.short_write_skip = skipped_writes;
    faults_.short_write_after = maximum;
  }
  std::vector<uint8_t> bytes(const char* path) const;

 private:
  friend class File;
  struct Entry final {
    std::vector<uint8_t> bytes{};
    bool directory{false};
  };

  Entry* find(const std::string& path) noexcept;
  const Entry* find(const std::string& path) const noexcept;
  std::unordered_map<std::string, Entry> entries_{};
  Faults faults_{};
};

inline File::File(FS* owner, std::string path, const char* mode) noexcept
    : owner_(owner), path_(std::move(path)) {
  if (owner_ == nullptr || mode == nullptr) return;
  if (std::string(mode) == FILE_WRITE) {
    auto* entry = owner_->find(path_);
    if (entry == nullptr) {
      owner_->entries_.emplace(path_, FS::Entry{});
      entry = owner_->find(path_);
    }
    if (entry != nullptr) entry->bytes.clear();
    cursor_ = 0U;
  } else if (std::string(mode) == FILE_APPEND) {
    const auto* entry = owner_->find(path_);
    cursor_ = entry == nullptr ? 0U : entry->bytes.size();
  }
}

inline FS::Entry* FS::find(const std::string& path) noexcept {
  const auto iterator = entries_.find(path);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

inline const FS::Entry* FS::find(const std::string& path) const noexcept {
  const auto iterator = entries_.find(path);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

inline File FS::open(const char* path, const char* mode) noexcept {
  const bool path_matches = faults_.fail_open_path.empty() ||
                            (path != nullptr && faults_.fail_open_path == path);
  if (path == nullptr || mode == nullptr ||
      (faults_.fail_open && path_matches)) {
    faults_.fail_open = false;
    faults_.fail_open_path.clear();
    return File{};
  }
  const std::string key(path);
  const bool write_mode = std::string(mode) != FILE_READ;
  if (!write_mode && find(key) == nullptr) return File{};
  if (write_mode && find(key) == nullptr) entries_.emplace(key, Entry{});
  return File(this, key, mode);
}

inline bool FS::exists(const char* path) const noexcept {
  return path != nullptr && find(path) != nullptr;
}

inline bool FS::mkdir(const char* path) noexcept {
  if (path == nullptr) return false;
  entries_[path].directory = true;
  return true;
}

inline bool FS::rename(const char* source, const char* destination) noexcept {
  if (source == nullptr || destination == nullptr || faults_.fail_rename) {
    faults_.fail_rename = false;
    return false;
  }
  const auto iterator = entries_.find(source);
  if (iterator == entries_.end()) return false;
  entries_[destination] = std::move(iterator->second);
  entries_.erase(iterator);
  return true;
}

inline bool FS::remove(const char* path) noexcept {
  return path != nullptr && entries_.erase(path) != 0U;
}

inline void FS::append_raw(const char* path, const uint8_t* bytes,
                           std::size_t length) noexcept {
  if (path == nullptr || bytes == nullptr) return;
  auto& entry = entries_[path];
  entry.bytes.insert(entry.bytes.end(), bytes, bytes + length);
}

inline std::vector<uint8_t> FS::bytes(const char* path) const {
  const auto* entry = path == nullptr ? nullptr : find(path);
  return entry == nullptr ? std::vector<uint8_t>{} : entry->bytes;
}

inline std::size_t File::size() const noexcept {
  const auto* entry = owner_ == nullptr ? nullptr : owner_->find(path_);
  return entry == nullptr ? 0U : entry->bytes.size();
}

inline int File::available() const noexcept {
  return static_cast<int>(cursor_ < size() ? size() - cursor_ : 0U);
}

inline int File::read(uint8_t* destination, std::size_t length) noexcept {
  if (owner_ == nullptr || destination == nullptr) return 0;
  auto* entry = owner_->find(path_);
  if (entry == nullptr || cursor_ >= entry->bytes.size()) return 0;
  std::size_t actual = std::min(length, entry->bytes.size() - cursor_);
  if (owner_->faults_.short_read_once != 0U) {
    actual = std::min(actual, owner_->faults_.short_read_once);
    owner_->faults_.short_read_once = 0U;
  }
  std::copy_n(entry->bytes.data() + cursor_, actual, destination);
  cursor_ += actual;
  return static_cast<int>(actual);
}

inline std::size_t File::write(const uint8_t* source,
                               std::size_t length) noexcept {
  if (owner_ == nullptr || source == nullptr) return 0U;
  auto* entry = owner_->find(path_);
  if (entry == nullptr) return 0U;
  std::size_t actual = length;
  if (owner_->faults_.short_write_skip != 0U) {
    --owner_->faults_.short_write_skip;
  } else if (owner_->faults_.short_write_once != 0U) {
    actual = std::min(actual, owner_->faults_.short_write_once);
    owner_->faults_.short_write_once = 0U;
  } else if (owner_->faults_.short_write_after != 0U) {
    actual = std::min(actual, owner_->faults_.short_write_after);
    owner_->faults_.short_write_after = 0U;
  }
  if (cursor_ + actual > entry->bytes.size()) entry->bytes.resize(cursor_ + actual);
  std::copy_n(source, actual, entry->bytes.data() + cursor_);
  cursor_ += actual;
  return actual;
}

inline bool File::flush() noexcept {
  if (owner_ == nullptr) return false;
  if (owner_->faults_.fail_flush) {
    owner_->faults_.fail_flush = false;
    return false;
  }
  return true;
}

inline bool File::seek(std::size_t position, SeekMode mode) noexcept {
  if (owner_ == nullptr) return false;
  std::size_t next = position;
  if (mode == SeekCur) next = cursor_ + position;
  if (mode == SeekEnd) next = size() + position;
  if (next > size()) return false;
  cursor_ = next;
  return true;
}

}  // namespace fs

using fs::File;
