#pragma once
#include "test_platform.h"
#include "Arduino.h"

inline constexpr const char* FILE_READ = "r";
inline constexpr const char* FILE_WRITE = "w";
inline constexpr const char* FILE_APPEND = "a";
enum SeekMode { SeekSet, SeekCur, SeekEnd };
namespace test_platform {
struct Entry { std::vector<uint8_t> data; bool directory{false}; };
inline std::map<std::string, std::shared_ptr<Entry>> entries;
inline std::size_t fs_mutations{0};
}
namespace fs {
class File {
 public:
  File() = default;
  File(std::shared_ptr<test_platform::Entry> entry, std::string path, bool append)
      : entry_(std::move(entry)), path_(std::move(path)),
        at_(append ? entry_->data.size() : 0) {}
  explicit operator bool() const { return bool(entry_); }
  bool isDirectory() const { return entry_ && entry_->directory; }
  size_t size() const { ++test_platform::fs_calls; return entry_ ? entry_->data.size() : 0; }
  size_t position() const { return at_; }
  bool seek(uint32_t offset, SeekMode mode = SeekSet) {
    ++test_platform::fs_calls;
    at_ = (mode == SeekCur ? at_ : mode == SeekEnd ? size() : 0) + offset;
    return true;
  }
  size_t write(const uint8_t* data, size_t size) {
    ++test_platform::fs_calls; ++test_platform::fs_mutations;
    if (!entry_) return 0;
    entry_->data.resize(std::max(entry_->data.size(), at_ + size));
    std::memcpy(entry_->data.data() + at_, data, size);
    at_ += size;
    return size;
  }
  size_t read(uint8_t* data, size_t size) {
    ++test_platform::fs_calls;
    if (!entry_) return 0;
    size = std::min(size, entry_->data.size() - std::min(at_, entry_->data.size()));
    if (size) std::memcpy(data, entry_->data.data() + at_, size);
    at_ += size;
    return size;
  }
  int read() { uint8_t byte; return read(&byte, 1) ? byte : -1; }
  bool truncate(size_t size) {
    ++test_platform::fs_calls; ++test_platform::fs_mutations;
    if (!entry_) return false;
    entry_->data.resize(size);
    return true;
  }
  void flush() { ++test_platform::fs_calls; }
  void close() { entry_.reset(); }
  int available() { return size() > at_ ? size() - at_ : 0; }
  const char* name() const { return path_.c_str(); }
  const char* path() const { return path_.c_str(); }
  String getNextFileName(bool* directory = nullptr) {
    if (directory) *directory = false;
    if (!isDirectory()) return String("");
    const auto prefix = path_ + (path_.back() == '/' ? "" : "/");
    size_t index = 0;
    for (const auto& item : test_platform::entries) {
      if (item.first.rfind(prefix, 0) != 0) continue;
      const auto child = item.first.substr(prefix.size());
      if (child.empty() || child.find('/') != std::string::npos) continue;
      if (index++ != at_) continue;
      ++at_;
      if (directory) *directory = item.second->directory;
      return String(item.first.c_str());
    }
    return String("");
  }
  File openNextFile() {
    ++test_platform::fs_calls;
    auto name = getNextFileName();
    const auto found = test_platform::entries.find(name.c_str());
    return found == test_platform::entries.end() ? File{} : File(found->second, found->first, false);
  }
 private:
  std::shared_ptr<test_platform::Entry> entry_;
  std::string path_;
  size_t at_{0};
};
class FS {
 public:
  bool exists(const char* path) { ++test_platform::fs_calls; return test_platform::entries.count(path); }
  bool mkdir(const char* path) {
    ++test_platform::fs_calls; ++test_platform::fs_mutations;
    auto entry = std::make_shared<test_platform::Entry>();
    entry->directory = true;
    test_platform::entries[path] = entry;
    return true;
  }
  File open(const char* path, const char* mode = FILE_READ, bool = false) {
    ++test_platform::fs_calls;
    auto& entries = test_platform::entries;
    auto found = entries.find(path);
    if (mode[0] != 'r') ++test_platform::fs_mutations;
    if (found == entries.end()) {
      if (mode[0] == 'r') return {};
      found = entries.emplace(path, std::make_shared<test_platform::Entry>()).first;
    }
    if (mode[0] == 'w') found->second->data.clear();
    return File(found->second, path, mode[0] == 'a');
  }
  bool remove(const char* path) {
    ++test_platform::fs_calls; ++test_platform::fs_mutations;
    return test_platform::entries.erase(path) > 0;
  }
  bool rename(const char* source, const char* destination) {
    ++test_platform::fs_calls; ++test_platform::fs_mutations;
    const auto found = test_platform::entries.find(source);
    if (found == test_platform::entries.end()) return false;
    test_platform::entries[destination] = found->second;
    test_platform::entries.erase(found);
    return true;
  }
};
}
using fs::File;
