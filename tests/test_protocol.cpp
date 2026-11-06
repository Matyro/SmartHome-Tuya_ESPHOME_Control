// Host-side tests for components/kaeltebringer/protocol.h.
// Build and run:  g++ -std=c++17 -Wall -Wextra -o test_protocol test_protocol.cpp && ./test_protocol

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../components/kaeltebringer/protocol.h"

using namespace esphome::kaeltebringer::protocol;

static int failures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
      failures++;                                                           \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance) CHECK(std::fabs((actual) - (expected)) <= (tolerance))

static std::vector<uint8_t> hex(const char *text) {
  std::vector<uint8_t> bytes;
  for (const char *p = text; *p != '\0';) {
    if (*p == ' ') {
      p++;
      continue;
    }
    bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
    p += 2;
  }
  return bytes;
}

static std::vector<Frame> feed(FrameParser &parser, const std::vector<uint8_t> &bytes,
                               std::vector<std::vector<uint8_t>> *payloads = nullptr) {
  std::vector<Frame> frames;
  for (uint8_t byte : bytes) {
    parser.push(byte);
    Frame frame;
    while (parser.poll(&frame)) {
      frames.push_back(frame);
      if (payloads != nullptr)
        payloads->emplace_back(frame.payload, frame.payload + frame.payload_length);
    }
  }
  return frames;
}

// Documented example state frame (heat, 22 C, room 26.0 C).
static const char *STATE_FRAME =
    "BB 01 00 03 37 04 00 34 D6 00 00 00 00 00 00 00 00 73 03 88 00 00 00 00 00 00 00 00 00 00 91 FF 40 00 6C 1F "
    "1B 4F 52 18 CA 00 00 00 00 E0 01 00 00 44 40 00 00 00 00 1A 00 00 00 00 E6";

static void test_build_frame() {
  const uint8_t request[] = {0x01, 0x00};
  uint8_t out[8];
  CHECK(build_frame(CMD_GET, request, sizeof(request), out) == 8);
  CHECK(std::memcmp(out, hex("BB 00 01 04 02 01 00 BD").data(), 8) == 0);
}

static void test_parser_noise_and_split_frames() {
  FrameParser parser;
  std::vector<std::vector<uint8_t>> payloads;
  auto bytes = hex("55 aa 20");  // debug text before the frame
  auto frame = hex(STATE_FRAME);
  bytes.insert(bytes.end(), frame.begin(), frame.end());
  auto frames = feed(parser, bytes, &payloads);
  CHECK(frames.size() == 1);
  CHECK(frames[0].command == CMD_SET);
  CHECK(payloads[0].size() == STATE_PAYLOAD_SIZE);
}

static void test_parser_bad_checksum_resynchronizes() {
  auto bad = hex("BB 01 00 04 02 04 00 00");
  auto good = hex("BB 01 00 04 02 04 00 B8");
  CHECK(checksum(good.data(), good.size() - 1) == good.back());
  FrameParser parser;
  bad.insert(bad.end(), good.begin(), good.end());
  auto frames = feed(parser, bad);
  CHECK(frames.size() == 1);
  CHECK(frames[0].command == CMD_GET);
}

static void test_parser_truncated_frame_resynchronizes() {
  // The first frame promises 0x30 payload bytes but is cut off by a complete frame. The parser can only
  // reject it once enough further bytes arrived, but the complete frame must then still be found.
  auto bytes = hex("BB 01 00 04 30 04 01");
  auto good = hex("BB 01 00 04 02 04 00 B8");
  bytes.insert(bytes.end(), good.begin(), good.end());
  bytes.insert(bytes.end(), 40, 0x00);
  FrameParser parser;
  CHECK(feed(parser, bytes).size() == 1);
}

static void test_parser_payload_containing_preamble() {
  const uint8_t payload[] = {0x04, 0xBB, 0xBB, 0x01};
  uint8_t out[16];
  const size_t length = build_frame(CMD_GET, payload, sizeof(payload), out);
  out[1] = 0x01;  // AC -> module direction
  out[2] = 0x00;
  out[length - 1] = checksum(out, length - 1);
  FrameParser parser;
  std::vector<std::vector<uint8_t>> payloads;
  auto frames = feed(parser, std::vector<uint8_t>(out, out + length), &payloads);
  CHECK(frames.size() == 1);
  CHECK(payloads[0] == std::vector<uint8_t>(payload, payload + sizeof(payload)));
}

static void test_parser_back_to_back_frames() {
  auto bytes = hex("BB 01 00 04 02 04 00 B8");
  auto copy = bytes;
  bytes.insert(bytes.end(), copy.begin(), copy.end());
  FrameParser parser;
  CHECK(feed(parser, bytes).size() == 2);
}

