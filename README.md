| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | ----- | ----- | ----- | ----- | ----- | ----- | ----- | ----- | ----- | ----- | ----- |

# ESP32-C3 RGB Invaders

This project is a small ESP-IDF arcade game built around a WS2812 LED strip, three color buttons, and an I2C character LCD. A colored invader moves toward the base, and you fire a matching colored laser with the corresponding button. Score increases on a correct hit, and the current score is shown on the LCD.

## Hardware

The current firmware uses these fixed pins and addresses:

* LED strip data: GPIO 3
* Red button: GPIO 0
* Green button: GPIO 1
* Blue button: GPIO 2
* LCD I2C SDA: GPIO 8
* LCD I2C SCL: GPIO 9
* LCD address: `0x27` or `0x3F`

The code expects a WS2812-style strip and an HD44780-compatible LCD behind a PCF8574 I2C backpack.

## How It Works

* The invader starts at the far end of the strip and moves toward the base.
* Pressing a color button fires a single laser of that color.
* If the laser color matches the invader color, the score increases and a new invader spawns.
* If the invader reaches the base, the game enters game-over mode.
* Press any of the three buttons after game over to restart.
* The LCD shows the title on the first line and the current score on the second line.
* NVS is initialized and used to store the best score when a new high score is reached.

## Build And Flash

Set the target for your chip before building:

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash monitor
```

Replace `esp32c3` with the target for your board if needed.

## Notes

* If the LCD is not detected at startup, the app probes `0x27` and `0x3F` and exits when neither responds.
* If the colors appear in the wrong order on the LED strip, adjust the strip color format in the source.
* The current `main/Kconfig.projbuild` still contains the stock blink-example menu entries, but the firmware behavior is the RGB Invaders game described above.

## Example Output

The serial log will show task startup, LCD detection, game hits, score updates, and game-over events.
