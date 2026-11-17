#include "core/ClientAddress.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace whist;
using Catch::Matchers::ContainsSubstring;

namespace
{

std::string keyOf(const std::string& text)
{
    const std::optional<IpAddress> address = IpAddress::parse(text);
    REQUIRE(address);
    return address->key();
}

// What the image's configuration trusts.
const TrustedProxies imageProxies({"127.0.0.1", "::1", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"});

std::string client(const std::string& peer, const std::string& forwardedFor)
{
    return clientKey(peer, forwardedFor, imageProxies);
}

} // namespace

TEST_CASE("An address is read as a header or a socket writes it", "[address]")
{
    CHECK(keyOf("203.0.113.9") == "203.0.113.9");
    CHECK(keyOf("  203.0.113.9\t") == "203.0.113.9");
    CHECK(keyOf("203.0.113.9:4711") == "203.0.113.9");
    CHECK(keyOf("2001:db8:1:2:3:4:5:6") == "2001:db8:1:2::/64");
    CHECK(keyOf("[2001:db8:1:2:3:4:5:6]") == "2001:db8:1:2::/64");
    CHECK(keyOf("[2001:db8:1:2:3:4:5:6]:443") == "2001:db8:1:2::/64");
    CHECK(keyOf("::1") == "::/64");
    CHECK(keyOf("[::1]:80") == "::/64");

    // An IPv4 address that arrives inside IPv6, as on a socket that listens for both.
    CHECK(keyOf("::ffff:203.0.113.9") == "203.0.113.9");
    CHECK(keyOf("[::ffff:203.0.113.9]:80") == "203.0.113.9");
}

TEST_CASE("Addresses of one IPv6 network are counted together, and IPv4 ones are not", "[address]")
{
    CHECK(keyOf("2001:db8:1:2::1") == keyOf("2001:db8:1:2:ffff:ffff:ffff:ffff"));
    CHECK(keyOf("2001:db8:1:2::1") != keyOf("2001:db8:1:3::1"));
    CHECK(keyOf("203.0.113.9") != keyOf("203.0.113.10"));
}

TEST_CASE("Text that is not an address is refused", "[address]")
{
    for(const char* text : {"", " ", "unknown", "localhost", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4:",
                            "1.2.3.4:99999", "1.2.3.4:http", "[1.2.3.4]", "[::1", "[::1]x", "[::1]:", "::1::1",
                            "fe80::1%eth0", "1.2.3.4 evil", "1.2.3.4,5.6.7.8", "-1.2.3.4", "0x7f.0.0.1"})
    {
        CAPTURE(text);
        CHECK_FALSE(IpAddress::parse(text));
    }

    // inet_pton would stop at the NUL and read the rest as nothing.
    CHECK_FALSE(IpAddress::parse(std::string("1.2.3.4\0evil", 12)));
}

TEST_CASE("A range is an address with a prefix, and an entry that is neither is refused", "[address]")
{
    const TrustedProxies proxies({"10.0.0.0/8", "172.16.0.0/12", "192.0.2.7", "fd00::/8", "2001:db8:1::5/128", "::1"});
    const auto trusts = [&](const char* text) { return proxies.contains(*IpAddress::parse(text)); };

    CHECK(trusts("10.0.0.1"));
    CHECK(trusts("10.255.255.255"));
    CHECK_FALSE(trusts("11.0.0.1"));
    CHECK_FALSE(trusts("9.255.255.255"));

    // A prefix that is not a whole number of bytes.
    CHECK(trusts("172.16.0.1"));
    CHECK(trusts("172.31.255.255"));
    CHECK_FALSE(trusts("172.32.0.0"));
    CHECK_FALSE(trusts("172.15.255.255"));

    // A bare address is a range of one.
    CHECK(trusts("192.0.2.7"));
    CHECK_FALSE(trusts("192.0.2.8"));

    CHECK(trusts("fd12:3456::1"));
    CHECK_FALSE(trusts("fe00::1"));
    CHECK(trusts("2001:db8:1::5"));
    CHECK_FALSE(trusts("2001:db8:1::6"));
    CHECK(trusts("::1"));

    // An IPv4 address is the same address however a socket writes it.
    CHECK(trusts("::ffff:10.1.2.3"));
    CHECK_FALSE(trusts("::ffff:11.1.2.3"));

    // A range of everything, and of nothing.
    CHECK(TrustedProxies({"0.0.0.0/0"}).contains(*IpAddress::parse("203.0.113.9")));
    CHECK_FALSE(TrustedProxies({"0.0.0.0/0"}).contains(*IpAddress::parse("2001:db8::1")));
    CHECK_FALSE(TrustedProxies().contains(*IpAddress::parse("127.0.0.1")));
}

TEST_CASE("A list of trusted proxies names the entry that is wrong", "[address]")
{
    for(const char* entry : {"", "localhost", "10.0.0.0/33", "10.0.0.0/", "10.0.0.0/8/8", "10.0.0.0/x", "::1/129",
                             "[::1]", "10.0.0.1:80", "10.0.0.0/-1", "10.0.0.0/0008"})
    {
        CAPTURE(entry);
        CHECK_THROWS_WITH(TrustedProxies({"127.0.0.1", entry}), ContainsSubstring(std::string("\"") + entry + "\""));
    }
}

TEST_CASE("A peer that is not trusted is the client, whatever it writes", "[address]")
{
    CHECK(client("203.0.113.9", "") == "203.0.113.9");
    CHECK(client("203.0.113.9", "198.51.100.1") == "203.0.113.9");
    CHECK(client("203.0.113.9", "10.0.0.1, 198.51.100.1") == "203.0.113.9");
    CHECK(client("2001:db8:1:2::9", "198.51.100.1") == "2001:db8:1:2::/64");

    // No proxy is trusted: the header is ignored from the loopback too.
    CHECK(clientKey("127.0.0.1", "198.51.100.1", TrustedProxies()) == "127.0.0.1");
}

TEST_CASE("A trusted peer names its client in X-Forwarded-For", "[address]")
{
    // Caddy, which writes the one address it saw.
    CHECK(client("172.18.0.3", "203.0.113.9") == "203.0.113.9");
    CHECK(client("127.0.0.1", "203.0.113.9") == "203.0.113.9");
    CHECK(client("::1", "203.0.113.9") == "203.0.113.9");
    CHECK(client("::ffff:127.0.0.1", "203.0.113.9") == "203.0.113.9");
    CHECK(client("172.18.0.3", "2001:db8:1:2::9") == "2001:db8:1:2::/64");
    CHECK(client("172.18.0.3", "[2001:db8:1:2::9]:5000") == "2001:db8:1:2::/64");
    CHECK(client("172.18.0.3", "203.0.113.9:5000") == "203.0.113.9");

    // No header: the peer is all there is.
    CHECK(client("172.18.0.3", "") == "172.18.0.3");
}

TEST_CASE("Of several entries the rightmost one that is not trusted is the client", "[address]")
{
    // nginx adds the address it saw to what it was sent, so the left of the list is the client's own claim.
    CHECK(client("127.0.0.1", "198.51.100.1, 203.0.113.9") == "203.0.113.9");
    CHECK(client("127.0.0.1", "10.0.0.5, 198.51.100.1, 203.0.113.9") == "203.0.113.9");

    // A proxy of ours in the middle of the chain is passed over.
    CHECK(client("127.0.0.1", "203.0.113.9, 10.0.0.2") == "203.0.113.9");
    CHECK(client("127.0.0.1", "198.51.100.1, 203.0.113.9, 10.0.0.2, 192.168.1.1") == "203.0.113.9");
    CHECK(client("127.0.0.1", "203.0.113.9,10.0.0.2") == "203.0.113.9");

    // What a client writes to look like one of our own proxies gets it nothing.
    CHECK(client("127.0.0.1", "10.1.1.1, 203.0.113.9") == "203.0.113.9");
    CHECK(client("127.0.0.1", "127.0.0.1, 203.0.113.9") == "203.0.113.9");
}

TEST_CASE("When no entry can be the client, the peer stands for it", "[address]")
{
    // Everything in the chain is ours.
    CHECK(client("127.0.0.1", "10.0.0.1") == "127.0.0.1");
    CHECK(client("172.18.0.3", "192.168.1.5, 10.0.0.2") == "172.18.0.3");

    // An entry that is no address stops the search: whatever stands left of it is no better.
    CHECK(client("127.0.0.1", "unknown") == "127.0.0.1");
    CHECK(client("127.0.0.1", "198.51.100.1, unknown") == "127.0.0.1");
    CHECK(client("127.0.0.1", "198.51.100.1, garbage, 10.0.0.2") == "127.0.0.1");
    CHECK(client("127.0.0.1", "203.0.113.9,") == "127.0.0.1");
    CHECK(client("127.0.0.1", ",") == "127.0.0.1");
    CHECK(client("127.0.0.1", "   ") == "127.0.0.1");
    CHECK(client("127.0.0.1", "203.0.113.9 evil") == "127.0.0.1");

    // A peer that is no address cannot be trusted, and is counted as it is written.
    CHECK(client("unix", "203.0.113.9") == "unix");
}
