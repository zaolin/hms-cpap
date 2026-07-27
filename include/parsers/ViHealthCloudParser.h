#pragma once

#include "parsers/CpapdashBridge.h"
#include <cstdint>
#include <optional>
#include <string>

namespace hms_cpap {

/**
 * ViHealthCloudParser - Parse raw oximetry files downloaded from the
 * ViHealth cloud (ai.viatomtech.com).
 *
 * File format (reverse-engineered from ViHealth Android app 2.75.63):
 *   Header: 10 bytes
 *     - bytes 0-1: version (0x0301 little-endian)
 *     - bytes 2-7: reserved (zeros)
 *     - bytes 8-9: sample size field (value=4, but actual data is 3 bytes)
 *   Samples: 3 bytes each at 1 Hz
 *     - byte 0: SpO2 (0-100, 255 = invalid sentinel)
 *     - byte 1: Heart rate (0-254, 255 = invalid sentinel)
 *     - byte 2: Motion (0-255)
 *
 * Timestamps are derived from the filename (YYYYMMDDHHMMSS format).
 */
class ViHealthCloudParser {
public:
    /**
     * Parse raw ViHealth cloud file bytes into an OximetrySession.
     * @param data    raw file bytes
     * @param size    byte count
     * @param filename session filename (YYYYMMDDHHMMSS) used for timestamps
     * @return parsed session, or nullopt if the data doesn't match the format
     */
    static std::optional<cpapdash::parser::OximetrySession> parse(
        const uint8_t* data, size_t size, const std::string& filename);
};

} // namespace hms_cpap