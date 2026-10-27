// slick_log_decode - render files written by EncodedFileSink as text.
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
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
        "slick_log_decode - render files written by EncodedFileSink as text\n"
        "\n"
        "usage: slick_log_decode <file>... [-o <path>] [--pattern <pattern>]\n"
        "\n"
        "Files are rendered in the order given, so list rotated ones oldest first,\n"
        "e.g. app_2.slog app_1.slog app.slog.\n"
        "\n"
        "  -o <path>             write to <path>, replacing it (default: stdout)\n"
        // Percent signs are doubled: this whole block is a printf format string.
        "  --pattern <pattern>   line layout, spdlog style, e.g. \"%%T.%%e %%l %%v\".\n"
        "                        Default: the layout FileSink writes.\n"
        "\n"
        "exit status: 0 read it all, 1 a file stopped at a record it could not read\n"
        "(cut short by a crash; everything before that record and every later file\n"
        "is still written), 2 bad arguments or a file it cannot read at all\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> inputs;
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
        } else if (!arg.starts_with('-')) {
            inputs.push_back(arg);
        } else {
            std::fprintf(stderr, "unexpected argument %s\n\n", arg.c_str());
            return usage();
        }
    }
    if (inputs.empty()) {
        return usage();
    }

    int status = 0;
    try {
        std::shared_ptr<slick::logger::ISink> sink;
        if (output.empty()) {
            std::ios::sync_with_stdio(false);
            sink = std::make_shared<slick::logger::ConsoleSink>(false, false);
        } else {
            std::error_code ec;
            for (const std::string& input : inputs) {
                if (std::filesystem::equivalent(input, output, ec)) {
                    std::fprintf(stderr, "slick_log_decode: -o names an input file\n");
                    return 2;
                }
            }
            std::filesystem::remove(output, ec); // FileSink appends
            sink = std::make_shared<slick::logger::FileSink>(output);
        }
        if (!pattern.empty()) {
            sink->set_pattern(pattern);
        }
        for (const std::string& input : inputs) {
            slick::logger::EncodedLogReader reader(input);
            while (const slick::logger::LogEntry* entry = reader.next()) {
                sink->write(*entry);
            }
            if (!reader.error().empty()) {
                std::fprintf(stderr, "slick_log_decode: %s: %s\n", input.c_str(), reader.error().c_str());
                status = 1;
            }
        }
        sink->flush();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "slick_log_decode: %s\n", e.what());
        return 2;
    }
    return status;
}
