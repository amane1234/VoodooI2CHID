// Unit tests for VoodooI2CHID/VoodooI2CHIDProtocol.hpp. Build and run with `make test` in this directory.
//
// GET_REPORT/SET_REPORT framing was rewritten in 851fcb3. The implementation before that (d8e8bf6) is kept
// below as a reference, and the new encoder must produce the same bytes on the wire for every case.

#include "VoodooI2CHIDProtocol.hpp"
#include "Reference.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    ++checks; \
    if (!(cond)) { \
        ++failures; \
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    ++checks; \
    const auto va = (a); const auto vb = (b); \
    if (!(va == vb)) { \
        ++failures; \
        std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                     static_cast<long long>(va), static_cast<long long>(vb)); \
    } \
} while (0)

static uint32_t rng_state = 0x9e3779b9u;
static uint32_t rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}


static VoodooI2CHIDDeviceHIDDescriptor random_descriptor() {
    VoodooI2CHIDDeviceHIDDescriptor d;
    uint8_t* p = reinterpret_cast<uint8_t*>(&d);
    for (size_t i = 0; i < sizeof(d); ++i)
        p[i] = static_cast<uint8_t>(rnd());
    return d;
}

// Frame a SET_REPORT the way VoodooI2CHIDDevice::setReportGated does
static std::vector<uint8_t> frame(const VoodooI2CHIDDeviceHIDDescriptor& d, bool feature, uint8_t report_id,
                                  const std::vector<uint8_t>& report, bool* ok = nullptr) {
    uint8_t header[i2chid::MaxReportCommandLength];
    const uint16_t header_length = i2chid::encodeReportCommand(header, d, i2chid::OpcodeSetReport,
                                                               feature ? i2chid::ReportTypeFeature : i2chid::ReportTypeOutput,
                                                               report_id);
    const uint32_t capacity = i2chid::setReportCapacity(header_length, report_id, static_cast<uint32_t>(report.size()));
    std::vector<uint8_t> storage(capacity, 0xCC);
    std::memcpy(storage.data() + (capacity - report.size()), report.data(), report.size());
    i2chid::WireSpan span = {};
    const bool framed = i2chid::frameSetReport(storage.data(), capacity, header, header_length,
                                               static_cast<uint32_t>(report.size()), report_id, span);
    if (ok)
        *ok = framed;
    if (!framed)
        return {};
    // The span must lie inside the buffer and end at its last byte
    CHECK(span.offset + span.length == capacity);
    return std::vector<uint8_t>(storage.begin() + span.offset, storage.begin() + span.offset + span.length);
}

static void test_set_report_matches_reference() {
    const uint8_t ids[] = { 0, 1, 2, 0x0E, 0x0F, 0x10, 0x7F, 0xFF };
    for (int round = 0; round < 4000; ++round) {
        const auto d = random_descriptor();
        const bool feature = rnd() & 1;
        const uint8_t report_id = (round & 1) ? ids[rnd() % 8] : static_cast<uint8_t>(rnd());
        size_t length = 1 + rnd() % 80;
        if (round % 97 == 0)
            length = 1;
        std::vector<uint8_t> report(length);
        for (auto& b : report)
            b = static_cast<uint8_t>(rnd());
        // Half the numbered reports already start with their ID, the other half do not
        if (report_id && (rnd() & 1))
            report[0] = report_id;
        bool ok = false;
        const auto got = frame(d, feature, report_id, report, &ok);
        CHECK(ok);
        const auto want = reference::set_report(d, feature, report_id, report);
        CHECK(got == want);
    }
}

