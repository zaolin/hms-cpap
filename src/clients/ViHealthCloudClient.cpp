#include "clients/ViHealthCloudClient.h"
#include <openssl/evp.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace hms_cpap {

using json = nlohmann::json;

// ── Write callbacks ──────────────────────────────────────────────────────

size_t ViHealthCloudClient::writeCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

size_t ViHealthCloudClient::writeBinCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    buf->insert(buf->end(), static_cast<uint8_t*>(ptr),
                static_cast<uint8_t*>(ptr) + size * nmemb);
    return size * nmemb;
}

// ── MD5 ──────────────────────────────────────────────────────────────────

std::string ViHealthCloudClient::md5Hex(const std::string& input) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    unsigned char digest[16];
    unsigned int len = 0;
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream ss;
    for (unsigned int i = 0; i < len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return ss.str();
}

// ── Sorted JSON (TreeMap equivalent) ─────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string ViHealthCloudClient::sortedJson(const std::map<std::string, std::string>& m) {
    // std::map is already sorted by key (alphabetical) — matches Java TreeMap.
    std::ostringstream ss;
    ss << '{';
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << jsonEscape(k) << "\":";
        // The ViHealth app stores values as strings in the TreeMap for signing.
        // If the value is a JSON array/object string (from the RetrofitAsk list
        // field), it's stored as-is (a JSON string in the map). Otherwise it's
        // the raw string value. We emit as a JSON string in both cases, matching
        // what the app does before MD5.
        ss << '"' << jsonEscape(v) << '"';
    }
    ss << '}';
    return ss.str();
}

// ── Sign computation ──────────────────────────────────────────────────────

std::string ViHealthCloudClient::computeSign(
    const std::map<std::string, std::string>& params,
    const std::string& timestamp_ms) {
    auto m = params;  // copy
    m["salt"] = SECRET;
    m["timeStamp"] = timestamp_ms;
    std::string j = sortedJson(m);
    // The app strips escaped backslashes for non-scale endpoints: replace("\\\\","")
    // Java's String.replace("\\\\", "") replaces literal "\\\\" (two backslashes
    // in the actual string) with empty. In our JSON, backslashes are escaped as
    // "\\\\", so a literal backslash in a value becomes "\\\\" in the JSON. The
    // app removes these double-backslashes before MD5.
    std::string cleaned;
    cleaned.reserve(j.size());
    for (size_t i = 0; i < j.size(); ++i) {
        if (j[i] == '\\' && i + 1 < j.size() && j[i + 1] == '\\') {
            // Skip one backslash (collapse \\ to nothing per app logic)
            ++i;
            continue;
        }
        cleaned += j[i];
    }
    std::string md5 = md5Hex(cleaned);
    std::transform(md5.begin(), md5.end(), md5.begin(), ::toupper);
    return md5;
}

ViHealthCloudClient::SignedBody ViHealthCloudClient::buildSignedBody(
    const std::map<std::string, std::string>& body_params) {
    SignedBody out;
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    out.timestamp = std::to_string(ts);
    out.sign = computeSign(body_params, out.timestamp);

    // The actual JSON body sent is the body_params as JSON (without salt/timeStamp).
    // The app re-serializes the TreeMap for the request body, but the server
    // only checks the sign header — the body can be the plain JSON. We emit a
    // clean JSON object from body_params.
    std::ostringstream ss;
    ss << '{';
    bool first = true;
    for (const auto& [k, v] : body_params) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << jsonEscape(k) << "\":\"" << jsonEscape(v) << '"';
    }
    ss << '}';
    out.json = ss.str();
    return out;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────

