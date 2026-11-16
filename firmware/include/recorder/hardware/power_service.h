#pragma once

#include "recorder/hardware/service.h"

namespace cardputer_recorder {

struct BatteryReading {
    int levelPercent = 0;
    int voltageMv = 0;
    bool valid = false;
};

class PowerService {
public:
    bool begin();
    void update();
    void end();

    BatteryReading readBattery();
    // The Cardputer ADV has no charge-status signal, so "on external power"
    // is a heuristic: a USB host is sending frames to the USB serial port,
    // or the battery sits at charging voltage (wall chargers send no
    // frames).
    bool externalPowerLikely(const BatteryReading& battery) const;
    ServiceState state() const;
    ErrorCode lastError() const;

private:
    ServiceState state_ = ServiceState::kStopped;
    ErrorCode error_ = ErrorCode::kNone;
};

}  // namespace cardputer_recorder
