#include "shm/shm_config_reader.hpp"

#include <fstream>
#include <sstream>

namespace shm {

namespace {

std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parse_line(const std::string& line, std::string& name, std::size_t& size) {
    std::string s = trim(line);
    if (s.empty() || s[0] == '#') return false;

    std::string name_val, size_val;
    std::istringstream iss(s);
    std::string token;

    while (std::getline(iss, token, ',')) {
        std::string t = trim(token);
        if (t.empty()) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key == "name") {
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                name_val = val.substr(1, val.size() - 2);
            } else {
                name_val = val;
            }
        } else if (key == "size") {
            size_val = val;
        }
    }

    if (name_val.empty() || size_val.empty()) return false;

    try {
        std::string num_part = size_val;
        double multiplier = 1.0;  // 1 = 字节
        if (size_val.size() >= 2 &&
            (size_val.compare(size_val.size() - 2, 2, "MB") == 0 ||
             size_val.compare(size_val.size() - 2, 2, "mb") == 0)) {
            num_part = trim(size_val.substr(0, size_val.size() - 2));
            multiplier = 1024.0 * 1024.0;
        } else if (size_val.size() >= 2 &&
                   (size_val.compare(size_val.size() - 2, 2, "GB") == 0 ||
                    size_val.compare(size_val.size() - 2, 2, "gb") == 0)) {
            num_part = trim(size_val.substr(0, size_val.size() - 2));
            multiplier = 1024.0 * 1024.0 * 1024.0;
        } else if (size_val.size() >= 1 && (size_val.back() == 'M' || size_val.back() == 'm')) {
            num_part = trim(size_val.substr(0, size_val.size() - 1));
            multiplier = 1024.0 * 1024.0;
        } else if (size_val.size() >= 1 && (size_val.back() == 'G' || size_val.back() == 'g')) {
            num_part = trim(size_val.substr(0, size_val.size() - 1));
            multiplier = 1024.0 * 1024.0 * 1024.0;
        }

        std::size_t pos = 0;
        if (multiplier != 1.0) {
            double val = std::stod(num_part, &pos);
            if (pos != num_part.size()) return false;
            size = static_cast<std::size_t>(val * multiplier);
        } else {
            size = std::stoull(num_part, &pos);
            if (pos != num_part.size()) return false;
        }
    } catch (...) {
        return false;
    }

    name = name_val;
    return true;
}

}  // namespace

std::map<std::string, std::size_t> ShmConfigReader::load(const std::string& filename) const {
    std::map<std::string, std::size_t> result;
    std::ifstream f(filename);
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        std::string name;
        std::size_t size = 0;
        if (parse_line(line, name, size)) {
            result[name] = size;
        }
    }
    return result;
}

}  // namespace shm