static void test_set_report_layout() {
    VoodooI2CHIDDeviceHIDDescriptor d = {};
    d.wCommandRegister = 0x0005;
    d.wDataRegister = 0x0006;
    // Feature report 3, caller left the ID out: cmd(05 00) type/id(33) opcode(03) data(06 00) size(05 00) id(03) a b
    auto wire = frame(d, true, 3, {0xAA, 0xBB});
    const std::vector<uint8_t> expected = {0x05, 0x00, 0x33, 0x03, 0x06, 0x00, 0x05, 0x00, 0x03, 0xAA, 0xBB};
    CHECK(wire == expected);
    // Same report with the ID already in front: identical bytes on the wire
    CHECK(frame(d, true, 3, {0x03, 0xAA, 0xBB}) == expected);
    // Report ID 0x20 needs the extended byte
    wire = frame(d, false, 0x20, {0x01});
    const std::vector<uint8_t> extended = {0x05, 0x00, 0x2F, 0x03, 0x20, 0x06, 0x00, 0x04, 0x00, 0x20, 0x01};
    CHECK(wire == extended);
    // Unnumbered output report
    wire = frame(d, false, 0, {0x11, 0x22, 0x33});
    const std::vector<uint8_t> plain = {0x05, 0x00, 0x20, 0x03, 0x06, 0x00, 0x05, 0x00, 0x11, 0x22, 0x33};
    CHECK(wire == plain);

    // Rejected inputs
    uint8_t header[i2chid::MaxReportCommandLength];
    const uint16_t header_length = i2chid::encodeReportCommand(header, d, i2chid::OpcodeSetReport, i2chid::ReportTypeFeature, 1);
    uint8_t storage[64] = {};
    i2chid::WireSpan span;
    CHECK(!i2chid::frameSetReport(storage, i2chid::setReportCapacity(header_length, 1, 0), header, header_length, 0, 1, span));
    CHECK(!i2chid::frameSetReport(storage, 10, header, header_length, 8, 1, span));   // capacity mismatch
    CHECK(!i2chid::frameSetReport(storage, 0xFFFFFFFFu, header, header_length, i2chid::MaxSetReportLength + 1, 1, span));
}

static void test_largest_set_report() {
    // The largest allowed report still fits the 16-bit lengths writeI2C() takes
    VoodooI2CHIDDeviceHIDDescriptor d = {};
    std::vector<uint8_t> report(i2chid::MaxSetReportLength, 0x5A);
    for (uint8_t id : {static_cast<uint8_t>(0), static_cast<uint8_t>(1), static_cast<uint8_t>(0xFF)}) {
        bool ok = false;
        const auto wire = frame(d, true, id, report, &ok);
        CHECK(ok);
        CHECK(wire.size() <= 0xFFFFu);
        CHECK(wire == reference::set_report(d, true, id, report));
    }
}

static void test_get_report_command_matches_reference() {
    for (int round = 0; round < 20000; ++round) {
        const auto d = random_descriptor();
        const bool feature = rnd() & 1;
        const uint8_t id = static_cast<uint8_t>(rnd());
        uint8_t bytes[i2chid::MaxReportCommandLength];
        const uint16_t n = i2chid::encodeReportCommand(bytes, d, i2chid::OpcodeGetReport,
                                                       feature ? i2chid::ReportTypeFeature : i2chid::ReportTypeInput, id);
        CHECK(n <= i2chid::MaxReportCommandLength);
        CHECK(std::vector<uint8_t>(bytes, bytes + n) == reference::get_report_command(d, feature, id));
    }
}

static void test_lengths() {
    // Input reports: d8e8bf6/059886d rejected length <= 2 or > capacity
    for (uint32_t capacity = 2; capacity < 70; ++capacity)
        for (uint32_t length = 0; length < 80; ++length)
            CHECK_EQ(i2chid::validInputLength(static_cast<uint16_t>(length), static_cast<uint16_t>(capacity)),
                     length > 2 && length <= capacity);
    CHECK(!i2chid::validInputLength(0xFFFF, 0xFFFE));
    CHECK(i2chid::validInputLength(0xFFFF, 0xFFFF));

    // GET_REPORT replies: rejected below 2 or above the read length
    for (uint32_t read_length = 2; read_length < 70; ++read_length)
        for (uint32_t response = 0; response < 80; ++response)
            CHECK_EQ(i2chid::validResponseLength(static_cast<uint16_t>(response), read_length),
                     response >= 2 && response <= read_length);

    const uint8_t le[] = {0x34, 0x12};
    CHECK_EQ(i2chid::le16(le), 0x1234);
}

