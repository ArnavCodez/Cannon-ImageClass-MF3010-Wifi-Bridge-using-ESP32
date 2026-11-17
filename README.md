# Canon imageCLASS MF3010 Wi-Fi bridge (ESP32-S3)

Making my USB-only Canon MF3010 laser printer a Wi-Fi printer with a cheap ESP32-S3.

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

## What works right now

```
PC (Canon driver) --Wi-Fi--> port 9100 --> ESP32-S3 --USB--> MF3010
```

* The board is a USB host for the printer and accepts raw print jobs on port 9100,
  so any Windows PC with the Canon driver can print to it over Wi-Fi.
* Status page with a live log at `http://<board ip>` / `http://mf3010.local`.
* Firmware updates over Wi-Fi at `http://<board ip>/update`.

Setup, wiring and the Windows port settings are in [docs/hardware.md](docs/hardware.md).

## What's next

This still needs the Canon driver on every computer, and phones can't print at
all. The MF3010 is a "host-based" printer: it doesn't understand PDFs or images,
only Canon's own UFR II LT language, which only Canon's PC driver produces. So the
plan is to figure out what that driver sends and do the same thing on the ESP32.

## License

GPL-3.0, see [LICENSE](LICENSE).
