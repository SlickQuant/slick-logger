// EncodedFileSink, EncodedLogReader and slick_log_decode: entries written in
// binary must come back rendering exactly as FileSink wrote them.

#include <slick/logger.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "run_process.hpp"

// Resolved at cmake configure time; the build system defines this macro.
#ifndef DECODE_EXE_PATH
#  error "DECODE_EXE_PATH must be defined by CMake"
#endif

using slick::logger::EncodedFileSink;
using slick::logger::EncodedLogReader;
using slick::logger::FileSink;
using slick::logger::LogConfig;
using slick::logger::LogEntry;
using slick::logger::Logger;
using slick::logger::LogLevel;
using slick::logger::StringRef;

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// Render @p encoded into a FileSink at @p text, as slick_log_decode -o does,
/// returning the reader's error.
std::string decode_to(const std::filesystem::path& encoded, const std::filesystem::path& text) {
    std::filesystem::remove(text);
    EncodedLogReader reader(encoded);
    FileSink sink(text);
    while (const LogEntry* entry = reader.next()) {
        sink.write(*entry);
    }
    sink.flush();
    return reader.error();
}

/// Every decoded entry's message.
std::vector<std::string> decoded_messages(const std::filesystem::path& encoded, std::string* error = nullptr) {
    std::vector<std::string> messages;
    EncodedLogReader reader(encoded);
    while (const LogEntry* entry = reader.next()) {
        std::string message;
        slick::logger::append_formatted_message(message, *entry);
        messages.push_back(std::move(message));
    }
    if (error) {
        *error = reader.error();
    }
    return messages;
}

