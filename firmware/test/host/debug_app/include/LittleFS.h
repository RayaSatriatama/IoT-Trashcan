#pragma once
#include "FS.h"
struct LittleFSClass:fs::FS {bool begin(bool){return true;}size_t totalBytes(){return 8*1024*1024;}size_t usedBytes(){return 0;}};inline LittleFSClass LittleFS;
