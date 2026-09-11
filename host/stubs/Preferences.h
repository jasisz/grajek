// In-memory NVS used to exercise the real settings adapter, including failed
// writes. Writes assert that the adapter parked audio before opening NVS.
#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

namespace fake_nvs {
inline std::map<std::string, std::vector<uint8_t>> values;
inline bool parked = false;
inline bool failPark = false;
inline bool failOpen = false;
inline bool failBlob = false;
inline int writes = 0;
inline int parks = 0;
inline int resumes = 0;
inline int saveRequests = 0;
}  // namespace fake_nvs

struct TestSerial { void println(const char*) {} };
inline TestSerial Serial;

class Preferences {
 public:
  bool begin(const char*, bool readOnly) {
    readOnly_ = readOnly;
    if (readOnly) return !fake_nvs::values.empty();
    assert(fake_nvs::parked);
    return !fake_nvs::failOpen;
  }
  void end() {}
  bool isKey(const char* key) { return fake_nvs::values.count(key) != 0; }
  uint8_t getUChar(const char* key, uint8_t fallback = 0) {
    const auto it = fake_nvs::values.find(key);
    return it != fake_nvs::values.end() && it->second.size() == 1 ? it->second[0] : fallback;
  }
  bool getBool(const char* key, bool fallback = false) { return getUChar(key, fallback) != 0; }
  size_t getBytesLength(const char* key) {
    const auto it = fake_nvs::values.find(key);
    return it == fake_nvs::values.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* key, void* out, size_t capacity) {
    const auto it = fake_nvs::values.find(key);
    if (it == fake_nvs::values.end() || it->second.size() > capacity) return 0;
    memcpy(out, it->second.data(), it->second.size());
    return it->second.size();
  }
  size_t putBytes(const char* key, const void* bytes, size_t size) {
    assert(!readOnly_ && fake_nvs::parked);
    if (fake_nvs::failBlob) return 0;
    const auto* data = static_cast<const uint8_t*>(bytes);
    fake_nvs::values[key] = std::vector<uint8_t>(data, data + size);
    ++fake_nvs::writes;
    return size;
  }
  size_t putUChar(const char* key, uint8_t value) { return putBytes(key, &value, 1); }
 private:
  bool readOnly_ = true;
};
