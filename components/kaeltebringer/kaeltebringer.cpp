#include "kaeltebringer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstdio>

namespace esphome {
namespace kaeltebringer {

static const char *const TAG = "kaeltebringer";

static constexpr uint32_t INITIAL_GET_RETRY_MS = 3000;
static constexpr uint32_t DIAGNOSTIC_FIRST_DELAY_MS = 180;
static constexpr uint32_t DIAGNOSTIC_NEXT_DELAY_MS = 190;
static constexpr uint32_t DIAGNOSTIC_REPLY_TIMEOUT_MS = 1200;

static bool elapsed(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

static bool to_protocol_mode(climate::ClimateMode mode, protocol::Mode *out) {
  switch (mode) {
    case climate::CLIMATE_MODE_COOL: *out = protocol::Mode::COOL; return true;
    case climate::CLIMATE_MODE_HEAT: *out = protocol::Mode::HEAT; return true;
    case climate::CLIMATE_MODE_DRY: *out = protocol::Mode::DRY; return true;
    case climate::CLIMATE_MODE_FAN_ONLY: *out = protocol::Mode::FAN; return true;
    case climate::CLIMATE_MODE_AUTO:
    case climate::CLIMATE_MODE_HEAT_COOL: *out = protocol::Mode::AUTO; return true;
    default: return false;
  }
}

static climate::ClimateMode to_climate_mode(protocol::Mode mode) {
  switch (mode) {
    case protocol::Mode::COOL: return climate::CLIMATE_MODE_COOL;
    case protocol::Mode::DRY: return climate::CLIMATE_MODE_DRY;
    case protocol::Mode::FAN: return climate::CLIMATE_MODE_FAN_ONLY;
    case protocol::Mode::HEAT: return climate::CLIMATE_MODE_HEAT;
    case protocol::Mode::AUTO: return climate::CLIMATE_MODE_AUTO;
  }
  return climate::CLIMATE_MODE_OFF;
}

void KaeltebringerSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_extra_control(this->type_, state);
}

climate::ClimateTraits KaeltebringerClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_HEAT,
                              climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_AUTO});
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_BOTH,
                                    climate::CLIMATE_SWING_VERTICAL, climate::CLIMATE_SWING_HORIZONTAL});
  traits.set_visual_min_temperature(protocol::MIN_TARGET_TEMPERATURE);
  traits.set_visual_max_temperature(protocol::MAX_TARGET_TEMPERATURE);
  traits.set_visual_target_temperature_step(0.5f);
  return traits;
}

void KaeltebringerClimate::setup() {
  this->set_supported_custom_fan_modes({protocol::FAN_NAME_TURBO, protocol::FAN_NAME_MUTE,
                                        protocol::FAN_LEVEL_NAMES[0], protocol::FAN_LEVEL_NAMES[1],
                                        protocol::FAN_LEVEL_NAMES[2], protocol::FAN_LEVEL_NAMES[3],
                                        protocol::FAN_LEVEL_NAMES[4], protocol::FAN_LEVEL_NAMES[5]});
  protocol::init_set_payload(this->set_payload_);
  this->diagnostics_enabled_ = this->sensors_[static_cast<size_t>(SensorType::ENERGY)] != nullptr ||
                               this->sensors_[static_cast<size_t>(SensorType::COMPRESSOR_TICKS)] != nullptr ||
                               this->binary_sensors_[static_cast<size_t>(BinarySensorType::ENERGY_AVAILABLE)] != nullptr;
}

void KaeltebringerClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Kaeltebringer extended climate:");
  ESP_LOGCONFIG(TAG, "  Beep on commands: %s", YESNO(this->beep_enabled_));
  ESP_LOGCONFIG(TAG, "  Energy diagnostics: %s", YESNO(this->diagnostics_enabled_));
  if (this->diagnostics_enabled_)
    ESP_LOGCONFIG(TAG, "  Diagnostic interval: %" PRIu32 " s", this->diagnostic_interval_ms_ / 1000);
  LOG_CLIMATE("  ", "Climate", this);
  LOG_UPDATE_INTERVAL(this);

  // The LOG_* macros need string literals, hence no table-driven loop.
  LOG_SENSOR("  ", "Room Temperature", this->sensors_[static_cast<size_t>(SensorType::ROOM_TEMPERATURE)]);
  LOG_SENSOR("  ", "Outlet Temperature", this->sensors_[static_cast<size_t>(SensorType::OUTLET_TEMPERATURE)]);
  LOG_SENSOR("  ", "Target Temperature", this->sensors_[static_cast<size_t>(SensorType::TARGET_TEMPERATURE)]);
  LOG_SENSOR("  ", "Energy", this->sensors_[static_cast<size_t>(SensorType::ENERGY)]);
  LOG_SENSOR("  ", "Compressor Ticks", this->sensors_[static_cast<size_t>(SensorType::COMPRESSOR_TICKS)]);
  LOG_SENSOR("  ", "On Timer Minutes", this->sensors_[static_cast<size_t>(SensorType::ON_TIMER_MINUTES)]);
  LOG_SENSOR("  ", "Off Timer Minutes", this->sensors_[static_cast<size_t>(SensorType::OFF_TIMER_MINUTES)]);
  LOG_BINARY_SENSOR("  ", "Indoor Active", this->binary_sensors_[static_cast<size_t>(BinarySensorType::INDOOR_ACTIVE)]);
  LOG_BINARY_SENSOR("  ", "Mute", this->binary_sensors_[static_cast<size_t>(BinarySensorType::MUTE)]);
  LOG_BINARY_SENSOR("  ", "On Timer Armed", this->binary_sensors_[static_cast<size_t>(BinarySensorType::ON_TIMER_ARMED)]);
  LOG_BINARY_SENSOR("  ", "Off Timer Armed",
                    this->binary_sensors_[static_cast<size_t>(BinarySensorType::OFF_TIMER_ARMED)]);
  LOG_BINARY_SENSOR("  ", "Energy Available",
                    this->binary_sensors_[static_cast<size_t>(BinarySensorType::ENERGY_AVAILABLE)]);
  LOG_TEXT_SENSOR("  ", "Compressor State", this->text_sensors_[static_cast<size_t>(TextSensorType::COMPRESSOR_STATE)]);
  LOG_TEXT_SENSOR("  ", "Compressor State Raw",
                  this->text_sensors_[static_cast<size_t>(TextSensorType::COMPRESSOR_STATE_RAW)]);
  LOG_TEXT_SENSOR("  ", "Fan Speed", this->text_sensors_[static_cast<size_t>(TextSensorType::FAN_SPEED)]);
  LOG_SWITCH("  ", "Eco", this->switches_[static_cast<size_t>(SwitchType::ECO)]);
  LOG_SWITCH("  ", "Health", this->switches_[static_cast<size_t>(SwitchType::HEALTH)]);
  LOG_SWITCH("  ", "Display", this->switches_[static_cast<size_t>(SwitchType::DISPLAY_PANEL)]);
  LOG_SWITCH("  ", "8C Heater", this->switches_[static_cast<size_t>(SwitchType::HEATER_8C)]);
}

void KaeltebringerClimate::update() {
  // A pending set request is answered with a state frame as well, so it replaces the poll.
  if (this->diagnostic_phase_ != DiagnosticPhase::IDLE || this->pending_set_)
    return;
  this->send_get_();
}

void KaeltebringerClimate::loop() {
  uint8_t byte;
  while (this->available() > 0 && this->read_byte(&byte)) {
    this->parser_.push(byte);
    protocol::Frame frame;
    while (this->parser_.poll(&frame))
      this->handle_frame_(frame);
  }

  const uint32_t now = millis();
  this->process_diagnostics_(now);
  if (this->diagnostic_phase_ != DiagnosticPhase::IDLE)
    return;
  if (this->pending_set_) {
    this->pending_set_ = false;
    this->send_set_();
  } else if (!this->state_valid_ && elapsed(now, this->last_get_ms_ + INITIAL_GET_RETRY_MS)) {
    // Do not wait a full update interval for the first state; no command can be sent before it.
    this->send_get_();
  }
}

void KaeltebringerClimate::send_frame_(uint8_t command, const uint8_t *payload, size_t length) {
  uint8_t frame[protocol::SET_PAYLOAD_SIZE + protocol::FRAME_OVERHEAD];
  if (length > protocol::SET_PAYLOAD_SIZE)
    return;
  const size_t frame_length = protocol::build_frame(command, payload, length, frame);
  this->write_array(frame, frame_length);
  ESP_LOGV(TAG, "TX: %s", format_hex_pretty(frame, frame_length).c_str());
}

