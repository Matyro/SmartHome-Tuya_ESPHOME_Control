#pragma once

// Pure protocol logic for the Kaeltebringer/TCL indoor unit UART protocol (see AC-hack/Readme.md).
// This header has no ESPHome dependencies so it can be unit-tested with a host compiler.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace kaeltebringer {
namespace protocol {

constexpr uint8_t PREAMBLE = 0xBB;
constexpr size_t FRAME_OVERHEAD = 6;  // preamble, 2 direction bytes, command, length, checksum
constexpr size_t MAX_PAYLOAD_SIZE = 0xF9;
constexpr size_t MAX_FRAME_SIZE = MAX_PAYLOAD_SIZE + FRAME_OVERHEAD;
constexpr size_t STATE_PAYLOAD_SIZE = 55;
constexpr size_t SET_PAYLOAD_SIZE = 29;
constexpr size_t DIAGNOSTIC_PAYLOAD_SIZE = 45;

constexpr uint8_t CMD_SET = 0x03;
constexpr uint8_t CMD_GET = 0x04;
constexpr uint8_t CMD_STATUS_DIAGNOSTIC = 0x09;
constexpr uint8_t CMD_ENERGY_DIAGNOSTIC = 0x0A;

constexpr float MIN_TARGET_TEMPERATURE = 16.0f;
constexpr float MAX_TARGET_TEMPERATURE = 31.5f;
constexpr unsigned MINUTES_PER_ENERGY_TICK = 2;

inline uint8_t checksum(const uint8_t *data, size_t length) {
  uint8_t result = 0;
  for (size_t i = 0; i < length; i++)
    result ^= data[i];
  return result;
}

// Builds a module -> AC frame. `out` must hold payload_length + FRAME_OVERHEAD bytes.
inline size_t build_frame(uint8_t command, const uint8_t *payload, size_t payload_length, uint8_t *out) {
  out[0] = PREAMBLE;
  out[1] = 0x00;
  out[2] = 0x01;
  out[3] = command;
  out[4] = static_cast<uint8_t>(payload_length);
  if (payload_length != 0)
    memcpy(out + 5, payload, payload_length);
  out[5 + payload_length] = checksum(out, 5 + payload_length);
  return payload_length + FRAME_OVERHEAD;
}

struct Frame {
  uint8_t command{0};
  const uint8_t *payload{nullptr};  // valid until the next push()/poll() call
  size_t payload_length{0};
};

// Incremental AC -> module frame parser. The line also carries debug text, so bytes outside of a frame are
// dropped and the parser resynchronizes on the next preamble. Payload bytes may legitimately contain 0xBB,
// so a candidate frame is only rejected once its header or checksum is proven wrong.
//
// Usage: push() every received byte, then call poll() until it returns false.
class FrameParser {
 public:
  void push(uint8_t byte) {
    if (this->length_ == 0 && byte != PREAMBLE)
      return;
    if (this->length_ < sizeof(this->buffer_))
      this->buffer_[this->length_++] = byte;
  }

  bool poll(Frame *frame) {
    if (this->consumed_ != 0) {
      this->drop_(this->consumed_);
      this->consumed_ = 0;
    }
    while (this->length_ > 0) {
      if (this->buffer_[0] != PREAMBLE) {
        this->drop_to_next_preamble_();
        continue;
      }
      switch (this->check_()) {
        case Status::COMPLETE:
          frame->command = this->buffer_[3];
          frame->payload = this->buffer_ + 5;
          frame->payload_length = this->buffer_[4];
          this->consumed_ = static_cast<size_t>(this->buffer_[4]) + FRAME_OVERHEAD;
          return true;
        case Status::INCOMPLETE:
          return false;
        case Status::INVALID:
          this->drop_to_next_preamble_();
          break;
      }
    }
    return false;
  }

 protected:
  enum class Status : uint8_t { INCOMPLETE, COMPLETE, INVALID };

