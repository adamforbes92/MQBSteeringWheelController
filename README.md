# MFSW Controller

The **MFSW Controller** is a designed to:
> control backlights
> read button pressses and translate into LIN, CAN, resistive or high-side outputs

It's based on VAG (PQ) steering wheels and can be used as a man-in-the-middle adapter to control different steering wheels fitted into different marques.  It **reads button presses and backlight data off the steering-wheel LIN bus**, **re-maps** each new button to
whatever your old hardware expects, and **sends** the result on up to four output
channels at once: a **translated LIN frame**, a **CAN broadcast**, a **resistive
(digital-potentiometer) line** for legacy resistor based radios (up to 10k), and a **high-side
driver output** for switching a relay or light. An optional PWM **auxiliary light input** feeds
automatic backlight dimming (for MK4 chassis, etc), and everything is configured from a phone over **Wi-Fi** —
no laptop or serial cable needed in the car.

It runs on an **ESP32 DevKit V1 (WROOM-32)** under the Arduino framework, and uses the
[LIN_master_portable_Arduino](https://github.com/gicking/LIN_master_portable_Arduino)
library for the LIN state machines, the ESP32 **TWAI** peripheral for CAN, and
[X9C10X](https://github.com/RobTillaart/X9C10X) for the digital potentiometer.

> The buttons are designed with a known-good PQ baseline that you re-learn to suit your wheel.

---

## Features at a Glance

| Feature | Detail |
|---|---|
| LIN input (wheel) | Polls the steering-wheel LIN bus (ID `0x0E`) for button IDs and paddle state at 19.2 kbit/s |
| LIN light input | Reads chassis-bus dimming (ID `0x0D`) to drive the wheel backlight |
| Button re-mapping | Up to **24** mappings, each with a name, original ID, new LIN ID, CAN bit, resistance and flags |
| LIN output | Sends the translated button as a LIN master request on the chassis bus (configurable ID) |
| CAN output | Broadcasts an 8-byte button frame (configurable ID, default `0x1E0`) on 500 kbit/s TWAI |
| Paddle via CAN | Emits a GRA (cruise/shift) frame (`0x38A`) carrying paddle up/down |
| Resistive output | Drives an X9C10X digital pot to emulate a resistor-ladder radio input (per button specified ohms) |
| High-side driver | Switchable output (up to 5A) — momentary or latched per button, plus diagnostic override |
| Aux backlight | PWM light input decoded to a brightness %, with learnable dim/bright duty calibration |
| Forced / source-select backlight | Choose Aux PWM, forced fixed %, or pass-through LIN brightness |
| Learn mode | Capture a wheel's button IDs and aux duty limits directly from the live bus |
| Wi-Fi UI | Dashboard, setup, button builder, diagnostics, OTA |
| OTA updates | Flash new firmware from the browser over Wi-Fi |
| Bus health | Live CAN / LIN 1 / LIN 2 / resistive status in the UI |
| Power management | Auto Wi-Fi-off + CPU reduction 1 min after the last client disconnects |
| EEPROM | All settings and the button map stored to ESP32 Preferences (NVS) |

---

### Purchase
If you want to purchase an assembled controller, you can do so here: [LIN MFSW Controller - Forbes Automotive](https://forbes-automotive.com/products/lin-mfsw-controller)

## Compatibility

Designed for VAG multi-function steering wheels that report buttons over a **LIN slave
response** and (optionally) carry paddle shifters and a LIN-dimmed backlight. Button IDs,
LIN frame IDs and the CAN/GRA frame set are **car- and wheel-specific** — treat the
shipped `buttonMappings[]` table (in [src/globals.cpp](src/globals.cpp)) as a known-good
baseline and re-learn each button to suit your hardware.

> Frame IDs and calibration live in [include/defs.h](include/defs.h) and
> [src/globals.cpp](src/globals.cpp). The resistive output values are tuned to your
> radio's resistor ladder — characterise them with the **Test Resistance** diagnostic or a datasheet for your radio.

---

## Hardware Overview

### External Interfaces

| Function | Part / circuit |
| --- | --- |
| Steering-wheel LIN (LIN 1) | LIN transceiver on `Serial1`, 19.2 kbit/s — the steering wheel is the slave, this device is master |
| Chassis LIN (LIN 2) | LIN transceiver on `Serial2`, 19.2 kbit/s — reads light dimming value from chassis, sends translated buttons |
| CAN | ESP32 TWAI + transceiver (TJA1050 / SN65HVD230), 500 kbit/s |
| Aux light input | 12V PWM dimming signal → level-shifted / opto-isolated |
| Resistive output | X9C10X digital potentiometer for the radio's resistor values |
| High-side driver | Transistor output (OUTPUT1) for a relay / light (5A max.) |

### Pin Map

Defined in [include/defs.h](include/defs.h):

| Group | Signal | GPIO |
| --- | --- | --- |
| LIN 1 (wheel) | TX / RX | 17 / 16 |
| LIN 2 (chassis) | TX / RX | 23 / 22 |
| LIN 2 | Wake / CS | 18 / 19 |
| CAN / TWAI | RX / TX | 13 / 14 |
| Aux light | PWM input (input-only pin) | 39 |
| Resistive pot (X9C10X) | UD / INC / CS | 25 / 26 / 27 |
| High-side driver | Output ("PNP") | 21 |

### Main Connector

The device breaks out to a single 12-way **MX23A12NF1** connector:

| Pin | Signal | Notes |
| --- | --- | --- |
| 1 | `PWR_IN` | 12 V switched / ignition supply |
| 2 | `GND` | Ground |
| 3 | `LIN1` | Steering-wheel LIN bus |
| 4 | `LIN2` | Chassis LIN bus |
| 5 | `CHASSIS_CANH` | CAN high |
| 6 | `CHASSIS_CANL` | CAN low |
| 7 | `ANALOG_LIGHT_IN` | PWM auxiliary backlight input |
| 8 | `RA` | Resistive output (digital-pot wiper to the radio) |
| 9 | `5V` | 5 V rail - typically not required |
| 10 | `OUTPUT1` | High-side driver output |
| 11 | — | Unused |
| 12 | — | Unused |

---

## How It Works

The steering wheel LIN bus, the chassis LIN bus and the CAN bus are ran by dedicated FreeRTOS
tasks. A single steering wheel task owns both LIN state machines sequentially (guarded by
mutexes) to avoid corrupting the shared LIN peripheral, and a separate output task decides what data to send - either LIN, CAN, the high-side driver or the resistive line.

```
Steering-wheel LIN (0x0E)          Chassis LIN light (0x0D)         Paddle bits
        │                                  │                             │
        ▼ steeringWheelLinTask (core 1)    ▼ getLightLINFrame()          ▼ broadcastGRATask
 read button ID + paddles           read dimming %               GRA frame 0x38A
        │                                  │                             │
        ▼ sendButtonLINFrame()             ▼ updateBacklightState()      ▼ (if Paddle via CAN)
 look up mapping ──────────────┐    aux / forced / LIN source
        │                      │           │
        │                      │           ▼ sendLightLINFrame() → wheel backlight
        ▼                      ▼
 chassis LIN out         canHoldFrame[] (bit set)
 (if LIN Output on)            │
                              ▼ debounceOutputTask (core 0, ~20 Hz)
                    ┌──────────┼─────────────────────┐
                    ▼          ▼                     ▼
             CAN broadcast   high-side driver   resistive output (X9C10X)
             (0x1E0, if on)  (momentary/latch)  (per-button ohms)
```

### Button Mapping

Each of the up to 24 rows in the button map carries:

| Field | Meaning |
| --- | --- |
| **Name** | Purely for understanding, not used in code (e.g. `Volume +`) |
| **Button ID_LIN (original)** | The raw ID reported by the wheel — learnable |
| **Button ID_LIN (new)** | The ID re-emitted on the chassis LIN output — learnable |
| **CAN Byte / Bit** | Which bit of the 8-byte CAN frame to set (byte `0xFF`/255 = no CAN) |
| **Resistive Output (Ohm)** | Resistance to present on the X9C10X while held (0 = none) |
| **PNP** | Flag: drive the high-side output while pressed |
| **Latch** | Flag: toggle the high-side output on each press instead of momentary |

When a mapped button is seen, its CAN bit is held for the configurable **Send on CAN**
window (`canHoldMs`, 50–5000 ms) so brief presses still produce a clean pulse.

### Backlight Sources

`updateBacklightState()` picks a brightness in priority order:

1. **Aux** — decode the PWM duty on GPIO 39, map it between the learned dim/bright duty
   limits to the LIN brightness range (0–`0x7F`).
2. **Forced** — hold a fixed user-set percentage.
3. **LIN pass-through** — forward the chassis-bus dimming value read from ID `0x0D`.

The Aux input is measured by an edge ISR that accumulates period/on-time over a 1-second
window and averages hundreds of cycles to cancel optocoupler jitter.

---

## Wi-Fi & Web Interface

Connect to the **`MFSWController`** Wi-Fi access point and browse to
**`http://192.168.1.1/`**.  All changed settings are automatically saved.

| Tab | Purpose |
| --- | --- |
| **Dashboard** | Live active button (LIN ID + resolved name), CAN output frame with per-bit view, backlight source/state/brightness, and raw incoming/outgoing LIN + CAN frames |
| **Setup** | Broadcast-over-CAN toggle + CAN ID, Paddle via CAN, Send-on-CAN window, aux-light source, force backlight + brightness slider, LIN output enable + ID, and aux dim/bright duty calibration with **Learn** buttons |
| **Buttons** | The button builder table — add/delete up to 24 rows, edit every field, and **Learn** original/new LIN IDs directly from the wheel |
| **Diagnostics** | Bus health (CAN / LIN 1 / LIN 2 / resistive), high-side driver test toggle, resistive-output hold, and a stepped Test Resistance probe |
| **OTA** | Upload a firmware `.bin` over Wi-Fi and reboot |

---

### Learn Mode

Learn mode captures live values from the bus into a specific field:

- **Button IDs** — press *Learn* on a row's original or new LIN column, then press the
  physical button; the next non-zero ID seen on the wheel is captured. Learn windows time
  out after 5 seconds.
- **Aux duty limits** — with a PWM light signal present, *Learn Dim* / *Learn Bright*
  capture the current duty (in tenths of a percent) as the calibration endpoints.

---

## Power Management

The firmware bundles the universal `power_manager` module that is used across most of the projects and works by monitoring **1 minute
after the last Wi-Fi client disconnects**. Once there are no WiFi clients, the CPU power and WiFi is disabled, reducing power consumption.

| Action | Saving |
| --- | --- |
| Wi-Fi radio off | ~80–120 mA average (single biggest) |
| CPU 240 MHz → 80 MHz | Moderate reduction in active current |
| Bluetooth controller released at boot | ~60 KB RAM freed; small idle saving |
| Onboard LED off at boot | Tiny but persistent saving |

A power-cycle (ignition off/on) brings Wi-Fi back. 

---

## Configuration

### Feature Flags

| Define | Default | Effect |
| --- | --- | --- |
| `ENABLE_DEBUG` | on | Mirror `DEBUG(...)` output to Serial |
| `hasCAN` | 1 | Compile in CAN/TWAI output and RX |
| `hasResistiveStereo` | 1 | Compile in the X9C10X resistive output |
| `hasAuxLight` | 1 | Default the backlight source to the aux PWM input |
| `ChassisCANDebug` | 0 | Print every received CAN frame |
| `detailedDebugWiFi` | 0 | Print Wi-Fi events |

### Key Defaults

| Setting | Default |
| --- | --- |
| CAN broadcast ID | `0x1E0` |
| CAN hold window | 250 ms |
| LIN baud / poll | 19.2 kbit/s / 100 ms |
| Aux dim / bright duty | 19.7 % / 98.0 % |
| Backlight max (LIN) | `0x7F` |
| Base resistance | 10 kΩ |
| Wi-Fi AP / IP | `MFSWController` / `192.168.1.1` |

---

## Version History

The firmware version (`FW_VERSION` in [include/defs.h](include/defs.h)) is shown in the
web UI header badge and on the OTA tab. The full history lives in
[include/ver.h](include/ver.h):

```
V1.00 — initial release
```

---

## Disclaimer

Forbes Automotive accepts no responsibility for any incidents arising from the use of this
adapter. Frame IDs, resistances and calibration are vehicle-specific — verify against your
own hardware before relying on any output.
