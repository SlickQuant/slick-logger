#include <slick/logger.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

using slick::logger::detail::producer_ring;
using slick::logger::detail::ring_reader;

namespace {

using payload_t = std::optional<std::span<const uint8_t>>;

// Byte i holds the low byte of (seq + i), so a record read back from the wrong
// place, cut short or overwritten won't match.
void fill(uint8_t* at, uint32_t size, uint64_t seq) {
    for (uint32_t i = 0; i < size; ++i) {
        at[i] = static_cast<uint8_t>(seq + i);
    }
}

// The bytes a record of this payload occupies, before any end-of-lap skip.
uint64_t footprint(uint32_t payload) {
    return (uint64_t{producer_ring::kRecordHeader} + payload + 7) & ~uint64_t{7};
}

// Payloads come back rounded up to the record alignment, so compare the padded size.
bool matches(const payload_t& payload, uint32_t size, uint64_t seq) {
    if (!payload || payload->size() != footprint(size) - producer_ring::kRecordHeader) {
        return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
        if ((*payload)[i] != static_cast<uint8_t>(seq + i)) {
            return false;
        }
    }
    return true;
}

// Gives the record up, and counts it, when the ring is full.
bool write_record(producer_ring& ring, uint32_t size, uint64_t seq) {
    uint8_t* at = ring.try_reserve(size);
    if (!at) {
        ring.drop();
        return false;
    }
    fill(at, size, seq);
    ring.publish();
    return true;
}

// Pops everything published and hands the space back.
uint64_t drain(ring_reader& reader) {
    uint64_t count = 0;
    reader.refresh();
    while (reader.peek()) {
        reader.pop();
        ++count;
    }
    reader.release();
    return count;
}

} // namespace

TEST(ProducerRing, RoundTripsRecordsInOrderAcrossManyLaps) {
    auto ring = producer_ring::allocate(4096);
    ring_reader reader(*ring);
    uint64_t written = 0;
    uint64_t read = 0;
    const auto size_of = [](uint64_t seq) { return static_cast<uint32_t>((seq * 37) % 300); };
    for (int batch = 0; batch < 500; ++batch) {
        // A batch of mixed sizes that always fits, read back and released in one go.
        for (int i = 0; i < 12; ++i, ++written) {
            ASSERT_TRUE(write_record(*ring, size_of(written), written));
        }
        ASSERT_TRUE(reader.refresh());
        while (const auto payload = reader.peek()) {
            ASSERT_TRUE(matches(payload, size_of(read), read)) << "record " << read;
            reader.pop();
            ++read;
        }
        EXPECT_TRUE(reader.batch_done());
        reader.release();
    }
    EXPECT_EQ(read, written);
    EXPECT_GT(ring->written(), 10 * ring->capacity());
    EXPECT_EQ(ring->drops(), 0u);
    EXPECT_EQ(reader.corrupt(), 0u);
}

TEST(ProducerRing, EmptyPayloadIsStillARecord) {
    auto ring = producer_ring::allocate(64);
    ring_reader reader(*ring);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(write_record(*ring, 0, 0));
        ASSERT_TRUE(reader.refresh());
        const auto payload = reader.peek();
        ASSERT_TRUE(payload.has_value());
        EXPECT_TRUE(payload->empty());
        reader.pop();
        EXPECT_FALSE(reader.peek());
        reader.release();
    }
}

