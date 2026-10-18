from pathlib import Path


def write_link_stubs(root: Path):
    (root / 'esp_system.h').write_text('''#pragma once
#include <cstdint>
inline uint32_t esp_random() { return 0x12345678; }
''')
    (root / 'SkeletonBLEMock.h').write_text(r'''
#pragma once
#include <deque>
#include <vector>
namespace SkeletonLink {
inline std::deque<Packet> mockIncoming;
inline std::vector<Packet> mockSent;
inline std::vector<unsigned long> mockSentAt;
inline bool mockConnected = true;
inline bool mockAutoFinish = SKELETON_ROLE == 1;
inline bool begin() { return true; }
inline bool isConnected() { return mockConnected; }
inline void service() {}
inline void cancelSend() {}
inline bool send(const Packet& packet) {
  if (SKELETON_ROLE == 1 && !mockConnected) return false;
  mockSent.push_back(packet);
  mockSentAt.push_back(millis());
  if (mockAutoFinish && packet.kind == PLAY) {
    Packet reply = packet;
    reply.kind = DONE;
    mockIncoming.push_back(reply);
  }
  return true;
}
inline bool receive(Packet& packet) {
  if (mockIncoming.empty()) return false;
  packet = mockIncoming.front(); mockIncoming.pop_front(); return true;
}
}
''')
    (root / 'Preferences.h').write_text(r'''
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstring>
inline bool preferenceWritesSucceed = true;
inline std::map<std::string,std::vector<uint8_t>> storedPreferences;
class Preferences {
 public:
 bool begin(const char*, bool) { return true; }
 size_t getBytesLength(const char* key) { return storedPreferences[key].size(); }
 size_t getBytes(const char* key, void* out, size_t size) {
   const auto& bytes=storedPreferences[key];
   if(size<bytes.size()) return 0;
   memcpy(out,bytes.data(),bytes.size()); return bytes.size();
 }
 size_t putBytes(const char* key, const void* value, size_t size) {
   if(!preferenceWritesSucceed) return 0;
   const uint8_t* bytes=static_cast<const uint8_t*>(value);
   storedPreferences[key]=std::vector<uint8_t>(bytes,bytes+size); return size;
 }
};
''')
