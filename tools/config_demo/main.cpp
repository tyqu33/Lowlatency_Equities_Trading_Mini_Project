// tools/config_demo/main.cpp
// Smoke test for the "buy, don't build" tooling: CLI11 (args) + toml++ (config) + fmt (output).
// NOT business logic — it just proves the three libraries are wired and working end-to-end.
//
// Run:  hft_config_demo --config config/venue.toml -v

#include <fmt/color.h>
#include <fmt/core.h>

#include <cstdint>
#include <string>

#include "hft/appkit/cli.hpp"
#include "hft/appkit/config.hpp"

int main(int argc, char** argv) {
    const hft::appkit::CommonArgs args = hft::appkit::parse_common_args(
        argc, argv, "config_demo", "Parse CLI args, load a TOML config, print with fmt.");

    fmt::print(fmt::emphasis::bold, "config_demo\n");
    fmt::print("  config path : {}\n", args.config_path.empty() ? "(none)" : args.config_path);
    fmt::print("  cpu core    : {}\n", args.cpu_core);
    fmt::print("  verbosity   : {}\n", args.verbosity);

    if (args.config_path.empty()) {
        fmt::print("No --config given; try: --config config/venue.toml\n");
        return 0;
    }

    try {
        const toml::table cfg = hft::appkit::load_config(args.config_path);
        // Read a couple of well-known keys if present (matches the example config/venue.toml).
        if (const auto name = cfg["venue"]["name"].value<std::string>()) {
            fmt::print("  venue.name  : {}\n", *name);
        }
        if (const auto core = cfg["cpu"]["me_core"].value<std::int64_t>()) {
            fmt::print("  cpu.me_core : {}\n", *core);
        }
        fmt::print(fmt::fg(fmt::color::green), "TOML parsed OK ({} top-level entries)\n",
                   cfg.size());
    } catch (const toml::parse_error& e) {
        fmt::print(stderr, "TOML parse error: {}\n", e.description());
        return 1;
    }
    return 0;
}
