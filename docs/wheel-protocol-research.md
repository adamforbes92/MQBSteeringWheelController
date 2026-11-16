# Steering-wheel protocol research (16/11/2026)

This is desk research from public sources, done to decide which wheels the controller should
support next. Each fact is tagged by how good its evidence is:

- **CAPTURE**: someone posted real bus data.
- **CODE**: the value is in published source code.
- **CLAIM**: stated somewhere without data.

Checksums marked "recomputed" were checked by us against the posted bytes. No byte value in
this file was guessed; if something wasn't found, it says so.

What the firmware supports today (V1.10):

| Wheel protocol | Frames | Checksum |
| --- | --- | --- |
| PQ / MQB | `0x0D` light, `0x0E` buttons, `0x0F` accessory, `0x3A` temperature | LIN 2.x enhanced |
| VAG LIN 1.3 | `0x01` master frame, `0x30` buttons, `0x21` status | LIN 1.x classic |

All of them run at 19200 baud.

---

## 1. VW group

### VAG LIN 1.3: one protocol across Audi and early PQ35 (now supported)

The frames are `0x30` (6 bytes, from the wheel), `0x21` (2 bytes, from the wheel) and `0x01`
(2 bytes, from the master). The checksum is classic.

- **Leon 1P `1P0959542` / Octavia 2 `3C0959542`** on J527 `1K0953549BP`
  ([katzenberger](https://github.com/katzenberger/vag-mfsw-lin-bus-protocol), CAPTURE,
  checksums recomputed):
  - `0x30` byte 2 holds the button code. Leon: `1E 2A 1D 01 02 06 03 07`. Octavia:
    `04 2B 05 50 02 06 03 07`. These match radio CAN `0x5C1`. Two buttons at once give `00`.
  - The horn is byte 3 = `04` (it also shows on CAN `0x2C1`).
  - `0x21` byte 1 is the button layout: `22` Leon, `33` Octavia, `03` right pod only, `30`
    left pod only.
  - The master sends `0x01` as `50 03` with ignition on and `00 03` with it off.
  - Schedule: `0x30` every 20 ms, `0x01` every 40 ms, `0x21` every 200 ms. At sleep the master
    sends `0x3C` with 8 × `00`.
- **Audi A4 2013**
  ([Arduino forum 934599](https://forum.arduino.cc/t/audi-steering-wheel-lin-bus-reading/934599),
  CAPTURE):
  - The PIDs seen are `C1` / `61` / `F0` / `50`, i.e. IDs `0x01` / `0x21` / `0x30` / `0x10`.
  - The car's own frames: `0x01` = `00 FF` and `0x10` = `00 80 FF`. Neither is decoded.
  - Byte 2: `08` mute, `04` voice, `80` select press, `01` mode.
  - Byte 4: `10` / `F0` volume, `01` / `0F` selector.
- **Audi A8 (customer)**, CAPTURE: idle `02 10 00 00 00 3F`; the only change seen was byte 4's
  high nibble, 1 to 6.
- **Service manuals** (CLAIM) describe the wheel as a LIN slave on the A3 8P, TT 8J, A4
  B6–B8, A5 8T, Q5 8R, A6 C6, A8 D3 and R8 gen 1.
  - A8 D3 (SSP 293) and A3 8P: the tiptronic paddles go through the wheel module (E221 /
    J453) onto LIN.
- **B7/B8-era hardware** (A4 B8, A5 8T, Q5 8R), CLAIM, from J527 socket B pinouts in three
  workshop sources:
  - Pin 11 is "dimming (terminal 58s)", so the button lighting is a PWM wire, not LIN.
  - The paddles are hardwired: pin 10 up, pin 7 down, pin 12 tip ground. Pin 13 on the A4 B7
    cabrio is heated wheel +.
- **Ross-Tech**, CLAIM: J527 `1K0953549` up to -C(x), and `3C0953549` up to -AF, are LIN 1.3
  only. -CD / -CH / -CK and 3C0…AH are 1.3 and 2.0. -CE / -CF / -CJ and 3C0…AJ / AK are 2.0
  only.

### LIN 2.x PQ / MQB: extra detail for what we support

- **Codes in two independent repos** (CODE, mka1ugin + ufnalski):

  | Code | Meaning |
  | --- | --- |
  | `06` | right thumbwheel scroll (byte 3 `01`–`03` up / `0D`–`0F` down) |
  | `12` | left thumbwheel scroll (volume) |
  | `07` | right thumbwheel press / OK |
  | `13` | left thumbwheel press (mute) |
  | `25` | heated-wheel button |
  | `0C` | assist |
  | `02` / `03` | next / previous window (source) |
  | `19` | voice |
  | `15` / `16` | next / previous track |
  | `1C` | phone (v-ivanyshyn) |

  `10` / `11` (volume) appear only on rocker wheels.
- **Byte 3** is 1 for a click, then 4 / 5 / 6 for longer presses (matches our long-press
  rule).
- **Byte 4** marks the wheel type: `A3` Skoda, `90` VW (Dimka8901, CODE). The PQ responses
  that PQ46 J527s accept use byte 4 = `60`, not `42` (v-ivanyshyn, mka1ugin, CODE).
- **`0x0F` cruise/ACC bitmap** (v-ivanyshyn, CODE):
  - byte 2: bit 0 SET, bit 1 +, bit 2 −, bit 3 RES, bit 5 DIST (bit 6 on wheels with a View
    button);
  - byte 1: bit 4 ON/OFF, bit 7 MODE.
- **`0x3A` temperature**: °C = raw − 68, heater cut-off `0x62` (v-ivanyshyn, CODE). It is
  only present on heated wheels.
- **`0x0D` backlight**: byte 0 is brightness `00`–`7F`. Byte 1 must keep its enable bits;
  working values `FF`, `F9` and `81` all have bits 7 and 0 set (our inference). Examples:
  - `FF FF 00 00` lights the buttons and `00 FF 00 00` turns them off (drive2, CAPTURE);
  - `1A F9 FF FF` works (CODE);
  - Dimka8901 uses `xx 81 64 40`, with BCM brightness `00`–`64` copied into byte 0 (CODE);
  - A3 8V: the light times out about 7 s after the last `0x0D` (CLAIM).
- **Up! / Citigo / Mii (NSF)**: `0x0D` is only 2 bytes, `brightness 00–7F, FF`. Without the
  `FF` the wheel stops reporting buttons (riquenunes, CODE). The horn is a wire, not LIN.
- **A PQ25/PQ46 J527 reads the wheel's identity over LIN diagnostics** (CODE, mka1ugin /
  Dimka8901):
  - The request is NAD `0A` on master frame `0x3C`, answered on `0x3D`.
  - It reads DIDs `065E 06BE 06EE 068E 062E 074E 07BE 071E`.
  - If our LIN2 doesn't answer these, a PQ25 car (T5/T6, Polo) may reject the wheel.
- **MQB wake**: polling `0x0D`, `0x0E`, `0x0F` and `0x3A` together in a schedule, not one ID
  alone, wakes the wheel (drive2, CAPTURE).

### MQB-Evo (Golf 8 / Octavia 4 / Leon 4) and MEB

- **Golf 8 buttons** are on LIN (CLAIM). The byte layout and touch-slider encoding are **not
  public**, and we found no independent source for our `81 64 40` activation bytes.
- **Golf 8 clock spring `5Q1953549F`** carries a second LIN for the hands-on sensor (KLR),
  CLAIM.
- **MEB pre-facelift capacitive pads** are **CAN**, not LIN (two independent CLAIMs). Wiring
  one to LIN sounds the horn non-stop, so they can't be supported.
- **Golf 8 pad in an ID.3**: everything works except the backlight and the ACC button. That
  fits "MEB's `0x0D` doesn't light MQB pads".

### Older / other

- **Pre-2004 MFL (Golf 4 / Passat B5 era)** is a single 5 V wire carrying an NEC-like pulse
  code, packet `41 E8 code ~code`. UP `D0`, DOWN `50`, LEFT `40`, RIGHT `C0`, VOL+ `80`,
  VOL− `00` ([tomaskovacik/VAGRadioRemote](https://github.com/tomaskovacik/VAGRadioRemote),
  CODE).
- **Crafter 2006–16 and Touareg 7L**: buttons are on CAN (CLAIM). Not found: T5 combined
  switch, Amarok, Phaeton, Touareg 7P / CR.
- **Porsche 958.1** is LIN 1.3 with PWM backlight; **958.2** is LIN 2.0 with the backlight
  and heater on LIN (adapter vendor on Rennlist, CLAIM). 991.1 / 981 are LIN, with captures
  only in photos.
- **Audi LIN 2.x** (B9, C7, D4 and later, MLB-evo): no frames are published.
  - A B9 wheel behind a LIN-to-CAN converter in a B8 lights for 5 s and then goes dark, which
    suggests a backlight keep-alive.
  - The MLB-evo body-control LIN LDF uses `DI_KL_58d` (8 bit, 0–253, 254 Init, 255 Error) for
    dimming (hackaday, CODE). That's a candidate format for the wheel's light frame.

## 2. Other brands

| Wheel | Bus | Frames | Evidence | Fit |
| --- | --- | --- | --- | --- |
| Volvo P3 (V40 2012+, S60/V60/XC60 2011–18) | LIN, **9600**, classic | master polls `0x30` (left) and `0x20` (right); light `0x10` = `FF 00 00 00 00 00 00 00` every 500 ms | CODE (Arduino forum 1435919) | excellent once LIN1 baud is selectable; button bytes need a capture |
| Volvo P1 (V50/S40/C30/C70) | LIN, **9600**, classic | `0x20`, 4 bytes; e.g. NEXT `00 10 00 00`, VOL+ `00 00 01 00`, joystick bits in byte 0 | CODE ([laurynas/volvo_linbus](https://github.com/laurynas/volvo_linbus)); Back/Enter labels look swapped (checksums recomputed) | good with a 9600 baud mode |
| Volvo P2 XC90 | CAN 125k, 29-bit `0x404066` | `data[6]&0x3F`, `~data[7]&0x3F` | CODE ([olegel/VolvoCan](https://github.com/olegel/VolvoCan)) | CAN listener |
| BMW E-series / Mini R5x MFL | K-bus / I-bus 9600 8E1, no master | src `0x50`, XOR checksum; vol `50 04 68 32 11 1F` / `…10 1E`; buttons cmd `0x3B` (01 next, 08 prev, 40 R/T, 80 tel; press 00 / hold 10 / release 20) | CODE + docs ([wilhelm-docs](https://piersholt.github.io/wilhelm-docs), [sda2](https://github.com/sda2/arduino-ibus-integration)) | read-only via a LIN transceiver (MCP2025 used in CODE); TJA1021 untested |
| BMW E8x/E9x/E84 | K-CAN `0x1D6`, 2 bytes | idle `C0 0C`, vol+ `C8 0C`, vol− `C4 0C`, up `E0 0C`, down `D0 0C`, tel `C1 0C`, voice `C0 0D` | CAPTURE (loopybunny) | CAN listener; bitrate unconfirmed |
| Tesla Model 3 (gen A) | LIN 19200 | poll ID `0x26`, 4 bytes; table in source | CODE ([dance-parrot/m3-yoke-lin](https://github.com/dance-parrot/m3-yoke-lin)) | fits today's hardware |
| Tesla Model 3/Y (gen B) | LIN 19200, enhanced | `0x2A` left wheel (7 bytes, byte 0 idle `0C` / up `0D` / down `0B` / click `2C`), `0x2B` right (6 bytes) | CAPTURE (lakewoodphone/xiao-lin-bench) | fits; right wheel values missing |
| Mercedes W203/W211 | CAN-B 83.3k `0x1CA` data[1] | `01` up, `02` down, `40` answer, `80` hang up | CODE (rnd-ash/W203-canbus); vol codes unconfirmed, `0x330` disputed | CAN listener |
| Mercedes C117 (N135 → N80) | LIN, ID **`0x15`** (PID `0x55`, the same byte as SYNC), enhanced | pairs of buttons send identical frames | CAPTURE (mbworld 924667, recomputed) | low |
| Jeep Liberty 2011 | LIN **9600**, ID `0x1F`, 5 bytes | B2 always `FF`, horn in B1 | CAPTURE-ish (jk-forum) | needs data |
| PSA (AEE2004) | CAN `0x21F`, 3 bytes | byte 1 bits SEEK / VOL / SRC / LIST, byte 2 signed scroll | CODE (prototux/PSA-RE) | CAN listener |
| Opel Astra H / Corsa D era | MS-CAN 95.2k `0x206` | byte 1 `81` / `91` / `92` | CODE (PNKP237/EHU32) | CAN listener |
| Ford Fiesta | MS-CAN 125k `0x2D5` byte 2 | `10` mode, `40` / `80` seek | CODE (roncapat) | partial |
| GM 2014+ | LIN (CLAIM, ~10.4 kbit/s) | not found | CLAIM | needs a capture plus 10417 baud |
| BMW F/G, Mercedes W205+, Volvo SPA, Ford LIN packs | LIN (CLAIM) | not found | — | needs a capture |
| Hyundai/Kia, Toyota, Honda, Nissan, Mazda, Subaru, Lada/Renault | resistive ladder (CLAIM) | — | — | no analogue input on this hardware |

## 3. What this points to (in order of evidence × demand)

1. **Done in V1.10**: the VAG LIN 1.3 profile (above), a wheel-detect LIN1 scan, and LIN2
   capture of every chassis frame.
2. **Done in V1.12**: per-wheel profiles. These are PQ, MQB, VAG LIN 1.3, VW Up!/Citigo/Mii
   (2-byte `0x0D`), Volvo P1, Volvo P3, BMW K-bus (listen only), and **Custom** (baud
   9600 / 10417 / 19200, checksum, 1–2 button frames with auto length, code-byte or bit
   decoder, light frame template + brightness byte). The scan also tries 9600. None of the
   new profiles has been tried on a real wheel yet. Jeep (9600, `0x1F`) and GM (10417) can
   be set up through Custom once someone captures their button bytes.
3. **`0x0D` options still open**: copying brightness from the car's own frame (we already
   send `0x0D` every 100 ms, well inside the ~7 s keep-alive).
4. **LIN2 side for PQ25/PQ46 J527s**: answer LIN diagnostic identity reads (NAD `0A` on
   `0x3C`/`0x3D`).
   - Commercial demand: the OEMplus CANBridge G7MFL, polo.blue's cruise emulator, and
     Dimka8901's MQB-Evo to PQ25 adapter, all for Golf 7/8 wheels into T5/T6 or Polo.
5. **Decode MQB `0x0F`** (cruise bitmap) and `0x3A` (temperature, raw − 68) into named
   outputs.
6. **CAN listener profiles** for column modules (PSA, Opel, Volvo P2, BMW E9x, Mercedes
   W203), feeding the existing mapping and resistive output.
7. **Hardware idea for B7/B8-era Audi**: the high-side output could drive the terminal-58s
   button-lighting PWM wire. The paddles there are plain switches to ground.
8. **Needs captures from customers** before anything can be built: Golf 8 and MQB-Evo, Audi
   LIN 2.x, BMW F/G, Porsche, GM. The V1.10 wheel scan and LIN2 capture are what to ask
   them to export.
