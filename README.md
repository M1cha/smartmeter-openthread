# smartmeter-lora
Sends data from a smart power meter to homeassistant via [LoRa](https://en.wikipedia.org/wiki/LoRa)

- implements listen-before-send to prevent interference with LoRaWAN or other devices.
- the current config uses `SF7BW250` and sends every 60s which is reasonably above
  the 1% duty-cycle from Germanys regulations
- encrypts data using [ChaCha20-Poly1305](https://en.wikipedia.org/wiki/ChaCha20-Poly1305)

# Requirements
- `rustup`: so you can install the nightly version required by this project
- `cbindgen`: The CLI, for generating C bindings to the SML library
- rust-src for the currently used toolchain. E.g. `rustup component add rust-src --toolchain nightly-2022-07-01-x86_64-unknown-linux-gnu`
- `poppler`: Provides `pdftotext` which is used to convert the SML specification to code
- [Zephyr RTOS](https://docs.zephyrproject.org/3.1.0/develop/getting_started/index.html) dependencies
- Currently, the build system builds the rust part for `thumbv6m-none-eabi`
  but it should be easy to extend if more platforms are needed

# supported boards
## `b_l072z_lrwan1`
Very useful for development but maybe a little expensive and physically large.

## `heltec_lora_node_151`
Seemed like a good idea at first but is a VERY BAD choice and only kept for
reference of how to use this board in zephyr.
- doesn't have a cryptographically secure RNG(Random Number Generator).
  My implementation uses an insecure way to turn the temperature sensor into entropy.
- it's support in zephyr is pretty bad(no USB, no power management, consumes 6mA in idle)
- power circuitry is excluded from the schematics
- [official documentation](https://heltec-automation-docs.readthedocs.io/en/latest/stm32/lora_node_151/index.html)

## encryption key
### generate
```bash
openssl rand -out key.bin 32
objcopy --change-addresses 0x8028000 -I binary -O ihex key.bin key.hex
```

### flash
```bash
west flash -r openocd -d build-send --skip-rebuild --hex-file key.hex
```

# Run lora2mqtt
```bash
lora2mqtt --port /dev/ttyACM0 --baud-rate 115200 --key /path/to/key.bin --mqtt-uri "tcp://127.0.0.1:1883"
```
