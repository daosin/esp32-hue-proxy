# ESP32 Hue Proxy

Emulated Hue proxy + SSDP for ESP32-C3 (SuperMini and similar).

Lets Amazon Echo / Alexa discover and control Home Assistant devices via the **Emulated Hue** integration when Echo and Home Assistant are on **different subnets / VLANs**.

## How it works

```
Amazon Echo  ── SSDP (UDP 1900) ──►  ESP32 (this device)
             ── HTTP port 80 ─────►  ESP32  ── proxy ──►  Home Assistant Emulated Hue
```

- ESP answers SSDP discovery as a Philips Hue Bridge
- All HTTP requests are transparently proxied to Home Assistant
- IP addresses in responses are rewritten so Echo thinks the bridge is the ESP

## Hardware

- ESP32-C3 SuperMini (or any ESP32 / ESP32-C3)
- Arduino core

## Requirements

- Home Assistant with [Emulated Hue](https://www.home-assistant.io/integrations/emulated_hue/) enabled (`listen_port: 80` recommended)
- Echo and ESP must be on the same L2 network (same subnet or multicast allowed)
- Home Assistant must be reachable from the ESP over HTTP

## Quick start

1. Clone the repository
2. Copy `secrets.h.example` → `secrets.h` and fill in your Wi-Fi and OTA password
3. Open `hue-proxy.ino` in Arduino IDE
4. Edit network settings in the sketch:
   - `ESP_IP`, `GATEWAY`, `SUBNET`
   - `HA_HOST` / `HA_IP_STR`
   - `ESP_IP_STR` (must have **exactly the same length** as `HA_IP_STR`)
5. Select board **ESP32C3 Dev Module** (or your board) and upload
6. In Alexa app: **Devices → Add Device → Philips Hue → Discover devices**

## Features

- Full HTTP proxy (GET / PUT / POST / DELETE)
- Automatic IP rewriting in responses
- Small cache for frequently requested endpoints
- SSDP M-SEARCH replies + periodic NOTIFY
- ArduinoOTA support
- Automatic restart on prolonged Wi-Fi or upstream failure

## Configuration notes

`HA_IP_STR` and `ESP_IP_STR` **must** have the same number of characters  
(e.g. both `10.10.10.112` and `172.20.1.180` are 12 characters).  
This is required by the in-place IP replacement.

## Home Assistant configuration

Emulated Hue is configured in `configuration.yaml`  
(**there is no full UI setup** for `host_ip` / `advertise_ip` / entity list).

Example:

```yaml
emulated_hue:
  host_ip: 10.10.10.112          # IP of Home Assistant
  advertise_ip: 172.20.1.180     # IP of this ESP proxy (must match ESP_IP)
  listen_port: 80
  expose_by_default: false       # only expose entities listed below
  entities:
    light.rgb_1:
      name: "Dracenna"
      hidden: false
    switch.tv_outlet:
      name: "TV"
      hidden: false
    switch.pc_outlet_pc:
      name: "Monitor"
      hidden: false
```

### Notes

- `host_ip` — address where Home Assistant actually runs Emulated Hue
- `advertise_ip` — address that appears in `description.xml` / discovery; set it to the **ESP proxy IP** so Alexa talks to the ESP
- `listen_port: 80` is required for modern Echo devices
- After changing the config, restart Home Assistant
- Then in the Alexa app: **Devices → Add Device → Philips Hue → Discover**
- Replace the example entity IDs and names with your own
- Only entities listed under `entities:` (with `hidden: false`) will be visible to Alexa when `expose_by_default: false`

## License

MIT
