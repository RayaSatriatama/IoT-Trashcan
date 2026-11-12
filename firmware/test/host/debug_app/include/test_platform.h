#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace test_platform {
inline uint64_t now_us=100;
inline int64_t wall_seconds=1700000010;
inline size_t sd_begins=0;
inline size_t nvs_writes=0;
inline size_t fs_calls=0,nvs_calls=0,serial_polls=0,restarts=0;
inline size_t serial_writable=4096;
inline std::string serial_output;
inline std::string serial_input;
struct Task { void(*function)(void*); void* argument; };
inline std::vector<Task> tasks;
inline std::function<void()> on_http_end,on_tls_destroy;
inline size_t debug_starts=0,control_starts=0,archive_starts=0,http_calls=0;
inline std::string last_debug_body,last_debug_url;
inline const char* borrowed_body=nullptr;
inline size_t borrowed_size=0;
inline bool wifi_up=true;
inline void reset_io() { fs_calls=0;nvs_calls=0; }
inline void run_one_task() { assert(!tasks.empty());auto t=tasks.front();tasks.erase(tasks.begin());t.function(t.argument); }
}
