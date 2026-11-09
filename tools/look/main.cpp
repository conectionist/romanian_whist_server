#include "look/LookFixtures.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace whist::look;

bool write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

// Removes the .json files in `directory` that `files` does not name, so a picture that is gone
// takes its file with it. Only .json, so that the README beside them stays.
void removeLeftovers(const std::filesystem::path& directory, const std::vector<LookFile>& files)
{
    std::vector<std::filesystem::path> leftovers;
    for(const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
    {
        const std::string name = entry.path().filename().string();
        const bool written = std::any_of(files.begin(), files.end(),
                                         [&name](const LookFile& file) { return file.name == name; });
        if(entry.path().extension() == ".json" && !written)
            leftovers.push_back(entry.path());
    }
    for(const std::filesystem::path& path : leftovers)
        std::filesystem::remove(path);
}

int run(int argc, char** argv)
{
    if(argc > 2)
    {
        std::cerr << "usage: whist_look [directory]\n";
        return 2;
    }

    // A directory named by hand may hold .json files that are not ours, so only the default one is
    // cleaned.
    const bool clean = argc == 1;
    const std::filesystem::path directory = clean ? lookDirectory() : std::filesystem::path(argv[1]);

    const Built built = buildLookFiles();
    if(!built.problems.empty())
    {
        for(const std::string& problem : built.problems)
            std::cerr << problem << '\n';
        std::cerr << "Nothing was written.\n";
        return 1;
    }

    std::filesystem::create_directories(directory);
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

    // After every write, so a run that fails does not also remove files.
    if(clean)
        removeLeftovers(directory, built.files);
    return 0;
}

} // namespace

// Writes protocol/look/, or the directory named as the argument. It builds every file first and
// writes nothing when anything is not as the desktop's tool has it.
int main(int argc, char** argv)
{
    try
    {
        return run(argc, argv);
    }
    catch(const std::exception& error)
    {
        std::cerr << "whist_look: " << error.what() << '\n';
        return 1;
    }
}