void KaeltebringerClimate::send_get_() {
  static const uint8_t REQUEST[] = {0x01, 0x00};
  this->last_get_ms_ = millis();
  this->send_frame_(protocol::CMD_GET, REQUEST, sizeof(REQUEST));
}

void KaeltebringerClimate::send_set_() {
  this->send_frame_(protocol::CMD_SET, this->set_payload_, protocol::SET_PAYLOAD_SIZE);
}

void KaeltebringerClimate::handle_frame_(const protocol::Frame &frame) {
  ESP_LOGV(TAG, "RX cmd=0x%02X: %s", frame.command, format_hex_pretty(frame.payload, frame.payload_length).c_str());
  const uint32_t now = millis();

  switch (frame.command) {
    case protocol::CMD_SET:
    case protocol::CMD_GET:
      if (frame.payload_length != protocol::STATE_PAYLOAD_SIZE) {
        ESP_LOGW(TAG, "Unexpected state payload size %u", static_cast<unsigned>(frame.payload_length));
        return;
      }
      this->handle_state_(frame.payload);
      if (frame.command == protocol::CMD_GET)
        this->start_diagnostics_(now);
      break;

    case protocol::CMD_ENERGY_DIAGNOSTIC:
      if (this->diagnostic_phase_ == DiagnosticPhase::ENERGY_SENT) {
        this->handle_energy_(frame.payload, frame.payload_length);
        this->diagnostic_phase_ = DiagnosticPhase::STATUS_PENDING;
        this->phase_deadline_ms_ = now + DIAGNOSTIC_NEXT_DELAY_MS;
      }
      break;

    case protocol::CMD_STATUS_DIAGNOSTIC:
      // The status diagnostic carries no mapped fields; it only completes the transaction chain.
      if (this->diagnostic_phase_ == DiagnosticPhase::STATUS_SENT)
        this->diagnostic_phase_ = DiagnosticPhase::IDLE;
      break;

    default:
      ESP_LOGD(TAG, "Ignoring frame with command 0x%02X", frame.command);
      break;
  }
}

