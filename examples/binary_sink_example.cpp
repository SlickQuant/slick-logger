// Binary sink example
//
// Captures raw, application-encoded records into a binary file while a text sink
// keeps a readable trail of the same run. Shows:
//   - BinarySink writing payloads byte for byte, with no framing of its own
//   - as_binary() over a POD, a std::vector and a std::array
//   - LOG_SINK_* macros skipping argument evaluation for filtered-out calls
//   - a subclass adding its own framing through write_payload()

#include <slick/logger.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace slick::logger;

#pragma pack(push, 1)
struct Trade {
    uint64_t timestamp_ns;
    uint32_t quantity;
    double price;
    char symbol[8];
};
#pragma pack(pop)

// BinarySink writes payloads with no separators, so a reader needs framing that
// the application defines. Here every record gets a 2-byte little-endian length.
class LengthPrefixedBinarySink : public BinarySink {
public:
    using BinarySink::BinarySink;

protected:
    void write_payload(const std::byte* data, size_t size) override {
        const auto length = static_cast<uint16_t>(size);
        BinarySink::write_payload(reinterpret_cast<const std::byte*>(&length), sizeof(length));
        BinarySink::write_payload(data, size);
    }
};

int main() {
    auto& logger = Logger::instance();

    // Raw capture: exactly the bytes we hand it, nothing else.
    auto capture = logger.add_binary_sink("trades.bin", "capture");

    // Framed capture, for a file that can be walked record by record.
    auto framed = std::make_shared<LengthPrefixedBinarySink>("trades_framed.bin", "framed");
    logger.add_sink(framed);

    // A readable trail alongside it. Binary payloads render as hex here.
    logger.add_console_sink();

    logger.init(1 << 16);

    const Trade trades[] = {
        {1'725'000'000'000'000'000ULL, 100, 431.25, {'A', 'A', 'P', 'L', 0, 0, 0, 0}},
        {1'725'000'000'000'000'500ULL, 250, 118.40, {'M', 'S', 'F', 'T', 0, 0, 0, 0}},
    };

    for (const auto& trade : trades) {
        // The same call feeds both binary sinks; each writes its own file.
        LOG_SINK_INFO(capture, "{}", as_binary(&trade, sizeof(trade)));
        LOG_SINK_INFO(framed, "{}", as_binary(&trade, sizeof(trade)));
        // ... and the console sink renders it as hex.
        LOG_INFO("trade {} qty={} px={:.2f}", trade.symbol, trade.quantity, trade.price);
    }

    // Contiguous ranges work directly - no pointer/size arithmetic at the call site.
    const std::vector<uint8_t> heartbeat{0xFE, 0xED, 0x00, 0x0A};
    const std::array<uint32_t, 3> counters{7, 8, 9};
    LOG_SINK_INFO(capture, "{}", as_binary(heartbeat));
    LOG_SINK_INFO(capture, "{}", as_binary(counters));

    // Raising a sink's minimum level also filters entries that are already queued
    // but not yet drained - the writer thread re-checks the level when it
    // dispatches. Flush first so the records logged above are safely on disk.
    logger.flush();

    // Filtered out before the argument is touched: raising the sink's minimum
    // level means encode_snapshot() is never called.
    capture->set_min_level(LogLevel::L_WARN);
    int encode_calls = 0;
    auto encode_snapshot = [&]() {
        ++encode_calls;
        return as_binary(heartbeat);
    };
    LOG_SINK_INFO(capture, "{}", encode_snapshot());   // skipped entirely
    LOG_SINK_ERROR(capture, "{}", encode_snapshot());  // written

    logger.shutdown();

    const size_t expected_raw = sizeof(trades) + heartbeat.size() + sizeof(counters)
                              + heartbeat.size();
    std::cout << "\nencode_snapshot() calls: " << encode_calls
              << " (1 = the INFO call never evaluated its argument)\n"
              << "trades.bin        : " << std::filesystem::file_size("trades.bin")
              << " bytes (expected " << expected_raw << ")\n"
              << "trades_framed.bin : " << std::filesystem::file_size("trades_framed.bin")
              << " bytes (expected " << sizeof(trades) + 2 * sizeof(uint16_t) << ")\n";

    // Walk the framed file back, proving the round trip.
    std::ifstream in("trades_framed.bin", std::ios::binary);
    size_t record = 0;
    uint16_t length = 0;
    while (in.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        std::vector<char> buffer(length);
        if (!in.read(buffer.data(), length)) {
            break;
        }
        Trade decoded{};
        std::memcpy(&decoded, buffer.data(), std::min<size_t>(length, sizeof(decoded)));
        std::cout << "record " << record++ << ": " << decoded.symbol
                  << " qty=" << decoded.quantity << " px=" << decoded.price << '\n';
    }

    return 0;
}
