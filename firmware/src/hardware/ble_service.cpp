#include "recorder/hardware/ble_service.h"

#include <NimBLEDevice.h>

// The Arduino core frees the Bluetooth controller's memory at boot unless
// btInUse() says otherwise (esp32-hal-misc.c), and NimBLE-Arduino 1.4 does
// not override it. Without this, NimBLEDevice::init fails at runtime.
// The cost is that this memory stays reserved even when BLE is off.
extern "C" bool btInUse()
{
    return true;
}

namespace cardputer_recorder {
namespace {

// Keep in sync with desktop/src-tauri/src/pairing.rs.
constexpr const char* kServiceUuid = "6f1c0001-5b2e-4c8f-9a4d-3e7b1f2a9c00";
constexpr const char* kInfoUuid = "6f1c0002-5b2e-4c8f-9a4d-3e7b1f2a9c00";
constexpr const char* kControlUuid = "6f1c0003-5b2e-4c8f-9a4d-3e7b1f2a9c00";
constexpr std::uint16_t kCompanyId = 0xFFFF;  // "no company", for testing
constexpr std::uint8_t kFlagPairing = 0x01;
constexpr std::uint8_t kFlagPaired = 0x02;
constexpr std::size_t kMaxInbox = 8;

// One BLE service exists at a time; static callbacks avoid allocating new
// callback objects (which NimBLE does not free) every time pairing opens.
BleService* gOwner = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override
    {
        if (gOwner != nullptr) {
            gOwner->onConnect(desc->conn_handle);
        }
    }
    void onDisconnect(NimBLEServer*) override
    {
        if (gOwner != nullptr) {
            gOwner->onDisconnect();
        }
        // Stay discoverable for the rest of pairing mode.
        NimBLEDevice::startAdvertising();
    }
    void onMTUChange(std::uint16_t mtu, ble_gap_conn_desc*) override
    {
        if (gOwner != nullptr) {
            gOwner->onMtu(mtu);
        }
    }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* characteristic) override
    {
        NimBLEAttValue value = characteristic->getValue();
        if (gOwner != nullptr) {
            gOwner->onWrite(value.data(), value.length());
        }
    }
};

ServerCallbacks gServerCallbacks;
ControlCallbacks gControlCallbacks;

}  // namespace

bool BleService::begin(const std::uint8_t deviceId[8], bool hasPeer,
                       const std::string& info)
{
    if (active_) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.clear();
        reassembler_.reset();
        connected_ = false;
        disconnected_ = false;
        mtu_ = 23;
    }

    gOwner = this;
    NimBLEDevice::init("Cardputer Rec");
    NimBLEDevice::setMTU(247);
    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(&gServerCallbacks, false);
    NimBLEService* service = server_->createService(kServiceUuid);
    NimBLECharacteristic* infoCharacteristic =
        service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
    infoCharacteristic->setValue(
        reinterpret_cast<const std::uint8_t*>(info.data()), info.size());
    control_ = service->createCharacteristic(
        kControlUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    control_->setCallbacks(&gControlCallbacks);
    service->start();

    std::string manufacturer;
    manufacturer += static_cast<char>(kCompanyId & 0xFF);
    manufacturer += static_cast<char>(kCompanyId >> 8);
    manufacturer += "CP";
    manufacturer += static_cast<char>(kFlagPairing |
                                      (hasPeer ? kFlagPaired : 0));
    manufacturer.append(reinterpret_cast<const char*>(deviceId), 8);

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(kServiceUuid);
    NimBLEAdvertisementData response;
    response.setName("Cardputer Rec");
    response.setManufacturerData(manufacturer);
    advertising->setScanResponseData(response);
    advertising->setScanResponse(true);
    active_ = advertising->start();
    if (!active_) {
        NimBLEDevice::deinit(true);
        server_ = nullptr;
        control_ = nullptr;
    }
    return active_;
}

void BleService::end()
{
    if (!active_) {
        return;
    }
    NimBLEDevice::getAdvertising()->stop();
    // Frees the host stack and controller memory for WiFi and TLS.
    NimBLEDevice::deinit(true);
    server_ = nullptr;
    control_ = nullptr;
    active_ = false;
    gOwner = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    inbox_.clear();
    connected_ = false;
}

bool BleService::connected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool BleService::takeDisconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const bool value = disconnected_;
    disconnected_ = false;
    return value;
}

bool BleService::takeMessage(std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (inbox_.empty()) {
        return false;
    }
    message.swap(inbox_.front());
    inbox_.pop_front();
    return true;
}

bool BleService::send(const std::string& frame)
{
    std::uint16_t mtu;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || control_ == nullptr) {
            return false;
        }
        mtu = mtu_;
    }
    // ATT notifications carry MTU - 3 bytes.
    const std::size_t maxFragment = mtu > 23 ? mtu - 3 : 20;
    for (const std::string& fragment : fragmentMessage(frame, maxFragment)) {
        control_->setValue(
            reinterpret_cast<const std::uint8_t*>(fragment.data()),
            fragment.size());
        control_->notify();
        delay(5);  // let the controller drain its notification buffers
    }
    return true;
}

void BleService::onConnect(std::uint16_t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    reassembler_.reset();
    inbox_.clear();
}

void BleService::onDisconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    disconnected_ = true;
    mtu_ = 23;
    reassembler_.reset();
}

void BleService::onMtu(std::uint16_t mtu)
{
    std::lock_guard<std::mutex> lock(mutex_);
    mtu_ = mtu;
}

void BleService::onWrite(const std::uint8_t* data, std::size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string message;
    const Reassembler::Result result =
        reassembler_.push(data, size, message);
    if (result == Reassembler::Result::kComplete &&
        inbox_.size() < kMaxInbox) {
        inbox_.push_back(std::move(message));
    }
}

}  // namespace cardputer_recorder
