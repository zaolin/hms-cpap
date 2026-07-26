# ViHealth Cloud API — Reverse-Engineered from ViHealth 2.75.63

Source: decompiled `com.viatom.vihealth.apk` (v2.75.63, code 330).
All findings from `com/viatom/baselib/net/` package via jadx.

## Base URL

- Production (US): `https://ai.viatomtech.com/`
- Production (EU alt): `https://eu-cloud.viatomtech.com` (referenced in official docs)
- Debug/Test: `https://testai.viatomtech.com/`
- Lepu dev: `https://testai.lepudev.com/`

`NodeConfig.java:38-47` declares these as constants. `curBaseUrl` is selected at
runtime from a per-user `NodeInfo` (region-based routing — EU users get the EU
server). For our integration: default to US, allow override via config.

## Authentication

There are **two** auth schemes, depending on URL host (`SignInterceptor.java:199-205`):

### 1. ViHealth main API (default — `ai.viatomtech.com`)
- Header: `Authorization: <token>` (token from login, no `Bearer` prefix)
- Header: `timeStamp: <unix_ms>` (current epoch millis)
- Header: `sign: <MD5_UPPER>` (see Sign computation below)
- Header: `timezone: <minutes>` (TZ offset in minutes, e.g. `-120` for UTC+2)
- Header: `platform: 1` (always 1 = Android)
- Header: `version: <app_version_name>` (e.g. `2.75.63`)
- Header: `Connection: close`

### 2. Remote linker / FHIR API (`cloud.viatomtech.com`)
- Header: `Authorization: <remoteToken>` (separate token, stored as
  `cur_login_remote_authorization` in DataStore)
- Header: `Accept: application/json+fhir`
- Header: `Content-Type: application/json+fhir`

For our use case (pulling O2Ring session data), we use **scheme #1**.

### Sign computation (`SignInterceptor.java:183-204`)

```
treeMap = all request body params (JSON fields flattened to Map<String,String>)
        + query params
        + form params
treeMap.put("salt", SECRET)    // hardcoded value below
treeMap.put("timeStamp", currentTimeMs)

json = toJSON(treeMap)         // sorted map -> JSON string
// For "scale/batch/data/update" endpoint only:
//   md5_hex = md5(json).toUpperCase()
// For all other endpoints:
//   md5_hex = md5(json.replace("\\\\", "")).toUpperCase()

sign = md5_hex
```

`SECRET = "a64255ab64344fb99612badde43d5365"` (`SignInterceptor.java:52`)

The `treeMap` is a `java.util.TreeMap` — keys are sorted alphabetically before
JSON serialization, so the MD5 is deterministic. The sign covers: all body
fields, `salt`, `timeStamp`. Query params are added to the treeMap for signing
too.

For list payloads (`RetrofitAsk<List<T>>`), the `list` field is a JSON array
string in the map, with `measureTime` converted from long to ISO-8601 string
(`yyyy-MM-dd'T'HH:mm:ss'Z'`) before signing.

## Login flow

`CommonService.java:647-650`:
```java
@Headers({"url_name:login"})
@POST("login/new")
Object login(@Body Map<String, Object> map, Continuation<RetrofitRes<LoginUser>>)
```

Request body fields (from `LoginHelper.getLoginParam`):
- `email` — user email
- `password` — user password (plaintext over TLS)
- `platform` — `"1"` (Android)
- `brand` — e.g. `"VIATOM"` (or `"LEPU"` depending on build)
- `deviceId` — Android device ID

Response (`LoginUser`):
- `token` — auth token (used as `Authorization` header)
- `userId` — used in subsequent requests as `userId` query/body param
- `email`, `name`, `countryCode`, `memberFlag`, `openTime`, `endTime`

Token is stored in DataStore key `cur_login_user_info_5_5` (a JSON-serialized
`LoginUser`). Token has no known expiry mechanism in the client — it's used
until logout. (Server may expire it; client just retries login on 401.)

## Key endpoints for O2Ring ingest

All POST, all require auth headers above. Base URL is prepended.

### 1. List SpO2 sessions (download metadata)
`CommonService.java:543-545`
```java
@POST("v1/oxygen/data/async")
Object getOxygenDataList(@Body Map<String, Object> map,
    Continuation<RetrofitRes<SyncRes<BloodOxyData>>>)
```
Request body:
- `userId` — user ID from login
- `current` — page number (1-based)
- `size` — page size (e.g. 20)
- `startTime` — optional: epoch ms
- `endTime` — optional: epoch ms

Response (`SyncRes<BloodOxyData>`):
- `records: List<BloodOxyData>`
- `total: int`
- `size: int`
- `current: int`

### 2. BloodOxyData fields (the SpO2 session record)
`com/vihealth/db/room/BloodOxyData.java`:
- `id` (SerializedName "id") — server record ID
- `userId`, `memberId`
- `deviceSn`, `deviceName`, `deviceType`, `deviceMacAddress`
- `fileName` — local file name (e.g. `20260412065307.vld`)
- `measureTime` (long, epoch ms) — session start
- `measureDuration` (int, seconds)
- `averageSpo2`, `maxSpo2`, `lowestSpo2` (int)
- `averagePr`, `maxPr`, `minPr` (int)
- `o2Score` (String)
- `low90Count`, `low90PercentDuration` (int)
- `m3PercentOdi`, `m4PercentOdi` (int)
- `movementList: List<Integer>`, `prList: List<Integer>`
- `interval` (int, seconds — sampling interval)
- `originalFileUrl` (String) — **URL to the raw .vld/.dat file on S3**
- `lead` (int)
- `deepSleepDuration`, `lightSleepDuration`, `awakeDuration` (int)
- `createTime`, `isDeleted`, `isRead`, `isUploaded`, `isGoogleFitUploaded`
- `branchCode`, `remark`
- `listShowingState`

