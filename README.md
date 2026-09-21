[![Twitter: @NorowaretaGemu](https://img.shields.io/badge/X-@NorowaretaGemu-blue.svg?style=flat)](https://x.com/NorowaretaGemu)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

<div align="center">
  <a href="https://ko-fi.com/cursedentertainment">
    <img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi" style="width: 20%;"/>
  </a>
</div>
<div align="center">
  <img alt="C++" src="https://img.shields.io/badge/c++%20-%23323330.svg?&style=for-the-badge&logo=c%2B%2B&logoColor=white"/>
</div>

<div align="center">
  <img alt="Arduino" src="https://img.shields.io/badge/-Arduino-323330?style=for-the-badge&logo=arduino&logoColor=white"/>
</div>

<div align="center">
  <img alt="Git" src="https://img.shields.io/badge/git%20-%23323330.svg?&style=for-the-badge&logo=git&logoColor=white"/>
  <img alt="Shell" src="https://img.shields.io/badge/Shell-%23323330.svg?&style=for-the-badge&logo=gnu-bash&logoColor=white"/>
</div>



---

# MILA
## MINIATURE INTEGRATED LOGIC AUTOMATON

- Robot Type: Tank

---

### Software
- [Arduino IDE](https://docs.arduino.cc/software/ide/)

---

## Overview

**Website: <https://cursedprograms.github.io/MILA/>**

MILA is a small tank-chassis robot you can drive over WiFi. It hosts its own web dashboard, streams live sensor data, and can steer itself around obstacles.

- **Drive modes:** WASD (car-style), TANK (independent tracks) and OBSTACLE (autonomous).
- **Live telemetry:** front, left and right distance, temperature, humidity and the last IR remote key.
- **WiFi dashboard:** served by the ESP8266 on port `5010`. MILA hosts her own access point at `192.168.4.1`, or joins NORA's network in fleet mode.
- **Safety:** a collision guard force-stops the robot in manual modes, and a watchdog stops it if the connection drops.
- **Speed:** cycle 100 / 75 / 50 / 25 % from the IR remote, or set an exact value from the dashboard slider.

---

## Hardware

- Small Tank Robot Chassis
- Arduino Bluetooth (Integrated ESP8266)
- 2S 18650
- L298N
- MG95 Servo
- Ultrasonic Sensor
- 5V DC Motors
- RGB LED
- Temperature & Humidity Sensor
- IR Receiver

---

## Quick start

Flash the firmware from `scripts/esp8266` and `scripts/MILA` with the Arduino IDE, power the robot on, and join her WiFi network. Then open the dashboard in a browser, or run a desktop controller:

```
./run.sh                      # macOS / Linux / Git Bash
run.bat                       # Windows (cmd)
.\run.ps1                     # Windows (PowerShell)

./run.sh --host 192.168.4.1 --port 5010
```

If MILA joined NORA's network (fleet mode) she won't be at `192.168.4.1`. Press **SCAN NETWORK** in the C++, C# or F# controller, or start it with `--host auto`.

## Desktop controllers

Every controller lives in [`scripts/`](scripts) and has a build-and-run script in `.sh`, `.bat` and `.ps1` form.

| Language | UI toolkit | Run | Radar & graphs | Gamepad |
|---|---|---|:---:|:---:|
| Python | pygame | `run.sh` | | |
| C++ | Win32 / GDI | `scripts/run-cpp.*` | ✓ | ✓ |
| C# | WinForms | `scripts/run-csharp.*` | ✓ | ✓ |
| F# (preview) | WinForms | `scripts/run-fsharp.*` | ✓ | ✓ |
| Rust | egui | `scripts/run-rust.*` | | |
| Go | Ebitengine | `scripts/run-go.*` | | |
| Julia | GLMakie | `scripts/run-julia.*` | | |

**Controls** (all controllers): `1` `2` `3` switch mode, `WASD` / arrow keys drive, `Q` `A` `E` `D` drive the tracks in TANK mode, `Space` stops, `Esc` quits. Everything is also clickable.

**The C++, C# and F# controllers** also have a proximity radar, live distance / temperature / humidity graphs with CSV export, an exact speed slider, network discovery (`--host auto`), remembered host / port / mode, and Xbox gamepad support (stick or D-pad to drive, triggers for speed, `A` to stop, bumpers to change mode).

## Testing without a robot

`scripts/sim` holds a mock MILA that serves the same endpoints as the firmware with simulated sensor data:

```
scripts/run-sim.sh
scripts/run-cpp.sh --host 127.0.0.1
```

<br>
<div align="center">© Cursed Entertainment 2026</div>
<br>
<div align="center">
  <a href="https://cursed-entertainment.itch.io/" target="_blank">
    <img src="https://github.com/CursedPrograms/cursedentertainment/raw/main/images/logos/logo-wide-grey.png" alt="CursedEntertainment Logo" style="width:250px;">
  </a>
</div>
<br>
<div align="center">
  <a href="https://github.com/SynthWomb" target="_blank">
    <img src="https://github.com/SynthWomb/synth.womb/blob/main/logos/synthwomb07.png" alt="SynthWomb" style="width:200px;"/>
  </a>
</div>
