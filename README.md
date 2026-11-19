# Canon imageCLASS MF3010 Wi-Fi bridge (ESP32-S3)

My USB-only Canon MF3010 laser printer, now a Wi-Fi printer that phones and laptops
find on their own, no Canon driver needed. The whole "driver" runs on a ~₹700
ESP32-S3 plugged into the printer's USB port.

## Why

I have a Canon imageCLASS MF3010. It's a great little laser printer: cheap to run,
fast, and it never complains. It also only has a USB port, and that port is plugged
into my PC.

So whenever my mom needed to print something, a document on her phone or a file
on her laptop, it came to me. Either she sent it over and I printed it, or I
unplugged the printer from my PC and connected it to her laptop. Half the time her
laptop wouldn't run the Canon driver properly and nothing printed. It's a small
thing, but it happened often enough that I wanted to fix it properly. She should be
able to tap Print on her phone, and it should just come out.

I had a cheap ESP32 lying around, so my first thought was: can't I just wire it up
and make the printer wireless? Turns out the classic ESP32 can't act as a USB host
at all. So I bought an **ESP32-S3 Super Mini** locally for ₹700 and powered it from
my old ESP32's VIN and GND pins.

Then the fun started:

- **Serial went silent.** The board's only USB port was now talking to the printer,
  so it had nowhere to print its logs. So the board got a status page with a live log instead.
- **The printer was never detected.** I plugged in a webcam to test, and its LEDs lit
  up, so power was fine. The real cause was that newer ESP32 Arduino cores quietly
  cancel USB setup unless you give them one specific callback. Two lines of code, a
  whole evening to find them.
- **Windows jobs sat at "Unknown status"** until the port settings were right.

Then the first page came out over Wi-Fi.

But my mom's phone still couldn't print. The MF3010 is a "host-based" printer: it
doesn't understand PDFs or images, only Canon's own language, UFR II LT, which only
Canon's PC driver produces. So I captured what the driver sends and took it apart:

- Canon's packet wrapper, called CPCA;
- the page drawn at a weird **600 x 400 dpi**;
- the page sent in 256-line strips, each compressed with JBIG.

[pkhr's notes on the MF4700 series](https://github.com/pkhr/mf4700-print-scan) were a
huge help here. Once my own encoder matched Canon's output byte for byte, I printed a
test page. The circles came out round and the ruler measured right.

The last step was getting all of that onto the chip. Phones send plain page images;
the ESP32 turns them into the printer's black-and-white dot pattern, compresses each
strip, and feeds it to the printer.

Now the printer shows up as **"Canon MF3010 Wi-Fi"** on my mom's phone and laptop on
its own. She taps Print and it comes out, with no drivers, no cables and no asking me.
Windows adds it without any Canon driver, and the USB port is still free when I need
to scan something on my PC.

## What it does

* **Driverless printing (IPP Everywhere / AirPrint / Mopria)** on port 631. Android,
  iPhone, Windows and Mac all find "Canon MF3010 Wi-Fi" on the network by themselves.
* **Converts the pages on the chip.** Phones send PWG raster or Apple raster (URF).
  The board scales them to the printer's 600 x 400 dpi page, halftones gray and
  photos, compresses each 256-line band with JBIG and wraps it all up as a UFR II LT
  job, while the data is still coming in. Only a few KB per band are kept in RAM.
* **Raw port 9100** still works for PCs that already have the Canon driver.
* Status page with a live log and a table of recent jobs, firmware updates over Wi-Fi.

## How it works

| file | what it does |
|------|--------------|
| `firmware/MF3010_bridge/MF3010_bridge.ino` | Wi-Fi, USB host, port 9100, mDNS, status page |
| `firmware/MF3010_bridge/ipp.cpp` | small IPP/HTTP server (one printer, ~10 jobs in a table) |
| `firmware/MF3010_bridge/raster.cpp` | PWG / URF decoder, scaling and halftoning |
| `firmware/MF3010_bridge/ufr_job.cpp` | writes the CPCA + UFR II LT job, band by band |
| `firmware/MF3010_bridge/jbig_stream.cpp` | JBIG encoder that only needs the last two lines |

The print format itself is written up in [docs/ufr2lt-format.md](docs/ufr2lt-format.md).

## Build your own

You need an ESP32-S3 board, a USB-C OTG adapter and the printer's USB cable.
Wiring, Arduino settings and flashing are in [docs/hardware.md](docs/hardware.md).

Short version:

1. Copy `firmware/MF3010_bridge/wifi_config.example.h` to `wifi_config.h` and put in
   your Wi-Fi name and password.
2. Arduino IDE, board "ESP32S3 Dev Module", USB CDC On Boot disabled, partition
   scheme "Minimal SPIFFS (1.9MB APP with OTA)". Flash it.
3. Power the board from its 5V/GND pins, plug the printer into its USB-C port with
   the OTG adapter.
4. **Android:** Settings > Connections > More connection settings > Printing >
   turn on Default print service, then print from any app and pick "Canon MF3010 Wi-Fi"
   (under "All printers").
   **iPhone:** Share > Print.
   **Windows:** Settings > Bluetooth & devices > Printers & scanners > Add device.

## Testing without the printer

Everything except the USB and Wi-Fi parts also builds on a normal PC:

```
sudo apt install libjbig-dev jbigkit-bin cups-ipp-utils python3-pil
make check
```

That compares the JBIG encoder with JBIG-KIT, rebuilds two jobs captured from the
Windows driver byte for byte, and runs a test page through the PWG and URF decoders.
`build/ippserver` runs the IPP part on your PC so you can throw `ipptool` at it. It
passes CUPS' `ipp-everywhere.test` except for the JPEG and `overrides-supported`
checks (it only takes raster).

`tools/ufrdump.py` takes apart captured jobs, `tools/pbm2ufr` makes jobs from
1-bit images and `tools/testpage.py` draws the test page.

## Known issues

* A4 only, one-sided, black and white (it's an MF3010).
* Margins on prints from phones don't always match what the PC driver gives.
  Still looking into it.
* Scanning still needs the USB cable to the PC.
* Only tested with my MF3010. Other UFR II LT printers might work if their page
  size and resolution match, but I can't promise anything.

## Credits

* [pkhr/mf4700-print-scan](https://github.com/pkhr/mf4700-print-scan) for the UFR II LT notes
* [JBIG-KIT](https://www.cl.cam.ac.uk/~mgk25/jbigkit/) by Markus Kuhn, the arithmetic coder tables come from it
* CUPS' `ipptool` and `ippeveprinter`, which I tested the IPP server against

Not made or endorsed by Canon.

## License

GPL-3.0, see [LICENSE](LICENSE).
