#pragma once
#include "../Json/json.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace shader6 {
using json = nlohmann::json;
struct Result {
    std::vector<unsigned> code;
    std::string error;
    json constants = json::array();
    std::string warnings;
};
Result compile(const json &, const std::filesystem::path &);
std::filesystem::path resolve(const std::filesystem::path &, const std::string &);
bool validate(const std::vector<unsigned> &, bool vertex, std::string &error);
json reflect(const std::vector<unsigned> &);
} // namespace shader6
