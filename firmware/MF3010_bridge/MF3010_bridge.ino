/*
  MF3010 Wi-Fi bridge (ESP32-S3)

  The Canon MF3010 only has USB. This sketch turns an ESP32-S3 into a USB host
  for it and accepts print jobs over Wi-Fi on port 9100 (raw / JetDirect), so
  any PC with the normal Canon driver can print to it through a TCP/IP port.

    PC (Canon driver) -> Wi-Fi -> port 9100 -> ESP32-S3 -> USB -> MF3010

  Status page + log:  http://<board ip>  or  http://mf3010.local
  OTA update:         http://<board ip>/update
  Wi-Fi settings:     copy wifi_config.example.h to wifi_config.h

  LED  red blinking   - joining Wi-Fi
       blue           - Wi-Fi ok, no printer on USB
       green          - printer ready
       green blinking - printing

  Arduino IDE settings (esp32 core 3.x): board "ESP32S3 Dev Module",
  USB CDC On Boot: Disabled, USB Mode: Hardware CDC and JTAG, PSRAM: Disabled,
  Flash Size: 4MB, Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA).
  (CDC On Boot Enabled prints the IP on serial for 20 s before USB host starts.)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>
#include <atomic>
#include <stdarg.h>
#include "esp_log.h"
#include "usb/usb_host.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#error "Copy wifi_config.example.h to wifi_config.h and put your Wi-Fi name and password in it"
#endif

#define FW_VERSION "1.0"

const char *HOST_NAME = "mf3010";

#define RAW_PORT 9100
#define LED_PIN 48
#define LED_LEVEL 20
// bytes per USB transfer. 64 (one packet) also works, just slower
#define USB_CHUNK 4096
#define USB_TIMEOUT_MS 600000UL
#define IDLE_TIMEOUT_MS 120000UL
#define LOG_SIZE 8000

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#define SHOW_IP_ON_SERIAL 1
#else
#define SHOW_IP_ON_SERIAL 0
#endif

// ---------------------------------------------------------------- log

static char logBuf[LOG_SIZE];
static size_t logPos = 0;
static bool logFull = false;
static SemaphoreHandle_t logLock;
static volatile bool serialOn = true;

static void appendLog(const char *text, TickType_t wait) {
  if (xSemaphoreTake(logLock, wait) != pdTRUE) return;
  for (const char *c = text; *c; c++) {
    logBuf[logPos++] = *c;
    if (logPos >= LOG_SIZE) {
      logPos = 0;
      logFull = true;
    }
  }
  xSemaphoreGive(logLock);
}

void logMsg(const char *fmt, ...) {
  char line[224];
  int n = snprintf(line, sizeof(line), "[%6lus] ", (unsigned long)(millis() / 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = 0;
  if (serialOn) Serial.print(line);
  appendLog(line, portMAX_DELAY);
}

// send ESP-IDF's own log lines (USB stack, Wi-Fi) to the web log as well
static int idfLogHook(const char *fmt, va_list ap) {
  char raw[200];
  int n = vsnprintf(raw, sizeof(raw), fmt, ap);
  char line[232];
  size_t j = snprintf(line, sizeof(line), "[%6lus] sys: ", (unsigned long)(millis() / 1000));
  bool esc = false;
  bool any = false;
  for (size_t i = 0; raw[i] && j < sizeof(line) - 2; i++) {
    char c = raw[i];
    if (esc) {
      if (c == 'm') esc = false;
      continue;
    }
    if (c == 0x1b) {
      esc = true;
      continue;
    }
    if (c == '\r' || c == '\n') continue;
    line[j++] = c;
    any = true;
  }
  if (!any) return n;
  line[j++] = '\n';
  line[j] = 0;
  appendLog(line, pdMS_TO_TICKS(20));
  return n;
}

String logText() {
  String s;
  s.reserve(LOG_SIZE + 16);
  xSemaphoreTake(logLock, portMAX_DELAY);
  if (logFull) {
    for (size_t i = logPos; i < LOG_SIZE; i++) s += logBuf[i];
  }
  for (size_t i = 0; i < logPos; i++) s += logBuf[i];
  xSemaphoreGive(logLock);
  return s;
}

// ---------------------------------------------------------------- LED

void led(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 0)
  rgbLedWrite(LED_PIN, r, g, b);
#else
  neopixelWrite(LED_PIN, r, g, b);
#endif
}

// ---------------------------------------------------------------- USB

static usb_host_client_handle_t usbClient = NULL;
static usb_device_handle_t usbDev = NULL;
static usb_transfer_t *outXfer = NULL;
static usb_transfer_t *ctrlXfer = NULL;
static SemaphoreHandle_t outDone;

static std::atomic<bool> usbStarted{ false };
static std::atomic<bool> printerReady{ false };
static std::atomic<bool> usbBusy{ false };
static volatile uint8_t pendingAddr = 0;
static volatile bool pendingGone = false;
static volatile bool ctrlDone = true;
static uint8_t skipAddr = 0;
static int numDevices = -1;
static int readyDevices = 0;
static int stuckSecs = 0;
static int resetTries = 0;
static volatile bool resetRequest = false;

static uint8_t prnIface = 0;
static uint8_t prnAlt = 0;
static uint8_t epOut = 0;
static uint16_t usbVid = 0;
static uint16_t usbPid = 0;
static char printerId[200] = "";

static volatile uint16_t seenVid = 0;
static volatile uint16_t seenPid = 0;
static volatile uint32_t seenCount = 0;

// Needed since arduino-esp32 3.3.11. Without this callback the host stack
// quietly stops enumerating every device and nothing ever shows up.
static bool enumFilter(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  seenVid = dev_desc->idVendor;
  seenPid = dev_desc->idProduct;
  seenCount = seenCount + 1;
  *bConfigurationValue = 1;
  return true;
}

static void usbLibTask(void *arg) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void clientEventCb(const usb_host_client_event_msg_t *msg, void *arg) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    pendingAddr = msg->new_dev.address;
    logMsg("USB: device finished setting up (address %u)", msg->new_dev.address);
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (msg->dev_gone.dev_hdl == usbDev) pendingGone = true;
  }
}

static void outCb(usb_transfer_t *t) {
  xSemaphoreGive(outDone);
}

static void ctrlCb(usb_transfer_t *t) {
  ctrlDone = true;
}

// GET_DEVICE_ID (printer class request), only for the log / status page
static void readDeviceId() {
  uint8_t *s = ctrlXfer->data_buffer;
  s[0] = 0xA1;
  s[1] = 0x00;
  s[2] = 0x00;
  s[3] = 0x00;
  s[4] = prnAlt;
  s[5] = prnIface;
  s[6] = 0x00;
  s[7] = 0x01;
  ctrlXfer->num_bytes = 8 + 256;
  ctrlXfer->device_handle = usbDev;
  ctrlXfer->bEndpointAddress = 0;
  ctrlXfer->callback = ctrlCb;
  ctrlXfer->context = NULL;
  ctrlDone = false;
  esp_err_t e = usb_host_transfer_submit_control(usbClient, ctrlXfer);
  if (e != ESP_OK) {
    ctrlDone = true;
    logMsg("USB: could not ask the printer for its ID (%s)", esp_err_to_name(e));
    return;
  }
  uint32_t t0 = millis();
  while (!ctrlDone && millis() - t0 < 3000) {
    usb_host_client_handle_events(usbClient, pdMS_TO_TICKS(20));
  }
  if (!ctrlDone) {
    logMsg("USB: printer did not answer the ID request");
    return;
  }
  if (ctrlXfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    logMsg("USB: ID request failed (status %d)", (int)ctrlXfer->status);
    return;
  }
  int got = ctrlXfer->actual_num_bytes - 8;
  if (got > 2) {
    int n = got - 2;
    if (n > (int)sizeof(printerId) - 1) n = sizeof(printerId) - 1;
    memcpy(printerId, ctrlXfer->data_buffer + 10, n);
    printerId[n] = 0;
    logMsg("USB: printer says: %s", printerId);
  }
}

static void openDevice(uint8_t addr) {
  skipAddr = addr;
  usb_device_handle_t dev = NULL;
  esp_err_t e = usb_host_device_open(usbClient, addr, &dev);
  if (e != ESP_OK) {
    logMsg("USB: could not open device %u (%s)", addr, esp_err_to_name(e));
    return;
  }
  const usb_device_desc_t *dd = NULL;
  const usb_config_desc_t *cfg = NULL;
  usb_host_get_device_descriptor(dev, &dd);
  usb_host_get_active_config_descriptor(dev, &cfg);
  if (dd == NULL || cfg == NULL) {
    logMsg("USB: could not read the device descriptors");
    usb_host_device_close(usbClient, dev);
    return;
  }
  usbVid = dd->idVendor;
  usbPid = dd->idProduct;
  logMsg("USB: device detected, VID %04X PID %04X", usbVid, usbPid);

  // first printer interface (class 07) with a bulk OUT endpoint
  const uint8_t *p = (const uint8_t *)cfg;
  int total = cfg->wTotalLength;
  bool inPrinter = false;
  bool found = false;
  uint8_t ifNum = 0, ifAlt = 0, out = 0;
  int i = 0;
  while (i + 2 <= total) {
    uint8_t len = p[i];
    uint8_t type = p[i + 1];
    if (len < 2 || i + len > total) break;
    if (type == 0x04 && len >= 9) {
      if (found) break;
      logMsg("USB:   interface %u alt %u class %02X sub %02X proto %02X",
             p[i + 2], p[i + 3], p[i + 5], p[i + 6], p[i + 7]);
      inPrinter = (p[i + 5] == 0x07);
      ifNum = p[i + 2];
      ifAlt = p[i + 3];
    } else if (type == 0x05 && len >= 7) {
      uint8_t ep = p[i + 2];
      uint8_t attr = p[i + 3];
      uint16_t mps = p[i + 4] | (p[i + 5] << 8);
      logMsg("USB:     endpoint %02X type %u max packet %u", ep, attr & 3, mps);
      if (inPrinter && !found && (attr & 3) == 2 && (ep & 0x80) == 0) {
        out = ep;
        found = true;
      }
    }
    i += len;
  }
  if (!found) {
    logMsg("USB: this device is not a printer, ignoring it (the USB port works though)");
    usb_host_device_close(usbClient, dev);
    return;
  }
  if (ifAlt != 0) {
    logMsg("USB: warning, printer interface uses alternate setting %u", ifAlt);
  }

  e = usb_host_interface_claim(usbClient, dev, ifNum, ifAlt);
  if (e != ESP_OK) {
    logMsg("USB: could not claim the printer interface (%s)", esp_err_to_name(e));
    usb_host_device_close(usbClient, dev);
    return;
  }
  if (usb_host_transfer_alloc(USB_CHUNK, 0, &outXfer) != ESP_OK
      || usb_host_transfer_alloc(8 + 256, 0, &ctrlXfer) != ESP_OK) {
    logMsg("USB: out of memory for transfers");
    if (outXfer) usb_host_transfer_free(outXfer);
    outXfer = NULL;
    ctrlXfer = NULL;
    usb_host_interface_release(usbClient, dev, ifNum);
    usb_host_device_close(usbClient, dev);
    return;
  }

  usbDev = dev;
  prnIface = ifNum;
  prnAlt = ifAlt;
  epOut = out;
  outXfer->device_handle = dev;
  outXfer->bEndpointAddress = out;
  outXfer->callback = outCb;
  outXfer->context = NULL;

  readDeviceId();
  skipAddr = 0;
  printerReady = true;
  logMsg("USB: printer ready (interface %u, data endpoint %02X)", prnIface, epOut);
}

static void closeDevice() {
  if (outXfer) usb_host_transfer_free(outXfer);
  outXfer = NULL;
  if (ctrlXfer && ctrlDone) usb_host_transfer_free(ctrlXfer);
  ctrlXfer = NULL;
  usb_host_interface_release(usbClient, usbDev, prnIface);
  usb_host_device_close(usbClient, usbDev);
  usbDev = NULL;
  printerId[0] = 0;
  pendingGone = false;
  logMsg("USB: printer disconnected");
}

// power cycle the port, same as replugging the cable
static void resetPort() {
  esp_err_t e = usb_host_lib_set_root_port_power(false);
  if (e != ESP_OK) {
    logMsg("USB: port reset failed (%s)", esp_err_to_name(e));
    return;
  }
  uint32_t t = millis();
  while (millis() - t < 1000) {
    usb_host_client_handle_events(usbClient, pdMS_TO_TICKS(50));
  }
  e = usb_host_lib_set_root_port_power(true);
  if (e != ESP_OK) logMsg("USB: port power-on failed (%s)", esp_err_to_name(e));
}

static void usbClientTask(void *arg) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = clientEventCb;
  cc.async.callback_arg = NULL;
  esp_err_t e = usb_host_client_register(&cc, &usbClient);
  if (e != ESP_OK) {
    logMsg("USB: client register failed (%s)", esp_err_to_name(e));
    vTaskDelete(NULL);
    return;
  }
  uint32_t lastCheck = 0;
  uint32_t startedAt = millis();
  bool warned = false;
  uint32_t loggedSeen = 0;
  for (;;) {
    usb_host_client_handle_events(usbClient, pdMS_TO_TICKS(50));
    if (seenCount != loggedSeen) {
      loggedSeen = seenCount;
      logMsg("USB: device %04X:%04X introduced itself, accepting it", seenVid, seenPid);
    }
    if (pendingGone) {
      printerReady = false;
      if (!usbBusy) closeDevice();
    }
    if (pendingAddr != 0 && usbDev == NULL) {
      uint8_t a = pendingAddr;
      pendingAddr = 0;
      openDevice(a);
    }
    if (millis() - lastCheck >= 1000) {
      lastCheck = millis();
      usb_host_lib_info_t info;
      if (usb_host_lib_info(&info) == ESP_OK && info.num_devices != numDevices) {
        numDevices = info.num_devices;
        logMsg("USB: devices connected: %d", numDevices);
        if (numDevices == 0) skipAddr = 0;
      }
      if (usbDev == NULL && numDevices > 0) {
        // a device only gets an address once enumeration is done
        uint8_t list[8];
        int num = 0;
        if (usb_host_device_addr_list_fill(8, list, &num) != ESP_OK) num = 0;
        if (num != readyDevices) {
          readyDevices = num;
          logMsg("USB: devices ready to open: %d", num);
        }
        // in case it finished enumerating before we got the NEW_DEV event
        if (pendingAddr == 0) {
          for (int k = 0; k < num; k++) {
            if (list[k] != skipAddr) {
              pendingAddr = list[k];
              break;
            }
          }
        }
        if (num == 0) {
          stuckSecs++;
          if (stuckSecs == 5) logMsg("USB: device is connected but has not finished the USB handshake yet");
          if (stuckSecs >= 15 && resetTries < 3) {
            resetTries++;
            logMsg("USB: handshake stuck, resetting the USB port (try %d of 3)", resetTries);
            resetPort();
            stuckSecs = 0;
          }
        } else {
          stuckSecs = 0;
        }
      } else {
        stuckSecs = 0;
        if (numDevices <= 0) readyDevices = 0;
      }
      if (resetRequest) {
        resetRequest = false;
        if (usbDev != NULL) {
          logMsg("USB: printer is in use, unplug it instead of resetting");
        } else {
          logMsg("USB: resetting the USB port (asked from the web page)");
          resetTries = 0;
          resetPort();
        }
      }
      if (!warned && numDevices <= 0 && resetTries == 0 && millis() - startedAt > 15000) {
        warned = true;
        logMsg("USB: still nothing after 15 s. Unplug and replug the OTG adapter, or flip it over.");
      }
    }
  }
}

// Blocks while the printer is busy, which also slows the sender down over
// TCP, so nothing gets dropped.
static bool usbWrite(const uint8_t *data, size_t len) {
  while (len > 0) {
    usbBusy = true;
    if (!printerReady) {
      usbBusy = false;
      return false;
    }
    size_t n = len > USB_CHUNK ? USB_CHUNK : len;
    memcpy(outXfer->data_buffer, data, n);
    outXfer->num_bytes = n;
    xSemaphoreTake(outDone, 0);
    esp_err_t e = usb_host_transfer_submit(outXfer);
    if (e != ESP_OK) {
      usbBusy = false;
      logMsg("USB: could not send data (%s)", esp_err_to_name(e));
      return false;
    }
    bool ok = true;
    if (xSemaphoreTake(outDone, pdMS_TO_TICKS(USB_TIMEOUT_MS)) != pdTRUE) {
      logMsg("USB: printer took no data for %lu s, giving up on this job", USB_TIMEOUT_MS / 1000);
      usb_host_endpoint_halt(usbDev, epOut);
      usb_host_endpoint_flush(usbDev, epOut);
      xSemaphoreTake(outDone, pdMS_TO_TICKS(5000));
      usb_host_endpoint_clear(usbDev, epOut);
      ok = false;
    } else if (outXfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      logMsg("USB: transfer failed (status %d)", (int)outXfer->status);
      if (outXfer->status == USB_TRANSFER_STATUS_STALL) {
        usb_host_endpoint_clear(usbDev, epOut);
      }
      ok = false;
    }
    usbBusy = false;
    if (!ok) return false;
    data += n;
    len -= n;
  }
  return true;
}

static void startUsbHost() {
  // the board has one USB port, so serial has to go before host mode
  serialOn = false;
  Serial.flush();
  Serial.end();
  delay(100);

  usb_host_config_t hc = {};
  hc.skip_phy_setup = false;
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
#ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
  hc.enum_filter_cb = enumFilter;
#endif
  esp_err_t e = usb_host_install(&hc);
  if (e != ESP_OK) {
    logMsg("USB: host failed to start (%s)", esp_err_to_name(e));
    return;
  }
  usbStarted = true;
  logMsg("USB: host started, waiting for a device");
  xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(usbClientTask, "usb_client", 8192, NULL, 9, NULL, 1);
}

// ---------------------------------------------------------------- port 9100

static WiFiServer rawServer(RAW_PORT);
static uint8_t netBuf[USB_CHUNK];
static std::atomic<bool> printing{ false };
static uint32_t jobCount = 0;
static uint32_t lastJobBytes = 0;

static void handleJob(WiFiClient &c) {
  if (!printerReady) {
    logMsg("Print job refused: printer is not detected on USB");
    c.stop();
    return;
  }
  String from = c.remoteIP().toString();
  printing = true;
  uint32_t total = 0;
  uint32_t last = millis();
  bool ok = true;
  while (c.connected() || c.available()) {
    int n = c.read(netBuf, sizeof(netBuf));
    if (n > 0) {
      if (total == 0) logMsg("Print job started from %s", from.c_str());
      if (!usbWrite(netBuf, n)) {
        ok = false;
        break;
      }
      total += n;
      last = millis();
    } else {
      if (millis() - last > IDLE_TIMEOUT_MS) {
        logMsg("No data from %s for a long time, closing", from.c_str());
        break;
      }
      delay(2);
    }
  }
  c.stop();
  printing = false;
  if (total == 0 && ok) return;
  jobCount++;
  lastJobBytes = total;
  logMsg("Print job %s: %lu bytes sent to the printer", ok ? "finished" : "FAILED", (unsigned long)total);
}

static void printTask(void *arg) {
  for (;;) {
    WiFiClient c = rawServer.accept();
    if (c) {
      handleJob(c);
    } else {
      delay(20);
    }
  }
}

// ---------------------------------------------------------------- web page

static WebServer web(80);
static HTTPUpdateServer updater;

static String esc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  return s;
}

static void pageRoot() {
  String h = F("<!doctype html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<meta http-equiv='refresh' content='3'><title>MF3010 bridge</title>"
               "<style>body{font-family:sans-serif;margin:16px}"
               "pre{background:#111;color:#ddd;padding:10px;overflow:auto;font-size:12px}</style>"
               "</head><body><h3>MF3010 Wi-Fi bridge " FW_VERSION "</h3><p>");
  h += "Running for: " + String((unsigned long)(millis() / 1000)) + " s<br>";
  h += "Wi-Fi: " + WiFi.localIP().toString() + " (signal " + String(WiFi.RSSI()) + " dBm)<br>";
  h += "USB host: ";
  h += usbStarted.load() ? "running" : "not started yet";
  h += ", devices connected: " + String(numDevices < 0 ? 0 : numDevices);
  h += ", ready: " + String(readyDevices) + "<br>";
  h += "Printer on USB: <b>";
  h += printerReady.load() ? "ready" : "not detected";
  h += "</b><br>";
  if (printerId[0]) h += "Printer ID: " + esc(String(printerId)) + "<br>";
  h += "State: ";
  h += printing.load() ? "printing" : "idle";
  h += "<br>Jobs so far: " + String(jobCount) + ", last job " + String(lastJobBytes) + " bytes</p>";
  h += "<pre>" + esc(logText()) + "</pre>";
  h += "<p><a href='/update'>Update firmware</a> | <a href='/usbreset'>Reset USB port</a> | "
       "<a href='/reboot'>Reboot</a></p></body></html>";
  web.send(200, "text/html", h);
}

static void pageLog() {
  web.send(200, "text/plain", logText());
}

static void pageUsbReset() {
  resetRequest = true;
  web.sendHeader("Location", "/");
  web.send(303, "text/plain", "");
}

static void pageReboot() {
  web.send(200, "text/html", "Rebooting. <a href='/'>Back</a>");
  delay(500);
  ESP.restart();
}

// ---------------------------------------------------------------- setup / loop

static const char *wifiWhy() {
  switch (WiFi.status()) {
    case WL_NO_SSID_AVAIL: return "network name not found - check spelling, it must be 2.4 GHz";
    case WL_CONNECT_FAILED: return "connection refused - check the password";
    case WL_CONNECTION_LOST: return "connection lost";
    default: return "waiting";
  }
}

void setup() {
  Serial.begin(115200);
  logLock = xSemaphoreCreateMutex();
  outDone = xSemaphoreCreateBinary();
  esp_log_set_vprintf(idfLogHook);
  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("ENUM", ESP_LOG_DEBUG);
  esp_log_level_set("HUB", ESP_LOG_DEBUG);
  esp_log_level_set("USBH", ESP_LOG_INFO);
  esp_log_level_set("USB HOST", ESP_LOG_INFO);
  led(LED_LEVEL, 0, 0);
#if SHOW_IP_ON_SERIAL
  delay(2000);
#endif
  logMsg("MF3010 bridge %s starting", FW_VERSION);
  esp_log_write(ESP_LOG_WARN, "bridge", "system messages are captured\n");

#if !SHOW_IP_ON_SERIAL
  startUsbHost();
#endif

  WiFi.setHostname(HOST_NAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logMsg("Wi-Fi: joining %s", WIFI_SSID);
  uint32_t t0 = millis();
  uint32_t lastSay = millis();
  bool on = false;
  while (WiFi.status() != WL_CONNECTED) {
    on = !on;
    led(on ? LED_LEVEL : 0, 0, 0);
    delay(300);
    if (millis() - lastSay > 2000) {
      lastSay = millis();
      logMsg("Wi-Fi: not connected yet (%s)", wifiWhy());
    }
    if (millis() - t0 > 20000) {
      logMsg("Wi-Fi: trying again");
      WiFi.disconnect();
      delay(200);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      t0 = millis();
    }
  }
  String ip = WiFi.localIP().toString();
  logMsg("Wi-Fi: connected, IP address %s", ip.c_str());
  led(0, 0, LED_LEVEL);

  if (MDNS.begin(HOST_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  updater.setup(&web);
  web.on("/", pageRoot);
  web.on("/log", pageLog);
  web.on("/usbreset", pageUsbReset);
  web.on("/reboot", pageReboot);
  web.begin();

  rawServer.begin();
  xTaskCreatePinnedToCore(printTask, "print", 8192, NULL, 5, NULL, 1);
  logMsg("Listening for print jobs on port %d", RAW_PORT);

#if SHOW_IP_ON_SERIAL
  // print the IP on serial for a bit, then hand the port over to the printer
  for (int k = 0; k < 20; k++) {
    Serial.print("IP: ");
    Serial.println(ip);
    uint32_t t = millis();
    while (millis() - t < 1000) {
      web.handleClient();
      delay(5);
    }
  }
  Serial.println("Switching the USB port to printer mode now. The Serial Monitor will go quiet.");
  delay(300);
  startUsbHost();
#endif
}

void loop() {
  web.handleClient();

  static uint32_t lastLed = 0;
  static bool blink = false;
  if (millis() - lastLed > 400) {
    lastLed = millis();
    blink = !blink;
    if (WiFi.status() != WL_CONNECTED) {
      led(blink ? LED_LEVEL : 0, 0, 0);
    } else if (printing) {
      led(0, blink ? LED_LEVEL : 0, 0);
    } else if (printerReady) {
      led(0, LED_LEVEL, 0);
    } else {
      led(0, 0, LED_LEVEL);
    }
  }
  delay(2);
}