static void test_decode_documented_state() {
  FrameParser parser;
  std::vector<std::vector<uint8_t>> payloads;
  feed(parser, hex(STATE_FRAME), &payloads);
  State state;
  CHECK(decode_state(payloads[0].data(), payloads[0].size(), &state));
  CHECK(state.power);
  CHECK(state.mode_known && state.mode == Mode::HEAT);
  CHECK_NEAR(state.target_temperature, 22.0, 0.001);
  CHECK(state.fan_level == 4);
  CHECK(!state.turbo && !state.eco && !state.mute);
  CHECK_NEAR(state.room_temperature, 26.0, 0.001);
  CHECK_NEAR(state.outlet_temperature, 38.0, 0.001);
  CHECK(state.indoor_active);
  CHECK(!state.on_timer_armed && !state.off_timer_armed);
  CHECK(state.compressor_code == 0xCA);
  CHECK(std::strcmp(fan_name(state), "4") == 0);
}

static std::vector<uint8_t> blank_state() {
  std::vector<uint8_t> payload(STATE_PAYLOAD_SIZE, 0);
  payload[0] = 0x04;
  return payload;
}

static void test_decode_state_fields() {
  auto payload = blank_state();
  payload[2] = 0x13;  // cool, power on
  payload[3] = 0x52;  // 18 C, state fan code 5
  payload[4] = 0x02 | 0x40 | 0x80;
  payload[5] = 0x20 | 0x40;
  payload[6] = 1;
  payload[7] = 30;
  payload[8] = 2;
  payload[9] = 0;
  payload[0x21] = 0x80;
  payload[0x22] = 0x30;
  State state;
  CHECK(decode_state(payload.data(), payload.size(), &state));
  CHECK_NEAR(state.target_temperature, 18.5, 0.001);
  CHECK(state.horizontal_swing && state.vertical_swing);
  CHECK(state.off_timer_armed && state.on_timer_armed);
  CHECK(state.off_timer_minutes == 90);
  CHECK(state.on_timer_minutes == 120);
  CHECK(state.mute && state.compressor == Compressor::COOLING);
  CHECK(state.compressor_code == 0x30);
  CHECK(std::strcmp(fan_name(state), "Mute") == 0);

  payload[7] = 17;  // not a 30 minute step
  CHECK(decode_state(payload.data(), payload.size(), &state));
  CHECK(state.off_timer_minutes == 0);

  payload[0x22] = 0x2C;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(state.compressor == Compressor::HEATING);
  CHECK(state.compressor_code == 0x2C);
  payload[0x22] = 0xCA;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(state.compressor == Compressor::RUNNING);
  CHECK(state.compressor_code == 0xCA);
  CHECK(std::strcmp(compressor_name(state.compressor), "running") == 0);
  CHECK(compressor_in_mode(state) == Compressor::COOLING);
  payload[2] = 0x14;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(compressor_in_mode(state) == Compressor::HEATING);
  payload[2] = 0x13;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(compressor_in_mode(state) == Compressor::COOLING);
  payload[2] = 0x15;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(compressor_in_mode(state) == Compressor::RUNNING);
  payload[2] = 0x1F;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(compressor_in_mode(state) == Compressor::RUNNING);
  payload[0x22] = 0x00;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(state.compressor == Compressor::OFF);
  CHECK(state.compressor_code == 0x00);

  payload[0] = 0x00;
  CHECK(!decode_state(payload.data(), payload.size(), &state));
  CHECK(!decode_state(payload.data(), payload.size() - 1, &state));
}

static void test_turbo_and_eco_flags() {
  auto payload = blank_state();
  payload[2] = 0xB3;  // turbo, power on, cool
  State state;
  decode_state(payload.data(), payload.size(), &state);
  CHECK(state.turbo && !state.eco);
  CHECK(std::strcmp(fan_name(state), "Turbo") == 0);
  payload[2] = 0x73;  // eco
  decode_state(payload.data(), payload.size(), &state);
  CHECK(state.eco && !state.turbo);
}

static void test_fan_codes_round_trip() {
  // Reference pairs of {state code, set code} from the AC protocol notes.
  const uint8_t pairs[][2] = {{0, 0}, {1, 2}, {4, 6}, {2, 3}, {5, 7}, {3, 5}};
  for (const auto &pair : pairs) {
    auto state = blank_state();
    state[2] = 0x13;
    state[3] = static_cast<uint8_t>(pair[0] << 4);
    uint8_t set[SET_PAYLOAD_SIZE];
    init_set_payload(set);
    sync_set_payload(set, state.data(), true, Extras{});
    CHECK((set[5] & 0x07) == pair[1]);
  }
}

