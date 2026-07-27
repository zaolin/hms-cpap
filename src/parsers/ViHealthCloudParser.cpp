#include "parsers/ViHealthCloudParser.h"
#include "utils/TimeCompat.h"
#include <cpapdash/parser/VLDParser.h>
#include <chrono>
#include <ctime>
#include <string>

namespace hms_cpap {

using OximetrySample  = cpapdash::parser::OximetrySample;
using OximetrySession = cpapdash::parser::OximetrySession;
using VLDParser       = cpapdash::parser::VLDParser;

static constexpr uint16_t VIHEALTH_VERSION = 0x0301;
static constexpr size_t   HEADER_SIZE      = 10;
static constexpr size_t   SAMPLE_SIZE      = 3;

std::optional<OximetrySession> ViHealthCloudParser::parse(
    const uint8_t* data, size_t size, const std::string& filename) {

    if (size < HEADER_SIZE + SAMPLE_SIZE) return std::nullopt;

    // Verify the ViHealth cloud format marker
    uint16_t version = static_cast<uint16_t>(data[0]) |
                       (static_cast<uint16_t>(data[1]) << 8);
    if (version != VIHEALTH_VERSION) return std::nullopt;

    size_t data_size   = size - HEADER_SIZE;
    size_t num_samples = data_size / SAMPLE_SIZE;
    if (num_samples == 0) return std::nullopt;

    OximetrySession session;
    session.filename = filename;

    // Derive start timestamp from filename (YYYYMMDDHHMMSS), else use now
    std::chrono::system_clock::time_point start_time;
    if (filename.size() >= 14) {
        std::tm tm{};
        tm.tm_year = std::stoi(filename.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(filename.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(filename.substr(6, 2));
        tm.tm_hour = std::stoi(filename.substr(8, 2));
        tm.tm_min  = std::stoi(filename.substr(10, 2));
        tm.tm_sec  = std::stoi(filename.substr(12, 2));
        start_time = std::chrono::system_clock::from_time_t(timegm_utc(&tm));
    } else {
        start_time = std::chrono::system_clock::now();
    }

    for (size_t i = 0; i < num_samples; ++i) {
        size_t offset = HEADER_SIZE + i * SAMPLE_SIZE;
        uint8_t spo2   = data[offset];
        uint8_t hr     = data[offset + 1];
        uint8_t motion = data[offset + 2];

        OximetrySample sample;
        sample.timestamp    = start_time + std::chrono::seconds(i);
        bool sp_ok = spo2 > 0 && spo2 <= 100;
        bool hr_ok = hr > 0 && hr < 255;
        sample.spo2         = sp_ok ? spo2 : 0xFF;
        sample.heart_rate   = hr_ok ? hr  : 0xFF;
        sample.invalid_flag = sp_ok ? 0   : 1;
        sample.motion       = motion;
        sample.vibration    = 0;
        session.samples.push_back(sample);
    }

    session.sample_interval  = 1.0;  // 1 Hz
    session.start_time       = session.samples.front().timestamp;
    session.end_time         = session.samples.back().timestamp;
    session.duration_seconds = static_cast<int>(num_samples);
    session.metrics          = VLDParser::calculateMetrics(
        session.samples, session.sample_interval);

    return session;
}

} // namespace hms_cpap