  Status check_() const {
    // Only AC -> module frames (BB 01 00 ...) are expected.
    if (this->length_ >= 2 && this->buffer_[1] != 0x01)
      return Status::INVALID;
    if (this->length_ >= 3 && this->buffer_[2] != 0x00)
      return Status::INVALID;
    if (this->length_ < 5)
      return Status::INCOMPLETE;
    if (this->buffer_[4] > MAX_PAYLOAD_SIZE)
      return Status::INVALID;
    // Every AC response payload starts with 0x04, which rejects bogus headers early.
    if (this->buffer_[4] != 0 && this->length_ >= 6 && this->buffer_[5] != 0x04)
      return Status::INVALID;
    const size_t frame_length = static_cast<size_t>(this->buffer_[4]) + FRAME_OVERHEAD;
    if (this->length_ < frame_length)
      return Status::INCOMPLETE;
    return checksum(this->buffer_, frame_length - 1) == this->buffer_[frame_length - 1] ? Status::COMPLETE
                                                                                         : Status::INVALID;
  }

  void drop_(size_t count) {
    this->length_ -= count;
    memmove(this->buffer_, this->buffer_ + count, this->length_);
  }

  // Discards the first byte and everything up to the next preamble.
  void drop_to_next_preamble_() {
    size_t next = 1;
    while (next < this->length_ && this->buffer_[next] != PREAMBLE)
      next++;
    this->drop_(next);
  }

