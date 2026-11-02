// UPnP (D14, knowledge 22 section 7.1): the path that needs no router. The tests/net build does not link
// miniupnpc, so upnp_open must report "not built with UPnP" (the log text the host prints when the port was
// built without the prefix) and upnp_close must be a safe no-op. The real mapping is checked by hand on a LAN
// (the card's Done-when), never in this test.
#include <string>

#include "check.h"
#include "upnp.h"

using namespace don_net;

int main()
{
    const UpnpResult r = upnp_open(7877);
    CHECK(!r.mapped);
    CHECK(!r.message.empty());
    CHECK(r.message.find("not built with UPnP") != std::string::npos);
    CHECK(r.external_ip.empty());
    CHECK(r.local_ip.empty());

    // Calling close without a mapping (and twice) is a no-op, never a crash.
    upnp_close();
    upnp_close();
    return check_result("test_net_upnp");
}
