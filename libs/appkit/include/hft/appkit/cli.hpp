// libs/appkit/include/hft/appkit/cli.hpp
// Responsibility: common command-line parsing shared by all hft processes (thin CLI11 wrapper).
//
// "Buy, don't build" application plumbing, off the hot path.
#pragma once

#include <string>
#include <string_view>

namespace hft::appkit {

// Options every hft process understands. Extend per-app with its own CLI::App as needed.
struct CommonArgs {
    std::string config_path;  // -c / --config : path to a TOML config file
    int cpu_core = -1;        // --core        : core to pin the hot thread to (-1 = none)
    int verbosity = 0;        // -v            : logging verbosity (repeatable)
};

// Parse argv into CommonArgs. On --help or a parse error this prints the message and
// exits the process (standard CLI11 behavior), so callers get a valid CommonArgs back.
CommonArgs parse_common_args(int argc, char** argv, std::string_view app_name,
                             std::string_view description);

}  // namespace hft::appkit
