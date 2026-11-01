#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"

#include "protocol.h"

namespace esphome {
namespace kaeltebringer {

enum class SensorType : uint8_t {
  ROOM_TEMPERATURE,
  OUTLET_TEMPERATURE,
  TARGET_TEMPERATURE,
  ENERGY,
  COMPRESSOR_TICKS,
  ON_TIMER_MINUTES,
  OFF_TIMER_MINUTES,
};
constexpr size_t SENSOR_TYPE_COUNT = 7;

enum class BinarySensorType : uint8_t {
  INDOOR_ACTIVE,
  MUTE,
  ON_TIMER_ARMED,
  OFF_TIMER_ARMED,
  ENERGY_AVAILABLE,
};
constexpr size_t BINARY_SENSOR_TYPE_COUNT = 5;

enum class TextSensorType : uint8_t {
  COMPRESSOR_STATE,
  FAN_SPEED,
};
constexpr size_t TEXT_SENSOR_TYPE_COUNT = 2;

enum class SwitchType : uint8_t {
  ECO,
  HEALTH,
  DISPLAY_PANEL,  // not "DISPLAY": that is an Arduino macro
  HEATER_8C,
};
constexpr size_t SWITCH_TYPE_COUNT = 4;

class KaeltebringerClimate;

class KaeltebringerSwitch : public switch_::Switch {
 public:
  void set_parent(KaeltebringerClimate *parent) { this->parent_ = parent; }
  void set_type(SwitchType type) { this->type_ = type; }

 protected:
  void write_state(bool state) override;

  KaeltebringerClimate *parent_{nullptr};
  SwitchType type_{SwitchType::ECO};
};

class KaeltebringerClimate : public PollingComponent, public climate::Climate, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void set_beep_enabled(bool enabled) { this->beep_enabled_ = enabled; }
  void set_diagnostic_interval(uint32_t interval_ms) { this->diagnostic_interval_ms_ = interval_ms; }
  void set_sensor(SensorType type, sensor::Sensor *entity) { this->sensors_[static_cast<size_t>(type)] = entity; }
  void set_binary_sensor(BinarySensorType type, binary_sensor::BinarySensor *entity) {
    this->binary_sensors_[static_cast<size_t>(type)] = entity;
  }
  void set_text_sensor(TextSensorType type, text_sensor::TextSensor *entity) {
    this->text_sensors_[static_cast<size_t>(type)] = entity;
  }
  void set_switch(SwitchType type, KaeltebringerSwitch *entity) {
    entity->set_parent(this);
    entity->set_type(type);
    this->switches_[static_cast<size_t>(type)] = entity;
  }

  void set_extra_control(SwitchType type, bool enabled);

 protected:
  // Diagnostics only get answered when chained onto a completed GET exchange:
  // GET -> state, ~180 ms, 0x0A -> energy, ~190 ms, 0x09 -> status.
  enum class DiagnosticPhase : uint8_t {
    IDLE,
    ENERGY_PENDING,  // waiting to send 0x0A
    ENERGY_SENT,     // waiting for the 0x0A reply
    STATUS_PENDING,  // waiting to send 0x09
    STATUS_SENT,     // waiting for the 0x09 reply
  };

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void send_frame_(uint8_t command, const uint8_t *payload, size_t length);
  void send_get_();
  void send_set_();
  void handle_frame_(const protocol::Frame &frame);
  void handle_state_(const uint8_t *payload);
  void handle_energy_(const uint8_t *payload, size_t length);
  void start_diagnostics_(uint32_t now);
  void process_diagnostics_(uint32_t now);

  void publish_(SensorType type, float value) {
    auto *entity = this->sensors_[static_cast<size_t>(type)];
    if (entity != nullptr)
      entity->publish_state(value);
  }
  void publish_(BinarySensorType type, bool value) {
    auto *entity = this->binary_sensors_[static_cast<size_t>(type)];
    if (entity != nullptr)
      entity->publish_state(value);
  }
  void publish_(TextSensorType type, const char *value) {
    auto *entity = this->text_sensors_[static_cast<size_t>(type)];
    if (entity != nullptr)
      entity->publish_state(value);
  }
  void publish_(SwitchType type, bool value) {
    auto *entity = this->switches_[static_cast<size_t>(type)];
    if (entity != nullptr)
      entity->publish_state(value);
  }

  protocol::FrameParser parser_;
  protocol::Extras extras_;
  uint8_t set_payload_[protocol::SET_PAYLOAD_SIZE]{};

  bool beep_enabled_{true};
  bool state_valid_{false};
  bool pending_set_{false};
  bool diagnostics_enabled_{false};
  DiagnosticPhase diagnostic_phase_{DiagnosticPhase::IDLE};
  uint32_t diagnostic_interval_ms_{300000};
  uint32_t next_diagnostic_ms_{0};
  uint32_t phase_deadline_ms_{0};
  uint32_t last_get_ms_{0};

  sensor::Sensor *sensors_[SENSOR_TYPE_COUNT]{};
  binary_sensor::BinarySensor *binary_sensors_[BINARY_SENSOR_TYPE_COUNT]{};
  text_sensor::TextSensor *text_sensors_[TEXT_SENSOR_TYPE_COUNT]{};
  KaeltebringerSwitch *switches_[SWITCH_TYPE_COUNT]{};
};

}  // namespace kaeltebringer
}  // namespace esphome
