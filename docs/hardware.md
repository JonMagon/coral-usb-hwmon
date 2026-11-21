# Reading the die sensor over USB

The [Coral Accelerator Module datasheet, §4.5.3](https://coral.ai/static/files/Coral-Accelerator-Module-datasheet.pdf#page=14) describes temperature telemetry as unavailable over USB. The [Coral support discussion](https://github.com/google-coral/edgetpu/issues/102) describes the public interface. On the USB Accelerator tested here the stock firmware implements the register transfers required to enable and sample the sensor.

## Register sequence

The module sends device-recipient vendor control transfers on endpoint zero. It never claims the ML interface. It follows the [USB runtime's 32-bit CSR protocol](https://github.com/google-coral/libedgetpu/blob/e35aed18fea2e2d25d98352e5a5bd357c170bd4d/driver/usb/usb_ml_commands.cc#L78) and the [PCIe driver's thermal initialization](https://github.com/google/gasket-driver/blob/5815ee3908a46a415aac616ac7b9aedcb98a504c/src/apex_driver.c#L931).

| Register | Operation |
| --- | --- |
| OMC0_D0, `0x1a0d0` | Enable the sensor clock (bit 7). |
| OMC0_D8, `0x1a0d8` | Enable ENAD, ENVR and ENBG (bits 0–2). |
| OMC0_DC, `0x1a0dc` | After at least 100 µs, set ENTHMC (bit 0). Sample the ADC in bits 16–25. |

The [conversion](https://github.com/google/gasket-driver/blob/5815ee3908a46a415aac616ac7b9aedcb98a504c/src/apex_driver.c#L632) is `T_m°C = (662 − ADC) × 250 + 550`. The original linear fit is optimized for 25–100 °C. On the test unit the disabled sensor returned `0x03ff0000` (ADC 1023). Disabled or saturated values are unavailable readings.

Both sysfs attributes share a one-second cache. A fresh read enables the sensor if the controller is disabled. Unloading the module restores the enable bits it changed on accessible devices. The stock runtime may separately update thermal registers during device open/close. Read-modify-write operations across the two drivers have no shared transaction. USB resets or suspend can temporarily make a read fail.

## Load test

A standalone probe sampled the enabled sensor roughly once a second. The test opened the stock Frigate Edge TPU model with the USB delegate and repeatedly invoked it on the same input. The original host log has been converted to [CSV](thermal-response.csv) with timestamps relative to workload start.

| Phase | Median ADC | Estimated °C |
| --- | ---: | ---: |
| Idle baseline (first 35 samples) | 505 | 39.80 |
| Final 10 seconds of continuous inference | 484 | 45.05 |
| Final 10 seconds of cooldown | 502 | 40.55 |

The workload lasted 55.007 s at 121.4 inferences/s. The peak estimate was 45.55 °C. These measurements are from one physical accelerator.

The [standalone Linux probe](../tools/coral-usb-temp.c) also works without the kernel module. Pass the USB device path to read the temperature sensor. With `--enable-test`, the probe temporarily enables the sensor and restores its previous enable bits. Use the module or the diagnostic enable test one at a time.

## Read the configured TPU clock

Build the probe from the repository root. Find the accelerator's current bus and device numbers with `lsusb -d 18d1:9302` and substitute them in the device path.

```console
$ cc -O2 -Wall -o coral-usb-temp tools/coral-usb-temp.c
$ sudo ./coral-usb-temp --clock /dev/bus/usb/002/003
Configured TPU clock: 500 MHz
```

This read-only command works while Frigate holds the accelerator open. It reads the compute core clock divider from bits 29–28 of `SCU_CTRL_3` at `0x1a318`. The reported MHz describe the runtime configuration. Measuring instantaneous frequency or utilization requires a separate measurement. On the test unit Frigate selected 500 MHz. Switching the same model from the Medium profile to High and back produced 125, 250 and 125 MHz respectively. [Clock divider definitions in libedgetpu](https://github.com/google-coral/libedgetpu/blob/e35aed18fea2e2d25d98352e5a5bd357c170bd4d/driver/config/beagle_csr_helper.h#L362-L390)
