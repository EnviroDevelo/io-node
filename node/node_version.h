/* node_version.h - node firmware identity (FW-52: FW_VERSION changes with every release).
 * FW_VERSION register = major << 8 | minor. FW_BUILD is set by the build (CI or the release
 * script passes -DNODE_FW_BUILD=n); a local build reports 0. */
#ifndef NODE_VERSION_H
#define NODE_VERSION_H

#define NODE_FW_MAJOR 0u
#define NODE_FW_MINOR 1u
#define NODE_FW_VERSION ((NODE_FW_MAJOR << 8) | NODE_FW_MINOR)
#ifndef NODE_FW_BUILD
#define NODE_FW_BUILD 0u
#endif

/* channel counts of this board (registers 75-78) */
#define NODE_N_OUT_A   8u
#define NODE_N_OUT_B   8u
#define NODE_N_RELAYS  2u
#define NODE_N_INPUTS  8u

#endif
