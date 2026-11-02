// lan_ipv4 (docs/knowledge/22-netplay.md, the host's lobby when UPnP answers nothing): the routing trick must
// return a real LAN address on a networked machine (this PC has docker bridges at 172.x that it must skip) and
// an empty string when there is no default route. Empty is a valid answer, so the test never fails for that.
#include "transport.h"

#include <cstdio>

#include "check.h"

using namespace don_net;

namespace
{

void test_lan_ipv4()
{
    const std::string ip = lan_ipv4();
    if (ip.empty())
        return; // no network / no route: an allowed result
    int a = -1, b = -1, c = -1, d = -1;
    char end = 0;
    const int parsed = std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &end);
    CHECK_EQ(parsed, 4); // four octets and nothing else
    for (int o : {a, b, c, d})
        CHECK(o >= 0 && o <= 255);
    CHECK(!(a == 0 && b == 0 && c == 0 && d == 0)); // not 0.0.0.0 (no route)
    CHECK(a != 127);                                // not loopback
}

} // namespace

int main()
{
    test_lan_ipv4();
    return check_result("test_net_lan");
}
