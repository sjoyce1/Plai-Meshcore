#pragma once

#include "Dispatcher.h"
#include "hal/hal.h"
#include "hal/radio/sx1262.h"

namespace mesh {

class EspRadioWrapper : public Radio {
  HAL::RadioInterface* _radio;
  bool _is_tx_complete;
  bool _has_rx_packet;
  float _last_rssi;
  float _last_snr;

public:
  EspRadioWrapper(HAL::RadioInterface* radio) : _radio(radio), _is_tx_complete(false), _has_rx_packet(false), _last_rssi(0), _last_snr(0) {}

  void begin() override {
    _radio->setEventCallback([this](HAL::RadioEvent event) {
      this->handleRadioEvent(event);
    });
    _radio->setMode(HAL::RadioMode::STANDBY);
  }

  void handleRadioEvent(HAL::RadioEvent event) {
    if (event == HAL::RadioEvent::TX_DONE) {
      _is_tx_complete = true;
      _radio->setMode(HAL::RadioMode::RX); // automatically fallback to RX mode
    } else if (event == HAL::RadioEvent::RX_DONE) {
      _has_rx_packet = true;
    } else if (event == HAL::RadioEvent::RX_TIMEOUT || event == HAL::RadioEvent::RX_ERROR) {
      _radio->startReceive(0); // restart continuous receive
    } else if (event == HAL::RadioEvent::TX_TIMEOUT) {
      _is_tx_complete = true;
      _radio->setMode(HAL::RadioMode::RX);
    }
  }

  int recvRaw(uint8_t* bytes, int sz) override {
    if (!_has_rx_packet) return 0;
    _has_rx_packet = false;

    HAL::RxPacketInfo info;
    int len = _radio->readPacket(bytes, sz, &info);
    if (len > 0) {
      _last_rssi = info.rssi;
      _last_snr = info.snr;
      _radio->startReceive(0); // restart receive mode
      return len;
    }
    return 0;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    HAL::LoRaConfig config = _radio->getConfig();
    uint8_t sf = config.spreading_factor;
    uint32_t bw = config.bandwidth_hz;
    
    // Symbol duration in ms: T_sym = (1 << SF) / BW (in kHz)
    double t_sym = (double)(1 << sf) / (bw / 1000.0);
    
    // Number of symbols for preamble
    double n_preamble = config.preamble_length + 4.25;
    
    // Number of payload symbols (simplified, assuming CRC enabled, explicit header)
    double bits_per_symbol = sf - (sf >= 11 ? 3 : 2); // low data rate optimization applies SF>=11
    double coding_rate_factor = (double)config.coding_rate / 4.0;
    double n_payload = (8.0 * len_bytes) / bits_per_symbol * coding_rate_factor;
    
    uint32_t airtime_ms = (uint32_t)((n_preamble + n_payload) * t_sym);
    return airtime_ms;
  }

  float packetScore(float snr, int packet_len) override {
    float s = (snr + 20.0f) / 25.0f;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    _is_tx_complete = false;
    return _radio->transmit(bytes, len);
  }

  bool isSendComplete() override {
    return _is_tx_complete;
  }

  void onSendFinished() override {
    _is_tx_complete = false;
    _radio->startReceive(0); // restart continuous receive after send is done
  }

  void loop() override {
    _radio->processEvents();
  }

  bool isInRecvMode() const override {
    return _radio->getMode() == HAL::RadioMode::RX;
  }

  bool isReceiving() override {
    return _radio->isBusy() && _radio->getMode() == HAL::RadioMode::RX;
  }

  float getLastRSSI() const override { return _last_rssi; }
  float getLastSNR() const override { return _last_snr; }
};

}
