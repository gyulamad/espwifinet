// remote_log.h — the node's remote logging. Assembled nodes have no serial
// monitor attached, so every log line is queued and delivered to the relay
// (in-band over the same TCP connection the node already keeps open — see
// net.h) and echoed to Serial while it is still connected by USB.
//
// The same ideas as espcamsys2's remote_log.h, scaled down for a base
// project: a bounded ring buffer (nothing is lost just because the link is
// down — the oldest entries go first when it overflows), repeat suppression
// for spammy messages, and delivery only while the relay connection is up.
// It is deliberately dependency-free and single-threaded: called from loop().

#pragma once

#include <Arduino.h>

namespace espnet {

const size_t LOG_CAPACITY = 24;   // entries buffered between flushes
const size_t LOG_MSG_MAX = 140;   // characters per message
const size_t LOG_BATCH_MAX = 6;   // entries per flush (one TCP write each)

enum LogLevel : uint8_t { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 };

struct LogEntry {
  uint32_t ms;           // millis() when it was logged
  LogLevel level;
  char msg[LOG_MSG_MAX];
};

class RemoteLog {
 public:
  void begin(Stream* serial) {
    serial_ = serial;
    count_ = 0;
    head_ = 0;
    lastMsg_[0] = '\0';
    repeats_ = 0;
    lastRepeatMs_ = 0;
  }

  // Logs one printf-style line. Repeats of the same message inside 5 s are
  // summarised instead of queued (a flapping WiFi link otherwise floods the
  // log file — learned from the sample project).
  void logf(LogLevel level, const char* fmt, ...) {
    char buf[LOG_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const uint32_t now = millis();
    if (strcmp(buf, lastMsg_) == 0 && now - lastRepeatMs_ < 5000) {
      repeats_++;
      lastRepeatMs_ = now;
      return;
    }
    if (repeats_ > 0) {
      enqueue(LOG_WARN, lastRepeatMs_, "(repeated %u more times)", repeats_);
      repeats_ = 0;
    }
    snprintf(lastMsg_, sizeof(lastMsg_), "%s", buf);
    lastRepeatMs_ = now;
    enqueue(level, now, "%s", buf);

    if (serial_ != nullptr) {
      serial_->printf("[%08lu][%s] %s\r\n", now, levelName(level), buf);
    }
  }

  // True when at least one entry is waiting.
  bool pending() const { return count_ > 0; }

  // Pops the next entry into `out` (oldest first). Returns false when empty.
  bool pop(LogEntry& out) {
    if (count_ == 0) return false;
    out = ring_[tail_];
    tail_ = (tail_ + 1) % LOG_CAPACITY;
    count_--;
    return true;
  }

  size_t dropped() const { return dropped_; }

 private:
  void enqueue(LogLevel level, uint32_t ms, const char* fmt, ...) {
    LogEntry& slot = ring_[head_];
    va_list args;
    va_start(args, fmt);
    vsnprintf(slot.msg, sizeof(slot.msg), fmt, args);
    va_end(args);
    slot.ms = ms;
    slot.level = level;
    head_ = (head_ + 1) % LOG_CAPACITY;
    if (count_ < LOG_CAPACITY) {
      count_++;
    } else {
      dropped_++; // ring full: the oldest entry was overwritten
    }
  }

  static const char* levelName(LogLevel level) {
    switch (level) {
      case LOG_WARN: return "warn";
      case LOG_ERROR: return "error";
      default: return "info";
    }
  }

  Stream* serial_ = nullptr;
  LogEntry ring_[LOG_CAPACITY];
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  size_t dropped_ = 0;
  char lastMsg_[LOG_MSG_MAX];
  uint32_t lastRepeatMs_ = 0;
  uint32_t repeats_ = 0;
};

} // namespace espnet
