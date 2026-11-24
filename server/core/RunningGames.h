#ifndef WHIST_CORE_RUNNINGGAMES_H
#define WHIST_CORE_RUNNINGGAMES_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace whist
{

// The game threads running, and the games among them that count against the most that may run: those
// someone is at. A game nobody is at holds and uses no processor, so it is no reason to refuse another.
// A game takes a Slot before its thread starts and gives it back when the thread is done with the engine.
class RunningGames : public std::enable_shared_from_this<RunningGames>
{
public:
    // `max` and `maxInstant` 0 allow any number of attended games, or attended Instant games, and
    // `maxInstantPerAddress` 0 any number of them from one address.
    explicit RunningGames(std::size_t max, std::size_t maxInstant = 0, std::size_t maxInstantPerAddress = 0);

    // Which most refused a game.
    enum class Full
    {
        All,
        Instant,
        InstantAddress
    };

    struct Counts
    {
        std::size_t threads = 0;
        std::size_t attended = 0;
        std::size_t attendedInstant = 0;
    };

    class Slot
    {
    public:
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&& other) noexcept;
        ~Slot();

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        // Whether the game counts against the most. A slot is attended when it is taken. Making it
        // attended again is never refused, so a player who comes back always has their game. One
        // thread at a time.
        void setAttended(bool attended);

        // Whether the game runs at Instant. Becoming Instant is refused, and nothing changes, when the
        // game is attended and `maxInstant` attended Instant games are running, or its address has
        // `maxInstantPerAddress` of them. `full`, when given, says which. One thread at a time.
        bool setInstant(bool instant, Full* full = nullptr);

        // The game is over, or its table closed: it no longer counts against its address, though its
        // thread may still be ending. Safe to call again.
        void leaveAddress();

        // Gives the place back at once. Safe to call again.
        void release();

    private:
        friend class RunningGames;
        Slot(std::shared_ptr<RunningGames> games, bool instant, std::string address);

        std::shared_ptr<RunningGames> games_;
        bool attended_ = true;
        bool instant_ = false;

        // Whoever started the game. An empty address is never limited.
        std::string address_;
        bool countsForAddress_ = true;
    };

    // A place for one more game, or nothing when `max` attended games are running, or, for an Instant
    // game, `maxInstant` attended Instant games, or `maxInstantPerAddress` Instant games from `address`,
    // attended or not. `full`, when given, says which.
    std::optional<Slot> take(bool instant = false, Full* full = nullptr, std::string address = {});

    // The three counts below, read together.
    Counts counts() const;

    // The game threads running.
    std::size_t count() const;

    // The games that count against the most.
    std::size_t attended() const;

    // The attended games at Instant, which count against `maxInstant` too.
    std::size_t attendedInstant() const;

    // The Instant games that `address` started, attended or not, until each is over.
    std::size_t instantFrom(const std::string& address) const;

    std::size_t max() const;
    std::size_t maxInstant() const;

private:
    // Why an Instant game more from `address`, attended if `attended`, would be refused, if it would.
    std::optional<Full> instantFullLocked(const std::string& address, bool attended) const;

    // One Instant game more, or one fewer, from `address`.
    void countAddressLocked(const std::string& address, bool more);

    const std::size_t max_;

    // Instant games' computer players move back to back, so each takes several times the processor
    // of a game at any other speed.
    const std::size_t maxInstant_;

    // So that one address cannot hold every Instant place. Its games count while they run, attended
    // or not: one that is attended again is never refused, so counting only those would let an address
    // leave a game, start another, and come back to both.
    const std::size_t maxInstantPerAddress_;

    mutable std::mutex mutex_;
    std::size_t count_ = 0;
    std::size_t attended_ = 0;
    std::size_t attendedInstant_ = 0;

    // Only addresses with at least one.
    std::unordered_map<std::string, std::size_t> instantByAddress_;
};

} // namespace whist

#endif
