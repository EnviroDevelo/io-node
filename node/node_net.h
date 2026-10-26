/* node_net.h - the I/O board on Ethernet (decision D27, revised 24 Oct): the Modbus TCP server and
 * the discovery responder, as the platform's iob_net.h describes them. Pure C like node.c: the
 * port's network stack (lwIP raw API on the H5, sockets on the PC) only moves bytes.
 *
 * Modbus TCP (port IOB_TCP_PORT): each complete frame is turned into the RTU frame node_frame()
 * already handles, on the master port (NODE_PORT_RS485: every RS-485 rule holds, the address is
 * the unit id, the comms watchdog counts these requests), and the reply goes back the same way.
 *   - No IP set (a new board): no Modbus TCP at all.
 *   - One connection at a time; a new one replaces the old one (the supervisor restarted).
 *   - Once the board has a supervisor IP, connections from any other address are refused.
 *   - A stream that is not Modbus TCP, or a frame too long, closes the connection.
 *
 * Discovery (UDP, IOB_DISC_PORT): FIND -> HERE with the identity and settings; SET for this board's
 * UID (iob_disc_set_accept: usable settings, from its supervisor once it has one) -> the IP settings
 * and the address are saved and read back, then HERE with what is now saved. The port sends the
 * reply to 255.255.255.255 at the sender's port, then applies the new IP settings.
 *
 * Back to new: setting the address to 0 over USB (tools/set_address.py --clear) also clears the IP
 * settings, so the board can be commissioned again from any supervisor (node_net_poll). */
#ifndef NODE_NET_H
#define NODE_NET_H
#include <stdbool.h>
#include <stdint.h>
#include "iob_net.h"
#include "node.h"

typedef struct { uint32_t ip, mask, gw, master_ip; } node_ipcfg_t;   /* host byte order; ip 0 = not set */

typedef struct {
    node_t      *node;
    node_ipcfg_t cfg;               /* as saved */
    uint8_t      mac[6];
    /* the IP settings in the board's non-volatile store: written and read back, NODE_HW_* */
    int        (*store)(void *ctx, const node_ipcfg_t *c);
    void        *ctx;
    uint8_t      rx[2u * IOB_MBAP_MAX];   /* the connection's stream */
    uint16_t     have;
    uint32_t     tcp_frames, refused, disc, sets;   /* statistics */
} node_net_t;

void     node_net_init(node_net_t *nn, node_t *node, const node_ipcfg_t *saved, const uint8_t mac[6],
                       int (*store)(void *ctx, const node_ipcfg_t *c), void *ctx);
bool     node_net_serving(const node_net_t *nn);                /* the Modbus TCP port is open */
bool     node_net_accept(node_net_t *nn, uint32_t from_ip);     /* a new connection: take it? (replaces the old) */
/* Bytes received on the connection. Replies (if any) go to tx, at most cap bytes (2 x IOB_MBAP_MAX
 * is always enough); returns their length, or -1: close the connection. */
int      node_net_tcp(node_net_t *nn, uint32_t now_ms, const uint8_t *data, uint16_t len, uint8_t *tx, uint16_t cap);
/* A discovery datagram. Returns the reply's length (IOB_DISC_LEN) or 0; *changed: the IP settings
 * changed, apply them once the reply is sent (and drop the Modbus TCP connection). */
uint16_t node_net_disc(node_net_t *nn, uint32_t now_ms, const uint8_t *d, uint16_t len, uint32_t from_ip,
                       uint8_t reply[IOB_DISC_LEN], bool *changed);
/* Every pass: true when the IP settings changed outside discovery (address cleared over USB). */
bool     node_net_poll(node_net_t *nn);

#endif
