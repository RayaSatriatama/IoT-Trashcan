#pragma once
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "freertos/FreeRTOS.h"
#define MYNEWT_VAL(name) 0
inline constexpr int BLE_HS_IO_NO_INPUT_OUTPUT = 3;
namespace NIMBLE_PROPERTY { inline constexpr int WRITE=1, WRITE_NR=2, READ=4, NOTIFY=8; }
class NimBLECharacteristic;
class NimBLEServer;
class NimBLEConnInfo {
 public:
  explicit NimBLEConnInfo(uint16_t handle) : handle_(handle) {}
  uint16_t getConnHandle() const { return handle_; }
 private: uint16_t handle_;
};
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks()=default;
  virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
};
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks()=default;
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
};
class NimBLEAttValue {
 public:
  std::string bytes;
  bool copied{};
  NimBLEAttValue()=default;
  NimBLEAttValue(const NimBLEAttValue& other) : bytes(other.bytes),copied(true) {}
  ~NimBLEAttValue() { if(copied)for(unsigned char byte:bytes)assert(byte==0); }
  uint16_t length() const { return static_cast<uint16_t>(bytes.size()); }
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(bytes.data()); }
  operator std::string() const { return bytes; }
};
struct HostNotification { uint16_t handle; std::string bytes; };
using HostMtuHook = void (*)(uint16_t, uint16_t);
inline HostMtuHook host_mtu_hook{};
class NimBLECharacteristic {
 public:
  std::unique_ptr<NimBLECharacteristicCallbacks> callbacks;
  NimBLEAttValue value;
  mutable std::vector<HostNotification> notifications;
  mutable bool reject_next_notification{};
  mutable size_t notification_attempts{};
  mutable size_t reject_notification_attempt{};
  void setCallbacks(NimBLECharacteristicCallbacks* c) { callbacks.reset(c); }
  NimBLEAttValue getValue() const { return value; }
 protected:
  const NimBLEAttValue& getAttVal() const { return value; }
 public:
  void setValue(const uint8_t* data, size_t size) { assert(host_critical_depth==0); value.bytes.assign(reinterpret_cast<const char*>(data),size); }
  bool notify(uint16_t handle=0xFFFFU) const {
    assert(host_critical_depth==0);
    ++notification_attempts;
    if (reject_next_notification || notification_attempts==reject_notification_attempt) {
      reject_next_notification=false;
      return false;
    }
    notifications.push_back({handle,value.bytes});
    return true;
  }
  void write(std::string_view bytes,uint16_t handle) { value.bytes.assign(bytes); NimBLEConnInfo info(handle); callbacks->onWrite(this,info); }
};
class NimBLEService {
 public:
  std::vector<std::unique_ptr<NimBLECharacteristic>> characteristics;
  NimBLECharacteristic* createCharacteristic(const char*,int,size_t) { characteristics.emplace_back(new NimBLECharacteristic); return characteristics.back().get(); }
};
class NimBLEAdvertising { public: void addServiceUUID(const char*) {} };
class NimBLEServer {
 public:
  std::unique_ptr<NimBLEServerCallbacks> callbacks;
  NimBLEService service;
  NimBLEAdvertising advertising;
  std::vector<uint16_t> disconnects;
  std::vector<uint16_t> live_peers;
  bool advertise_on_disconnect{},advertising_stopped{};
  NimBLEService* createService(const char*) { return &service; }
  void setCallbacks(NimBLEServerCallbacks* c) { callbacks.reset(c); }
  NimBLEAdvertising* getAdvertising() { return &advertising; }
  void advertiseOnDisconnect(bool v) { advertise_on_disconnect=v; }
  bool startAdvertising() { assert(host_critical_depth==0); return true; }
  bool stopAdvertising() { assert(host_critical_depth==0); advertising_stopped=true; return true; }
  bool disconnect(uint16_t handle) { assert(host_critical_depth==0); disconnects.push_back(handle); peer_disconnect(handle); return true; }
  // SDK-shaped presence model: only genuine peer transitions affect live peers.
  uint16_t getPeerMTU(uint16_t handle) const { assert(host_critical_depth==0); const auto mtu=std::find(live_peers.begin(),live_peers.end(),handle)!=live_peers.end()?23U:0U; if(host_mtu_hook){const auto hook=host_mtu_hook;host_mtu_hook=nullptr;hook(handle,mtu);} return mtu; }
  void connect(uint16_t handle) { if(std::find(live_peers.begin(),live_peers.end(),handle)==live_peers.end())live_peers.push_back(handle); NimBLEConnInfo info(handle); callbacks->onConnect(this,info); }
  void peer_disconnect(uint16_t handle) { live_peers.erase(std::remove(live_peers.begin(),live_peers.end(),handle),live_peers.end()); NimBLEConnInfo info(handle); callbacks->onDisconnect(this,info,0); }
  // Deliver a possibly delayed callback without changing current SDK presence.
  void deliver_old_disconnect(uint16_t handle) { NimBLEConnInfo info(handle); callbacks->onDisconnect(this,info,0); }
};
inline NimBLEServer host_server;
class NimBLEDevice {
 public:
  static bool init(std::string) { return true; }
  static void setSecurityAuth(bool,bool,bool) {}
  static void setSecurityIOCap(int) {}
  static NimBLEServer* createServer() { return &host_server; }
};