static void test_descriptor() {
    VoodooI2CHIDDeviceHIDDescriptor d = {};
    d.wHIDDescLength = sizeof(d);
    d.bcdVersion = 0x0100;
    CHECK(i2chid::descriptorValid(d));
    d.bcdVersion = 0x0101;
    CHECK(!i2chid::descriptorValid(d));
    d.bcdVersion = 0x0100;
    d.wHIDDescLength = 29;
    CHECK(!i2chid::descriptorValid(d));
    // Byte layout as it comes off the bus (little-endian fields)
    const uint8_t raw[30] = {0x1E, 0x00, 0x00, 0x01, 0x40, 0x02, 0x02, 0x00, 0x03, 0x00, 0x20, 0x00,
                             0x04, 0x00, 0x11, 0x00, 0x05, 0x00, 0x06, 0x00, 0x4E, 0x04, 0x0C, 0x12};
    std::memcpy(&d, raw, sizeof(d));
    CHECK(i2chid::descriptorValid(d));
    CHECK_EQ(d.wReportDescLength, 0x0240);
    CHECK_EQ(d.wMaxInputLength, 0x0020);
    CHECK_EQ(d.wCommandRegister, 0x0005);
    CHECK_EQ(d.wDataRegister, 0x0006);
    CHECK_EQ(d.wVendorID, 0x044E);
    CHECK_EQ(d.wProductID, 0x120C);
}

static void test_polling_interval() {
    // Matches simulateInterrupt() at 059886d
    const uint64_t ms = 1000000ULL;
    CHECK_EQ(i2chid::pollingIntervalMs(true, false, 0), INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
    CHECK_EQ(i2chid::pollingIntervalMs(true, true, 10000 * ms), INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
    CHECK_EQ(i2chid::pollingIntervalMs(false, true, 249 * ms), INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
    CHECK_EQ(i2chid::pollingIntervalMs(false, true, 250 * ms), INTERRUPT_SIMULATOR_TIMEOUT);
    CHECK_EQ(i2chid::pollingIntervalMs(false, false, 100 * ms), INTERRUPT_SIMULATOR_TIMEOUT);   // no change seen yet
    CHECK_EQ(i2chid::pollingIntervalMs(false, true, 1499 * ms), INTERRUPT_SIMULATOR_TIMEOUT);
    CHECK_EQ(i2chid::pollingIntervalMs(false, true, 1500 * ms), INTERRUPT_SIMULATOR_TIMEOUT_IDLE);
    CHECK_EQ(i2chid::pollingIntervalMs(false, false, 1500 * ms), INTERRUPT_SIMULATOR_TIMEOUT_IDLE);
    CHECK_EQ(i2chid::pollingIntervalMs(false, false, ~0ULL), INTERRUPT_SIMULATOR_TIMEOUT_IDLE);
    for (int round = 0; round < 10000; ++round) {
        const uint64_t idle = static_cast<uint64_t>(rnd()) * 1000;
        const bool reset = rnd() & 1, seen = rnd() & 1;
        const unsigned v = i2chid::pollingIntervalMs(reset, seen, idle);
        CHECK(v == INTERRUPT_SIMULATOR_TIMEOUT_BUSY || v == INTERRUPT_SIMULATOR_TIMEOUT || v == INTERRUPT_SIMULATOR_TIMEOUT_IDLE);
        // Never back off while a reset is pending
        if (reset)
            CHECK_EQ(v, INTERRUPT_SIMULATOR_TIMEOUT_BUSY);
    }
}

int main() {
    test_set_report_matches_reference();
    test_set_report_layout();
    test_largest_set_report();
    test_get_report_command_matches_reference();
    test_lengths();
    test_descriptor();
    test_polling_interval();
    if (failures) {
        std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("All %d checks passed\n", checks);
    return 0;
}
