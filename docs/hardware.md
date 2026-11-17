# Hardware and setup

## What you need

* **ESP32-S3 board.** I used an ESP32-S3 Super Mini (ESP32-S3FH4R2: 4 MB flash,
  2 MB PSRAM, one USB-C port), bought locally for about ₹700. It has to be an
  **S3** (or S2): the normal ESP32 has no USB host, so it can't talk to a printer.
* **USB OTG adapter**, USB-C male to USB-A female.
* The printer's normal **USB A to B cable**.
* **5 V for the board**, anything that can give a few hundred mA.

## Wiring

The Super Mini only has one USB-C port, and that port now goes to the printer.
So the board gets its power through the **5V and GND pins** instead. I just took
5 V from the VIN/GND pins of an old ESP32 dev board that was plugged into USB.
Any 5 V supply works.

On this board the 5V pin is connected to VBUS on the USB-C port, so the printer
side gets 5 V as well (USB devices want to see VBUS before they connect). An easy
way to check: plug a webcam or anything with an LED into the OTG adapter. If it
lights up, power is fine.

The LED on the board (GPIO48, RGB) shows what's going on:

| LED | meaning |
|-----|---------|
| red blinking | joining Wi-Fi |
| blue | Wi-Fi ok, no printer found on USB |
| green | printer ready |
| green blinking | printing |

## Flashing

1. Arduino IDE with the esp32 core 3.x, board **ESP32S3 Dev Module**:
   * USB CDC On Boot: **Disabled**
   * USB Mode: Hardware CDC and JTAG
   * PSRAM: Disabled
   * Flash Size: 4MB
   * Partition Scheme: **Minimal SPIFFS (1.9MB APP with OTA)**
2. In `firmware/MF3010_bridge/`, copy `wifi_config.example.h` to `wifi_config.h`
   and put in your Wi-Fi name and password (2.4 GHz only). `wifi_config.h` is
   ignored by git.
3. The first time, flash over USB from the PC like any other sketch. If the port
   doesn't show up, hold BOOT while plugging it in.
4. After that the USB port belongs to the printer, so updates go over Wi-Fi:
   Sketch > Export Compiled Binary, then upload the `.bin` at
   `http://<board ip>/update` (Firmware).

Since the USB port is busy being a host, the serial monitor goes quiet once the
firmware runs. To find the board's IP, check your router or open
`http://mf3010.local`. If you really want it on serial, set USB CDC On Boot to
Enabled: the IP is printed for 20 seconds and then USB switches to the printer.

The status page at `http://<board ip>` shows the printer state and a live log,
including messages from the ESP-IDF USB stack.

## Printing from Windows over port 9100

This works with the normal Canon driver, the board just forwards the bytes.

1. Install the Canon MF3010 driver as usual (you can do it with the printer on USB).
2. Printer properties > **Ports** > Add Port > **Standard TCP/IP Port** > the board's IP.
3. Device type: Custom > Settings: **Raw**, port **9100**, **SNMP off**.
4. Untick "Enable bidirectional support" on the Ports tab.

If jobs sit in the queue with "Unknown status", the port was probably created with
SNMP on. Fix the port settings, then restart the print spooler and clear the queue.

## Problems I ran into

* **Nothing ever detected on USB.** Since arduino-esp32 3.3.11 the USB host needs
  an `enum_filter_cb`, otherwise enumeration just stops without any error. The
  sketch sets one.
* **No serial output.** Expected, see above. The USB port is in host mode.
* **Device shows up but never finishes connecting.** Flip the OTG adapter or
  replug it. The firmware also power-cycles the port by itself after 15 seconds
  (and there's a "Reset USB port" link on the status page).
