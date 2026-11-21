// SPDX-License-Identifier: GPL-2.0-only
/* Linux probe for an initialized Coral USB Accelerator.
 * Build: cc -std=c11 -Wall -Wextra -Werror -O2 -o coral-usb-temp coral-usb-temp.c
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <inttypes.h>
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
    /* The temperature and clock registers are in CSR bank 1. */
    if (control_transfer(fd, 0xc0, 1, offset, 1, data, sizeof data))
        return -1;
    *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
             ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return 0;
}

static int read_register64(int fd, uint32_t address, uint64_t *value)
{
    uint8_t data[8];
    if (control_transfer(fd, 0xc0, 0, address & 0xffff, address >> 16, data, sizeof data))
        return -1;
    *value = 0;
    for (size_t i = 0; i < sizeof data; ++i)
        *value |= (uint64_t)data[i] << (8 * i);
    return 0;
}

static unsigned configured_mhz(uint32_t raw)
{
    /* SCU_CTRL_3[29:28] selects the GCB (compute core) clock divider. */
    static const unsigned clock_mhz[] = {500, 250, 125, 63};
    return clock_mhz[(raw >> 28) & 3];
}

static int read_configured_clock(int fd)
{
    uint32_t raw;
    if (read_register(fd, 0xa318, &raw))
        return 1;
    printf("Configured TPU clock: %u MHz\n", configured_mhz(raw));
    return 0;
}

static int decode_temperature(uint32_t raw, double *celsius)
{
    unsigned adc = (raw >> 16) & 0x3ff;
    if (!(raw & 1) || adc == 0 || adc == 0x3ff)
        return -1;
    *celsius = ((662 - (int)adc) * 250 + 550) / 1000.0;
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
    double celsius;
    if (decode_temperature(raw, &celsius)) {
        fprintf(stderr, "No valid temperature sample.\n");
        return 2;
    }
    printf("Estimated junction temperature: %.2f C%s\n", celsius,
           celsius < 25 || celsius > 100 ? " (outside PCIe fit range)" : "");
    return 0;
}

static int print_hib_register(int fd, uint32_t address, const char *name)
{
    uint64_t value;
    if (read_register64(fd, address, &value)) {
        printf(" %s=unavailable", name);
        return 1;
    }
    printf(" %s=0x%016" PRIx64, name, value);
    return 0;
}

/* The caller has already printed the UTC timestamp and checked the USB ID. */
static int print_status(int fd, int include_hib)
{
    uint32_t raw;
    double celsius;
    int temp_error = read_register(fd, 0xa0dc, &raw);
    if (temp_error || decode_temperature(raw, &celsius))
        printf(" temp_c=unavailable");
    else
        printf(" temp_c=%.2f", celsius);

    int clock_error = read_register(fd, 0xa318, &raw);
    if (clock_error)
        printf(" clock_mhz=unavailable");
    else
        printf(" clock_mhz=%u", configured_mhz(raw));

    /* Bit 0 in the USB fatal error interrupt status register is W0C. */
    uint64_t irq;
    int irq_error = read_register64(fd, 0x4c068, &irq);
    if (irq_error)
        printf(" usb_fatal_irq_pending=unavailable");
    else
        printf(" usb_fatal_irq_pending=%u", (unsigned)(irq & 1));

    int failed = temp_error || clock_error || irq_error;
    if (include_hib) {
        failed |= print_hib_register(fd, 0x486f0, "hib_error_status");
        failed |= print_hib_register(fd, 0x48700, "hib_first_error_status");
    }
    putchar('\n');
    return failed;
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
    int status_mode = 0;
    int include_hib = 0;
    int (*measure)(int) = sample_temperature;
    const char *usage = "Usage: %s [--clock | --enable-test | --status [--hib-errors]] /dev/bus/usb/BUS/DEVICE\n";
    if (argc < 2 || argv[argc - 1][0] == '-') {
        fprintf(stderr, usage, argv[0]);
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--clock") == 0) {
        measure = read_configured_clock;
    } else if (argc == 3 && strcmp(argv[1], "--enable-test") == 0) {
        measure = test_sensor;
    } else if (strcmp(argv[1], "--status") == 0 &&
               (argc == 3 || (argc == 4 && strcmp(argv[2], "--hib-errors") == 0))) {
        status_mode = 1;
        include_hib = argc == 4;
    } else if (argc != 2) {
        fprintf(stderr, usage, argv[0]);
        return 1;
    }
    if (status_mode) {
        time_t now = time(NULL);
        struct tm utc;
        char stamp[32];
        if (now == (time_t)-1 || !gmtime_r(&now, &utc) ||
            !strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc)) {
            fprintf(stderr, "Could not read UTC time\n");
            return 1;
        }
        printf("%s", stamp);
    }
    /* usbfs requires a writable fd for USBDEVFS_CONTROL, even for IN requests. */
    int fd = open(argv[argc - 1], O_RDWR);
    if (fd < 0) {
        perror(argv[argc - 1]);
        if (status_mode)
            puts(" device=unavailable");
        return 1;
    }
    int status = check_device(fd);
    if (status_mode && status)
        puts(" device=unavailable");
    else if (status_mode)
        status = print_status(fd, include_hib);
    else if (status == 0)
        status = measure(fd);
    close(fd);
    return status;
}