std::string ViHealthCloudClient::httpPost(const std::string& url,
                                           const std::string& body,
                                           const std::string& content_type,
                                           long& status) {
    CURL* c = curl_easy_init();
    if (!c) { status = 0; return ""; }
    std::string resp;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Accept: application/json");
    std::string ct = "Content-Type: " + content_type;
    h = curl_slist_append(h, ct.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return resp;
}

std::string ViHealthCloudClient::httpGet(const std::string& url, long& status) {
    CURL* c = curl_easy_init();
    if (!c) { status = 0; return ""; }
    std::string resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return resp;
}

std::vector<uint8_t> ViHealthCloudClient::httpGetBinary(const std::string& url, long& status) {
    CURL* c = curl_easy_init();
    if (!c) { status = 0; return {}; }
    std::vector<uint8_t> buf;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeBinCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return buf;
}

// ── Constructor / Destructor ──────────────────────────────────────────────

ViHealthCloudClient::ViHealthCloudClient(const Config& cfg) : cfg_(cfg) {
    if (cfg_.base_url.empty()) cfg_.base_url = "https://ai.viatomtech.com";
    // Strip trailing slash for consistent URL building
    if (!cfg_.base_url.empty() && cfg_.base_url.back() == '/')
        cfg_.base_url.pop_back();
    std::cout << "ViHealth: client created (base=" << cfg_.base_url
              << ", email=" << cfg_.email << ")" << std::endl;
}

ViHealthCloudClient::~ViHealthCloudClient() = default;

// ── Auth ──────────────────────────────────────────────────────────────────

bool ViHealthCloudClient::login() {
    if (cfg_.email.empty() || cfg_.password.empty()) {
        std::cerr << "ViHealth: no credentials configured" << std::endl;
        return false;
    }

    // Build request body — the app sends only email, password, and timezone
    // (offset in minutes). No platform/brand/deviceId.
    std::map<std::string, std::string> body;
    body["email"] = cfg_.email;
    body["password"] = cfg_.password;

    // Timezone offset in minutes (matches Java: (DST_OFFSET + ZONE_OFFSET) / 1000 / 60)
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    int tz_offset_min = -(lt->tm_gmtoff / 60);  // GMT offset in minutes (negative = east of UTC)
    body["timezone"] = std::to_string(tz_offset_min);

    SignedBody sb = buildSignedBody(body);

    // Build URL
    std::string url = cfg_.base_url + "/login/new";

    // Build headers — for login (no token), the SignInterceptor sends
    // timeStamp + sign only (no Authorization header).
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string resp;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, ("timeStamp: " + sb.timestamp).c_str());
    h = curl_slist_append(h, ("sign: " + sb.sign).c_str());
    h = curl_slist_append(h, "Connection: close");
    h = curl_slist_append(h, "platform: 1");
    h = curl_slist_append(h, "version: 2.75.63");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, sb.json.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    long status = 0;
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (status != 200) {
        std::cerr << "ViHealth: login failed (HTTP " << status << ")" << std::endl;
        if (!resp.empty()) std::cerr << "ViHealth: login response: " << resp << std::endl;
        http_fail_log_.onFailure("login HTTP " + std::to_string(status));
        return false;
    }

    json j;
    try {
        j = json::parse(resp, nullptr, false);
    } catch (...) {
        j = json::value_t::discarded;
    }
    if (j.is_discarded() || !j.contains("data")) {
        std::cerr << "ViHealth: login response parse error: " << resp << std::endl;
        return false;
    }

    auto& data = j["data"];
    if (!data.contains("token") || !data.contains("userId")) {
        std::cerr << "ViHealth: login response missing token/userId: " << resp << std::endl;
        return false;
    }

    token_ = data["token"].get<std::string>();
    user_id_ = data["userId"].get<std::string>();
    if (data.contains("countryCode"))
        country_code_ = data["countryCode"].get<std::string>();
    std::cout << "ViHealth: login successful (userId=" << user_id_
              << ", country=" << country_code_ << ")" << std::endl;
    http_fail_log_.onSuccess();
    return true;
}

bool ViHealthCloudClient::resolveRegion() {
    if (country_code_.empty()) return true;  // nothing to resolve, keep default

    // Build request: the app sends an empty body map for this call.
    std::map<std::string, std::string> body;
    SignedBody sb = buildSignedBody(body);
    std::string url = cfg_.base_url + "/node/country/node/get?country=" + country_code_;

    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string resp;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, ("Authorization: " + token_).c_str());
    h = curl_slist_append(h, ("timeStamp: " + sb.timestamp).c_str());
    h = curl_slist_append(h, ("sign: " + sb.sign).c_str());
    h = curl_slist_append(h, "Connection: close");
    h = curl_slist_append(h, "platform: 1");
    h = curl_slist_append(h, "version: 2.75.63");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, sb.json.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    long status = 0;
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (status != 200) return true;  // keep default — don't fail on region miss

    json j;
    try { j = json::parse(resp, nullptr, false); } catch (...) { return true; }
    if (j.is_discarded() || !j.contains("data")) return true;
    auto& d = j["data"];
    if (!d.contains("nodeUrl")) return true;
    auto& nu = d["nodeUrl"];
    if (!nu.contains("vihealth")) return true;
    std::string region_url = nu["vihealth"].get<std::string>();
    if (!region_url.empty() && region_url != cfg_.base_url) {
        // Strip trailing slash for consistency
        if (!region_url.empty() && region_url.back() == '/')
            region_url.pop_back();
        std::cout << "ViHealth: region routing " << country_code_
                  << " -> " << region_url << std::endl;
        cfg_.base_url = region_url;
    }
    return true;
}

