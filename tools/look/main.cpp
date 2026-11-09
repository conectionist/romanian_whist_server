#include "look/LookFixtures.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

bool write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

} // namespace

// Writes protocol/look/, or the directory named as the argument. It builds every file first and
// writes nothing when anything is not as the desktop's tool has it.
int main(int argc, char** argv)
{
    using namespace whist::look;

    if(argc > 2)
    {
        std::cerr << "usage: whist_look [directory]\n";
        return 2;
    }
    const std::filesystem::path directory = argc == 2 ? std::filesystem::path(argv[1]) : lookDirectory();

    const Built built = buildLookFiles();
    if(!built.problems.empty())
    {
        for(const std::string& problem : built.problems)
            std::cerr << problem << '\n';
        std::cerr << "Nothing was written.\n";
        return 1;
    }

    std::filesystem::create_directories(directory);

    // Only .json files, so that the README beside them stays.
    for(const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
    {
        const std::string name = entry.path().filename().string();
        const bool written = std::any_of(built.files.begin(), built.files.end(),
                                         [&name](const LookFile& file) { return file.name == name; });
        if(entry.path().extension() == ".json" && !written)
            std::filesystem::remove(entry.path());
    }

    for(const LookFile& file : built.files)
    {
        const std::filesystem::path path = directory / file.name;
        if(!write(path, lookText(file.content)))
        {
            std::cerr << "Cannot write " << path.string() << '\n';
            return 1;
        }
        std::cout << path.string() << ": " << file.moment << '\n';
    }
    return 0;
}
