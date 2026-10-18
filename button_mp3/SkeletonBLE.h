#pragma once
#include "SkeletonProtocol.h"

#ifdef SKELETON_LINK_TEST
#include <SkeletonBLEMock.h>
#else
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>

namespace SkeletonLink {
// Change SERVICE_UUID in BOTH folders if you build another independent pair.
constexpr const char* SERVICE_UUID = "af04b301-a1a4-4a6f-8d73-8e95ad72c101";
constexpr const char* COMMAND_UUID = "af04b302-a1a4-4a6f-8d73-8e95ad72c101";
constexpr const char* REPLY_UUID   = "af04b303-a1a4-4a6f-8d73-8e95ad72c101";
static QueueHandle_t incoming = nullptr;
static QueueHandle_t outgoing = nullptr;
static std::atomic<bool> connected{false};
static std::atomic<bool> advertiseAgain{false};

inline bool isConnected() { return connected.load(); }
inline bool receive(Packet& packet) { return incoming && xQueueReceive(incoming, &packet, 0) == pdTRUE; }

#if SKELETON_ROLE == 1
struct Outgoing { Packet packet; uint32_t queuedAt; };
inline bool send(const Packet& packet) {
  if (!outgoing || !isConnected()) return false;
  Outgoing message{packet, static_cast<uint32_t>(millis())};
  return xQueueOverwrite(outgoing, &message) == pdPASS;
}
inline void cancelSend() { if (outgoing) xQueueReset(outgoing); }
class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {}
  void onDisconnect(BLEClient*) override { connected.store(false); }
};

// Scanning, connection discovery and GATT calls can block. Keep them off loop()
// so UART replies, button debounce and motion handling remain responsive.
inline void clientTask(void*) {
  BLEClient* client = BLEDevice::createClient();
  static ClientCallbacks callbacks;
  client->setClientCallbacks(&callbacks);
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  BLERemoteCharacteristic* command = nullptr;
  BLERemoteCharacteristic* reply = nullptr;
  for (;;) {
    if (!client->isConnected()) {
      connected.store(false);
      command = nullptr;
      reply = nullptr;
      BLEScanResults results = scan->start(2, false);
      for (int i = 0; i < results.getCount(); ++i) {
        BLEAdvertisedDevice peer = results.getDevice(i);
        if (!peer.haveServiceUUID() || !peer.isAdvertisingService(BLEUUID(SERVICE_UUID))) continue;
        if (client->connect(&peer)) {
          client->getServices();  // Refresh handles after every reconnect.
          BLERemoteService* service = client->getService(SERVICE_UUID);
          if (service) {
            command = service->getCharacteristic(COMMAND_UUID);
            reply = service->getCharacteristic(REPLY_UUID);
          }
          if (command && command->canWrite() && reply && reply->canRead()) connected.store(true);
          else client->disconnect();
        }
        break;
      }
      scan->clearResults();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!command || !reply || !isConnected()) {
      client->disconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    Outgoing message;
    if (xQueueReceive(outgoing, &message, 0) == pdTRUE &&
        static_cast<uint32_t>(millis()) - message.queuedAt < 1500) {
      uint8_t wire[WIRE_SIZE];
      encode(message.packet, wire);
      command->writeValue(wire, sizeof(wire), true);
    }
    if (client->isConnected()) {
      const std::string value = reply->readValue();
      Packet packet;
      if (decode(reinterpret_cast<const uint8_t*>(value.data()), value.size(), packet)) {
        xQueueSend(incoming, &packet, 0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}
inline bool begin() {
  incoming = xQueueCreate(8, sizeof(Packet));
  outgoing = xQueueCreate(1, sizeof(Outgoing));
  if (!incoming || !outgoing) return false;
  BLEDevice::init("Skeleton-1");
  return xTaskCreate(clientTask, "skeleton-ble", 8192, nullptr, 1, nullptr) == pdPASS;
}
inline void service() {}
#else
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected.store(true); }
  void onDisconnect(BLEServer*) override {
    connected.store(false);
    advertiseAgain.store(true);
  }
};
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    Packet packet;
    if (decode(reinterpret_cast<const uint8_t*>(value.data()), value.size(), packet) && packet.kind == PLAY) {
      xQueueSend(incoming, &packet, 0);  // Main loop owns all MP3/state changes.
    }
  }
};
class ReplyCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    Packet packet;
    if (xQueuePeek(outgoing, &packet, 0) == pdTRUE) {
      uint8_t wire[WIRE_SIZE];
      encode(packet, wire);
      characteristic->setValue(wire, sizeof(wire));
    }
  }
};
inline bool send(const Packet& packet) {
  // Queue a snapshot; only the BLE callback task touches the characteristic.
  return outgoing && xQueueOverwrite(outgoing, &packet) == pdPASS;
}
inline void cancelSend() {}
inline bool begin() {
  incoming = xQueueCreate(8, sizeof(Packet));
  outgoing = xQueueCreate(1, sizeof(Packet));
  if (!incoming || !outgoing) return false;
  BLEDevice::init("Skeleton-2");
  BLEServer* server = BLEDevice::createServer();
  static ServerCallbacks serverCallbacks;
  static CommandCallbacks commandCallbacks;
  static ReplyCallbacks replyCallbacks;
  server->setCallbacks(&serverCallbacks);
  BLEService* service = server->createService(SERVICE_UUID);
  BLECharacteristic* command = service->createCharacteristic(COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  command->setCallbacks(&commandCallbacks);
  BLECharacteristic* replyCharacteristic = service->createCharacteristic(REPLY_UUID, BLECharacteristic::PROPERTY_READ);
  replyCharacteristic->setCallbacks(&replyCallbacks);
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  return true;
}
inline void service() {
  if (advertiseAgain.exchange(false)) BLEDevice::startAdvertising();
}
#endif
}  // namespace SkeletonLink
#endif