void KaeltebringerClimate::handle_state_(const uint8_t *payload) {
  protocol::State state;
  if (!protocol::decode_state(payload, protocol::STATE_PAYLOAD_SIZE, &state)) {
    ESP_LOGW(TAG, "Invalid state payload");
    return;
  }
  if (state.compressor == protocol::Compressor::RUNNING)
    ESP_LOGW(TAG, "Unknown compressor state code 0x%02X (treating as running)", state.compressor_code);
  const uint8_t fan_code = (payload[3] >> 4) & 0x07;
  if (fan_code >= protocol::FAN_LEVEL_COUNT)
    ESP_LOGW(TAG, "Unknown fan speed code 0x%X", fan_code);
  if (state.off_timer_armed && (payload[7] >= 60 || payload[7] % 30 != 0))
    ESP_LOGW(TAG, "Unknown off timer value 0x%02X:0x%02X", payload[6], payload[7]);
  if (state.on_timer_armed && (payload[9] >= 60 || payload[9] % 30 != 0))
    ESP_LOGW(TAG, "Unknown on timer value 0x%02X:0x%02X", payload[8], payload[9]);
  this->state_valid_ = true;
  // Pending changes must not be overwritten by a state reported before they were sent.
  if (!this->pending_set_)
    protocol::sync_set_payload(this->set_payload_, payload, this->beep_enabled_, this->extras_);

  if (!state.power)
    this->mode = climate::CLIMATE_MODE_OFF;
  else if (state.mode_known)
    this->mode = to_climate_mode(state.mode);
  else
    ESP_LOGW(TAG, "Unknown operating mode code 0x%X", payload[2] & 0x0F);

  const char *fan_name = protocol::fan_name(state);
  this->target_temperature = state.target_temperature;
  this->current_temperature = state.room_temperature;
  this->set_custom_fan_mode_(fan_name);
  if (state.horizontal_swing && state.vertical_swing)
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (state.vertical_swing)
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (state.horizontal_swing)
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    this->swing_mode = climate::CLIMATE_SWING_OFF;

  const auto compressor = protocol::compressor_in_mode(state);
  if (!state.power)
    this->action = climate::CLIMATE_ACTION_OFF;
  else if (compressor == protocol::Compressor::COOLING)
    this->action = this->mode == climate::CLIMATE_MODE_DRY ? climate::CLIMATE_ACTION_DRYING
                                                           : climate::CLIMATE_ACTION_COOLING;
  else if (compressor == protocol::Compressor::HEATING)
    this->action = climate::CLIMATE_ACTION_HEATING;
  else if (this->mode == climate::CLIMATE_MODE_FAN_ONLY)
    this->action = climate::CLIMATE_ACTION_FAN;
  else
    this->action = climate::CLIMATE_ACTION_IDLE;
  this->publish_state();

  this->publish_(SensorType::ROOM_TEMPERATURE, state.room_temperature);
  this->publish_(SensorType::OUTLET_TEMPERATURE, state.outlet_temperature);
  this->publish_(SensorType::TARGET_TEMPERATURE, state.target_temperature);
  this->publish_(SensorType::ON_TIMER_MINUTES, state.on_timer_minutes);
  this->publish_(SensorType::OFF_TIMER_MINUTES, state.off_timer_minutes);
  this->publish_(BinarySensorType::INDOOR_ACTIVE, state.indoor_active);
  this->publish_(BinarySensorType::MUTE, state.mute);
  this->publish_(BinarySensorType::ON_TIMER_ARMED, state.on_timer_armed);
  this->publish_(BinarySensorType::OFF_TIMER_ARMED, state.off_timer_armed);
  this->publish_(TextSensorType::COMPRESSOR_STATE, protocol::compressor_name(state.compressor));
  char compressor_code[5];
  std::snprintf(compressor_code, sizeof(compressor_code), "0x%02X", state.compressor_code);
  this->publish_(TextSensorType::COMPRESSOR_STATE_RAW, compressor_code);
  this->publish_(TextSensorType::FAN_SPEED, fan_name);
  if (!this->pending_set_)
    this->publish_(SwitchType::ECO, state.eco);
  this->publish_(SwitchType::HEALTH, this->extras_.health);
  this->publish_(SwitchType::DISPLAY_PANEL, this->extras_.display);
  this->publish_(SwitchType::HEATER_8C, this->extras_.heater_8c);

  ESP_LOGD(TAG, "State: power=%s mode=%u target=%.1f room=%.1f outlet=%.1f fan=%s", YESNO(state.power),
           static_cast<unsigned>(state.mode), state.target_temperature, state.room_temperature,
           state.outlet_temperature, fan_name);
}

void KaeltebringerClimate::handle_energy_(const uint8_t *payload, size_t length) {
  protocol::Energy energy;
  if (!protocol::decode_energy(payload, length, &energy)) {
    ESP_LOGW(TAG, "Invalid energy diagnostic payload");
    return;
  }
  this->publish_(BinarySensorType::ENERGY_AVAILABLE, energy.available);
  if (!energy.available) {
    ESP_LOGD(TAG, "Energy diagnostic carries no counters");
    return;
  }
  this->publish_(SensorType::ENERGY, static_cast<float>(energy.kwh));
  this->publish_(SensorType::COMPRESSOR_TICKS, static_cast<float>(energy.ticks * protocol::MINUTES_PER_ENERGY_TICK));
  ESP_LOGD(TAG, "Energy: %.6f kWh, %u ticks", energy.kwh, static_cast<unsigned>(energy.ticks));
}

void KaeltebringerClimate::start_diagnostics_(uint32_t now) {
  if (!this->diagnostics_enabled_ || this->diagnostic_phase_ != DiagnosticPhase::IDLE || this->pending_set_ ||
      !elapsed(now, this->next_diagnostic_ms_))
    return;
  this->next_diagnostic_ms_ = now + this->diagnostic_interval_ms_;
  this->diagnostic_phase_ = DiagnosticPhase::ENERGY_PENDING;
  this->phase_deadline_ms_ = now + DIAGNOSTIC_FIRST_DELAY_MS;
}

