// UPnP through miniupnpc (D14, docs/knowledge/22-netplay.md section 7.1). This is the only file that includes
// miniupnpc. When the port was built without the library (DON_WITH_UPNP not defined) upnp_open reports
// "not built with UPnP" and upnp_close does nothing, so src/host and tests/net build without the prefix.
#include "upnp.h"

#include <cstring>

#ifdef DON_WITH_UPNP

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <mutex>

namespace don_net
{
namespace
{

// The state of the mapping made by upnp_open, kept so upnp_close can remove exactly it. A mutex guards it: the
// host calls upnp_open on one thread and upnp_close on another at exit.
struct UpnpState
{
    std::mutex mutex;
    bool have = false;
    uint16_t port = 0;
    UPNPDev *devlist = nullptr;
    UPNPUrls urls{};
    IGDdatas data{};
    char local_ip[64] = {};
};

UpnpState &state()
{
    static UpnpState s;
    return s;
}

constexpr char kDescription[] = "Battle Stadium DON";
constexpr char kProtocol[] = "UDP";

void release(UpnpState &s)
{
    if (s.devlist)
    {
        freeUPNPDevlist(s.devlist);
        s.devlist = nullptr;
    }
    if (s.urls.controlURL) // FreeUPNPUrls needs a zeroed struct; controlURL is set by GetValidIGD
        FreeUPNPUrls(&s.urls);
    s.urls = UPNPUrls{};
    s.have = false;
    s.port = 0;
}

} // namespace

UpnpResult upnp_open(uint16_t port)
{
    UpnpState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    // A previous mapping is replaced: remove it first.
    if (s.have)
    {
        if (s.urls.controlURL)
            UPNP_DeletePortMapping(s.urls.controlURL, s.data.first.servicetype, std::to_string(s.port).c_str(),
                                   kProtocol, nullptr);
        release(s);
    }

    UpnpResult result;
    int error = 0;
    s.devlist = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_SAME, 0, 2, &error);
    if (!s.devlist)
    {
        result.message = "no UPnP router found (upnpDiscover failed: " +
                         (error ? std::string(strupnperror(error)) : std::string("no device")) + ")";
        return result;
    }

    const int igd = UPNP_GetValidIGD(s.devlist, &s.urls, &s.data, s.local_ip, sizeof(s.local_ip), nullptr, 0);
    if (igd == 0)
    {
        result.message = "no Internet Gateway Device (UPnP) on the router";
        release(s);
        return result;
    }
    result.local_ip = s.local_ip;

    const std::string ext = std::to_string(port);
    const int rc = UPNP_AddPortMapping(s.urls.controlURL, s.data.first.servicetype, ext.c_str(), ext.c_str(),
                                       s.local_ip, kDescription, kProtocol, nullptr, "0");
    if (rc != 0)
    {
        // 718 ConflictInMappingEntry: the port is already mapped (to another PC); left alone and reported.
        result.message = "the router did not open the port (UPNP_AddPortMapping: " +
                         std::string(strupnperror(rc)) + ")";
        release(s);
        return result;
    }

    char external[64] = {};
    if (UPNP_GetExternalIPAddress(s.urls.controlURL, s.data.first.servicetype, external) == 0 && external[0])
        result.external_ip = external;

    s.port = port;
    s.have = true;
    result.mapped = true;
    return result;
}

void upnp_close()
{
    UpnpState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.have)
        return;
    if (s.urls.controlURL)
        UPNP_DeletePortMapping(s.urls.controlURL, s.data.first.servicetype, std::to_string(s.port).c_str(),
                               kProtocol, nullptr);
    release(s);
}

} // namespace don_net

#else // !DON_WITH_UPNP

namespace don_net
{

UpnpResult upnp_open(uint16_t)
{
    UpnpResult result;
    result.message = "not built with UPnP";
    return result;
}

void upnp_close() {}

} // namespace don_net

#endif
