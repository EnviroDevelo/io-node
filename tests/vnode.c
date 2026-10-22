/* vnode.c - a virtual I/O board on a pseudo-terminal, for trying tools/at02.py without hardware.
 * The real node firmware core (node/node.c) with simulated chips (tests/sim_hw.c) answers Modbus
 * RTU frames on a pty, which the script opens like a serial port. A frame ends after 2 ms of
 * silence, as the USART's receiver timeout does on the board.
 *
 *   build/vnode [-a address] [-u] [-m mutation]
 *     -a  DIP address 1..15 (default 1)
 *     -u  behave as the USB port (answers at 247 too; RX_ERRORS not counted)
 *     -m  deliberately break one rule, to show the script catches it:
 *         1 answer another address too, 2 do not count bad CRCs, 3 answer unknown functions
 *         with exception 02, 4 let a rejected multi-register write change its first register
 * Prints the pty's path, then serves until killed. Host only (Linux / macOS). */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "mb_rtu.h"
#include "node.h"
#include "sim_hw.h"

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

int main(int argc, char **argv)
{
    int addr = 1, usb = 0, mut = 0, opt;
    while ((opt = getopt(argc, argv, "a:um:")) != -1) {
        if (opt == 'a') addr = atoi(optarg);
        else if (opt == 'u') usb = 1;
        else if (opt == 'm') mut = atoi(optarg);
        else { fprintf(stderr, "usage: vnode [-a address] [-u] [-m mutation]\n"); return 2; }
    }
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); return 1; }
    const char *path = ptsname(m);
    int keep = open(path, O_RDWR | O_NOCTTY);          /* held open: the pty survives the script closing it */
    struct termios t;
    tcgetattr(keep, &t);
    cfmakeraw(&t);
    tcsetattr(keep, TCSANOW, &t);
    printf("%s\n", path);
    fflush(stdout);

    static sim_hw_t hw;
    static node_t n;
    sim_hw_init(&hw);
    hw.field_a = hw.field_b = true;
    node_boot_t b = { (uint8_t)addr, 1, NODE_RST_POWER_ON, 0, { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 } };
    node_init(&n, &hw.hw, &b, now_ms());
    node_port_t port = usb ? NODE_PORT_USB : NODE_PORT_RS485;

    uint8_t rx[NODE_FRAME_MAX + 16], tx[NODE_FRAME_MAX];
    size_t len = 0;
    for (;;) {
        struct pollfd p = { m, POLLIN, 0 };
        int r = poll(&p, 1, len ? 2 : 1);
        node_poll(&n, now_ms());
        if (r > 0 && (p.revents & POLLIN)) {
            ssize_t k = read(m, rx + len, sizeof rx - len);
            if (k > 0) { len += (size_t)k; if (len >= sizeof rx) len = sizeof rx - 1; }
            continue;
        }
        if (!len) continue;                                /* idle */
        /* 2 ms of silence: the frame is complete */
        uint8_t f[sizeof rx];
        memcpy(f, rx, len);
        uint16_t fl = (uint16_t)len;
        len = 0;
        uint8_t orig = f[0];
        bool crc_ok = fl >= 4 && mbr_crc16(f, fl - 2u) == (uint16_t)(f[fl - 2] | f[fl - 1] << 8);
        if (mut == 1 && crc_ok && orig == (uint8_t)(addr + 1)) {   /* answer the neighbour's address as well */
            f[0] = (uint8_t)addr;
            uint16_t c = mbr_crc16(f, fl - 2u); f[fl - 2] = (uint8_t)c; f[fl - 1] = (uint8_t)(c >> 8);
        }
        uint16_t errs = n.rx_errors;
        uint16_t out = node_frame(&n, port, now_ms(), f, fl, false, tx);
        if (mut == 2 && n.rx_errors != errs) n.rx_errors = errs;
        if (mut == 3 && out == 5 && (tx[1] & 0x80u) && tx[2] == 1) {
            tx[2] = 2;
            uint16_t c = mbr_crc16(tx, 3); tx[3] = (uint8_t)c; tx[4] = (uint8_t)(c >> 8);
        }
        if (mut == 4 && out == 5 && (tx[1] & 0x7Fu) == 16 && f[2] == 0 && f[3] == IOB_R_HOLD_DS)
            n.hold_ds = (uint16_t)(f[7] << 8 | f[8]);      /* partial write: the rule says nothing may change */
        if (mut == 1 && orig != f[0] && out) {
            tx[0] = orig;
            uint16_t c = mbr_crc16(tx, out - 2u); tx[out - 2] = (uint8_t)c; tx[out - 1] = (uint8_t)(c >> 8);
        }
        if (out) {
            ssize_t w = write(m, tx, out);
            (void)w;
        }
    }
}
