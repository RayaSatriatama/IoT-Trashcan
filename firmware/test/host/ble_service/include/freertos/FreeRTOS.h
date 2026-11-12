#pragma once
#include <cassert>
#include <mutex>
inline thread_local int host_critical_depth=0;
struct portMUX_TYPE { std::mutex mutex; };
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE* mux) { mux->mutex.lock(); ++host_critical_depth; }
inline void portEXIT_CRITICAL(portMUX_TYPE* mux) { assert(host_critical_depth==1); --host_critical_depth; mux->mutex.unlock(); }
