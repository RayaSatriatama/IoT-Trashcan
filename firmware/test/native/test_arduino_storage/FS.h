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

class String final {
 public:
  String() noexcept = default;
  explicit String(const char* value) : value_(value == nullptr ? "" : value) {}
  explicit String(std::string value) : value_(std::move(value)) {}
  std::size_t length() const noexcept { return value_.size(); }
  const char* c_str() const noexcept { return value_.c_str(); }

 private:
  std::string value_{};
};

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
  bool isDirectory() const noexcept { return directory_; }
  File openNextFile(const char* mode = FILE_READ) noexcept;
  String getNextFileName() noexcept;
  String getNextFileName(bool* is_directory) noexcept;
  const char* name() const noexcept;
  const char* path() const noexcept { return path_.c_str(); }
  bool seek(std::size_t position, SeekMode mode) noexcept;

 private:
  FS* owner_{nullptr};
  std::string path_{};
  std::size_t cursor_{0U};
  bool directory_{false};
  std::vector<std::string> directory_entries_{};
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
  void track_missing_read_path(const char* path) noexcept {
    tracked_missing_read_path_ = path == nullptr ? std::string{} : path;
    missing_read_count_ = 0U;
  }
  std::size_t missing_read_count() const noexcept { return missing_read_count_; }
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
  std::string tracked_missing_read_path_{};
  std::size_t missing_read_count_{0U};
};

inline File::File(FS* owner, std::string path, const char* mode) noexcept
    : owner_(owner), path_(std::move(path)) {
  if (owner_ == nullptr || mode == nullptr) return;
  const auto* existing = owner_->find(path_);
  directory_ = existing != nullptr && existing->directory;
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

inline const char* File::name() const noexcept {
  const std::size_t separator = path_.find_last_of('/');
  return separator == std::string::npos ? path_.c_str()
                                        : path_.c_str() + separator + 1U;
}

inline File File::openNextFile(const char* mode) noexcept {
  if (owner_ == nullptr || !directory_ || mode == nullptr) return File{};
  if (directory_entries_.empty()) {
    std::string prefix = path_;
    if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
    for (const auto& [path, entry] : owner_->entries_) {
      if (path.rfind(prefix, 0U) != 0U) continue;
      const std::string child = path.substr(prefix.size());
      if (child.empty() || child.find('/') != std::string::npos) continue;
      directory_entries_.push_back(path);
    }
    std::sort(directory_entries_.begin(), directory_entries_.end());
  }
  if (cursor_ >= directory_entries_.size()) return File{};
  return File(owner_, directory_entries_[cursor_++], mode);
}

inline String File::getNextFileName() noexcept {
  if (owner_ == nullptr || !directory_) return String{};
  if (directory_entries_.empty()) {
    std::string prefix = path_;
    if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
    for (const auto& [path, entry] : owner_->entries_) {
      if (path.rfind(prefix, 0U) != 0U) continue;
      const std::string child = path.substr(prefix.size());
      if (child.empty() || child.find('/') != std::string::npos) continue;
      directory_entries_.push_back(path);
    }
    std::sort(directory_entries_.begin(), directory_entries_.end());
  }
  if (cursor_ >= directory_entries_.size()) return String{};
  return String(directory_entries_[cursor_++]);
}

inline String File::getNextFileName(bool* is_directory) noexcept {
  if (is_directory != nullptr) *is_directory = false;
  if (owner_ == nullptr || !directory_) return String{};
  const std::size_t before = cursor_;
  const String result = getNextFileName();
  if (is_directory != nullptr && before < directory_entries_.size()) {
    const auto* entry = owner_->find(directory_entries_[before]);
    *is_directory = entry != nullptr && entry->directory;
  }
  return result;
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
  if (!write_mode && find(key) == nullptr) {
    if (!tracked_missing_read_path_.empty() &&
        tracked_missing_read_path_ == key) {
      ++missing_read_count_;
    }
    return File{};
  }
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
