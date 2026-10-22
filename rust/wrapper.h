/* The server headers the Rust modules use, for bindgen (see build.rs). */
#include "config.h"
#include "options.h"
#include "structures.h"
#include "exceptions.h"
#include "log.h"
#include "name_lookup.h"
#include "net_proto.h"
#include "server.h"

/* Option values that are macros naming other macros, which bindgen does
 * not follow, as constants it can read. */
enum moo_rs_options {
#ifdef OUTBOUND_NETWORK
    MOO_RS_OUTBOUND_NETWORK_DEFAULT = OUTBOUND_NETWORK,
    MOO_RS_OUTBOUND_NETWORK_SUPPORTED = 1,
#else
    MOO_RS_OUTBOUND_NETWORK_DEFAULT = 0,
    MOO_RS_OUTBOUND_NETWORK_SUPPORTED = 0,
#endif
#if NETWORK_PROTOCOL == NP_TCP && NETWORK_STYLE == NS_BSD
    MOO_RS_BSD_TCP = 1,
#else
    MOO_RS_BSD_TCP = 0,
#endif
};
