// libFuzzer target: SET_REPORT framing (VoodooI2CHIDDevice::setReportGated) against the d8e8bf6 reference.
//
// Input layout: bytes 0-1 command register, 2-3 data register, byte 4 bit 0 feature/output, byte 5 the
// report ID passed in options, the rest is the report a client hands to setReport(). The frame is built in
// an exact-size heap buffer like the kext does, then compared byte for byte with the reference encoder and
// parsed back.

#include "VoodooI2CHIDProtocol.hpp"
#include "Reference.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#define REQUIRE(cond) do { if (!(cond)) std::abort(); } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 7)
        return 0;
    VoodooI2CHIDDeviceHIDDescriptor descriptor = {};
    descriptor.wCommandRegister = i2chid::le16(data);
    descriptor.wDataRegister = i2chid::le16(data + 2);
    const bool feature = data[4] & 1;
    const uint8_t report_id = data[5];
    const uint8_t* report = data + 6;
    const uint32_t report_length = static_cast<uint32_t>(size - 6);
    if (report_length > i2chid::MaxSetReportLength)
        return 0;

    uint8_t header[i2chid::MaxReportCommandLength];
    const uint16_t header_length = i2chid::encodeReportCommand(header, descriptor, i2chid::OpcodeSetReport,
                                                               feature ? i2chid::ReportTypeFeature : i2chid::ReportTypeOutput,
                                                               report_id);
    REQUIRE(header_length == (report_id >= 0x0F ? 7 : 6));
    const uint32_t capacity = i2chid::setReportCapacity(header_length, report_id, report_length);
    uint8_t* storage = static_cast<uint8_t*>(std::malloc(capacity));
    std::memcpy(storage + (capacity - report_length), report, report_length);

    i2chid::WireSpan span = {};
    REQUIRE(i2chid::frameSetReport(storage, capacity, header, header_length, report_length, report_id, span));
    REQUIRE(span.offset + span.length == capacity);
    REQUIRE(span.length <= 0xFFFF);   // writeI2C() takes a 16-bit length
    const uint8_t* wire = storage + span.offset;

    // Same bytes as before the rewrite
    const auto expected = reference::set_report(descriptor, feature, report_id,
                                                std::vector<uint8_t>(report, report + report_length));
    REQUIRE(expected.size() == span.length);
    REQUIRE(std::memcmp(expected.data(), wire, span.length) == 0);

    // Parse it back: the size field covers itself, the report ID and the data
    const uint16_t size_field = i2chid::le16(wire + header_length);
    REQUIRE(size_field == span.length - header_length);
    if (report_id)
        REQUIRE(wire[header_length + 2] == report_id);
    std::free(storage);
    return 0;
}
