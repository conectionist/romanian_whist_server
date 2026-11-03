#include "support/Oracle.h"

#include "core/Cards.h"

#include <romanian_whist/Snapshot.h>

#include <map>
#include <optional>
#include <set>

namespace whist::test
{

using namespace romanian_whist;

Oracle::Oracle(Recorder& recorder, std::vector<Viewer> viewers) : recorder_(recorder), viewers_(std::move(viewers))
{
}

void Oracle::onRoundStarted(const GameEngine& engine)
{
    Mark mark;
    mark.round = engine.getCurrentRoundIndex();
    for(const Viewer& viewer : viewers_)
    {
        std::optional<Seat> seat;
        if(viewer.seat)
            seat = Seat{*viewer.seat};

        std::set<std::string>& visible = mark.visible[viewer.member];
        const GameSnapshot snapshot = snapshotFrom(engine, seat);
        for(const SeatSnapshot& holder : snapshot.seats)
        {
            if(holder.hand)
            {
                for(const Card& card : *holder.hand)
                    visible.insert(cardCode(card));
            }
        }
        if(snapshot.trump)
            visible.insert(cardCode(*snapshot.trump));
    }
    recorder_.mark(std::move(mark));
}

void Oracle::onCardPlayed(const GameEngine&, Seat, const Card& card)
{
    Mark mark;
    mark.card = cardCode(card);
    recorder_.mark(std::move(mark));
}

namespace
{

// Every card in a string, in the server's spelling or the engine's ("10H").
std::vector<std::string> cardsIn(const std::string& text)
{
    constexpr std::string_view ranks = "23456789TJQKA";
    constexpr std::string_view suits = "HDSC";

    std::vector<std::string> found;
    for(std::size_t i = 1 ; i < text.size() ; i++)
    {
        if(suits.find(text[i]) == std::string_view::npos)
            continue;
        if(ranks.find(text[i - 1]) != std::string_view::npos)
            found.push_back(text.substr(i - 1, 2));
        else if(i >= 2 && text[i - 2] == '1' && text[i - 1] == '0')
            found.push_back(text.substr(i - 2, 3));
    }
    return found;
}

struct Scan
{
    unsigned int round = 0;
    std::map<MemberId, std::set<std::string>> current;
    std::map<unsigned int, std::map<MemberId, std::set<std::string>>> byRound;
    std::vector<std::string> leaks;

    const std::set<std::string>& allowed(MemberId member, unsigned int ofRound)
    {
        if(ofRound == round)
            return current[member];
        return byRound[ofRound][member];
    }

    void walk(const Json::Value& value, MemberId member, unsigned int ofRound, const std::string& path)
    {
        if(value.isString())
        {
            for(const std::string& card : cardsIn(value.asString()))
            {
                if(!allowed(member, ofRound).contains(card))
                    leaks.push_back("member " + std::to_string(member) + " saw " + card + " at " + path);
            }
            return;
        }

        if(value.isArray())
        {
            for(Json::ArrayIndex i = 0 ; i < value.size() ; i++)
            {
                unsigned int inRound = ofRound;
                if(path.ends_with(".history"))
                    inRound = value[i]["index"].asUInt();
                walk(value[i], member, inRound, path + "[" + std::to_string(i) + "]");
            }
            return;
        }

        if(value.isObject())
        {
            for(const std::string& key : value.getMemberNames())
            {
                // A table's code and a member's token are random letters, not cards.
                if(key == "code" || key == "token")
                    continue;
                unsigned int inRound = ofRound;
                if(key == "lastTrick" && value[key].isObject())
                    inRound = value[key]["round"].asUInt();
                walk(value[key], member, inRound, path + "." + key);
            }
        }
    }
};

} // namespace

std::vector<std::string> scanForLeaks(const std::vector<Entry>& entries)
{
    Scan scan;
    for(const Entry& entry : entries)
    {
        if(entry.mark)
        {
            if(entry.mark->round)
            {
                scan.round = *entry.mark->round;
                scan.current = entry.mark->visible;
                scan.byRound[scan.round] = entry.mark->visible;
            }
            if(entry.mark->card)
            {
                for(auto& [member, cards] : scan.current)
                    cards.insert(*entry.mark->card);
                for(auto& [member, cards] : scan.byRound[scan.round])
                    cards.insert(*entry.mark->card);
            }
            continue;
        }

        if(entry.message.isNull())
        {
            scan.leaks.push_back("a message that did not parse: " + entry.text);
            continue;
        }
        scan.walk(entry.message, entry.member, scan.round, entry.message["type"].asString());
    }
    return scan.leaks;
}

} // namespace whist::test
