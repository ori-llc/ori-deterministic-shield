# Getting Started: Build, Flash & Test (`ori-deterministic-shield`)

This guide covers:
1. **Zero-Hardware Host Testing** (works on any Linux, macOS, or Windows machine in <1 second).
2. **Linux (Ubuntu / Debian / Pop!_OS 24.04)** ARM cross-compilation, permissions, and flashing.
3. **macOS (Apple Silicon & Intel)** ARM toolchain and flashing.
4. **Windows 10 / 11** toolchain and zero-CLI drag-and-drop flashing.
5. **Live Interactive Hardware Tests** (`SET`, `RESET`, `PC13` E-Stop, `TEST_HANG` Watchdog, and Chromium WebSerial).

---

## 1. Zero-Hardware Host Tests (Any OS)

Verify the C11 deterministic safety kernel (`-std=c11 -Wall -Wextra -Werror -pedantic`) and Track B Python MCP Policy Guard without connecting any board:

```bash
git clone https://github.com/ori-llc/ori-deterministic-shield.git
cd ori-deterministic-shield

make test     # Compiles & runs C11 fault-injection & Q15.1 overflow tests
make py-test  # Runs Track B Python MCP Policy Guard unit tests
```

---

## 2. Linux (Ubuntu / Debian / Pop!_OS 24.04)

### A. Install Compilers & ST-LINK Tools
```bash
sudo apt update
sudo apt install -y \
  build-essential \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  gdb-multiarch \
  stlink-tools \
  picocom \
  usbutils \
  python3-serial
```

Verify installation:
```bash
gcc --version
arm-none-eabi-gcc --version
st-flash --version
```

### B. Configure Non-Root USB & Serial Permissions
```bash
# Allow st-flash, picocom, miniterm, and Chromium WebSerial without sudo
sudo usermod -aG dialout,plugdev "$USER"

# Prevent Ubuntu/Pop!_OS brltty Braille daemon from hijacking /dev/ttyACM0
sudo systemctl stop brltty-udev.service 2>/dev/null || true
sudo systemctl mask brltty-udev.service 2>/dev/null || true

sudo udevadm control --reload-rules
sudo udevadm trigger
```
> **Important:** Log out and log back into your desktop session once (or reboot) so `dialout` group membership applies to both your terminal and Chromium/Brave/Chrome. Confirm with `groups | grep dialout`.

### C. Connect, Cross-Compile & Flash NUCLEO-F401RE
Verify two physical jumpers on your NUCLEO-F401RE before plugging in the **USB Mini-B data cable** (not charge-only):
- **`CN2`** (two black ST-LINK jumpers): **both ON**
- **`JP5`** (power selector): **`U5V`**

Probe the board:
```bash
st-info --probe
# Expect: Found 1 stlink programmers, chipid 0x0433 (F401xD/E), flash 512KB, sram 96KB

ls -l /dev/ttyACM*
# Expect: /dev/ttyACM0 (group: dialout)
```

Build and flash:
```bash
cd firmware/nucleo_f401re
make
st-flash --reset write build/ori_shield_f401re.bin 0x08000000
```

---

## 3. macOS (Apple Silicon & Intel)

```bash
brew install --cask gcc-arm-embedded
brew install stlink
python3 -m pip install pyserial

cd firmware/nucleo_f401re
make
st-flash --reset write build/ori_shield_f401re.bin 0x08000000
```

Find your Virtual COM Port and open a serial terminal:
```bash
ls /dev/cu.usbmodem*
python3 -m serial.tools.miniterm /dev/cu.usbmodemXXXX 115200
```

---

## 4. Windows 10 / 11

1. **Install Arm GNU Toolchain & Make:**
   - Install via `winget install Arm.GnuArmEmbeddedToolchain` (or download from developer.arm.com) and ensure `arm-none-eabi-gcc` is on `PATH`.
   - Alternatively, build inside **WSL2 Ubuntu** using the Linux commands above.
2. **Install ST-LINK USB Driver (if `COM` port does not appear in Device Manager):**
   - Install ST's `STSW-LINK009` USB driver (includes Virtual COM Port support).
3. **Zero-CLI Drag-and-Drop Flashing (Works on Windows, macOS & Linux!):**
   - Plug in the NUCLEO-F401RE; it mounts as a USB mass-storage drive named **`NODE_F401RE`**.
   - Copy `firmware/nucleo_f401re/build/ori_shield_f401re.bin` onto `NODE_F401RE`. The ST-LINK LED flashes red/green and automatically resets the MCU.
   - > **Troubleshooting `FAIL.TXT`:** If older Nucleo ST-LINK firmware creates a `FAIL.TXT` file on `NODE_F401RE`, update the on-board ST-LINK firmware once using ST's **STSW-LINK007** utility (or flash via `st-flash` / STM32CubeProgrammer).
4. **Serial Console:**
   - Check Device Manager → *Ports (COM & LPT)* for `STMicroelectronics STLink Virtual COM Port (COMx)`, then run:
     ```powershell
     python -m serial.tools.miniterm COM3 115200
     ```

---

## 5. Live Hardware Test Checklist ("AI Proposes, Deterministic C Disposes")

Open your serial monitor at **`115200 8N1`** (e.g., `picocom -b 115200 /dev/ttyACM0` or `python3 -m serial.tools.miniterm /dev/ttyACM0 115200`; exit `picocom` with `Ctrl+A` then `Ctrl+X`, or `miniterm` with `Ctrl+]`).

You will immediately see 2 Hz JSON + CRC-16 telemetry frames:
```text
{"tick":10,"temp_q15_1":56,"proposed_u":0,"clamped_u":0,"faults":0,"cyccnt":42}*0xA1B2
```

Run these 4 live silicon tests:

1. **Nominal Slew-Rate Clamping:**
   Type `SET 400` + Enter → watch `clamped_u` ramp deterministically by at most `100` per 100 ms tick (`100 → 200 → 300 → 400`) with `faults:0` and live `DWT->CYCCNT` cycle count.
2. **Hallucinated / Out-of-Bounds AI Command:**
   Type `SET 9999` + Enter → bare-metal C clamps `clamped_u` strictly at `1000` (`ORI_DEFAULT_MAX_U`) with zero integer overflow.
3. **Physical Emergency-Stop (`PC13` Blue Button `B1`):**
   Press the blue button on the Nucleo → `clamped_u` drops immediately to `0`, `faults` latches bit 1 (`2`), and green LED `LD2` (`PA5`) switches to rapid 5 Hz fault-blink. Release the button and type `RESET` + Enter to clear the latch safely.
4. **Hardware Watchdog Recovery (`TEST_HANG`):**
   Type `TEST_HANG` + Enter → the firmware intentionally freezes in an infinite loop; ~3.2 seconds later, the hardware Independent Watchdog (`IWDG`) resets the chip and prints `ORI_FAULT_WATCHDOG latched` on reboot. Type `RESET` + Enter to recover.

### Chromium WebSerial Check (Chrome / Brave / Edge)
Close your terminal serial monitor first (only one process can hold `/dev/ttyACM0` / `COMx` at a time). Open a Chromium-based browser (Firefox/Safari do not support WebSerial) and verify `STM32 STLink` appears in the WebSerial device picker.
