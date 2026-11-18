#include "support/Fixtures.h"

#include <json/writer.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace whist::test
{

std::string fixtureText(const Json::Value& message)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, message) + "\n";
}

std::string checkFixture(const std::string& name, const Json::Value& message)
{
    const std::filesystem::path path = std::filesystem::path(WHIST_PROTOCOL_DIR) / "fixtures" / (name + ".json");
    const std::string wanted = fixtureText(message);

    const char* write = std::getenv("WHIST_WRITE_FIXTURES");
    if(write != nullptr && std::string(write) == "1")
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << wanted;
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if(!file)
        return path.string() + " is missing: run the fixture test with WHIST_WRITE_FIXTURES=1";

    std::stringstream contents;
    contents << file.rdbuf();
    if(contents.str() != wanted)
        return path.string() + " differs from what the server sends now:\n" + wanted;
    return {};
}

} // namespace whist::test
