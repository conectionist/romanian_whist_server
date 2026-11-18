#ifndef WHIST_CORE_SEATING_H
#define WHIST_CORE_SEATING_H

#include "core/Protocol.h"
#include "core/Settings.h"

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

// A person at a table: in a seat, or watching.
struct TableMember
{
    MemberId id = 0;
    std::optional<unsigned int> seat;

    // When their connection went, while it stays gone.
    std::optional<Clock::time_point> awaySince{};
};

// Who sits where at a table, who hosts it, and the rules for changing them. Plain data: a table
// holds its lock around it and tells everyone what changed. A person's profile lives in their seat's
// setting and nowhere else.
class Seating
{
public:
    Seating(Settings settings, std::vector<TableMember> members, MemberId host);

    const Settings& settings() const;

    // In the order they came.
    const std::vector<TableMember>& members() const;
    MemberId host() const;
    bool hasMember(MemberId member) const;
    std::optional<unsigned int> seatOf(MemberId member) const;
    std::optional<MemberId> memberAt(unsigned int seat) const;
    bool hasOpenSeat() const;

    // Solo: the speed and the wait, which may change at any time.
    void setPacing(int botSpeed, bool waitAfterRound);

    std::optional<Clock::time_point> awaySince(MemberId member) const;

    // Records when the member's connection went, or that it is back.
    void setAway(MemberId member, std::optional<Clock::time_point> since);

    // The first member after the host, in the order they came, for whom `here` holds takes over
    // hosting. False when there is no one.
    bool passHost(const std::function<bool(MemberId)>& here);

    // ---- Members who left a game under way ----

    // The member leaves a game under way, but their seat stays as it is, with a computer player in
    // it. They are no longer a member. A host who leaves hands over as in leave().
    void depart(MemberId member);

    bool hasDeparted() const;
    bool departed(MemberId member) const;

    // The member of a game still under way comes back to the seat they left.
    void rejoin(MemberId member);

    // The game is over: the seats of those who left open.
    void openDeparted();

    // ---- Online, between games. Each changes nothing when it refuses. ----

    // A newcomer, in the first open seat, with a number after their name when it is taken. The
    // profile is clean and checked.
    std::optional<Refusal> join(MemberId member, Profile profile);

    // Opens the member's seat. A host who leaves hands over to whoever has been there longest.
    void leave(MemberId member);

    // The member moves to an open seat.
    std::optional<Refusal> takeSeat(MemberId member, unsigned int seat);

    // The host makes a seat open, or a computer player of `level`. A computer player's name and
    // portrait are drawn by the catalogue's rules, its names in `language`; one whose level changes
    // keeps its gender.
    std::optional<Refusal> setSeat(MemberId by, unsigned int seat, SeatKind kind, std::optional<Level> level,
                                   std::string_view language, std::mt19937_64& random);

    // A clean, checked profile, whose name nobody else at the table has.
    std::optional<Refusal> updateProfile(MemberId member, Profile profile);

    // The host's settings. Of the seats only their number is read: seats added are open, and seats
    // taken off the end must hold nobody. What a seat holds changes through the calls above, so a
    // host's change never fails because someone else changed their profile meanwhile.
    std::optional<Refusal> updateSettings(MemberId by, const Settings& settings);

private:
    // The keys of every name at the table, but the one in `except`.
    std::set<std::string> takenKeys(std::optional<unsigned int> except) const;

    // How many seats wear each portrait, but the one in `except`.
    std::map<std::string, unsigned int> worn(std::optional<unsigned int> except) const;

    TableMember* find(MemberId member);

    Settings settings_;
    std::vector<TableMember> members_;
    std::vector<TableMember> departed_;
    MemberId host_;
};

} // namespace whist

#endif
