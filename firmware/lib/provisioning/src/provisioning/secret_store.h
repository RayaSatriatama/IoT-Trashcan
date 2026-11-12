#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sigap::provisioning {

inline constexpr std::size_t kMaxSecretBlobBytes{170U};
inline constexpr uint8_t kSecretSchemaVersion{1U};

struct NetworkSecrets {
  std::array<char, 33> ssid{};
  std::array<char, 65> password{};
  std::array<char, 129> device_token{};
};

struct SecretPresence {
  bool wifi_configured{false};
  bool device_token_configured{false};
};

enum class SecretStoreResult : uint8_t {
  Ok,
  NotFound,
  InvalidInput,
  UnsupportedSchema,
  Corrupt,
  StorageError,
  ConfirmationRequired,
};

enum class SecretSlot : uint8_t { Wifi, DeviceToken };

enum class SecretBlobReadStatus : uint8_t { Found, NotFound, Invalid, Error };

struct EncodedSecretBlob {
  std::array<uint8_t, kMaxSecretBlobBytes> bytes{};
  std::size_t size{0U};
};

// Best-effort clearing for transient parser, codec, and network buffers.
inline void secure_zero(void* pointer, std::size_t size) noexcept {
  volatile auto* bytes = static_cast<volatile uint8_t*>(pointer);
  while (size-- != 0U) *bytes++ = 0U;
}

class SecretBlobCodec final {
 public:
  static bool encode_wifi(const char* ssid, const char* password,
                          EncodedSecretBlob& output) noexcept;
  static bool encode_token(const char* token,
                           EncodedSecretBlob& output) noexcept;
  static SecretStoreResult decode_wifi(const uint8_t* bytes, std::size_t size,
                                       NetworkSecrets& output) noexcept;
  static SecretStoreResult decode_token(const uint8_t* bytes, std::size_t size,
                                        NetworkSecrets& output) noexcept;
};

class ISecretBlobStore {
 public:
  virtual ~ISecretBlobStore() = default;
  virtual SecretBlobReadStatus read(SecretSlot slot, uint8_t* output,
                                    std::size_t capacity,
                                    std::size_t& size) const noexcept = 0;
  virtual bool write(SecretSlot slot, const uint8_t* bytes,
                     std::size_t size) noexcept = 0;
  // Implementations must apply both erases as one backend transaction.
  virtual bool clear_all() noexcept = 0;
};

class SecretStore {
 public:
  virtual ~SecretStore() = default;
  // Only internal network/auth services may receive plaintext secrets.
  virtual bool load_for_network(NetworkSecrets& output) = 0;
  virtual bool load_wifi_for_network(NetworkSecrets& output) = 0;
  virtual SecretPresence status() const noexcept = 0;
  // Maintenance writes and clear operations are intentionally write-only.
  virtual bool set_wifi(const char* ssid, const char* password) = 0;
  virtual bool rotate_device_token(const char* token) = 0;
  virtual bool clear_all(bool physical_confirmation) = 0;
  virtual SecretStoreResult last_result() const noexcept = 0;
};

class PersistentSecretStore final : public SecretStore {
 public:
  explicit PersistentSecretStore(ISecretBlobStore& backend) noexcept
      : backend_(backend) {}
  bool load_for_network(NetworkSecrets& output) override;
  bool load_wifi_for_network(NetworkSecrets& output) override;
  SecretPresence status() const noexcept override;
  bool set_wifi(const char* ssid, const char* password) override;
  bool rotate_device_token(const char* token) override;
  bool clear_all(bool physical_confirmation) override;
  SecretStoreResult last_result() const noexcept override { return result_; }

 private:
  bool read_slot(SecretSlot slot, NetworkSecrets& output) const noexcept;
  void set_result(SecretStoreResult result) const noexcept { result_ = result; }
  ISecretBlobStore& backend_;
  mutable SecretStoreResult result_{SecretStoreResult::NotFound};
};

class MemorySecretBlobStore final : public ISecretBlobStore {
 public:
  enum class Fault : uint8_t {
    ReadWifi,
    ReadToken,
    WriteWifi,
    WriteToken,
    Clear,
  };
  SecretBlobReadStatus read(SecretSlot slot, uint8_t* output,
                            std::size_t capacity,
                            std::size_t& size) const noexcept override;
  bool write(SecretSlot slot, const uint8_t* bytes,
             std::size_t size) noexcept override;
  bool clear_all() noexcept override;
  void fail_next(Fault fault) noexcept;
  void corrupt(SecretSlot slot) noexcept;
  bool present(SecretSlot slot) const noexcept;

 private:
  static std::size_t index(SecretSlot slot) noexcept {
    return slot == SecretSlot::Wifi ? 0U : 1U;
  }
  std::array<std::array<uint8_t, kMaxSecretBlobBytes>, 2> blobs_{};
  std::array<std::size_t, 2> sizes_{};
  std::array<bool, 2> present_{};
  Fault next_fault_{Fault::Clear};
  bool fault_pending_{false};
};

class NvsSecretBlobStore final : public ISecretBlobStore {
 public:
  SecretBlobReadStatus read(SecretSlot slot, uint8_t* output,
                            std::size_t capacity,
                            std::size_t& size) const noexcept override;
  bool write(SecretSlot slot, const uint8_t* bytes,
             std::size_t size) noexcept override;
  bool clear_all() noexcept override;
};

class NvsSecretStore final : public SecretStore {
 public:
  NvsSecretStore() noexcept : store_(backend_) {}
  bool load_for_network(NetworkSecrets& output) override {
    return store_.load_for_network(output);
  }
  bool load_wifi_for_network(NetworkSecrets& output) override {
    return store_.load_wifi_for_network(output);
  }
  SecretPresence status() const noexcept override { return store_.status(); }
  bool set_wifi(const char* ssid, const char* password) override {
    return store_.set_wifi(ssid, password);
  }
  bool rotate_device_token(const char* token) override {
    return store_.rotate_device_token(token);
  }
  bool clear_all(bool physical_confirmation) override {
    return store_.clear_all(physical_confirmation);
  }
  SecretStoreResult last_result() const noexcept override {
    return store_.last_result();
  }

 private:
  NvsSecretBlobStore backend_{};
  PersistentSecretStore store_;
};

}  // namespace sigap::provisioning
