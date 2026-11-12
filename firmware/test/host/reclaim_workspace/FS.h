#pragma once

// Host-only finite filesystem. It does not simulate LittleFS allocator panics.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

enum SeekMode : uint8_t { SeekSet, SeekCur, SeekEnd };
inline constexpr const char* FILE_READ = "r";
inline constexpr const char* FILE_WRITE = "w";
inline constexpr const char* FILE_APPEND = "a";

namespace fs {
class String {
 public:
  String() = default;
  explicit String(std::string value) : value_(std::move(value)) {}
  std::size_t length() const noexcept { return value_.size(); }
  const char* c_str() const noexcept { return value_.c_str(); }
 private:
  std::string value_;
};
class FS;
struct Handle {
  FS* owner;
  std::string path;
  std::string mode;
  std::size_t cursor{0};
  bool closed{false};
  std::vector<std::string> children;
  Handle(FS* fs, std::string name, std::string access);
  ~Handle();
  void close() noexcept;
};
class File {
 public:
  File() = default;
  explicit File(std::shared_ptr<Handle> handle) : handle_(std::move(handle)) {}
  explicit operator bool() const noexcept { return handle_ && !handle_->closed; }
  std::size_t size() const noexcept;
  int available() const noexcept;
  int read(uint8_t* out, std::size_t length) noexcept;
  std::size_t write(const uint8_t* bytes, std::size_t length) noexcept;
  void close() noexcept { if (handle_) handle_->close(); handle_.reset(); }
  bool flush() noexcept { return static_cast<bool>(*this); }
  bool seek(std::size_t offset, SeekMode mode) noexcept;
  bool isDirectory() const noexcept;
  File openNextFile(const char* mode = FILE_READ) noexcept;
  String getNextFileName() noexcept;
  String getNextFileName(bool* directory) noexcept;
  const char* path() const noexcept { return *this ? handle_->path.c_str() : ""; }
  const char* name() const noexcept {
    if (!*this) return "";
    const auto pos = handle_->path.find_last_of('/');
    return handle_->path.c_str() + (pos == std::string::npos ? 0 : pos + 1);
  }
 private:
  std::shared_ptr<Handle> handle_;
};
class FS {
 public:
  struct Entry { std::vector<uint8_t> bytes; bool directory{false}; };
  struct Mutations {
    std::size_t open_write{0}, truncate{0}, write{0}, remove{0}, rename{0};
    std::size_t total() const noexcept {
      return open_write + truncate + write + remove + rename;
    }
  } mutations;
  std::size_t active_handles{0}, active_reads{0};
  File open(const char* path, const char* mode) noexcept;
  bool exists(const char* path) const noexcept { return entries_.count(path) != 0; }
  bool mkdir(const char* path) noexcept { entries_[path].directory = true; return true; }
  bool remove(const char* path) noexcept { ++mutations.remove; return entries_.erase(path) != 0; }
  bool rename(const char* from, const char* to) noexcept {
    ++mutations.rename;
    if (fail_rename_) { fail_rename_ = false; return false; }
    const auto found = entries_.find(from);
    if (found == entries_.end() || entries_.count(to)) return false;
    auto value = std::move(found->second);
    entries_.erase(found);
    entries_.emplace(to, std::move(value));
    return true;
  }
  void append_raw(const char* path, const uint8_t* data, std::size_t size) {
    auto file = open(path, FILE_APPEND);
    if (file) { file.write(data, size); file.close(); }
  }
  std::vector<uint8_t> bytes(const char* path) const {
    const auto it = entries_.find(path);
    return it == entries_.end() ? std::vector<uint8_t>{} : it->second.bytes;
  }
  std::size_t used_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& item : entries_) total += item.second.bytes.size();
    return total;
  }
  uint64_t free_bytes() const noexcept {
    const auto used = used_bytes();
    return capacity_ > used ? capacity_ - used : 0;
  }
  void set_capacity(uint64_t value) noexcept { capacity_ = value; }
  void reset_mutations() noexcept { mutations = {}; }
  void short_write_once(std::size_t value) noexcept { short_write_ = value; }
  void fail_next_rename() noexcept { fail_rename_ = true; }
  void override_size(std::string path, std::size_t value) {
    size_path_ = std::move(path); size_value_ = value;
  }
  void clear_size_override() { size_path_.clear(); }
 private:
  friend class File;
  friend struct Handle;
  std::unordered_map<std::string, Entry> entries_;
  uint64_t capacity_{65536};
  std::size_t short_write_{0};
  bool fail_rename_{false};
  std::string size_path_;
  std::size_t size_value_{0};
};
inline Handle::Handle(FS* fs, std::string name, std::string access)
    : owner(fs), path(std::move(name)), mode(std::move(access)) {
  ++owner->active_handles;
  if (mode == FILE_READ) ++owner->active_reads;
}
inline Handle::~Handle() { close(); }
inline void Handle::close() noexcept {
  if (closed) return;
  closed = true;
  --owner->active_handles;
  if (mode == FILE_READ) --owner->active_reads;
}
inline File FS::open(const char* path, const char* mode) noexcept {
  const std::string key(path), access(mode);
  auto found = entries_.find(key);
  if (access == FILE_READ && found == entries_.end()) return {};
  if (access != FILE_READ) {
    ++mutations.open_write;
    if (access == FILE_WRITE) { ++mutations.truncate; entries_[key].bytes.clear(); }
    else if (found == entries_.end()) entries_.emplace(key, Entry{});
  }
  auto handle = std::make_shared<Handle>(this, key, access);
  if (access == FILE_APPEND) handle->cursor = entries_[key].bytes.size();
  if (entries_[key].directory) {
    std::string prefix = key + (key.back() == '/' ? "" : "/");
    for (const auto& item : entries_) {
      if (item.first.rfind(prefix, 0) != 0) continue;
      const auto child = item.first.substr(prefix.size());
      if (!child.empty() && child.find('/') == std::string::npos)
        handle->children.push_back(item.first);
    }
    std::sort(handle->children.begin(), handle->children.end());
  }
  return File(std::move(handle));
}
inline std::size_t File::size() const noexcept {
  if (!*this) return 0;
  const auto& fs = *handle_->owner;
  if (fs.size_path_ == handle_->path) return fs.size_value_;
  const auto it = fs.entries_.find(handle_->path);
  return it == fs.entries_.end() ? 0 : it->second.bytes.size();
}
inline int File::available() const noexcept {
  if (!*this) return 0;
  const auto& bytes = handle_->owner->entries_.at(handle_->path).bytes;
  return static_cast<int>(bytes.size() > handle_->cursor ? bytes.size() - handle_->cursor : 0);
}
inline int File::read(uint8_t* out, std::size_t length) noexcept {
  if (!*this || handle_->mode != FILE_READ) return 0;
  const auto& bytes = handle_->owner->entries_.at(handle_->path).bytes;
  const auto left = bytes.size() > handle_->cursor ? bytes.size() - handle_->cursor : 0;
  const auto count = std::min(length, left);
  std::copy_n(bytes.data() + handle_->cursor, count, out);
  handle_->cursor += count;
  return static_cast<int>(count);
}
inline std::size_t File::write(const uint8_t* data, std::size_t length) noexcept {
  if (!*this || handle_->mode == FILE_READ) return 0;
  auto& fs = *handle_->owner;
  ++fs.mutations.write;
  auto& bytes = fs.entries_.at(handle_->path).bytes;
  auto count = length;
  if (fs.short_write_) { count = std::min(count, fs.short_write_); fs.short_write_ = 0; }
  const auto overwrite = bytes.size() > handle_->cursor ? bytes.size() - handle_->cursor : 0;
  count = static_cast<std::size_t>(std::min<uint64_t>(count, overwrite + fs.free_bytes()));
  if (handle_->cursor + count > bytes.size()) bytes.resize(handle_->cursor + count);
  std::copy_n(data, count, bytes.data() + handle_->cursor);
  handle_->cursor += count;
  return count;
}
inline bool File::seek(std::size_t offset, SeekMode mode) noexcept {
  if (!*this) return false;
  const auto base = mode == SeekSet ? 0 : mode == SeekCur ? handle_->cursor : size();
  if (offset > std::numeric_limits<std::size_t>::max() - base) return false;
  if (base + offset > size()) return false;
  handle_->cursor = base + offset;
  return true;
}
inline bool File::isDirectory() const noexcept {
  return *this && handle_->owner->entries_.at(handle_->path).directory;
}
inline String File::getNextFileName() noexcept {
  if (!isDirectory() || handle_->cursor >= handle_->children.size()) return {};
  return String(handle_->children[handle_->cursor++]);
}
inline String File::getNextFileName(bool* directory) noexcept {
  auto name = getNextFileName();
  if (directory) *directory = name.length() && handle_->owner->entries_.at(name.c_str()).directory;
  return name;
}
inline File File::openNextFile(const char* mode) noexcept {
  auto name = getNextFileName();
  return name.length() ? handle_->owner->open(name.c_str(), mode) : File{};
}
}  // namespace fs
using fs::File;
