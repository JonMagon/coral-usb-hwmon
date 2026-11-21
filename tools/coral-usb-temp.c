// SPDX-License-Identifier: GPL-2.0-only
/* Linux probe for an initialized Coral USB Accelerator.
 * Build: cc -std=c11 -Wall -Wextra -Werror -O2 -o coral-usb-temp coral-usb-temp.c
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int control_transfer(int fd, uint8_t type, uint8_t request, uint16_t value,
                        uint16_t index, void *data, uint16_t length)
{
    struct usbdevfs_ctrltransfer transfer = {
        .bRequestType = type, .bRequest = request, .wValue = value,
        .wIndex = index, .wLength = length, .timeout = 1500, .data = data,
    };
    int received = ioctl(fd, USBDEVFS_CONTROL, &transfer);
    if (received < 0) {
        perror("USB control transfer");
        return -1;
    }
    if (received != length) {
        fprintf(stderr, "Short USB reply (%d of %u bytes)\n", received, length);
        return -1;
    }
    return 0;
}

static int read_register(int fd, uint16_t offset, uint32_t *value)
{
    uint8_t data[4];
    /* The upper 16 bits of every register address used here are 0x0001. */
    if (control_transfer(fd, 0xc0, 1, offset, 1, data, sizeof data))
        return -1;
    *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
             ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return 0;
}

static int read_configured_clock(int fd)
{
    /* SCU_CTRL_3[29:28] selects the GCB (compute core) clock divider. */
    static const unsigned clock_mhz[] = {500, 250, 125, 63};
    uint32_t raw;
    if (read_register(fd, 0xa318, &raw))
        return 1;
    printf("Configured TPU clock: %u MHz\n", clock_mhz[(raw >> 28) & 3]);
    return 0;
}

static int sample_temperature(int fd)
{
    uint32_t raw;
    if (read_register(fd, 0xa0dc, &raw))
        return 1;
    int adc = (raw >> 16) & 0x3ff;
    int enabled = raw & 1;
    printf("OMC0_DC=0x%08x ENTHMC=%d ADC=%d\n", (unsigned)raw, enabled, adc);
    if (!enabled || adc == 0 || adc == 0x3ff) {
        fprintf(stderr, "No valid temperature sample.\n");
        return 2;
    }
    double celsius = ((662 - adc) * 250 + 550) / 1000.0;
    printf("Estimated junction temperature: %.2f C%s\n", celsius,
           celsius < 25 || celsius > 100 ? " (outside PCIe fit range)" : "");
    return 0;
}

static int update_register(int fd, uint16_t offset, uint32_t mask, uint32_t bits)
{
    uint32_t raw;
    if (read_register(fd, offset, &raw))
        return -1;
    if ((raw & mask) == bits)
        return 0;
    raw = (raw & ~mask) | bits;
    if (offset == 0xa0dc)
        raw &= ~UINT32_C(0x30000000); /* Do not acknowledge thermal alarms. */
    uint8_t data[4] = {raw, raw >> 8, raw >> 16, raw >> 24};
    return control_transfer(fd, 0x40, 1, offset, 1, data, sizeof data);
}

static int test_sensor(int fd)
{
    const uint16_t offsets[] = {0xa0d0, 0xa0d8, 0xa0dc};
    const uint32_t masks[] = {0x80, 0x07, 0x01};
    uint32_t original[3];
    for (int i = 0; i < 3; ++i) {
        if (read_register(fd, offsets[i], &original[i]))
            return 1;
        printf("Before OMC0_%02x=0x%08x\n", offsets[i] & 0xff, (unsigned)original[i]);
    }
    int attempted = 0;
    int status = 1;
    for (int i = 0; i < 3; ++i) {
        if (i == 2 && nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL))
            goto restore;
        attempted = i + 1;
        if (update_register(fd, offsets[i], masks[i], masks[i]))
            goto restore;
    }
    status = 0;
    for (int i = 0; i < 4; ++i) {
        nanosleep(&(struct timespec){.tv_nsec = 250000000}, NULL);
        int sample_status = sample_temperature(fd);
        if (sample_status)
            status = sample_status;
    }

restore:
    while (attempted > 0) {
        int i = --attempted;
        uint32_t raw;
        if (update_register(fd, offsets[i], masks[i], original[i] & masks[i]) ||
            read_register(fd, offsets[i], &raw) ||
            (raw & masks[i]) != (original[i] & masks[i])) {
            fprintf(stderr, "Could not verify restored bits at 0x1%04x\n", offsets[i]);
            status = 1;
        } else {
            printf("Restored OMC0_%02x=0x%08x\n", offsets[i] & 0xff, (unsigned)raw);
        }
    }
    return status;
}

static int check_device(int fd)
{
    uint8_t descriptor[18];
    if (control_transfer(fd, 0x80, 6, 0x0100, 0, descriptor, sizeof descriptor))
        return 1;

    unsigned vendor = descriptor[8] | (descriptor[9] << 8);
    unsigned product = descriptor[10] | (descriptor[11] << 8);
    if (vendor != 0x18d1 || product != 0x9302) {
        fprintf(stderr, "Expected Coral in runtime mode (18d1:9302), found %04x:%04x\n",
                vendor, product);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int (*measure)(int) = sample_temperature;
    if (argc == 3 && strcmp(argv[1], "--clock") == 0) {
        measure = read_configured_clock;
    } else if (argc == 3 && strcmp(argv[1], "--enable-test") == 0) {
        measure = test_sensor;
    } else if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "Usage: %s [--clock | --enable-test] /dev/bus/usb/BUS/DEVICE\n", argv[0]);
        return 1;
    }
    /* usbfs requires a writable fd for USBDEVFS_CONTROL, even for IN requests. */
    int fd = open(argv[argc - 1], O_RDWR);
    if (fd < 0) {
        perror(argv[argc - 1]);
        return 1;
    }
    int status = check_device(fd);
    if (status == 0)
        status = measure(fd);
    close(fd);
    return status;
}
