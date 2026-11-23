# ORI Deterministic Shield (`ori-deterministic-shield`)

[![CI](https://github.com/ori-llc/ori-deterministic-shield/actions/workflows/ci.yml/badge.svg)](https://github.com/ori-llc/ori-deterministic-shield/actions/workflows/ci.yml)
[![Software License: Apache-2.0](https://img.shields.io/badge/Software-Apache--2.0-16A34A.svg)](LICENSE-APACHE)
[![Hardware License: CERN-OHL-P v2](https://img.shields.io/badge/Hardware-CERN--OHL--P%20v2-0A0D12.svg)](LICENSE-CERN-OHL-P-v2.txt)

> **"AI Proposes, Deterministic Code Disposes."**
> Open-source bare-metal C11 safety kernel, STM32 NUCLEO-F401RE WebSerial firmware, and Track B Python MCP Policy Guard built for the [ORI.LLC](https://ori.llc) curriculum and Open-Source Product Foundry.

---

## Architecture

Modern LLMs and agentic planners excel at proposing control actions, setpoints, and portfolio adjustments—but probabilistic models must **never** hold unguarded authority over physical actuators or financial risk limits.

```text
  +---------------------------+      SET <val> / JSON      +------------------------------------+
  | Browser / AI MCP Planner  |  =======================>  |   ORI Deterministic Shield (v0.1)  |
  | (ori.llc / Agentic Tool)  |  <=======================  |                                    |
  +---------------------------+    Telemetry + CRC-16      |  1. Q15.1 Thermal Cutoff (int16_t) |
                                                           |  2. Ceiling / Floor & Slew Clamp   |
        Track A (Hardware): STM32F401RE Cortex-M4F         |  3. Physical E-Stop & IWDG Latch   |
        Track B (Software): python_mcp_guard/policy_guard  |  4. DWT->CYCCNT Cycle Profiling    |
                                                           +------------------------------------+
```

---

## Repository Map

| Path | Description | License |
| :--- | :--- | :--- |
| [`include/ori_shield.h`](include/ori_shield.h) & [`src/ori_shield.c`](src/ori_shield.c) | Pure C11, zero-allocation safety kernel (`int16_t` Q15.1 thermal cutoff, slew-rate clamp, fault latch, `TS_CAL1`/`TS_CAL2` integer math, CRC-16/CCITT-FALSE) | Apache-2.0 |
| [`tests/test_ori_shield.c`](tests/test_ori_shield.c) | Strict host unit tests (`-std=c11 -Wall -Wextra -Werror -pedantic`) covering overflow immunity, thermal latch, E-Stop, watchdog boot latch, and CRC-16 | Apache-2.0 |
| [`firmware/nucleo_f401re/`](firmware/nucleo_f401re/) | Bare-metal STM32F401RE firmware (`16 MHz HSI`, `USART2 PA2/PA3` @ `115200 8N1`, `ADC1_IN18` `TSVREFE=1`/`VBATE=0`, `PA5` LED, `PC13` E-Stop, `IWDG`, `DWT->CYCCNT`) | Apache-2.0 |
| [`python_mcp_guard/`](python_mcp_guard/) | **Track B Companion:** Deterministic bounds, slew-step, drawdown, allowlist, and kill-switch guard for autonomous AI/MCP tool calls | Apache-2.0 |
| [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) | Cross-platform Build, Flash & Test guide (**Linux / Pop!_OS 24.04**, **macOS**, **Windows 10/11**, plus zero-hardware host mode) | Apache-2.0 |

---

## 30-Second Host Verification (Zero Hardware Required)

```bash
# 1. Compile and execute strict C11 fault-injection unit tests
make test

# 2. Run Track B Python MCP Policy Guard unit tests
make py-test
```

For full flashing, serial testing (`SET 400`, `SET 9999`, `PC13` E-Stop, `TEST_HANG` hardware watchdog demo), and Chromium WebSerial setup across Linux/Pop!_OS, macOS, and Windows, see **[`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md)**.

---

## NUCLEO-F401RE Zero-Wiring Pinout (v0.1)

| Signal | STM32F401RE Pin / Peripheral | Role |
| :--- | :--- | :--- |
| USB Virtual COM Port | `USART2` (`PA2` TX / `PA3` RX via ST-LINK/V2-1) | `115200 8N1` command & JSON+CRC-16 telemetry |
| Silicon Temp Sensor | `ADC1_IN18` (`TSVREFE=1`, `VBATE=0`, 480 cycles) | On-chip Q15.1 (`0.5 °C/LSB`) thermal cutoff |
| Status / Fault LED | `PA5` (On-board Green `LD2`) | 1 Hz heartbeat (OK) / 5 Hz rapid blink (Fault) |
| Physical E-Stop | `PC13` (On-board Blue Button `B1`, active-low) | Instant `0` clamp + latched `ORI_FAULT_ESTOP` |
| Hardware Watchdog | `IWDG` (~3.2 s @ LSI/64) | Automatic recovery tested via `TEST_HANG` |
| Cycle Profiler | `DWT->CYCCNT` (`CoreDebug->DEMCR` `TRCENA=1`) | Cycle-accurate execution latency in telemetry |

---

## Roadmap (ORI Open-Source Product Foundry)

- **v0.1 (Live Now):** Pure C11 safety kernel + zero-wiring NUCLEO-F401RE bare-metal firmware + Track B `python_mcp_guard` + GitHub Actions CI.
- **v0.2 (Phase 3A — Snap-On Sensors & Browser Twin):** External NTC thermistor (`PA0` / `ADC1_IN0`), `MPU-6050` IMU (`I2C1 PB8/PB9`) tilt/vibration latch, and direct Chromium WebSerial bridge into [ori.llc](https://ori.llc).
- **v1.0 (Modules 03–06 Product Sprint Skin):** **FreeRTOS + CAN / Modbus RS-485 + CMSIS-NN TinyML Edge Diagnostics Node** targeting industrial solar/PV inverters, EV chargers, and robotics (on CAN-capable MCUs such as `STM32F446RE` or `STM32F401RE` + `MCP2515`).

---

## Licensing (Spin-Out Friendly)

- **Software & Firmware (`include/`, `src/`, `tests/`, `firmware/`, `python_mcp_guard/`):** [Apache License 2.0](LICENSE-APACHE) (permissive commercial use with explicit patent grant).
- **Hardware Schematics & Shield Specs:** [CERN-OHL-P v2](LICENSE-CERN-OHL-P-v2.txt) (permissive open hardware license compatible with university incubators and commercial spin-outs).
