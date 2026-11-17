#include "core/ClientAddress.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace whist
{

namespace
{

constexpr std::array<std::uint8_t, 12> mappedPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

std::string_view trim(std::string_view text)
{
    constexpr std::string_view blanks = " \t";
    const std::size_t first = text.find_first_not_of(blanks);
    if(first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

bool isPort(std::string_view text)
{
    if(text.empty() || text.size() > 5 || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    return std::stoul(std::string(text)) <= 65535;
}

// An address alone: no brackets, no port. A colon makes it IPv6.
std::optional<IpAddress> parseBare(std::string_view text)
{
    // inet_pton reads to the first NUL, so a string with one inside would be cut short.
    if(text.empty() || text.find('\0') != std::string_view::npos)
        return std::nullopt;

    const std::string terminated(text);
    std::array<std::uint8_t, 16> bytes{};
    if(terminated.find(':') == std::string::npos)
    {
        in_addr v4{};
        if(::inet_pton(AF_INET, terminated.c_str(), &v4) != 1)
            return std::nullopt;
        std::ranges::copy(mappedPrefix, bytes.begin());
        std::memcpy(bytes.data() + mappedPrefix.size(), &v4, sizeof v4);
    }
    else
    {
        in6_addr v6{};
        if(::inet_pton(AF_INET6, terminated.c_str(), &v6) != 1)
            return std::nullopt;
        std::memcpy(bytes.data(), &v6, sizeof v6);
    }
    return IpAddress(bytes);
}

bool isMapped(const std::array<std::uint8_t, 16>& bytes)
{
    return std::equal(mappedPrefix.begin(), mappedPrefix.end(), bytes.begin());
}

bool sharesPrefix(const std::array<std::uint8_t, 16>& a, const std::array<std::uint8_t, 16>& b, unsigned int bits)
{
    const unsigned int whole = bits / 8;
    const unsigned int rest = bits % 8;
    if(!std::equal(a.begin(), a.begin() + whole, b.begin()))
        return false;
    if(rest == 0)
        return true;

    const auto mask = static_cast<std::uint8_t>(0xFF << (8 - rest));
    return (a[whole] & mask) == (b[whole] & mask);
}

} // namespace

IpAddress::IpAddress(const std::array<std::uint8_t, 16>& bytes) : bytes_(bytes)
{
}

std::optional<IpAddress> IpAddress::parse(std::string_view text)
{
    text = trim(text);
    if(text.empty())
        return std::nullopt;

    if(text.front() == '[')
    {
        const std::size_t close = text.find(']');
        if(close == std::string_view::npos)
            return std::nullopt;

        const std::string_view inner = text.substr(1, close - 1);
        const std::string_view after = text.substr(close + 1);
        if(inner.find(':') == std::string_view::npos)
            return std::nullopt;
        if(!after.empty() && !(after.front() == ':' && isPort(after.substr(1))))
            return std::nullopt;
        return parseBare(inner);
    }

    // One colon is an IPv4 address and its port; an IPv6 address has at least two.
    if(std::ranges::count(text, ':') == 1)
    {
        const std::size_t colon = text.find(':');
        if(!isPort(text.substr(colon + 1)))
            return std::nullopt;
        return parseBare(text.substr(0, colon));
    }
    return parseBare(text);
}

std::string IpAddress::key() const
{
    char text[INET6_ADDRSTRLEN];
    if(isMapped(bytes_))
    {
        ::inet_ntop(AF_INET, bytes_.data() + mappedPrefix.size(), text, sizeof text);
        return text;
    }

    std::array<std::uint8_t, 16> prefix{};
    std::copy_n(bytes_.begin(), 8, prefix.begin());
    ::inet_ntop(AF_INET6, prefix.data(), text, sizeof text);
    return std::string(text) + "/64";
}

const std::array<std::uint8_t, 16>& IpAddress::bytes() const
{
    return bytes_;
}

TrustedProxies::TrustedProxies(const std::vector<std::string>& entries)
{
    for(const std::string& entry : entries)
    {
        const auto invalid = [&] { return std::invalid_argument("\"" + entry + "\" is not an address or a range"); };

        const std::string_view text = trim(entry);
        const std::size_t slash = text.find('/');
        const std::string_view host = text.substr(0, slash);
        const std::optional<IpAddress> address = parseBare(host);
        if(!address)
            throw invalid();

        // An IPv4 range counts its prefix from the start of the IPv4-mapped form.
        const bool v4 = host.find(':') == std::string_view::npos;
        unsigned int bits = 128;
        if(slash != std::string_view::npos)
        {
            const std::string_view length = text.substr(slash + 1);
            if(length.empty() || length.size() > 3 || !std::ranges::all_of(length, [](char c) { return c >= '0' && c <= '9'; }))
                throw invalid();

            const auto prefix = static_cast<unsigned int>(std::stoul(std::string(length)));
            if(prefix > (v4 ? 32U : 128U))
                throw invalid();
            bits = v4 ? 96 + prefix : prefix;
        }
        ranges_.push_back({address->bytes(), bits});
    }
}

bool TrustedProxies::contains(const IpAddress& address) const
{
    return std::ranges::any_of(ranges_, [&](const Range& range)
    {
        return sharesPrefix(address.bytes(), range.network, range.prefixBits);
    });
}

std::string clientKey(std::string_view peer, std::string_view forwardedFor, const TrustedProxies& trusted)
{
    const std::optional<IpAddress> peerAddress = IpAddress::parse(peer);
    if(!peerAddress)
        return std::string(peer);
    if(!trusted.contains(*peerAddress))
        return peerAddress->key();

    std::string_view rest = forwardedFor;
    while(!rest.empty())
    {
        const std::size_t comma = rest.rfind(',');
        const std::optional<IpAddress> entry =
            IpAddress::parse(comma == std::string_view::npos ? rest : rest.substr(comma + 1));
        if(!entry)
            break;
        if(!trusted.contains(*entry))
            return entry->key();
        if(comma == std::string_view::npos)
            break;
        rest = rest.substr(0, comma);
    }
    return peerAddress->key();
}

} // namespace whist
