/* vnode_net.c - a virtual I/O board on Ethernet, on the PC: the real node core (node/node.c) and its
 * Ethernet side (node/node_net.c) behind real sockets, with simulated chips (tests/sim_hw.c). The
 * supervisor's netsim talks to it with CEMS_IOB_TCP=1 (tests/net_e2e.py does the whole round).
 *
 *   build/vnode_net [-t tcp_port] [-d discovery_port] [-a address] [-s supervisor_ip]
 *     -t  Modbus TCP port (default 1502: 502 needs root)       -d  discovery port (default 50223)
 *     -a  the address as if read from the F-RAM (default 0: a new board, no IP)
 *     -s  the supervisor's IP: a peer on 127.0.0.1 counts as that address (default 192.168.1.50),
 *         since the PC has none of the plant's addresses
 * On the PC the board does not take the IP it is given (it listens on every address); answers to
 * discovery go back to the sender rather than to the broadcast address. Commands on stdin:
 * "in <mask>" inputs on. Prints "ready", then a line for every change of its settings. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "node_net.h"
#include "sim_hw.h"

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
static uint32_t parse_ip(const char *s) { struct in_addr a; return inet_aton(s, &a) ? ntohl(a.s_addr) : 0; }
static int store(void *ctx, const node_ipcfg_t *c) { (void)ctx; (void)c; return NODE_HW_OK; }

int main(int argc, char **argv) {
    int tport = 1502, dport = IOB_DISC_PORT, addr = 0, opt;
    uint32_t sup = 0xC0A80132u;
    while ((opt = getopt(argc, argv, "t:d:a:s:")) != -1) {
        if (opt == 't') tport = atoi(optarg);
        else if (opt == 'd') dport = atoi(optarg);
        else if (opt == 'a') addr = atoi(optarg);
        else if (opt == 's') sup = parse_ip(optarg);
        else { fprintf(stderr, "usage: vnode_net [-t tcp_port] [-d discovery_port] [-a address] [-s supervisor_ip]\n"); return 2; }
    }
    static sim_hw_t hw;
    static node_t n;
    static node_net_t nn;
    sim_hw_init(&hw);
    hw.field_a = hw.field_b = true;
    node_boot_t b = { .address = (uint8_t)addr, .nv_ok = true, .hw_rev = 90, .reset_cause = NODE_RST_POWER_ON,
                      .uid = { 0x0EE1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 } };
    memcpy(b.serial, "VNODE-NET", 9);
    node_init(&n, &hw.hw, &b, now_ms());
    uint8_t mac[6];
    iob_mac_from_uid(b.uid, mac);
    node_net_init(&nn, &n, NULL, mac, store, NULL);

    int one = 1;
    int l = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0), c = -1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)tport);
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 2)) { perror("tcp"); return 1; }
    a.sin_port = htons((uint16_t)dport);
    if (bind(u, (struct sockaddr *)&a, sizeof a)) { perror("udp"); return 1; }
    printf("ready\n");
    fflush(stdout);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    char line[64];
    size_t ll = 0;
    for (;;) {
        struct pollfd p[4] = { { l, POLLIN, 0 }, { u, POLLIN, 0 }, { 0, POLLIN, 0 }, { c, POLLIN, 0 } };
        int r = poll(p, c >= 0 ? 4 : 3, 1);
        uint32_t now = now_ms();
        node_poll(&n, now);
        if (node_net_poll(&nn)) { printf("settings cleared\n"); fflush(stdout); }
        if (r <= 0) continue;
        if (p[0].revents & POLLIN) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            int k = accept(l, (struct sockaddr *)&from, &fl);
            uint32_t ip = ntohl(from.sin_addr.s_addr);
            if (k >= 0 && node_net_accept(&nn, (ip >> 24) == 127u ? sup : ip)) {
                if (c >= 0) close(c);
                c = k;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            } else if (k >= 0) close(k);
        }
        if (p[1].revents & POLLIN) {
            uint8_t d[256], rep[IOB_DISC_LEN];
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t k = recvfrom(u, d, sizeof d, 0, (struct sockaddr *)&from, &fl);
            uint32_t ip = ntohl(from.sin_addr.s_addr);
            bool changed = false;
            uint16_t rl = k > 0 ? node_net_disc(&nn, now, d, (uint16_t)k, (ip >> 24) == 127u ? sup : ip, rep, &changed) : 0;
            if (rl) sendto(u, rep, rl, 0, (struct sockaddr *)&from, fl);
            if (changed) {
                printf("settings ip %u.%u.%u.%u address %u\n", (unsigned)(nn.cfg.ip >> 24), (unsigned)(nn.cfg.ip >> 16 & 255u),
                       (unsigned)(nn.cfg.ip >> 8 & 255u), (unsigned)(nn.cfg.ip & 255u), (unsigned)n.address);
                fflush(stdout);
                if (c >= 0) { close(c); c = -1; }        /* the board changes over to its new IP */
            }
        }
        if (p[2].revents & POLLIN) {
            char ch;
            while (read(0, &ch, 1) == 1) {
                if (ch != '\n') { if (ll < sizeof line - 1) line[ll++] = ch; continue; }
                line[ll] = 0; ll = 0;
                int v;
                if (sscanf(line, "in %i", &v) == 1) hw.inputs = (uint8_t)v;
            }
        }
        if (c >= 0 && (p[3].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t d[512], tx[2u * IOB_MBAP_MAX];
            ssize_t k = recv(c, d, sizeof d, 0);
            int out = k > 0 ? node_net_tcp(&nn, now, d, (uint16_t)k, tx, sizeof tx) : -1;
            if (out > 0) { ssize_t w = send(c, tx, (size_t)out, 0); (void)w; }
            if (out < 0) { close(c); c = -1; }
        }
    }
}