TEST(ProducerRing, WrapsCleanlyFromEveryOffset) {
    // Tries every record size at every start offset in a 256-byte ring. A record
    // that doesn't fit before the end of the lap must skip to offset 0 and arrive
    // intact. One that fits must not skip. Either way the next record still works.
    constexpr uint64_t kCapacity = 256;
    const uint32_t max = producer_ring::allocate(kCapacity)->max_payload();
    int wrapped = 0;
    for (uint64_t offset = 0; offset < kCapacity; offset += producer_ring::kRecordAlign) {
        for (uint32_t size = 0; size <= max; ++size) {
            auto ring = producer_ring::allocate(kCapacity);
            ring_reader reader(*ring);
            // Fill up to exactly the offset, then read and release the fillers.
            for (uint64_t at = 0; at < offset;) {
                const uint64_t step = std::min<uint64_t>(offset - at, footprint(max));
                ASSERT_TRUE(write_record(*ring, static_cast<uint32_t>(step - producer_ring::kRecordHeader), 0));
                at += step;
            }
            drain(reader);
            ASSERT_EQ(ring->written(), offset);

            const uint64_t seq = offset * 1000 + size;
            ASSERT_TRUE(write_record(*ring, size, seq));
            const bool fits = footprint(size) <= kCapacity - offset;
            const uint64_t skipped = fits ? 0 : kCapacity - offset;
            ASSERT_EQ(ring->written(), offset + skipped + footprint(size))
                << "size " << size << " at offset " << offset;
            wrapped += fits ? 0 : 1;

            ASSERT_TRUE(reader.refresh());
            ASSERT_TRUE(matches(reader.peek(), size, seq)) << "size " << size << " at offset " << offset;
            reader.pop();
            EXPECT_FALSE(reader.peek());
            reader.release();

            ASSERT_TRUE(write_record(*ring, 16, seq + 1));
            ASSERT_TRUE(reader.refresh());
            ASSERT_TRUE(matches(reader.peek(), 16, seq + 1)) << "after size " << size << " at offset " << offset;
            EXPECT_EQ(reader.corrupt(), 0u);
        }
    }
    EXPECT_GT(wrapped, 0);
}

TEST(ProducerRing, RecordExactlyFillingTheLapNeedsNoSkip) {
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    const uint32_t half = ring->max_payload(); // a record of exactly 128 bytes
    ASSERT_EQ(footprint(half), 128u);
    ASSERT_TRUE(write_record(*ring, half, 1));
    ASSERT_TRUE(write_record(*ring, half, 2));
    EXPECT_EQ(ring->written(), 256u); // the second one ends exactly at the end of the lap
    ASSERT_TRUE(reader.refresh());
    for (uint64_t seq : {1u, 2u}) {
        ASSERT_TRUE(matches(reader.peek(), half, seq));
        reader.pop();
    }
    reader.release();
    ASSERT_TRUE(write_record(*ring, half, 3));
    EXPECT_EQ(ring->written(), 384u); // and the next one starts the new lap at offset 0
    ASSERT_TRUE(reader.refresh());
    EXPECT_TRUE(matches(reader.peek(), half, 3));
}

TEST(ProducerRing, FullRingRefusesTheNewestAndKeepsTheOldest) {
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    uint64_t seq = 0;
    while (write_record(*ring, 24, seq)) { // 32-byte records
        ++seq;
    }
    EXPECT_EQ(seq, 8u);
    EXPECT_EQ(ring->drops(), 1u);
    EXPECT_FALSE(write_record(*ring, 0, 99));
    EXPECT_EQ(ring->drops(), 2u);

    ASSERT_TRUE(reader.refresh());
    for (uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(matches(reader.peek(), 24, i)) << "record " << i;
        reader.pop();
    }
    EXPECT_FALSE(reader.peek());
    reader.release();
    EXPECT_TRUE(write_record(*ring, 24, 8)); // the released space is reusable
    EXPECT_EQ(ring->drops(), 2u);
}

TEST(ProducerRing, NothingIsVisibleBeforePublish) {
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    uint8_t* at = ring->try_reserve(16);
    ASSERT_NE(at, nullptr);
    fill(at, 16, 7);
    EXPECT_FALSE(reader.refresh());
    EXPECT_FALSE(reader.peek());
    ring->publish();
    ASSERT_TRUE(reader.refresh());
    EXPECT_TRUE(matches(reader.peek(), 16, 7));
}