bool ViHealthCloudClient::ensureToken() {
    if (!token_.empty()) return true;
    if (!login()) return false;
    // After a fresh login, resolve the regional server for this account.
    // The app does this once after login; the resolved base_url sticks for
    // the session lifetime.
    resolveRegion();
    return true;
}

// ── IO2RingClient interface ───────────────────────────────────────────────

bool ViHealthCloudClient::isConnected() {
    return ensureToken();
}

IO2RingClient::LiveReading ViHealthCloudClient::getLive() {
    // Cloud has no live stream — it's session-based with minutes-to-hours lag.
    LiveReading r;
    r.active = false;
    r.valid = false;
    return r;
}

std::vector<std::string> ViHealthCloudClient::listFiles() {
    std::vector<std::string> result;
    if (!ensureToken()) return result;

    file_url_cache_.clear();

    int page = 1;
    const int page_size = 20;
    bool has_more = true;

    while (has_more) {
        std::map<std::string, std::string> body;
        body["userId"] = user_id_;
        body["current"] = std::to_string(page);
        body["size"] = std::to_string(page_size);
        if (last_cursor_ms_ > 0)
            body["startTime"] = std::to_string(last_cursor_ms_ + 1);

        SignedBody sb = buildSignedBody(body);
        std::string url = cfg_.base_url + "/v1/oxygen/data/async";

        CURL* c = curl_easy_init();
        if (!c) break;
        std::string resp;
        struct curl_slist* h = nullptr;
        h = curl_slist_append(h, "Accept: application/json");
        h = curl_slist_append(h, "Content-Type: application/json");
        h = curl_slist_append(h, ("Authorization: " + token_).c_str());
        h = curl_slist_append(h, ("timeStamp: " + sb.timestamp).c_str());
        h = curl_slist_append(h, ("sign: " + sb.sign).c_str());
        h = curl_slist_append(h, "Connection: close");
        h = curl_slist_append(h, "platform: 1");
        h = curl_slist_append(h, "version: 2.75.63");

        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, sb.json.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        long status = 0;
        curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);

        if (status == 401) {
            std::cerr << "ViHealth: token expired, re-login needed" << std::endl;
            token_.clear();
            return result;
        }
        if (status != 200) {
            std::cerr << "ViHealth: listFiles HTTP " << status << std::endl;
            http_fail_log_.onFailure("listFiles HTTP " + std::to_string(status));
            return result;
        }

        json j;
        try {
            j = json::parse(resp, nullptr, false);
        } catch (...) {
            j = json::value_t::discarded;
        }
        if (j.is_discarded() || !j.contains("data")) break;

        auto& data = j["data"];
        if (!data.contains("records") || !data["records"].is_array()) break;

        auto& records = data["records"];
        if (records.empty()) break;

        for (const auto& rec : records) {
            std::string data_tag = rec.value("id", "");
            std::string file_url = rec.value("originalFileUrl", "");
            std::string filename = rec.value("fileName", data_tag);
            long long measure_time = rec.value("measureTime", 0LL);

            if (data_tag.empty()) continue;

            // Use filename as the identifier (what OximetryService expects).
            // Fall back to data_tag if fileName is empty.
            std::string id = filename.empty() ? data_tag : filename;
            result.push_back(id);

            if (!file_url.empty())
                file_url_cache_[id] = file_url;
            else
                file_url_cache_[id] = "";  // no raw file available

            if (measure_time > last_cursor_ms_)
                last_cursor_ms_ = measure_time;
        }

        // Pagination: check if we got a full page
        int total = data.value("total", 0);
        has_more = (page * page_size) < total;
        page++;
    }

    http_fail_log_.onSuccess();
    std::cout << "ViHealth: listFiles returned " << result.size() << " sessions"
              << (last_cursor_ms_ > 0 ? " (cursor advanced)" : "") << std::endl;
    return result;
}

std::vector<uint8_t> ViHealthCloudClient::downloadFile(const std::string& filename) {
    // Resolve filename → originalFileUrl from cache. If not in cache (e.g.
    // called after a fresh listFiles), return empty — the caller skips it.
    auto it = file_url_cache_.find(filename);
    if (it == file_url_cache_.end() || it->second.empty()) {
        std::cerr << "ViHealth: no file URL for " << filename << std::endl;
        return {};
    }

    long status = 0;
    std::vector<uint8_t> data = httpGetBinary(it->second, status);
    if (status != 200 || data.empty()) {
        std::cerr << "ViHealth: download " << filename << " failed (HTTP "
                  << status << ")" << std::endl;
        return {};
    }

    std::cout << "ViHealth: downloaded " << filename << " (" << data.size()
              << " bytes)" << std::endl;
    return data;
}

} // namespace hms_cpap