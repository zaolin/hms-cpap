#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace hms_cpap {

class IO2RingClient {
public:
    virtual ~IO2RingClient() = default;

    struct LiveReading {
        int spo2 = 0;
        int hr = 0;
        int motion = 0;
        int vibration = 0;
        bool active = false;
        bool valid = false;
    };

    virtual bool isConnected() = 0;
    virtual std::vector<std::string> listFiles() = 0;
    virtual std::vector<uint8_t> downloadFile(const std::string& filename) = 0;
    virtual LiveReading getLive() = 0;
    virtual int getBattery() const = 0;

    /**
     * Returns true if this is a cloud-based client (no live stream, no
     * active→inactive state transition). Cloud clients are polled on a
     * timer rather than waiting for a device state change.
     */
    virtual bool isCloudClient() const { return false; }
};

} // namespace hms_cpap
