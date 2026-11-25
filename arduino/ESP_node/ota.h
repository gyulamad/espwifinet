// ota.h — the node half of over-the-air firmware updates: check the relay's
// version endpoint, download
// the image when it differs from FW_VERSION, flash it with Update, restart.
//
//   GET  http://<relay>:<port>/firmware/version  ->  "<version>\n<md5>\n"
//   GET  http://<relay>:<port>/firmware/node.bin
//
// The check runs from loop() on a timer (OTA_CHECK_SECONDS); flashing blocks
// for a few seconds, which is acceptable for this base project — the relay
// simply shows the node offline until it reboots back. Every step is logged
// through RemoteLog so the dashboard's Logs page tells the whole story.

#pragma once

#include <Arduino.h>
#include "board.h"
#include <WiFiClient.h>
#include <Update.h>

#include "remote_log.h"

namespace espnet {

const uint32_t OTA_CHECK_SECONDS = 60;   // between version checks
const uint32_t OTA_RETRY_SECONDS = 300;  // extra wait after a failed update
const size_t OTA_VERSION_REPLY_MAX = 128;

class OtaUpdater {
 public:
  void begin(const char* host, uint16_t httpPort, RemoteLog* log) {
    host_ = host;
    httpPort_ = httpPort;
    log_ = log;
    lastCheckMs_ = millis() - OTA_CHECK_SECONDS * 1000; // check shortly after boot
    nextDelaySeconds_ = OTA_CHECK_SECONDS;
  }

  // Called from loop(); does the whole check+flash when the timer fires.
  void tick(const char* currentVersion) {
    const uint32_t intervalMs = nextDelaySeconds_ * 1000;
    if (millis() - lastCheckMs_ < intervalMs) return;
    lastCheckMs_ = millis();

    if (WiFi.status() != WL_CONNECTED) return;

    char servedVersion[24];
    if (!fetchVersion(servedVersion, sizeof(servedVersion))) {
      log_->logf(LOG_WARN, "ota: version check failed, retry in %lu s", (unsigned long)OTA_RETRY_SECONDS);
      nextDelaySeconds_ = OTA_RETRY_SECONDS;
      return;
    }
    nextDelaySeconds_ = OTA_CHECK_SECONDS;

    if (strcmp(servedVersion, currentVersion) == 0) return; // up to date
    log_->logf(LOG_INFO, "ota: relay serves %s, running %s — updating", servedVersion, currentVersion);

    if (flashFirmware()) {
      log_->logf(LOG_INFO, "ota: flashed OK, restarting");
      delay(500); // give the log a moment to reach the relay
      ESP.restart();
    } else {
      log_->logf(LOG_ERROR, "ota: flash failed, retry in %lu s", (unsigned long)OTA_RETRY_SECONDS);
      nextDelaySeconds_ = OTA_RETRY_SECONDS;
    }
  }

 private:
  bool fetchVersion(char* out, size_t outLen) {
    WiFiClient client;
    HTTPClient http;
    // #131: the chip rides in the query so the relay serves the version of the
    // image built for THIS board; the relay keeps the chip-less URL answering
    // the esp8266 image for the nodes deployed before the split.
    String url = String("http://") + host_ + ":" + String(httpPort_) + "/firmware/version?chip=" + ESPNET_CHIP_NAME;
    if (!http.begin(client, url)) return false;
    http.setTimeout(5000);
    const int code = http.GET();
    if (code != 200) {
      http.end();
      return false;
    }
    // reply shape: "<version>\n<md5>\n" — the version is the first line
    String reply = http.getString();
    http.end();
    if (reply.length() == 0 || reply.length() > OTA_VERSION_REPLY_MAX) return false;
    const int nl = reply.indexOf('\n');
    String version = (nl == -1) ? reply : reply.substring(0, nl);
    version.trim();
    if (version.length() == 0 || version.length() >= (unsigned int)outLen) return false;
    snprintf(out, outLen, "%s", version.c_str());
    return true;
  }

  bool flashFirmware() {
    WiFiClient client;
    HTTPClient http;
    String url = String("http://") + host_ + ":" + String(httpPort_) + ESPNET_FIRMWARE_PATH;
    if (!http.begin(client, url)) return false;
    http.setTimeout(15000);
    const int code = http.GET();
    if (code != 200) {
      log_->logf(LOG_ERROR, "ota: image download http %d", code);
      http.end();
      return false;
    }
    const int size = http.getSize();
    if (size <= 0) {
      log_->logf(LOG_ERROR, "ota: unknown image size");
      http.end();
      return false;
    }

    if (!Update.begin(size)) {
      log_->logf(LOG_ERROR, "ota: not enough flash for %d bytes", size);
      http.end();
      return false;
    }
    const size_t written = Update.writeStream(client);
    const bool ok = Update.end(true) && written == (size_t)size;
    if (!ok) {
      log_->logf(LOG_ERROR, "ota: wrote %u/%d bytes, err %s", (unsigned)written, size,
                 Update.errorString());
      Update.abort();
    }
    http.end();
    return ok;
  }

  const char* host_ = nullptr;
  uint16_t httpPort_ = 80;
  RemoteLog* log_ = nullptr;
  uint32_t lastCheckMs_ = 0;
  uint32_t nextDelaySeconds_ = OTA_CHECK_SECONDS;
};

} // namespace espnet
