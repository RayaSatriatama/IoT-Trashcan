#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "acquisition/profile_store.h"
#include "acquisition/target_runtime.h"

namespace sigap::acquisition {

enum class AuditEventKind : uint8_t {
  ConfigCommitted,
  ProfileActivated,
  ProfileRolledBack,
  ProfileReset,
};

struct AuditEvent final {
  AuditEventKind kind{AuditEventKind::ProfileActivated};
  uint64_t monotonic_us{0U};
  ConfigRef config_ref{};
  ProfileRef previous{};
  ProfileRef current{};
  bool pending_activation{false};
};

class IProfileAuditSink {
 public:
  virtual ~IProfileAuditSink() = default;
  virtual void set_context(uint64_t, const ConfigRef&) noexcept {}
  virtual void profile_activated(const ProfileRef& previous,
                                 const ProfileRef& current) noexcept = 0;
  virtual void profile_rolled_back(const ProfileRef& previous,
                                   const ProfileRef& current) noexcept {}
  virtual void profile_reset(const ProfileRef& previous) noexcept {}
  virtual void config_committed(const ConfigRef& current) noexcept {}
};

template <std::size_t Capacity>
class ProfileAuditRing final : public IProfileAuditSink {
 public:
  void set_context(uint64_t monotonic_us,
                   const ConfigRef& config_ref) noexcept override {
    monotonic_us_ = monotonic_us;
    config_ref_ = config_ref;
  }

  void profile_activated(const ProfileRef& previous,
                         const ProfileRef& current) noexcept override {
    push(AuditEventKind::ProfileActivated, previous, current, false);
  }

  void profile_rolled_back(const ProfileRef& previous,
                           const ProfileRef& current) noexcept override {
    push(AuditEventKind::ProfileRolledBack, previous, current, false);
  }

  void profile_reset(const ProfileRef& previous) noexcept override {
    push(AuditEventKind::ProfileReset, previous, ProfileRef{}, false);
  }

  void config_committed(const ConfigRef& current) noexcept override {
    push(AuditEventKind::ConfigCommitted, ProfileRef{}, ProfileRef{}, true,
         current);
  }

  bool try_pop(AuditEvent& output) noexcept {
    if (count_ == 0U) return false;
    output = events_[read_index_];
    read_index_ = (read_index_ + 1U) % Capacity;
    --count_;
    return true;
  }

  bool try_peek(AuditEvent& output) const noexcept {
    if (count_ == 0U) return false;
    output = events_[read_index_];
    return true;
  }

  bool discard_front() noexcept {
    if (count_ == 0U) return false;
    read_index_ = (read_index_ + 1U) % Capacity;
    --count_;
    return true;
  }

  std::size_t size() const noexcept { return count_; }
  uint32_t overflow_count() const noexcept { return overflow_count_; }

 private:
  void push(AuditEventKind kind, const ProfileRef& previous,
            const ProfileRef& current, bool pending,
            const ConfigRef& event_config = ConfigRef{}) noexcept {
    if (Capacity == 0U) return;
    if (count_ == Capacity) {
      ++overflow_count_;
      return;
    }
    events_[write_index_] = AuditEvent{kind, monotonic_us_,
                                       event_config.present ? event_config : config_ref_,
                                       previous, current, pending};
    write_index_ = (write_index_ + 1U) % Capacity;
    ++count_;
  }

  std::array<AuditEvent, Capacity> events_{};
  std::size_t read_index_{0U};
  std::size_t write_index_{0U};
  std::size_t count_{0U};
  uint32_t overflow_count_{0U};
  uint64_t monotonic_us_{0U};
  ConfigRef config_ref_{};
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

enum class FactoryProfileInitResult : uint8_t {
  Preserved, Initialized, Busy, Invalid, Error, CommitFailed
};

class ProfileManager final {
 public:
  explicit ProfileManager(ProfilePersistence& persistence,
                          const ProfileCodec& codec = ProfilePersistence::default_codec(),
                          IProfileAuditSink* audit = nullptr) noexcept
      : persistence_(persistence), codec_(codec), audit_(audit) {}

  bool load(ProfileKey key) noexcept;
  FactoryProfileInitResult initialize_pristine(ProfileKey key,
                                                const EncodedProfile& encoded) noexcept;
  bool stage_import(ProfileKey key, const EncodedProfile& encoded) noexcept;
  bool validate(ProfileKey key) const noexcept;
  bool preview(ProfileKey key) noexcept;
  bool commit(ProfileKey key) noexcept;
  bool rollback(ProfileKey key) noexcept;
  bool previous(ProfileKey key, ActiveProfileSnapshot& output) const noexcept;
  bool reset(ProfileKey key) noexcept;
  bool cancel(ProfileKey key) noexcept;
  bool export_active(ProfileKey key, EncodedProfile& encoded) const noexcept;
  bool snapshot(ProfileKey key, ActiveProfileSnapshot& snapshot) const noexcept;
  ProfileManagerState state(ProfileKey key) const noexcept;
  void set_audit_context(uint64_t monotonic_us,
                         const ConfigRef& config_ref) noexcept {
    audit_monotonic_us_ = monotonic_us;
    audit_config_ref_ = config_ref;
  }

  bool load(ProfileTarget target) noexcept { return load(default_profile_key(target)); }
  bool stage_import(ProfileTarget target, const EncodedProfile& encoded) noexcept {
    return stage_import(default_profile_key(target), encoded);
  }
  bool validate(ProfileTarget target) const noexcept { return validate(default_profile_key(target)); }
  bool preview(ProfileTarget target) noexcept { return preview(default_profile_key(target)); }
  bool commit(ProfileTarget target) noexcept { return commit(default_profile_key(target)); }
  bool rollback(ProfileTarget target) noexcept { return rollback(default_profile_key(target)); }
  bool reset(ProfileTarget target) noexcept { return reset(default_profile_key(target)); }
  bool export_active(ProfileTarget target, EncodedProfile& encoded) const noexcept {
    return export_active(default_profile_key(target), encoded);
  }
  bool snapshot(ProfileTarget target, ActiveProfileSnapshot& output) const noexcept {
    return snapshot(default_profile_key(target), output);
  }
  ProfileManagerState state(ProfileTarget target) const noexcept {
    return state(default_profile_key(target));
  }
  void bind_runtime_profiles(TargetProfileSet& profiles) noexcept;
  uint32_t generation() const noexcept { return generation_; }

 private:
  static constexpr std::size_t kTargetCount{18U};
  static std::size_t index(ProfileTarget target) noexcept;
  static bool valid_key(ProfileKey key) noexcept;
  static std::size_t key_index(ProfileKey key) noexcept;
  static ProfileRef ref_from(const EncodedProfile& encoded) noexcept;
  bool ensure_loaded(ProfileTarget target) noexcept;
  bool ensure_loaded(ProfileKey key) noexcept;
  void sync_runtime_target(ProfileTarget target) noexcept;
  void sync_runtime_key(ProfileKey key) noexcept;

  ProfilePersistence& persistence_;
  const ProfileCodec& codec_;
  IProfileAuditSink* audit_{nullptr};
  uint64_t audit_monotonic_us_{0U};
  ConfigRef audit_config_ref_{};
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
  std::array<OperationalCapacityProfile, 3> capacity_runtime_{};
  uint32_t generation_{0U};
};

}  // namespace sigap::acquisition
