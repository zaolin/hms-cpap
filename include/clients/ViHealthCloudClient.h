#pragma once

#include "clients/IO2RingClient.h"
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include "utils/FailureLogThrottle.h"

namespace hms_cpap {

/**
 * ViHealthCloudClient - Pull O2Ring session data from the ViHealth cloud
 * (ai.viatomtech.com). Reverse-engineered from the ViHealth Android app
 * (com.viatom.vihealth 2.75.63).
 *
 * Implements the same IO2RingClient interface as O2RingClient/O2RingBleClient,
 * so it drops into OximetryService unchanged. `getLive()` returns inactive
 * (cloud has no live stream); `listFiles()` returns data_tag IDs; 
 * `downloadFile()` fetches the raw .vld from S3 via originalFileUrl.
 *
 * Auth: email/password login -> token. Every request is signed with
 * MD5(TreeMap(body + salt + timeStamp).toJSON()).toUpperCase().
 *
 * See docs/ViHealth_CLOUD_API_REVERSE_ENGINEERED.md for full protocol details.
 */
class ViHealthCloudClient : public IO2RingClient {
public:
    struct Config {
        std::string base_url = "https://ai.viatomtech.com";
        std::string email;
        std::string password;
        int poll_interval_seconds = 600;
    };

    explicit ViHealthCloudClient(const Config& cfg);
    ~ViHealthCloudClient();

    ViHealthCloudClient(const ViHealthCloudClient&) = delete;
    ViHealthCloudClient& operator=(const ViHealthCloudClient&) = delete;

    bool isConnected() override;
    std::vector<std::string> listFiles() override;
    std::vector<uint8_t> downloadFile(const std::string& filename) override;
    LiveReading getLive() override;
    int getBattery() const override { return cached_battery_; }

private:
    Config cfg_;
    FailureLogThrottle http_fail_log_;
    std::string token_;
    std::string user_id_;
    std::string country_code_;   // from login response, drives region routing
    int cached_battery_ = -1;

    // Cursor: highest measureTime seen, persisted by caller. Internally we
    // just query with startTime > last_cursor_ on each listFiles() call.
    long long last_cursor_ms_ = 0;
    // Cache of data_tag -> originalFileUrl from the last listFiles() call,
    // so downloadFile(data_tag) can resolve to the S3 URL.
    std::map<std::string, std::string> file_url_cache_;

    static constexpr long HTTP_TIMEOUT = 30L;
    static constexpr const char* SECRET = "a64255ab64344fb99612badde43d5365";

    // HTTP helpers (fresh CURL* per call, like SleepHqClient)
    static size_t writeCb(void* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t writeBinCb(void* ptr, size_t size, size_t nmemb, void* userdata);
    std::string httpPost(const std::string& url, const std::string& body,
                         const std::string& content_type, long& status);
    std::string httpGet(const std::string& url, long& status);
    std::vector<uint8_t> httpGetBinary(const std::string& url, long& status);

    // Auth
    bool login();
    bool ensureToken();

    // Region routing: after login, the app calls node/country/node/get with
    // the user's countryCode to discover the correct regional server. We
    // resolve once after login and update cfg_.base_url in-place so all
    // subsequent calls hit the right server. Returns true if base_url was
    // changed (or no change needed).
    bool resolveRegion();

    // Signing: MD5(TreeMap(params + salt + timeStamp).toJSON()).toUpperCase()
    // params = body JSON fields flattened to Map<String,String>. The TreeMap
    // sorts keys alphabetically for deterministic output.
    std::string computeSign(const std::map<std::string, std::string>& params,
                            const std::string& timestamp_ms);

    // Build the signed JSON body for a POST request. Merges body_params with
    // salt/timeStamp, signs, and returns the JSON string to send.
    struct SignedBody {
        std::string json;
        std::string timestamp;
        std::string sign;
    };
    SignedBody buildSignedBody(const std::map<std::string, std::string>& body_params);

    // Sorted JSON serializer (keys in alphabetical order, matching Java
    // TreeMap iteration order). Needed for deterministic MD5 signing.
    static std::string sortedJson(const std::map<std::string, std::string>& m);

    // MD5 hex digest (lowercase) of input string
    static std::string md5Hex(const std::string& input);
};

} // namespace hms_cpap