void KaeltebringerClimate::process_diagnostics_(uint32_t now) {
  if (this->diagnostic_phase_ == DiagnosticPhase::IDLE || !elapsed(now, this->phase_deadline_ms_))
    return;

  static const uint8_t ENERGY_REQUEST[] = {0x05, 0x00, 0x00};
  static const uint8_t STATUS_REQUEST[] = {0x05, 0x00};
  switch (this->diagnostic_phase_) {
    case DiagnosticPhase::ENERGY_PENDING:
      this->send_frame_(protocol::CMD_ENERGY_DIAGNOSTIC, ENERGY_REQUEST, sizeof(ENERGY_REQUEST));
      this->diagnostic_phase_ = DiagnosticPhase::ENERGY_SENT;
      this->phase_deadline_ms_ = now + DIAGNOSTIC_REPLY_TIMEOUT_MS;
      break;
    case DiagnosticPhase::STATUS_PENDING:
      this->send_frame_(protocol::CMD_STATUS_DIAGNOSTIC, STATUS_REQUEST, sizeof(STATUS_REQUEST));
      this->diagnostic_phase_ = DiagnosticPhase::STATUS_SENT;
      this->phase_deadline_ms_ = now + DIAGNOSTIC_REPLY_TIMEOUT_MS;
      break;
    case DiagnosticPhase::ENERGY_SENT:
      ESP_LOGW(TAG, "Energy diagnostic timed out");
      this->diagnostic_phase_ = DiagnosticPhase::IDLE;
      break;
    case DiagnosticPhase::STATUS_SENT:
      ESP_LOGW(TAG, "Status diagnostic timed out");
      this->diagnostic_phase_ = DiagnosticPhase::IDLE;
      break;
    case DiagnosticPhase::IDLE:
      break;
  }
}

// set_payload_ mirrors the last reported state until a change is pending, so edits can be applied directly.
void KaeltebringerClimate::control(const climate::ClimateCall &call) {
  if (!this->state_valid_) {
    ESP_LOGW(TAG, "Ignoring command: no state received from the AC yet");
    return;
  }
  uint8_t *set = this->set_payload_;

  const auto target = call.get_target_temperature();
  if (target.has_value())
    protocol::set_target_temperature(set, *target);

  const auto mode = call.get_mode();
  if (mode.has_value()) {
    protocol::Mode protocol_mode;
    if (to_protocol_mode(*mode, &protocol_mode)) {
      protocol::set_power(set, true);
      protocol::set_mode(set, protocol_mode);
    } else {
      protocol::set_power(set, false);
    }
  }

  const auto swing = call.get_swing_mode();
  if (swing.has_value()) {
    protocol::set_swing(set, *swing == climate::CLIMATE_SWING_BOTH || *swing == climate::CLIMATE_SWING_VERTICAL,
                        *swing == climate::CLIMATE_SWING_BOTH || *swing == climate::CLIMATE_SWING_HORIZONTAL);
  }

  if (call.has_custom_fan_mode()) {
    const auto fan = call.get_custom_fan_mode();
    if (fan == protocol::FAN_NAME_TURBO) {
      protocol::set_fan(set, protocol::FAN_LEVEL_TURBO, true, false);
    } else if (fan == protocol::FAN_NAME_MUTE) {
      protocol::set_fan(set, protocol::FAN_LEVEL_MUTE, false, true);
    } else {
      for (uint8_t level = 0; level < protocol::FAN_LEVEL_COUNT; level++) {
        if (fan == protocol::FAN_LEVEL_NAMES[level]) {
          protocol::set_fan(set, level, false, false);
          break;
        }
      }
    }
  }
  this->pending_set_ = true;
}

void KaeltebringerClimate::set_extra_control(SwitchType type, bool enabled) {
  if (!this->state_valid_) {
    ESP_LOGW(TAG, "Ignoring command: no state received from the AC yet");
    return;
  }
  switch (type) {
    case SwitchType::ECO: protocol::set_eco(this->set_payload_, enabled); break;
    case SwitchType::HEALTH: this->extras_.health = enabled; break;
    case SwitchType::DISPLAY_PANEL: this->extras_.display = enabled; break;
    case SwitchType::HEATER_8C: this->extras_.heater_8c = enabled; break;
  }
  protocol::set_extras(this->set_payload_, this->extras_);
  this->publish_(type, enabled);
  this->pending_set_ = true;
}

}  // namespace kaeltebringer
}  // namespace esphome