/// One of each argument type, spec and path an entry can take.
void log_every_kind() {
    const std::string text = "a std::string";
    const std::string_view view = "a string_view";
    const std::string empty;
    const std::string long_text(3000, 'x');
    const char* c_string = "a const char*";
    char array[] = "a char array";
    const unsigned char bytes[] = {0x00, 0x0A, 0xFF, 0x41};
    int value = 42;
    LOG_TRACE("trace, no arguments");
    LOG_DEBUG("bool {} {} char {} uchar {}", true, false, 'c', static_cast<unsigned char>(200));
    LOG_INFO("int8 {} int16 {} int32 {} int64 {}", int8_t{-8}, int16_t{-16}, int32_t{-32}, int64_t{-64});
    LOG_INFO("uint8 {} uint16 {} uint32 {} uint64 {}", uint8_t{8}, uint16_t{16}, uint32_t{32}, UINT64_MAX);
    LOG_WARN("float {} double {} long double {} specs {:>10.3f}|{:08.2f}", 1.5f, -2.25, 3.125L, 3.14159, 2.5);
    LOG_ERROR("hex {:x} {:#X} {:08b}", 255, 3054, 5);
    LOG_INFO("positional {1} {0} {1}", "a", "b");
    LOG_FATAL("strings {} {} {} {} [{}] {}", text, view, c_string, array, empty, long_text);
    LOG_INFO("pointer {} null {}", static_cast<const void*>(&value), static_cast<const void*>(nullptr));
    LOG_INFO("wide [{}] [{:d}] [{}]", L'A', L'é', L'中');
    LOG_INFO("blob {} {:X}", slick::logger::as_binary(bytes, sizeof(bytes)), slick::logger::as_binary(bytes, 2));
    LOG_INFO("missing {} {}", 1);
    LOG_INFO("{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
             13, 14, 15, 16, 17, 18, 19, 20);
    const std::string runtime_format = "runtime format {} {}";
    LOG_INFO(runtime_format, 7, "eight");
    Logger::instance().log_with_location(LogLevel::L_INFO, "/some/dir/runtime_path.cpp", 99, "runtime location {}", 1);
    Logger::instance().set_source_location_enabled(false);
    LOG_INFO("no location {}", 2);
    Logger::instance().set_source_location_enabled(true);
    LOG_INFO("two\nlines");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i) {
                LOG_INFO("thread {} line {} {}", t, i, i * 0.5);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

/// An entry written straight to a sink, with its format and file at given addresses.
LogEntry make_entry(const char* format, const char* file) {
    LogEntry entry{};
    entry.level = LogLevel::L_INFO;
    entry.flags = slick::logger::kEntryStaticFormat | slick::logger::kEntryHasSourceLocation;
    entry.line = 7;
    entry.format = StringRef{format, static_cast<uint32_t>(std::strlen(format))};
    entry.file = StringRef{file, static_cast<uint32_t>(std::strlen(file))};
    return entry;
}

class EncodedSinkTest : public ::testing::Test {
protected:
    void TearDown() override {
        Logger::instance().shutdown();
        Logger::instance().reset();
        for (const auto& path : temp_files_) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    std::filesystem::path temp(const std::string& name) {
        std::filesystem::path path = name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        temp_files_.push_back(path);
        return path;
    }

    /// Log @p body to a FileSink at @p text and an EncodedFileSink at @p encoded.
    template<typename Body>
    void log_to_both(const std::filesystem::path& text, const std::filesystem::path& encoded, Body body,
                     bool rings = true, size_t text_table_limit = 0) {
        auto encoded_sink = std::make_shared<EncodedFileSink>(encoded);
        if (text_table_limit) {
            encoded_sink->set_text_table_limit(text_table_limit);
        }
        LogConfig config;
        config.sinks.push_back(std::make_shared<FileSink>(text));
        config.sinks.push_back(encoded_sink);
        config.thread_rings = rings;
        Logger::instance().init(config);
        body();
        Logger::instance().shutdown();
        Logger::instance().reset();
    }

    std::vector<std::filesystem::path> temp_files_;
};

} // namespace

TEST_F(EncodedSinkTest, RendersExactlyAsFileSink) {
    for (const bool rings : {true, false}) {
        const auto text = temp("enc_text.log");
        const auto encoded = temp("enc_every.slog");
        const auto decoded = temp("enc_decoded.log");
        log_to_both(text, encoded, log_every_kind, rings);

        ASSERT_EQ(decode_to(encoded, decoded), "") << "rings " << rings;
        const std::string expected = read_all(text);
        EXPECT_GT(std::count(expected.begin(), expected.end(), '\n'), 400) << "rings " << rings;
        EXPECT_EQ(read_all(decoded), expected) << "rings " << rings;
        EXPECT_LT(std::filesystem::file_size(encoded), expected.size()) << "rings " << rings;
    }
}

// A plugin unloaded and another loaded in its place can put new text at an old
// literal's address: the id must follow the text, not the address.
TEST_F(EncodedSinkTest, NewTextAtAKnownAddressGetsItsOwnId) {
    const auto encoded = temp("enc_reuse.slog");
    char format[16];
    char file[16];
    {
        EncodedFileSink sink(encoded);
        const auto write = [&](const char* f, const char* name) {
            std::strcpy(format, f);
            std::strcpy(file, name);
            sink.write(make_entry(format, file));
        };
        write("first", "one.cpp");
        write("first", "one.cpp");
        write("other", "two.cpp"); // same sizes, same addresses
        write("first", "one.cpp");
    }
    std::vector<std::string> seen;
    EncodedLogReader reader(encoded);
    while (const LogEntry* entry = reader.next()) {
        seen.push_back(std::string(slick::logger::view_string_ref(entry->format)) + "@" +
                       std::string(slick::logger::view_string_ref(entry->file)));
    }
    EXPECT_EQ(reader.error(), "");
    EXPECT_EQ(seen, (std::vector<std::string>{"first@one.cpp", "first@one.cpp", "other@two.cpp", "first@one.cpp"}));
}

// A collector's formats arrive copied, at a new address each time: copies of a
// text share its id, so the file is no bigger than with one address.
TEST_F(EncodedSinkTest, CopiesOfATextShareItsId) {
    const auto copied = temp("enc_copied.slog");
    const auto fixed = temp("enc_fixed.slog");
    constexpr int kEntries = 50;
    {
        EncodedFileSink sink(copied);
        std::vector<std::string> formats(kEntries, "copied format {}");
        std::vector<std::string> files(kEntries, "copied.cpp");
        for (int i = 0; i < kEntries; ++i) {
            sink.write(make_entry(formats[i].c_str(), files[i].c_str()));
        }
    }
    {
        EncodedFileSink sink(fixed);
        for (int i = 0; i < kEntries; ++i) {
            sink.write(make_entry("copied format {}", "copied.cpp"));
        }
    }
    EXPECT_EQ(std::filesystem::file_size(copied), std::filesystem::file_size(fixed));
    EXPECT_EQ(decoded_messages(copied), decoded_messages(fixed));
}

// Each sink appending to the file starts a session numbering its texts from 1.
TEST_F(EncodedSinkTest, SessionsAppendedToOneFileReadInOrder) {
    const auto encoded = temp("enc_sessions.slog");
    for (int session = 0; session < 2; ++session) {
        LogConfig config;
        config.sinks.push_back(std::make_shared<EncodedFileSink>(encoded));
        Logger::instance().init(config);
        if (session == 0) {
            LOG_INFO("session {} only in the first", session);
        } else {
            LOG_WARN("session {} only in the second", session);
        }
        LOG_INFO("session {} in both", session);
        Logger::instance().shutdown();
        Logger::instance().reset();
    }
    std::string error;
    std::vector<std::string> messages;
    for (const auto& message : decoded_messages(encoded, &error)) {
        if (message.starts_with("session")) {
            messages.push_back(message);
        }
    }
    EXPECT_EQ(error, "");
    EXPECT_EQ(messages, (std::vector<std::string>{"session 0 only in the first", "session 0 in both",
                                                  "session 1 only in the second", "session 1 in both"}));
}

// A crash can cut the last record short: every entry before it still reads.
TEST_F(EncodedSinkTest, FileCutShortKeepsEveryCompleteEntry) {
    const auto encoded = temp("enc_cut.slog");
    {
        EncodedFileSink sink(encoded);
        for (int i = 0; i < 10; ++i) {
            LogEntry entry = make_entry("entry {}", "cut.cpp");
            entry.arg_count = 1;
            entry.args[0].type = slick::logger::ArgType::INT32_T;
            entry.args[0].value.i32 = i;
            sink.write(entry);
        }
    }
    std::filesystem::resize_file(encoded, std::filesystem::file_size(encoded) - 3);
    std::string error;
    const auto messages = decoded_messages(encoded, &error);
    ASSERT_EQ(messages.size(), 9u);
    EXPECT_EQ(messages.front(), "entry 0");
    EXPECT_EQ(messages.back(), "entry 8");
    EXPECT_NE(error.find("cut short"), std::string::npos) << error;
}

TEST_F(EncodedSinkTest, RefusesAFileItCannotRead) {
    const auto text = temp("enc_not_encoded.log");
    std::ofstream(text) << "a text log line\n";
    EXPECT_THROW(EncodedLogReader{text}, std::runtime_error);

    const auto newer = temp("enc_newer.slog");
    {
        std::ofstream out(newer, std::ios::binary);
        const uint16_t version = 99;
        const uint16_t reserved = 0;
        const uint32_t byte_order = 0x01020304;
        out.write("HSLICKLOG", 9);
        out.write(reinterpret_cast<const char*>(&version), 2);
        out.write(reinterpret_cast<const char*>(&reserved), 2);
        out.write(reinterpret_cast<const char*>(&byte_order), 4);
    }
    try {
        EncodedLogReader reader(newer);
        FAIL() << "read a file of a newer version";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("version 99"), std::string::npos) << e.what();
    }
}