static void test_sync_set_payload() {
  auto state = blank_state();
  state[2] = 0x31;  // cool, power on
  state[3] = 0x36;  // 22 C
  state[4] = 0x02;  // half degree
  state[5] = 0x40;  // vertical swing
  uint8_t set[SET_PAYLOAD_SIZE];
  init_set_payload(set);
  Extras extras;
  extras.health = true;
  extras.heater_8c = true;
  sync_set_payload(set, state.data(), true, extras);
  CHECK((set[2] & 0x04) != 0);        // power
  CHECK((set[2] & 0x20) != 0);        // beep
  CHECK((set[2] & 0x40) != 0);        // display (from extras)
  CHECK((set[3] & 0x0F) == 0x03);     // cool
  CHECK((set[3] & 0x10) != 0);        // health
  CHECK((set[4] & 0x0F) == 31 - 22);  // 0x0F + 16 - T
  CHECK((set[5] & 0x38) == 0x38);     // vertical vane
  CHECK((set[6] & 0x02) != 0);        // half degree
  CHECK((set[7] & 0x08) == 0);        // horizontal vane
  CHECK(set[0x19] == 0x60 && set[0x1A] == 0x01);

  sync_set_payload(set, state.data(), false, Extras{});
  CHECK((set[2] & 0x20) == 0);
  CHECK((set[3] & 0x10) == 0);
  CHECK(set[0x19] == 0x00 && set[0x1A] == 0x00);
}

static void test_set_modifiers() {
  uint8_t set[SET_PAYLOAD_SIZE];
  init_set_payload(set);
  set_target_temperature(set, 16.0f);
  CHECK((set[4] & 0x0F) == 0x0F && (set[6] & 0x02) == 0);
  set_target_temperature(set, 31.5f);
  CHECK((set[4] & 0x0F) == 0x00 && (set[6] & 0x02) != 0);
  set_target_temperature(set, 99.0f);  // clamped
  CHECK((set[4] & 0x0F) == 0x00 && (set[6] & 0x02) != 0);
  set_target_temperature(set, 22.5f);
  CHECK((set[4] & 0x0F) == 9 && (set[6] & 0x02) != 0);
  CHECK((set[4] & 0xF0) == 0x50);  // untouched upper nibble

  set_power(set, true);
  CHECK((set[2] & 0x04) != 0);
  set_power(set, false);
  CHECK((set[2] & 0x04) == 0);
  set_mode(set, Mode::AUTO);
  CHECK((set[3] & 0x0F) == 0x08);
  set_swing(set, true, true);
  CHECK((set[5] & 0x38) == 0x38 && (set[7] & 0x08) != 0);
  set_swing(set, false, false);
  CHECK((set[5] & 0x38) == 0 && (set[7] & 0x08) == 0);

  set_fan(set, FAN_LEVEL_TURBO, true, false);
  CHECK((set[3] & 0xC0) == 0x40 && (set[5] & 0x07) == 5);
  set_fan(set, FAN_LEVEL_MUTE, false, true);
  CHECK((set[3] & 0xC0) == 0x80 && (set[5] & 0x07) == 2);
  set_fan(set, 3, false, false);
  CHECK((set[3] & 0xC0) == 0x00 && (set[5] & 0x07) == 3);
  set_eco(set, true);
  CHECK((set[2] & 0x80) != 0);
}

static void test_energy() {
  Energy energy;
  auto reply = hex(
      "04 00 0C 19 01 00 00 00 00 00 00 00 00 00 00 75 48 73 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
      "00 00 00 00 00 00 00 00");
  CHECK(reply.size() == DIAGNOSTIC_PAYLOAD_SIZE);
  CHECK(decode_energy(reply.data(), reply.size(), &energy));
  CHECK(energy.available);
  CHECK_NEAR(energy.kwh, 1.194875, 1e-9);
  CHECK(energy.ticks == 0x0473);

  reply[2] = 0x04;  // no counters
  CHECK(decode_energy(reply.data(), reply.size(), &energy));
  CHECK(!energy.available);

  reply[2] = 0x0C;
  reply[3] = 0xFA;  // invalid BCD
  CHECK(!decode_energy(reply.data(), reply.size(), &energy));
  reply[3] = 0x19;
  reply[2] = 0x07;  // unknown marker
  CHECK(!decode_energy(reply.data(), reply.size(), &energy));
  CHECK(!decode_energy(reply.data(), reply.size() - 1, &energy));
}

int main() {
  test_build_frame();
  test_parser_noise_and_split_frames();
  test_parser_bad_checksum_resynchronizes();
  test_parser_truncated_frame_resynchronizes();
  test_parser_payload_containing_preamble();
  test_parser_back_to_back_frames();
  test_decode_documented_state();
  test_decode_state_fields();
  test_turbo_and_eco_flags();
  test_fan_codes_round_trip();
  test_sync_set_payload();
  test_set_modifiers();
  test_energy();
  if (failures == 0)
    std::printf("All protocol tests passed\n");
  return failures == 0 ? 0 : 1;
}
