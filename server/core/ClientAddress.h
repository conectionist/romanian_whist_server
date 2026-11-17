#ifndef WHIST_CORE_CLIENTADDRESS_H
#define WHIST_CORE_CLIENTADDRESS_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

// An IPv4 or IPv6 address. IPv4 is held as its IPv4-mapped IPv6 address, so 10.0.0.1 and
// ::ffff:10.0.0.1 are one address.
class IpAddress
{
public:
    explicit IpAddress(const std::array<std::uint8_t, 16>& bytes);

    // An address as a header or a socket writes it: 1.2.3.4, 2001:db8::1, [2001:db8::1], and the
    // first and last with a port after them. A name, a zone or a stray character gives nothing.
    static std::optional<IpAddress> parse(std::string_view text);

    // What per-address limits are counted under: an IPv4 address as it is written, an IPv6 one as
    // its /64, since one subscriber commonly holds a whole /64.
    std::string key() const;

    const std::array<std::uint8_t, 16>& bytes() const;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

// The proxies in front of the server whose X-Forwarded-For is believed.
class TrustedProxies
{
public:
    TrustedProxies() = default;

    // Each entry is an address or a range, such as 127.0.0.1, 10.0.0.0/8 or fd00::/8. Throws
    // std::invalid_argument naming the entry that is not one.
    explicit TrustedProxies(const std::vector<std::string>& entries);

    bool contains(const IpAddress& address) const;

private:
    struct Range
    {
        std::array<std::uint8_t, 16> network;
        unsigned int prefixBits;
    };

    std::vector<Range> ranges_;
};

// What a request's limits are counted under. `peer` is the connection's address, `forwardedFor` the
// X-Forwarded-For header, empty when there is none.
//
// An untrusted peer is the client, whatever the header says. A trusted one is a proxy of ours, and
// the client is the rightmost entry that is not trusted too: whatever stands left of that is the
// client's own claim. When every entry is trusted the leftmost is the client, as nginx's
// real_ip_recursive has it. With no header, or past an entry that is no address, the peer stands
// for the client, so everyone behind it is counted together, which is too strict rather than too lax.
std::string clientKey(std::string_view peer, std::string_view forwardedFor, const TrustedProxies& trusted);

} // namespace whist

#endif
