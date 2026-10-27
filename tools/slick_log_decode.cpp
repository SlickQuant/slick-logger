// slick_log_decode - render a file written by EncodedFileSink as text.
//
// Each entry is written exactly as FileSink would have written it, to a file or
// to stdout. Built only when BUILD_SLICK_LOGGER_DECODER=ON.

#include <slick/logger.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

int usage() {
    std::fprintf(stderr,
        "slick_log_decode - render a file written by EncodedFileSink as text\n"
        "\n"
        "usage: slick_log_decode <file> [-o <path>] [--pattern <pattern>]\n"
        "\n"
        "  -o <path>             write to <path>, replacing it (default: stdout)\n"
        // Percent signs are doubled: this whole block is a printf format string.
        "  --pattern <pattern>   line layout, spdlog style, e.g. \"%%T.%%e %%l %%v\".\n"
        "                        Default: the layout FileSink writes.\n"
        "\n"
        "exit status: 0 read it all, 1 stopped at a record it could not read (a file\n"
        "cut short by a crash; everything before it is written), 2 bad arguments or\n"
        "a file it cannot read at all\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    std::string pattern;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return usage();
        } else if (arg == "-o" || arg == "--pattern") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n\n", arg.c_str());
                return usage();
            }
            (arg == "-o" ? output : pattern) = argv[++i];
        } else if (input.empty() && !arg.starts_with('-')) {
            input = arg;
        } else {
            std::fprintf(stderr, "unexpected argument %s\n\n", arg.c_str());
            return usage();
        }
    }
    if (input.empty()) {
        return usage();
    }

    try {
        slick::logger::EncodedLogReader reader(input);
        std::shared_ptr<slick::logger::ISink> sink;
        if (output.empty()) {
            std::ios::sync_with_stdio(false);
            sink = std::make_shared<slick::logger::ConsoleSink>(false, false);
        } else {
            std::error_code ec;
            if (std::filesystem::equivalent(input, output, ec)) {
                std::fprintf(stderr, "slick_log_decode: -o names the input file\n");
                return 2;
            }
            std::filesystem::remove(output, ec); // FileSink appends
            sink = std::make_shared<slick::logger::FileSink>(output);
        }
        if (!pattern.empty()) {
            sink->set_pattern(pattern);
        }
        while (const slick::logger::LogEntry* entry = reader.next()) {
            sink->write(*entry);
        }
        sink->flush();
        if (!reader.error().empty()) {
            std::fprintf(stderr, "slick_log_decode: %s: %s\n", input.c_str(), reader.error().c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "slick_log_decode: %s\n", e.what());
        return 2;
    }
    return 0;
}