### 3. Upload SpO2 data (NOT needed for ingest — but documents the payload)
`CommonService.java:853-855`
```java
@POST("v1/oxygen/upload")
Object uploadOxygenData(@Body RequestBody, Continuation<RetrofitRes<List<BloodOxyData>>>)
```
Body is a multipart/JSON containing the raw file bytes + metadata.

### 4. Download raw file
`BloodOxyData.originalFileUrl` is a direct S3 URL:
`https://elasticbeanstalk-us-west-2-697648770036.s3.us-west-2.amazonaws.com/...`
No auth needed — plain HTTPS GET returns the `.vld`/`.dat` bytes.

### 5. Delete a record (if we ever need to)
`CommonService.java:275-281`
```java
@POST("v1/oxygen/delete")      // single by id
@POST("v1/oxygen/batch/delete") // batch
```

## Retrofit setup (`RetrofitHelperKt.java`)

```kotlin
OkHttpClient.Builder()
    .addInterceptor(BaseUrlInterceptor())        // swaps base URL per @Headers("url_name:...")
    .addInterceptor(BasicParamsInterceptor(      // adds timezone, platform, version, Connection: close
        timezone = (rawOffset + dstOffset) / 1000 / 60,
        Connection = "close",
        platform = "1",
        version = appVersionName))
    .addInterceptor(SignInterceptor(context))    // adds Authorization, timeStamp, sign
    .addInterceptor(HttpLoggingInterceptor(NONE))
    .connectTimeout(120s).readTimeout(120s).writeTimeout(120s)
    .connectionPool(ConnectionPool(0, 5min))
    .retryOnConnectionFailure(false)
    .hostnameVerifier(UnSafeHostnameVerifier)    // accepts any host — for testing
    .build()
```

`BaseUrlInterceptor` reads the `url_name` header on each request and swaps the
base URL:
- `url_name:login` → login server
- `url_name:link` → linker server
- `url_name:node` → node config server
- (none) → `curBaseUrl` (the user's region server)

For our integration: ignore the URL swapping — just hit `ai.viatomtech.com`
directly for everything except login (which goes to the same host anyway).

## Implementation notes for ViHealthCloudClient

1. **No OAuth refresh**: token is long-lived. Store it in config.json (or a
   state file). Re-login only when a 401 is returned.

2. **Sign must be computed for every request**: the MD5 covers the request
   body + `salt` + `timeStamp`. TreeMap sorts keys alphabetically. This is
   the trickiest part to replicate in C++ — need a deterministic JSON
   serializer with sorted keys.

3. **`RetrofitAsk` wrapper**: list endpoints wrap the payload as
   `{ "list": [...], "userId": "..." }`. Sign computation handles the list
   specially (converts `measureTime` longs to ISO strings inside the array
   before signing).

4. **For ingest-only**: we don't need upload. We need:
   - `login/new` → get token
   - `v1/oxygen/data/async` → list sessions (paginated)
   - HTTPS GET on `originalFileUrl` → download raw `.vld` bytes
   - Feed bytes to existing `VLDParser::parse()` → `OximetrySession`
   - `saveOximetrySession("vihealth", session)` to DB

5. **Cursor**: track the highest `measureTime` (or `id`) seen; on next poll,
   query `startTime > lastCursor`. Persist to `vihealth_state.json`.

6. **Rate limiting**: no explicit limits observed in the client. Be polite —
   10-minute poll interval is plenty (cloud data lags behind the app anyway).

7. **EU users**: if the user is on the EU server, `login/new` returns a
   `countryCode` that the app uses to fetch a `NodeInfo` from
   `node/country/node/get` which redirects to `eu-cloud.viatomtech.com`. The
   ViHealthCloudClient does this **automatically** — after login it calls
   `resolveRegion()` which fetches the user's regional server URL and updates
   `base_url` in-place. No user configuration needed beyond email/password.

## Files referenced (in decompiled tree at /tmp/vihealth_jadx/sources)

- `com/viatom/baselib/net/NodeConfig.java` — base URLs
- `com/viatom/baselib/net/CommonService.java` — Retrofit interface (all endpoints)
- `com/viatom/baselib/net/remote/SignInterceptor.java` — auth + signing
- `com/viatom/baselib/net/remote/RetrofitHelperKt.java` — OkHttp client setup
- `com/viatom/baselib/net/remote/BasicParamsInterceptor.java` — timezone/platform headers
- `com/viatom/baselib/net/remote/BaseUrlInterceptor.java` — URL swapping
- `com/viatom/baselib/user/LoginHelper.java` — token storage/retrieval
- `com/viatom/baselib/data/user/LoginUser.java` — login response shape
- `com/viatom/baselib/net/RetrofitAsk.java` — `{list, userId}` wrapper
- `com/viatom/baselib/net/SyncRes.java` — paginated response `{records, total, size, current}`
- `com/vihealth/db/room/BloodOxyData.java` — SpO2 session record shape