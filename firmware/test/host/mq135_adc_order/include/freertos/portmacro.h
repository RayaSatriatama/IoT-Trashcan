#pragma once

// Host stub of the FreeRTOS critical-section surface used by the target HX711
// adapter. No real locking is needed: the ordering test drives one reader.

#include <cstdint>

typedef struct {
  int unused;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}

inline void portENTER_CRITICAL_ISR(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE*) {}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