TEST(ProducerRing, PoppedRecordsStayIntactUntilReleased) {
    // A writer formats from a record after popping it, so popped space must not
    // go back to the producer until release().
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    for (uint64_t seq = 0; seq < 8; ++seq) {
        ASSERT_TRUE(write_record(*ring, 24, seq));
    }
    ASSERT_TRUE(reader.refresh());
    std::vector<payload_t> held;
    while (auto payload = reader.peek()) {
        held.push_back(payload);
        reader.pop();
    }
    ASSERT_EQ(held.size(), 8u);
    EXPECT_FALSE(write_record(*ring, 24, 100)); // still full
    for (uint64_t seq = 0; seq < 8; ++seq) {
        EXPECT_TRUE(matches(held[seq], 24, seq)) << "record " << seq;
    }
    reader.release();
    EXPECT_TRUE(write_record(*ring, 24, 100));
}

TEST(ProducerRing, AnImpossibleSizeEndsTheBatchInsteadOfBeingBelieved) {
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    ASSERT_TRUE(write_record(*ring, 16, 1));
    uint8_t* at = ring->try_reserve(16);
    ASSERT_NE(at, nullptr);
    const uint32_t garbage = 0x12345; // neither a multiple of 8 nor inside the ring
    std::memcpy(at - producer_ring::kRecordHeader, &garbage, sizeof(garbage));
    ring->publish();
    ASSERT_TRUE(reader.refresh());
    EXPECT_TRUE(matches(reader.peek(), 16, 1)); // the good record before it still reads
    reader.pop();
    EXPECT_FALSE(reader.peek());
    EXPECT_EQ(reader.corrupt(), 1u);
    EXPECT_TRUE(reader.batch_done());
    reader.release();
    // Reading resumes at the write position.
    ASSERT_TRUE(write_record(*ring, 16, 5));
    ASSERT_TRUE(reader.refresh());
    EXPECT_TRUE(matches(reader.peek(), 16, 5));
    EXPECT_EQ(reader.corrupt(), 1u);
}

TEST(ProducerRing, WorksWhereverItIsMapped) {
    // No pointers inside the block: a byte copy at another address reads the same.
    constexpr uint64_t kCapacity = 256;
    alignas(64) static unsigned char first[producer_ring::block_size(kCapacity)];
    alignas(64) static unsigned char second[producer_ring::block_size(kCapacity)];
    producer_ring* ring = producer_ring::create(first, kCapacity);
    for (uint64_t seq = 0; seq < 5; ++seq) {
        ASSERT_TRUE(write_record(*ring, 20, seq));
    }
    std::memcpy(second, first, sizeof(first));
    std::memset(first, 0, sizeof(first));
    auto* moved = std::launder(reinterpret_cast<producer_ring*>(second));
    EXPECT_EQ(moved->capacity(), kCapacity);
    ring_reader reader(*moved);
    ASSERT_TRUE(reader.refresh());
    for (uint64_t seq = 0; seq < 5; ++seq) {
        ASSERT_TRUE(matches(reader.peek(), 20, seq)) << "record " << seq;
        reader.pop();
    }
    EXPECT_FALSE(reader.peek());
    reader.release();
    EXPECT_TRUE(write_record(*moved, 20, 5)); // the producer side works from the copy too
}

TEST(ProducerRing, CountsRecordsPublishedAndReleased) {
    auto ring = producer_ring::allocate(256);
    ring_reader reader(*ring);
    for (uint64_t seq = 0; seq < 5; ++seq) {
        ASSERT_TRUE(write_record(*ring, 40, seq)); // 48 bytes each, 240 in all
    }
    EXPECT_FALSE(write_record(*ring, 16, 9)); // a drop is not a record
    EXPECT_EQ(ring->records(), 5u);
    ASSERT_TRUE(reader.refresh());
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(reader.peek());
        reader.pop();
    }
    EXPECT_EQ(ring->released_records(), 0u); // popped is not released
    reader.release();
    EXPECT_EQ(ring->released_records(), 2u);
    // A reader that picks the ring up later carries the count on.
    ring_reader later(*ring);
    ASSERT_TRUE(later.refresh());
    while (later.peek()) {
        later.pop();
    }
    later.release();
    EXPECT_EQ(ring->released_records(), 5u);
}

