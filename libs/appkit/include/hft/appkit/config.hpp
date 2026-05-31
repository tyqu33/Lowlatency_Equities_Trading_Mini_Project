// libs/appkit/include/hft/appkit/config.hpp
// Responsibility: load a TOML config file (thin toml++ wrapper).
//
// "Buy, don't build" application plumbing, off the hot path. The per-process config *schema*
// (which keys mean what) is intentionally NOT defined here; that is application logic
// each node adds later. This wrapper only handles parsing.
#pragma once

#include <string>
#include <toml++/toml.hpp>

namespace hft::appkit {

// Parse a TOML file into a table. Throws toml::parse_error on malformed input
// (the exception message includes the file position).
toml::table load_config(const std::string& path);

}  // namespace hft::appkit
