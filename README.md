# IERG4230 IoT Cold-Chain Project

## Hardware

- ESP32 Project Board
- AHT10 sensor at I2C address `0x38`
- LIS3DH-compatible acceleration sensor at I2C address `0x19`
- Built-in SSD1306 OLED at I2C address `0x3C`

Connect the AHT10 and acceleration sensor to the ESP32 Project Board I2C sockets. The firmware uses SDA GPIO 21 and SCL GPIO 22.

## Arduino IDE setup

Install the ESP32 board package using this Board Manager URL:

`https://espressif.github.io/arduino-esp32/package_esp32_index.json`

Install these libraries through **Sketch > Include Library > Manage Libraries**:

- ArduinoJson by Benoit Blanchon, version 6.x
- Adafruit GFX Library by Adafruit
- Adafruit SSD1306 by Adafruit

Open `esp32_firmware/esp32_cold_chain.ino` and edit:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_BASE = "http://YOUR_LAPTOP_IPV4:5000";
```

The ESP32 and laptop must connect to the same 2.4 GHz Wi-Fi network. Select the correct ESP32 board and COM port, upload the firmware, and open Serial Monitor at `115200` baud.

## Conda environment setup

Open Anaconda Prompt in the project directory:

```bash
conda env create -f environment.yml
conda activate iot_project
```

If the environment already exists:

```bash
conda env update -n iot_project -f environment.yml --prune
conda activate iot_project
```

Verify the installation:

```bash
python -c "import flask, flask_sqlalchemy, qrcode; print('Environment OK')"
```

## Find the laptop IPv4 address

Run:

```bash
ipconfig
```

Use the active Wi-Fi adapter IPv4 address for `SERVER_BASE`. Do not use `127.0.0.1` in the ESP32 firmware.

## Start the server

```bash
conda activate iot_project
cd server
python app.py
```

Open `http://127.0.0.1:5000` on the laptop, or `http://YOUR_LAPTOP_IPV4:5000` from another device on the same network. If Windows Firewall asks, allow Python on private networks.

## First test

1. Start the Flask server.
2. Create a shipment with device ID `TRUCK_01`.
3. Open the dashboard and select **Start journey**.
4. Wait about five seconds for configuration synchronization.
5. Wait about ten seconds for the first sensor summary.
6. Tap the model to create an impact event.
7. Select **End journey**.
8. Open or scan the generated QR code.

## Reset the database

Stop the server, delete `server/instance/iot_project.db`, and restart `python app.py`.
