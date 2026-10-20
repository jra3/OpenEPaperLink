// Cloud sync: the AP pulls a manifest of tag -> image from the cloud and pushes
// tag check-ins to it. Every connection is outbound over HTTPS.
//
// Manifest: GET {url}/v1/ap/{ap}/manifest, If-None-Match -> 304, or
//   {"v":1,"poll_s":60,"tags":{"<MAC>":{"img":"img/<hash>.jpg","wake_s":600}}}
// Images are immutable and named by content hash. A tag that is not in the
// manifest is left alone. A tag in it is put in CONTENTMODE_CLOUD with
// {"filename":"/cloud/<hash>.jpg"}, which drawNew converts as a static image.
//
// Events: POST {url}/v1/ap/{ap}/events {"events":[...]}; the reply carries the
// manifest when it changed.

#include "cloudsync.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <set>

#include "cloud_ca.h"
#include "storage.h"
#include "system.h"
#include "tag_db.h"
#include "web.h"

struct CloudEvent {
    uint8_t mac[8];
    uint32_t t;
    uint8_t reason;
    uint16_t batteryMv;
    int8_t temperature;
    int8_t rssi;
    uint8_t lqi;
};

static QueueHandle_t eventQueue = xQueueCreate(64, sizeof(CloudEvent));

static constexpr uint32_t EVENT_FLUSH_MS = 30000;  // check-ins wait at most this long; presses go at once
static constexpr uint32_t RETRY_MS = 30000;

struct CloudConfig {
    String url;
    String ap;
    String token;
};

static bool isPress(uint8_t reason) {
    return reason == WAKEUP_REASON_BUTTON1 || reason == WAKEUP_REASON_BUTTON2 || reason == WAKEUP_REASON_BUTTON3 ||
           reason == WAKEUP_REASON_NFC || reason == WAKEUP_REASON_GPIO;
}

void cloudsyncEvent(const uint8_t mac[8], const struct AvailDataReq& adr, uint32_t now) {
    CloudEvent e;
    memcpy(e.mac, mac, 8);
    e.t = now;
    e.reason = adr.wakeupReason;
    e.batteryMv = adr.batteryMv;
    e.temperature = adr.temperature;
    e.rssi = adr.lastPacketRSSI;
    e.lqi = adr.lastPacketLQI;
    if (xQueueSend(eventQueue, &e, 0) != pdTRUE) {
        // full: the cloud is unreachable. Keep the newest.
        CloudEvent dropped;
        xQueueReceive(eventQueue, &dropped, 0);
        xQueueSend(eventQueue, &e, 0);
    }
}

static bool loadConfig(CloudConfig& cfg) {
    JsonDocument doc;
    {
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        fs::File f = contentFS->open("/cloud.json", "r");
        if (!f) {
            xSemaphoreGive(fsMutex);
            return false;
        }
        const DeserializationError err = deserializeJson(doc, f);
        f.close();
        xSemaphoreGive(fsMutex);
        if (err) return false;
    }
    cfg.url = doc["url"].as<String>();
    cfg.ap = doc["ap"].as<String>();
    cfg.token = doc["token"].as<String>();
    while (cfg.url.endsWith("/")) cfg.url.remove(cfg.url.length() - 1);
    return cfg.url.startsWith("https://") && cfg.ap.length() && cfg.token.length();
}

static void beginRequest(HTTPClient& http, WiFiClientSecure& client, const CloudConfig& cfg, const String& path) {
    http.begin(client, cfg.url + "/v1/ap/" + cfg.ap + path);
    http.addHeader("Authorization", "Bearer " + cfg.token);
    http.setTimeout(10000);
}

static String localImagePath(const String& img) {
    // "img/<hash>.jpg" -> "/cloud/<hash>.jpg"
    return "/cloud/" + img.substring(img.lastIndexOf('/') + 1);
}

static bool fileExists(const String& path) {
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    const bool exists = contentFS->exists(path);
    xSemaphoreGive(fsMutex);
    return exists;
}

static bool downloadImage(WiFiClientSecure& client, const CloudConfig& cfg, const String& img, const String& path) {
    HTTPClient http;
    beginRequest(http, client, cfg, "/" + img);
    const int code = http.GET();
    bool ok = false;
    if (code == 200) {
        const String tmp = path + ".tmp";
        {
            // LockedFile: don't hold fsMutex while waiting for the network
            LockedFile f(tmp, "w");
            ok = f && http.writeToStream(&f) > 0;
        }
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        ok = ok && contentFS->rename(tmp, path);
        if (!ok) contentFS->remove(tmp);
        xSemaphoreGive(fsMutex);
    }
    http.end();
    if (!ok) logLineRateLimited("cloudimg", "cloud: image " + img + " failed, http " + String(code), 600);
    return ok;
}

