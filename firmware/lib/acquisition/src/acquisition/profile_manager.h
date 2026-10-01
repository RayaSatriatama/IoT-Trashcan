#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/profile_store.h"
#include "acquisition/target_runtime.h"

namespace sigap::acquisition {

class IProfileAuditSink {
 public:
  virtual ~IProfileAuditSink() = default;
  virtual void profile_activated(const ProfileRef& previous,
                                 const ProfileRef& current) noexcept = 0;
};

struct ActiveProfileSnapshot final {
  bool present{false};
  EncodedProfile encoded{};
  ProfileRef ref{};
};

enum class ProfileManagerState : uint8_t {
  Unloaded,
  Active,
  Draft,
  Preview,
  Error,
};

class ProfileManager final {
 public:
  explicit ProfileManager(ProfilePersistence& persistence,
                          const ProfileCodec& codec = ProfilePersistence::default_codec(),
                          IProfileAuditSink* audit = nullptr) noexcept
      : persistence_(persistence), codec_(codec), audit_(audit) {}

  bool load(ProfileTarget target) noexcept;
  bool stage_import(ProfileTarget target, const EncodedProfile& encoded) noexcept;
  bool validate(ProfileTarget target) const noexcept;
  bool preview(ProfileTarget target) noexcept;
  bool commit(ProfileTarget target) noexcept;
  bool rollback(ProfileTarget target) noexcept;
  bool reset(ProfileTarget target) noexcept;
  bool export_active(ProfileTarget target, EncodedProfile& encoded) const noexcept;
  bool snapshot(ProfileTarget target, ActiveProfileSnapshot& snapshot) const noexcept;
  ProfileManagerState state(ProfileTarget target) const noexcept;
  void bind_runtime_profiles(TargetProfileSet& profiles) noexcept;
  uint32_t generation() const noexcept { return generation_; }

 private:
  static constexpr std::size_t kTargetCount{14U};
  static std::size_t index(ProfileTarget target) noexcept;
  static ProfileRef ref_from(const EncodedProfile& encoded) noexcept;
  bool ensure_loaded(ProfileTarget target) noexcept;
  void sync_runtime_target(ProfileTarget target) noexcept;

  ProfilePersistence& persistence_;
  const ProfileCodec& codec_;
  IProfileAuditSink* audit_{nullptr};
  std::array<EncodedProfile, kTargetCount> active_{};
  std::array<EncodedProfile, kTargetCount> draft_{};
  std::array<bool, kTargetCount> active_present_{};
  std::array<bool, kTargetCount> draft_present_{};
  std::array<ProfileManagerState, kTargetCount> states_{};
  TargetProfileSet* runtime_profiles_{nullptr};
  std::array<Hx711Profile, 3> hx711_runtime_{};
  std::array<UltrasonicProfile, 3> ultrasonic_runtime_{};
  std::array<MqAdcProfile, 4> mq_runtime_{};
  Mq7HeaterProfile heater_runtime_{};
  Ds18b20Profile ds_runtime_{};
  std::array<Sht31Profile, 2> sht_runtime_{};
  uint32_t generation_{0U};
};

}  // namespace sigap::acquisition
