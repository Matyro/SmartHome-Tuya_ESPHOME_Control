# ESPHome Kältebringer

ESPHome climate platform (`kaeltebringer`) for Kältebringer KB34-12000BTU split ACs, plus the additional metrics reverse-engineered in `AC-hack/Readme.md`: outlet temperature, compressor state, compressor energy, timers and more.

The component targets the protocol described in `AC-hack/Readme.md`. Other TCL/Kesser/Pioneer rebrands may use similar framing but are not verified by this repository.

Requires a current ESPHome release (developed and compiled against 2026.10.1, e.g. the Home Assistant ESPHome add-on).

## Hardware and local build

The sample `esp8266_sample.yaml` uses an ESP-WROOM-02/ESP8266, GPIO15 TX, GPIO13 RX, 9600 baud and even parity (8E1). Logger output is disabled on UART (`logger.baud_rate: 0`) to keep text out of the binary protocol stream. For a local build, the sample loads `components/` as a local ESPHome external component; when building in Home Assistant, copy the `components/` folder next to the YAML file or point `external_components` at the hosting git repository.

## Climate features

The Climate entity supports off, cool, heat, fan-only, dry and auto modes, 16–31.5 °C setpoints at 0.5 °C steps, swing, and fan settings Automatic/1–5/Turbo/Mute. The climate `action` (cooling/heating/drying/fan/idle/off) is derived from the compressor state. Climate calls are combined into one full-snapshot `0x03` write, based on the most recently reported state, to avoid overwriting other settings with unrelated defaults. Commands are ignored (with a warning) until the first state frame was received; until then the state is requested every 3 s instead of waiting for `update_interval`.

## Optional entities

These are nested under the `climate: - platform: kaeltebringer` entry. Omit any entity not wanted.

| Key | Entity | Meaning |
| --- | --- | --- |
| `room_temperature` | Sensor (°C) | Indoor room temperature decoded with the documented payload formula. |
| `target_temperature` | Sensor (°C) | Current AC setpoint, including the half-degree bit. |
| `fan_speed` | Text sensor | Current fan mode/speed: `Automatic`, `1`–`5`, `Turbo`, or `Mute`. |
| `outlet_temperature` | Sensor (°C) | Payload `[0x19]` formula from the protocol notes. Its selected meaning may vary; validate against hardware before use. |
| `energy` | Sensor (kWh) | Outdoor-unit/compressor cumulative energy; only updated when the diagnostic reply reports available counters. Not whole-home energy. |
| `energy_available` | Binary sensor | Whether the most recent energy diagnostic contained counters. |
| `compressor_state` | Text sensor | `cooling`, `heating`, `off`, or `unknown` from the documented compressor code. |
| `compressor_ticks` | Sensor (min) | Powered-on counter converted at approximately two minutes per tick; compressor/outdoor-unit metric, not a wall-clock runtime. |
| `indoor_active` | Binary sensor | Indoor-unit active flag. |
| `mute` | Binary sensor | Mute/low-noise state. |
| `on_timer_armed`, `off_timer_armed` | Binary sensors | Reported timer arm bits. |
| `on_timer_minutes`, `off_timer_minutes` | Sensors (min) | Reported timer hours/minutes converted to minutes. Values are read-only. |
| `eco`, `health`, `display`, `heater_8c` | Switches | Mapped `0x03` controls for eco, health, display and 8 °C heater. `eco` follows the reported state; `health`, `display` and `heater_8c` are not reported by the AC and are tracked locally (display defaults to on, the others to off after boot). Verify controls on the target unit before relying on them. |

Additional options: `beep_enabled` (default `true`) and `diagnostic_interval` (default `5min`).

The energy diagnostic (`0x0A`) followed by the mostly unmapped status diagnostic (`0x09`) is chained onto a state GET at most once per `diagnostic_interval`. It is only scheduled when `energy`, `energy_available` or `compressor_ticks` is configured. The `0x09` fields are not published. If a diagnostic is unanswered, the scheduler times out and returns to normal polling.

## Protocol and limitations

- Frames use `0xBB`, direction/command/length, payload, and XOR checksum as documented in `AC-hack/Readme.md`. The protocol logic lives in `components/kaeltebringer/protocol.h`, which has no ESPHome dependency.
- Timer write offsets, date/time synchronization (`0x0B`), display messages (`0x05`), sleep-mode labels, and louver fixed-position/range controls are not exposed yet.
- Set requests carry the timer-armed bits from the last reported state but no timer values, since the write offsets are unknown.
- Diagnostics and temperature fields need live hardware verification.

## Tests

`tests/test_protocol.cpp` covers frame parsing/resynchronization, state and energy decoding and set-payload construction using the documented example frames. Run it with any host C++17 compiler:

```
g++ -std=c++17 -Wall -Wextra -o test_protocol tests/test_protocol.cpp && ./test_protocol
```

## YAML example

See `esp8266_sample.yaml` for a complete local configuration. Optional entities can be removed or customized independently.

## Based on

- [AC-hack](https://github.com/adaasch/AC-hack) — protocol investigation and reverse-engineering notes.
- [esphome-kaeltebringer](https://github.com/KG3RK3N/esphome-kaeltebringer) — related ESPHome component.

## AI disclosure

AI tools were used to assist with parts of this project's development and documentation. Their output was reviewed, but may contain errors; verify behavior on your hardware before relying on it.