// Apply a manifest. Returns false when a tag could not be set up yet (unknown to
// this AP, or its image failed), so the manifest is fetched again next poll.
static bool applyManifest(WiFiClientSecure& client, const CloudConfig& cfg, JsonObject manifest, uint32_t& pollSeconds) {
    if (manifest["poll_s"].is<uint32_t>()) pollSeconds = max<uint32_t>(manifest["poll_s"].as<uint32_t>(), 10);

    bool complete = true;
    std::set<String> keep;
    for (JsonPair kv : manifest["tags"].as<JsonObject>()) {
        const String img = kv.value()["img"].as<String>();
        const String path = localImagePath(img);
        keep.insert(path.substring(7));

        uint8_t mac[8];
        if (!hex2mac(String(kv.key().c_str()), mac)) continue;
        tagRecord* taginfo = tagRecord::findByMAC(mac);
        if (taginfo == nullptr) {
            complete = false;
            continue;
        }

        JsonDocument cfgdoc;
        cfgdoc["filename"] = path;
        cfgdoc["dither"] = 0;  // the cloud renders in the tag's palette
        String modeConfig;
        serializeJson(cfgdoc, modeConfig);
        {
            TagDBLock lock;
            if (taginfo->contentMode == CONTENTMODE_CLOUD && taginfo->modeConfigJson == modeConfig) continue;
        }

        if (!fileExists(path) && !downloadImage(client, cfg, img, path)) {
            complete = false;
            continue;
        }

        {
            TagDBLock lock;
            taginfo->contentMode = CONTENTMODE_CLOUD;
            taginfo->modeConfigJson = modeConfig;
            taginfo->nextupdate = 0;
        }
        wsLog("cloud: " + String(kv.key().c_str()) + " -> " + img);
        wsSendTaginfo(mac, SYNC_NOSYNC);
    }

    // drop images no tag in the manifest uses
    std::vector<String> stale;
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    fs::File dir = contentFS->open("/cloud");
    if (dir && dir.isDirectory()) {
        for (fs::File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            const String name = f.name();
            if (!keep.count(name)) stale.push_back("/cloud/" + name);
        }
        dir.close();
    }
    for (const String& path : stale) contentFS->remove(path);
    xSemaphoreGive(fsMutex);

    return complete;
}

static int postEvents(WiFiClientSecure& client, const CloudConfig& cfg, std::vector<CloudEvent>& batch, String& etag, JsonDocument& reply) {
    JsonDocument doc;
    JsonArray events = doc["events"].to<JsonArray>();
    char hexmac[17];
    for (const CloudEvent& e : batch) {
        JsonObject o = events.add<JsonObject>();
        mac2hex(e.mac, hexmac);
        o["mac"] = hexmac;
        o["t"] = e.t;
        o["reason"] = e.reason;
        o["batt"] = e.batteryMv;
        o["temp"] = e.temperature;
        o["rssi"] = e.rssi;
        o["lqi"] = e.lqi;
    }
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    beginRequest(http, client, cfg, "/events");
    http.addHeader("Content-Type", "application/json");
    if (etag.length()) http.addHeader("If-None-Match", etag);
    const char* keys[] = {"ETag"};
    http.collectHeaders(keys, 1);
    const int code = http.POST(body);
    if (code == 200) {
        deserializeJson(reply, http.getString());
        if (reply["manifest"].is<JsonObject>()) etag = http.header("ETag");
    }
    http.end();
    return code;
}

static int getManifest(WiFiClientSecure& client, const CloudConfig& cfg, String& etag, JsonDocument& manifest) {
    HTTPClient http;
    beginRequest(http, client, cfg, "/manifest");
    if (etag.length()) http.addHeader("If-None-Match", etag);
    const char* keys[] = {"ETag"};
    http.collectHeaders(keys, 1);
    const int code = http.GET();
    if (code == 200 && !deserializeJson(manifest, http.getString())) etag = http.header("ETag");
    http.end();
    return code;
}

void cloudsyncTask(void* parameter) {
    xSemaphoreTake(fsMutex, portMAX_DELAY);
    if (!contentFS->exists("/cloud")) contentFS->mkdir("/cloud");
    xSemaphoreGive(fsMutex);

    WiFiClientSecure client;
    client.setCACert(cloudRootCA);

    CloudConfig cfg;
    String etag;
    uint32_t pollSeconds = 60;
    uint32_t nextPoll = 0;
    uint32_t nextFlush = 0;
    std::vector<CloudEvent> batch;

    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if (config.runStatus == RUNSTATUS_STOP || WiFi.status() != WL_CONNECTED) continue;
        if (!loadConfig(cfg)) {
            vTaskDelay(30000 / portTICK_PERIOD_MS);
            continue;
        }

        CloudEvent e;
        bool press = false;
        while (batch.size() < 64 && xQueueReceive(eventQueue, &e, 0) == pdTRUE) {
            press |= isPress(e.reason);
            batch.push_back(e);
        }
        if (batch.empty()) nextFlush = millis() + EVENT_FLUSH_MS;

        JsonDocument manifest;
        bool haveManifest = false;
        if (!batch.empty() && (press || batch.size() >= 32 || (int32_t)(millis() - nextFlush) >= 0)) {
            JsonDocument reply;
            const int code = postEvents(client, cfg, batch, etag, reply);
            if (code == 200) {
                batch.clear();
                if (reply["manifest"].is<JsonObject>()) {
                    manifest.set(reply["manifest"]);
                    haveManifest = true;
                }
            } else {
                logLineRateLimited("cloudevents", "cloud: events http " + String(code), 600);
                nextFlush = millis() + RETRY_MS;
            }
        } else if ((int32_t)(millis() - nextPoll) >= 0) {
            const int code = getManifest(client, cfg, etag, manifest);
            haveManifest = code == 200;
            if (code != 200 && code != 304) logLineRateLimited("cloudmanifest", "cloud: manifest http " + String(code), 600);
            nextPoll = millis() + pollSeconds * 1000;
        }

        if (haveManifest && !applyManifest(client, cfg, manifest.as<JsonObject>(), pollSeconds)) {
            etag = "";  // not everything applied: fetch it whole again
        }
    }
}
