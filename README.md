# Bible Clock

The goal of this project is to build a clock that displays a bible verse for each minute of the day. The reference corresponds the current time (chapter:verse <--> hour:minute).

## Quickstart

- Connect the e-ink display to the ESP32
- Add you ssid and password in the file main.cpp and flash the firmware
- Build and upload the filesystem image (use the verse database in the data folder - generated using ESV)

## Generate bible verse database

### Download a bible in json format

1) https://www.biblesupersearch.com/bible-downloads/

2) https://github.com/jadenzaleski/BibleTranslations

### Build bible verse database

The will build a database of bible verses for each minute of the day. It first sample verse candidates, then asks GPT for the best one. If no suitable verse is found, an encouraging statement is generated instead.

1) export OPENAI_API_KEY=<api_key>

2) Run the parser script

The example below is for the ESV bible from the jadenzaleski repository.
```
python parser.py jadenzaleski esv.json
```

## Firmware

### Dependencies

- ArduinoJson

### Program ESP32

1) Make sure to update ssid and password in main.cpp
2) Compile and flash the firmware to the ESP32
3) Make sure a json file for each hour is located in the data folder
4) In the project tasks: 
    - esp32dev --> Platform --> Build Filesystem Image
    - esp32dev --> Platform --> Upload Filesystem Image


## Assembly

### 3D-printed clock chassis

- [Chassis](cad/chassis.step)
- [Lid](cad/lid.step)

### Wiring

- BUSY to PIN 25
- RST to PIN 26
- DC to PIN 27
- CS to PIN 15
- SCLK to PIN 13
- DIN to PIN 14
- GND to GND
- VCC to 3.3V

## Generate new fonts

https://github.com/zst-embedded/STM32-LCD_Font_Generator.git
