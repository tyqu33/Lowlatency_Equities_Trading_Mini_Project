// libs/appkit/src/config.cpp
// Implementation of load_config over toml++.

#include "hft/appkit/config.hpp"

namespace hft::appkit {

toml::table load_config(const std::string& path) {
    return toml::parse_file(path);
}

}  // namespace hft::appkit