// Past its limit the table takes no more text: what is new goes inline instead.
TEST_F(EncodedSinkTest, TextPastTheTableLimitGoesInline) {
    const auto text = temp("enc_full_text.log");
    const auto encoded = temp("enc_full.slog");
    const auto decoded = temp("enc_full_decoded.log");
    log_to_both(text, encoded, [] {
        for (int i = 0; i < 20; ++i) {
            const std::string format = "runtime format " + std::to_string(i) + " {}";
            LOG_INFO(format, i);
            LOG_INFO("literal {}", i);
        }
    }, true, 300);

    ASSERT_EQ(decode_to(encoded, decoded), "");
    EXPECT_EQ(read_all(decoded), read_all(text));
}

TEST_F(EncodedSinkTest, DecodeToolWritesWhatFileSinkWrote) {
    const auto text = temp("enc_tool_text.log");
    const auto encoded = temp("enc_tool.slog");
    const auto decoded = temp("enc_tool_decoded.log");
    log_to_both(text, encoded, log_every_kind);

    ASSERT_EQ(run_process(DECODE_EXE_PATH, encoded.string() + " -o " + decoded.string()), 0);
    EXPECT_EQ(read_all(decoded), read_all(text));

    // Cut short: everything before the cut is written, and the exit status says so.
    std::filesystem::resize_file(encoded, std::filesystem::file_size(encoded) - 3);
    EXPECT_EQ(run_process(DECODE_EXE_PATH, encoded.string() + " -o " + decoded.string()), 1);
    const std::string expected = read_all(text);
    const std::string partial = read_all(decoded);
    EXPECT_GT(partial.size(), expected.size() / 2);
    EXPECT_EQ(expected.compare(0, partial.size(), partial), 0) << "not a prefix of FileSink's file";
}
