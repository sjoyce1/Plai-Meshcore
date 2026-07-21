#pragma once

#include "MeshCore.h"
#include "Utils.h"
#include "esp_random.h"
#include <sys/time.h>
#include "esp_timer.h"

namespace mesh {

class EspRNG : public RNG {
public:
  void random(uint8_t* dest, size_t sz) override {
    esp_fill_random(dest, sz);
  }
};

class EspRTCClock : public RTCClock {
public:
  uint32_t getCurrentTime() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint32_t)tv.tv_sec;
  }
  
  void setCurrentTime(uint32_t time) override {
    struct timeval tv = { .tv_sec = (time_t)time, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
  }
};

class EspMillisecondClock : public MillisecondClock {
public:
  unsigned long getMillis() override {
    return (unsigned long)(esp_timer_get_time() / 1000);
  }
};

}
