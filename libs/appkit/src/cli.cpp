// libs/appkit/src/cli.cpp
// Implementation of parse_common_args over CLI11.

#include "hft/appkit/cli.hpp"

#include <CLI/CLI.hpp>
#include <cstdlib>

namespace hft::appkit {

CommonArgs parse_common_args(int argc, char** argv, std::string_view app_name,
                             std::string_view description) {
    CommonArgs args;
    CLI::App app{std::string(description), std::string(app_name)};
    app.add_option("-c,--config", args.config_path, "Path to a TOML config file");
    app.add_option("--core", args.cpu_core, "CPU core to pin the hot thread to (-1 = none)");
    app.add_flag("-v,--verbose", args.verbosity, "Increase logging verbosity (repeatable)");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // Prints help / error and returns an exit code; mirrors the CLI11_PARSE macro.
        std::exit(app.exit(e));
    }
    return args;
}

}  // namespace hft::appkit