TEST(ProducerRing, ConcurrentProducerAndReaderNeverSeeABadRecord) {
    // The ring is small, so the producer laps it thousands of times and often
    // finds it full. Refused records are retried here, not dropped, so the reader
    // must see every sequence number in order with every byte intact.
    constexpr uint64_t kRecords = 200'000;
    auto ring = producer_ring::allocate(4096);
    const uint32_t max = ring->max_payload();
    const auto size_of = [max](uint64_t seq) {
        return static_cast<uint32_t>((seq * 2654435761u) % (max + 1));
    };
    std::thread producer([&] {
        for (uint64_t seq = 0; seq < kRecords; ++seq) {
            while (!write_record(*ring, size_of(seq), seq)) {
                std::this_thread::yield();
            }
        }
    });
    ring_reader reader(*ring);
    uint64_t expected = 0;
    uint64_t bad = 0;
    while (expected < kRecords) {
        if (!reader.refresh()) {
            std::this_thread::yield();
            continue;
        }
        while (const auto payload = reader.peek()) {
            if (!matches(payload, size_of(expected), expected)) {
                ++bad;
            }
            reader.pop();
            ++expected;
        }
        reader.release();
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(expected, kRecords);
    EXPECT_EQ(reader.corrupt(), 0u);
    EXPECT_GT(ring->drops(), 0u) << "the ring never filled, so the full path went untested";
}

namespace {

/// @p size bytes followed by an inaccessible page, so reading one byte past
/// them faults, as reading past the end of a producer's mapping would.
class guarded_block {
public:
    explicit guarded_block(size_t size) {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
#else
        page_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
        span_ = (size + page_ - 1) / page_ * page_;
#ifdef _WIN32
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, span_ + page_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old_protection = 0;
        guarded_ = base_ && VirtualProtect(base_ + span_, page_, PAGE_NOACCESS, &old_protection);
#else
        void* const mapping = mmap(nullptr, span_ + page_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = mapping == MAP_FAILED ? nullptr : static_cast<uint8_t*>(mapping);
        guarded_ = base_ && mprotect(base_ + span_, page_, PROT_NONE) == 0;
#endif
        data_ = base_ ? base_ + span_ - size : nullptr;
    }
    ~guarded_block() {
        if (base_) {
#ifdef _WIN32
            VirtualFree(base_, 0, MEM_RELEASE);
#else
            munmap(base_, span_ + page_);
#endif
        }
    }
    guarded_block(const guarded_block&) = delete;
    guarded_block& operator=(const guarded_block&) = delete;

    uint8_t* data() const noexcept { return data_; }
    bool guarded() const noexcept { return guarded_; }

private:
    size_t page_ = 0;
    size_t span_ = 0;
    uint8_t* base_ = nullptr;
    uint8_t* data_ = nullptr;
    bool guarded_ = false;
};

} // namespace

// A record too short to be an entry can end where the ring does, and the memory
// after the ring can be unmapped (a producer's mapping ends there). The writer
// must not read a timestamp out of it.
TEST(RingView, RecordTooShortForAnEntryIsNotReadPastItsEnd) {
    constexpr uint64_t kCapacity = 4096;
    guarded_block block(producer_ring::block_size(kCapacity));
    ASSERT_TRUE(block.guarded());
    producer_ring* ring = producer_ring::create(block.data(), kCapacity);
    // 16-byte records fill the ring exactly, so the last one ends at the guard page.
    constexpr uint32_t kPayload = 8;
    const uint64_t records = kCapacity / footprint(kPayload);
    for (uint64_t i = 0; i < records; ++i) {
        uint8_t* const payload = ring->try_reserve(kPayload);
        ASSERT_NE(payload, nullptr);
        fill(payload, kPayload, i);
        ring->publish();
    }

    slick::logger::detail::ring_view view;
    view.ring = ring;
    view.reader = ring_reader(*ring);
    ASSERT_TRUE(view.reader.refresh());
    uint64_t held = 0;
    while (view.hold_next()) {
        EXPECT_EQ(view.head_timestamp, 0u) << "a timestamp read from a record too short to hold one";
        ++held;
        view.reader.pop();
        if (view.reader.batch_done()) {
            break;
        }
    }
    EXPECT_EQ(held, records);
}
