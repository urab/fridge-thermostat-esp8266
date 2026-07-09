# fridge-thermostat-esp8266

Smart fridge thermostat based on **Wemos D1 mini Pro**, **DS18B20**, **SSR**, **OLED** and **NeoPixel**.

## Overview

This project is a compact refrigerator / display cooler controller built on ESP8266.  
It measures temperature with a **DS18B20** sensor, controls the compressor through an **SSR**, shows the current status on a small **OLED display**, and uses an **8-pixel NeoPixel LED strip** for visual temperature indication.

The controller also creates its own Wi-Fi access point with a built-in web page, so temperature limits can be adjusted from a phone or laptop without extra software.

## Features

- Temperature control with **DS18B20**
- Compressor control with **SSR**
- Adjustable start delay to protect the compressor
- Configurable **ON** / **OFF** temperature thresholds
- Local Wi-Fi access point and web interface
- OLED display with large temperature readout
- NeoPixel LED strip color indication
- Sensor error fallback mode
- Settings saved in flash memory
- Prepared for future Home Assistant / MQTT integration

## Hardware

- Wemos D1 mini Pro (ESP8266)
- DS18B20 temperature sensor
- SSR (solid state relay)
- 0.96\" OLED I2C display
- WS2812 / NeoPixel LED strip (8 LEDs used in this build)
- 4.7k resistor for DS18B20 pull-up
- 5V power supply for controller / LEDs
- Small refrigerator / display cooler

## Pinout

- **D1** -> SSR control
- **D2** -> DS18B20 data
- **D5** -> NeoPixel data
- **D6** -> OLED SDA
- **D7** -> OLED SCL

## Wiring Notes

### DS18B20
- VCC -> 3.3V
- GND -> GND
- DATA -> D2
- **4.7k resistor between DATA and 3.3V**

### SSR
- Control input -> D1
- GND -> GND

### OLED
- SDA -> D6
- SCL -> D7
- VCC -> 3.3V
- GND -> GND

### NeoPixel strip
- DIN -> D5
- 5V -> 5V
- GND -> GND

## Software

The firmware is written for **Arduino IDE / Arduino CLI** using ESP8266 core.

Main libraries used:
- OneWire
- DallasTemperature
- Adafruit SSD1306
- Adafruit GFX
- Adafruit NeoPixel

## Web Interface

The controller creates its own Wi-Fi access point:

- **SSID:** `Fridge-Control`
- **Default IP:** `192.168.4.1`

From the web page you can:
- view current temperature
- see compressor status
- enable / disable cooling
- change ON and OFF temperature thresholds
- force compressor OFF

## LED Strip Logic

- **Blue** = colder than OFF threshold
- **Green** = normal range
- **Red** = warmer than ON threshold
- **Blinking red** = sensor error
- **Dim white** = cooling disabled

## OLED Display

The OLED shows:
- current temperature in large digits
- compressor status
- delay timer
- basic system state

## Safety

**Warning:** this project switches mains-powered refrigeration equipment.  
Use proper insulation, safe wiring, correct SSR selection, and electrical protection.  
If you are not qualified to work with mains voltage, ask a licensed electrician to check the final wiring.

## Photos

### Refrigerator
![Fridge Front](fridge-front.jpg)

### Wiring / Electronics
![Fridge Wiring](fridge-wiring.jpg)

### Display
![Fridge Display](fridge-display.jpg)

## File List

Suggested files for this repository:

- `fridge-thermostat-esp8266.ino`
- `fridge-front.jpg`
- `fridge-wiring.jpg`
- `fridge-display.jpg`
- `README.md`

## Status

This project is tested on a real mini-fridge and is working well for everyday use.

## Author

Created by **urab**
