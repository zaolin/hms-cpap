#include "services/OximetryService.h"
#include "utils/TimeCompat.h"
#include <cpapdash/parser/VLDParser.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>

namespace hms_cpap {

using VLDParser = cpapdash::parser::VLDParser;
using OximetrySample = cpapdash::parser::OximetrySample;
using OximetrySession = cpapdash::parser::OximetrySession;

/// Parse a ViHealth cloud binary file (3-byte samples: SpO2, HR, motion).
/// Header: 10 bytes (version[2] + reserved[6] + sample_size_field[2])
/// Samples: 3 bytes each (SpO2[1], HR[1], motion[1]) at 1Hz
/// Returns nullopt if the data doesn't look like a ViHealth cloud file.
static std::optional<OximetrySession> parseViHealthCloud(
    const uint8_t* data, size_t size, const std::string& filename) {

    if (size < 13) return std::nullopt;

    // ViHealth cloud files start with version 0x0301 (little-endian 01 03)
    uint16_t version = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    if (version != 0x0301) return std::nullopt;

    const size_t header_size = 10;
    const size_t sample_size = 3;
    if (size <= header_size) return std::nullopt;

    size_t data_size = size - header_size;
    size_t num_samples = data_size / sample_size;
    if (num_samples == 0) return std::nullopt;

    OximetrySession session;
    session.filename = filename;

    // Build timestamps from filename (YYYYMMDDHHMMSS) or current time
    std::chrono::system_clock::time_point start_time;
    if (filename.size() >= 14) {
        std::tm tm{};
        tm.tm_year = std::stoi(filename.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(filename.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(filename.substr(6, 2));
        tm.tm_hour = std::stoi(filename.substr(8, 2));
        tm.tm_min = std::stoi(filename.substr(10, 2));
        tm.tm_sec = std::stoi(filename.substr(12, 2));
        start_time = std::chrono::system_clock::from_time_t(timegm_utc(&tm));
    } else {
        start_time = std::chrono::system_clock::now();
    }

    for (size_t i = 0; i < num_samples; ++i) {
        size_t offset = header_size + i * sample_size;
        uint8_t spo2 = data[offset];
        uint8_t hr = data[offset + 1];
        uint8_t motion = data[offset + 2];

        OximetrySample sample;
        sample.timestamp = start_time + std::chrono::seconds(i);
        bool sp_ok = spo2 > 0 && spo2 <= 100;
        bool hr_ok = hr > 0 && hr < 255;
        sample.spo2 = sp_ok ? spo2 : 0xFF;
        sample.heart_rate = hr_ok ? hr : 0xFF;
        sample.invalid_flag = sp_ok ? 0 : 1;
        sample.motion = (motion <= 255) ? motion : 0;
        sample.vibration = 0;
        session.samples.push_back(sample);
    }

    session.sample_interval = 1.0;
    session.start_time = session.samples.front().timestamp;
    session.end_time = session.samples.back().timestamp;
    session.duration_seconds = static_cast<int>(num_samples);
    session.metrics = VLDParser::calculateMetrics(session.samples, session.sample_interval);

    return session;
}

OximetryService::OximetryService(std::shared_ptr<IO2RingClient> client,
                                 std::shared_ptr<IDatabase> db)
    : client_(std::move(client)),
      db_(std::move(db)) {}

bool OximetryService::collectAndPublish() {
    // Step 1: List available files
    auto files = client_->listFiles();
    if (files.empty()) {
        std::cout << "O2Ring: No files available" << std::endl;
        return false;
    }

    std::cout << "O2Ring: " << files.size() << " file(s) on device"
              << " (battery: " << client_->getBattery() << "%)" << std::endl;

    // Step 2: Filter out already-processed files
    bool any_new = false;
    for (const auto& filename : files) {
        // Skip if already processed in this session
        if (processed_files_.count(filename)) {
            continue;
        }

        // Skip if already in DB
        // Use a fixed device_id for the O2 Ring
        std::string device_id = "o2ring";
        if (db_->oximetrySessionExists(device_id, filename)) {
            processed_files_.insert(filename);
            continue;
        }

        // Step 3: Download and parse
        std::cout << "O2Ring: Processing new file: " << filename << std::endl;

        auto data = client_->downloadFile(filename);
        if (data.empty()) {
            std::cerr << "O2Ring: Failed to download " << filename << std::endl;
            continue;
        }

        // Debug: dump raw file to config dir (bind mount) for format inspection
        {
            std::string dump_dir = "/home/cpap/.hms-cpap/vihealth_debug";
            std::filesystem::create_directories(dump_dir);
            std::string dump_path = dump_dir + "/vihealth_" + filename + ".bin";
            std::ofstream f(dump_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(data.data()), data.size());
            std::cout << "O2Ring: Dumped raw file to " << dump_path
                      << " (" << data.size() << " bytes)" << std::endl;
        }

        // Try VLDParser first (for BLE/HTTP mule .vld files), then fall back
        // to the ViHealth cloud binary format (3-byte samples, 10-byte header)
        auto session = VLDParser::parse(data.data(), data.size(), filename);
        if (!session) {
            session = parseViHealthCloud(data.data(), data.size(), filename);
        }
        if (!session) {
            std::cerr << "O2Ring: Failed to parse " << filename << std::endl;
            continue;
        }

        std::cout << "O2Ring: Parsed " << filename
                  << " - " << session->samples.size() << " samples"
                  << ", " << session->duration_seconds << "s"
                  << ", avg SpO2 " << std::fixed << std::setprecision(1)
                  << session->metrics.avg_spo2 << "%"
                  << ", avg HR " << session->metrics.avg_hr << " bpm"
                  << std::endl;

        // Step 4: Save to database
        if (db_->saveOximetrySession(device_id, session.value())) {
            processed_files_.insert(filename);
            any_new = true;
            std::cout << "O2Ring: Saved " << filename << " to database" << std::endl;
        } else {
            std::cerr << "O2Ring: Failed to save " << filename << " to database" << std::endl;
        }
    }

    return any_new;
}

IO2RingClient::LiveReading OximetryService::pollLive() {
    last_live_ = client_->getLive();
    std::cout << "O2Ring: active=" << (last_live_.active ? "ON" : "OFF")
              << " SpO2=" << last_live_.spo2
              << " HR=" << last_live_.hr
              << " Motion=" << last_live_.motion << std::endl;
    return last_live_;
}

} // namespace hms_cpap