  uint8_t buffer_[MAX_FRAME_SIZE]{};
  size_t length_{0};
  size_t consumed_{0};
};

enum class Mode : uint8_t { COOL, DRY, FAN, HEAT, AUTO };
enum class Compressor : uint8_t { OFF, COOLING, HEATING, UNKNOWN };

// Fan level 0 is automatic, 1-5 are the manual steps. State and set frames use different codes per level.
constexpr size_t FAN_LEVEL_COUNT = 6;
constexpr uint8_t FAN_STATE_CODES[FAN_LEVEL_COUNT] = {0, 1, 4, 2, 5, 3};
constexpr uint8_t FAN_SET_CODES[FAN_LEVEL_COUNT] = {0, 2, 6, 3, 7, 5};
constexpr uint8_t FAN_LEVEL_TURBO = 5;
constexpr uint8_t FAN_LEVEL_MUTE = 1;

inline uint8_t fan_level_from_state_code(uint8_t code) {
  for (size_t level = 0; level < FAN_LEVEL_COUNT; level++) {
    if (FAN_STATE_CODES[level] == code)
      return static_cast<uint8_t>(level);
  }
  return 0;
}

inline bool mode_from_state_code(uint8_t code, Mode *mode) {
  switch (code) {
    case 0x01: *mode = Mode::COOL; return true;
    case 0x02: *mode = Mode::FAN; return true;
    case 0x03: *mode = Mode::DRY; return true;
    case 0x04: *mode = Mode::HEAT; return true;
    case 0x05: *mode = Mode::AUTO; return true;
    default: return false;
  }
}

inline uint8_t mode_to_set_code(Mode mode) {
  switch (mode) {
    case Mode::HEAT: return 0x01;
    case Mode::DRY: return 0x02;
    case Mode::COOL: return 0x03;
    case Mode::FAN: return 0x07;
    case Mode::AUTO: return 0x08;
  }
  return 0x03;
}

struct State {
  bool power{false};
  bool mode_known{false};
  Mode mode{Mode::COOL};
  float target_temperature{0};
  uint8_t fan_level{0};
  bool turbo{false};
  bool eco{false};
  bool mute{false};
  bool horizontal_swing{false};
  bool vertical_swing{false};
  float room_temperature{0};
  float outlet_temperature{0};
  bool indoor_active{false};
  Compressor compressor{Compressor::OFF};
  bool on_timer_armed{false};
  bool off_timer_armed{false};
  uint16_t on_timer_minutes{0};
  uint16_t off_timer_minutes{0};
};

// Timers use 30 minute steps; anything else is treated as "not set".
inline uint16_t timer_minutes(const uint8_t *payload, size_t hours_offset) {
  const uint8_t minutes = payload[hours_offset + 1];
  if (minutes >= 60 || minutes % 30 != 0)
    return 0;
  return static_cast<uint16_t>(payload[hours_offset] * 60 + minutes);
}

inline bool decode_state(const uint8_t *payload, size_t length, State *state) {
  if (length != STATE_PAYLOAD_SIZE || payload[0] != 0x04)
    return false;
  state->power = (payload[2] & 0x10) != 0;
  state->mode_known = mode_from_state_code(payload[2] & 0x0F, &state->mode);
  state->eco = (payload[2] & 0x40) != 0;
  state->turbo = (payload[2] & 0x80) != 0;
  state->target_temperature = static_cast<float>(payload[3] & 0x0F) + 16.0f + ((payload[4] & 0x02) ? 0.5f : 0.0f);
  state->fan_level = fan_level_from_state_code((payload[3] >> 4) & 0x07);
  state->off_timer_armed = (payload[4] & 0x40) != 0;
  state->on_timer_armed = (payload[4] & 0x80) != 0;
  state->horizontal_swing = (payload[5] & 0x20) != 0;
  state->vertical_swing = (payload[5] & 0x40) != 0;
  state->off_timer_minutes = timer_minutes(payload, 6);
  state->on_timer_minutes = timer_minutes(payload, 8);
  state->room_temperature =
      (static_cast<float>(payload[0x0C]) * 4.0f + static_cast<float>((payload[0x0D] >> 2) & 0x03) - 200.0f) / 10.0f;
  state->outlet_temperature = (static_cast<float>(payload[0x19]) * 4.0f - 200.0f) / 10.0f;
  state->indoor_active = payload[0x1D] != 0x00;
  state->mute = (payload[0x21] & 0x80) != 0;
  switch (payload[0x22]) {
    case 0x30: state->compressor = Compressor::COOLING; break;
    case 0x2C: state->compressor = Compressor::HEATING; break;
    case 0x00: state->compressor = Compressor::OFF; break;
    default: state->compressor = Compressor::UNKNOWN; break;
  }
  return true;
}

inline const char *compressor_name(Compressor compressor) {
  switch (compressor) {
    case Compressor::COOLING: return "cooling";
    case Compressor::HEATING: return "heating";
    case Compressor::OFF: return "off";
    default: return "unknown";
  }
}

// Fan labels shared by the climate entity and the fan speed text sensor.
constexpr const char *FAN_NAME_TURBO = "Turbo";
constexpr const char *FAN_NAME_MUTE = "Mute";
constexpr const char *FAN_LEVEL_NAMES[FAN_LEVEL_COUNT] = {"Automatic", "1", "2", "3", "4", "5"};

inline const char *fan_name(const State &state) {
  if (state.turbo)
    return FAN_NAME_TURBO;
  if (state.mute)
    return FAN_NAME_MUTE;
  return FAN_LEVEL_NAMES[state.fan_level];
}

// Settings the AC does not report back. They are tracked locally and re-sent with every set request.
struct Extras {
  bool display{true};
  bool health{false};
  bool heater_8c{false};
};

inline void init_set_payload(uint8_t *set) {
  static const uint8_t DEFAULTS[SET_PAYLOAD_SIZE] = {0x00, 0x00, 0x64, 0x01, 0x59, 0x07};
  memcpy(set, DEFAULTS, SET_PAYLOAD_SIZE);
}

inline void set_extras(uint8_t *set, const Extras &extras) {
  set[2] = (set[2] & ~0x40) | (extras.display ? 0x40 : 0);
  set[3] = (set[3] & ~0x10) | (extras.health ? 0x10 : 0);
  set[0x19] = extras.heater_8c ? 0x60 : 0x00;
  set[0x1A] = extras.heater_8c ? 0x01 : 0x00;
}

// A set request is a full snapshot: remap the reported state into the request, then overwrite changed fields.
inline void sync_set_payload(uint8_t *set, const uint8_t *state_payload, bool beep, const Extras &extras) {
  const uint8_t *s = state_payload;
  set[0] = 0x00;
  set[1] = 0x00;
  set[2] = ((s[2] & 0x10) ? 0x04 : 0) | ((s[4] & 0x40) ? 0x08 : 0) | ((s[4] & 0x80) ? 0x10 : 0) |
           (beep ? 0x20 : 0) | ((s[2] & 0x40) ? 0x80 : 0);
  uint8_t mode_code = set[3] & 0x0F;
  Mode mode;
  if (mode_from_state_code(s[2] & 0x0F, &mode))
    mode_code = mode_to_set_code(mode);
  set[3] = mode_code | ((s[2] & 0x80) ? 0x40 : 0) | ((s[0x21] & 0x80) ? 0x80 : 0);
  set[4] = (set[4] & 0xF0) | (0x0F - (s[3] & 0x0F));
  set[5] = (set[5] & 0xC0) | FAN_SET_CODES[fan_level_from_state_code((s[3] >> 4) & 0x07)] |
           ((s[5] & 0x40) ? 0x38 : 0);
  set[6] = (set[6] & ~0x02) | ((s[4] & 0x02) ? 0x02 : 0);
  set[7] = (set[7] & ~0x08) | ((s[5] & 0x20) ? 0x08 : 0);
  set_extras(set, extras);
}

inline void set_power(uint8_t *set, bool on) { set[2] = (set[2] & ~0x04) | (on ? 0x04 : 0); }

inline void set_eco(uint8_t *set, bool on) { set[2] = (set[2] & ~0x80) | (on ? 0x80 : 0); }

inline void set_mode(uint8_t *set, Mode mode) { set[3] = (set[3] & 0xF0) | mode_to_set_code(mode); }

inline void set_target_temperature(uint8_t *set, float temperature) {
  temperature = std::fmax(MIN_TARGET_TEMPERATURE, std::fmin(MAX_TARGET_TEMPERATURE, temperature));
  const int half_degrees = static_cast<int>(std::lround(temperature * 2.0f));
  set[4] = (set[4] & 0xF0) | static_cast<uint8_t>(0x0F + 16 - half_degrees / 2);
  set[6] = (set[6] & ~0x02) | ((half_degrees & 1) ? 0x02 : 0);
}

inline void set_swing(uint8_t *set, bool vertical, bool horizontal) {
  set[5] = (set[5] & ~0x38) | (vertical ? 0x38 : 0);
  set[7] = (set[7] & ~0x08) | (horizontal ? 0x08 : 0);
}

inline void set_fan(uint8_t *set, uint8_t level, bool turbo, bool mute) {
  set[3] = (set[3] & ~0xC0) | (turbo ? 0x40 : 0) | (mute ? 0x80 : 0);
  set[5] = (set[5] & ~0x07) | FAN_SET_CODES[level < FAN_LEVEL_COUNT ? level : 0];
}

struct Energy {
  bool available{false};  // false when the reply carries no counters
  double kwh{0};
  uint16_t ticks{0};
};

inline bool decode_bcd(uint8_t value, uint32_t *decoded) {
  const uint8_t high = value >> 4;
  const uint8_t low = value & 0x0F;
  if (high > 9 || low > 9)
    return false;
  *decoded = static_cast<uint32_t>(high) * 10 + low;
  return true;
}

// Returns false for malformed payloads (wrong size/marker, invalid BCD, unknown availability marker).
inline bool decode_energy(const uint8_t *payload, size_t length, Energy *energy) {
  if (length != DIAGNOSTIC_PAYLOAD_SIZE || payload[0] != 0x04)
    return false;
  if (payload[2] == 0x04) {
    *energy = Energy{};
    return true;
  }
  if (payload[2] != 0x0C)
    return false;
  uint32_t fraction[3], integer[3];
  if (!decode_bcd(payload[3], &fraction[0]) || !decode_bcd(payload[0x10], &fraction[1]) ||
      !decode_bcd(payload[0x0F], &fraction[2]) || !decode_bcd(payload[6], &integer[0]) ||
      !decode_bcd(payload[5], &integer[1]) || !decode_bcd(payload[4], &integer[2]))
    return false;
  const uint32_t fraction_ppm = fraction[0] * 10000 + fraction[1] * 100 + fraction[2];
  const uint32_t whole_kwh = integer[0] * 10000 + integer[1] * 100 + integer[2];
  energy->available = true;
  energy->kwh = static_cast<double>(whole_kwh) + static_cast<double>(fraction_ppm) / 1000000.0;
  energy->ticks = static_cast<uint16_t>(payload[0x11] | (payload[0x12] << 8));
  return true;
}

}  // namespace protocol
}  // namespace kaeltebringer
}  // namespace esphome
