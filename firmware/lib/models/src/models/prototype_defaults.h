#pragma once

#include "model_profiles.h"

namespace sigap::models {

// Builds a provisional four-input organic FIS model (fill%, weight_util%, OGI%, accumulation_hours)
// for TEST_ONLY prototype operation. Returns true on success.
bool build_prototype_organic_fis(FisModelProfile& output) noexcept;

// Builds a provisional organic temperature safety model (one required temperature channel)
// for TEST_ONLY prototype operation. Returns true on success.
bool build_prototype_organic_safety(SafetyModelProfile& output) noexcept;

}  // namespace sigap::models