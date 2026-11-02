// UPnP: the host opens its own UDP port on the router (D14, docs/knowledge/22-netplay.md section 7.1).
// upnp_open discovers the router, maps UDP <port> to this PC and reads the external IP; upnp_close removes
// the mapping. A failure never stops the host (on a LAN or with a manually forwarded port UPnP is not needed).
// Only upnp.cpp includes miniupnpc; everything here is plain C++ so the unit tests build it without the library.
#pragma once

#include <cstdint>
#include <string>

namespace don_net
{

struct UpnpResult
{
    bool mapped = false;
    std::string external_ip; // the router's public address, when known
    std::string local_ip;    // this PC's address the mapping points at, when known
    std::string message;     // why the mapping failed, or a note; logged by the host
};

// Discover the router (2 s), map UDP port -> this PC's port, read the external IP. The mapping's description
// is "Battle Stadium DON" and its lease 0 (until removed by upnp_close). A mapping of the same port to another
// PC is left alone and reported. Safe to call once; a later call replaces the stored mapping.
UpnpResult upnp_open(uint16_t port);

// Remove the mapping made by upnp_open. Nothing when there is none. Call at exit, and after a desync or error.
void upnp_close();

} // namespace don_net
