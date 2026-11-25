// libFuzzer target: VoodooI2CHIDDevice::getInputReport and the GET_REPORT reply path of getReportGated.
//
// Input layout: bytes 0-1 are wMaxInputLength (the read size the descriptor announces), byte 2 the
// GET_REPORT report length requested by a client, then the bytes the device returns. Both reads go into
// exact-size heap buffers and the copies follow the kext's length rules, so ASan reports any access
// beyond what the length checks allowed.

#include "VoodooI2CHIDProtocol.hpp"

#include <cstdlib>
#include <cstring>

#define REQUIRE(cond) do { if (!(cond)) std::abort(); } while (0)

static void input_report(uint16_t capacity, const uint8_t* device, size_t device_size) {
    // handleStart() refuses wMaxInputLength < 2
    if (capacity < 2)
        return;
    uint8_t* report = static_cast<uint8_t*>(std::malloc(capacity));
    uint8_t* previous = static_cast<uint8_t*>(std::malloc(capacity));
    // readI2C() fills the whole buffer; a short device answer leaves the rest as the bus reads it (0xff)
    std::memset(report, 0xff, capacity);
    std::memcpy(report, device, device_size < capacity ? device_size : capacity);
    std::memset(previous, 0, capacity);

    const uint16_t length = i2chid::le16(report);
    if (i2chid::validInputLength(length, capacity)) {
        REQUIRE(length > 2 && length <= capacity);
        // Polling compares the whole report with the previous one
        volatile int changed = std::memcmp(previous, report, length);
        (void)changed;
        // handleReport() gets the payload after the 2 length bytes
        const size_t payload = length - 2u;
        uint8_t* delivered = static_cast<uint8_t*>(std::malloc(payload));
        std::memcpy(delivered, report + 2, payload);
        std::free(delivered);
    }
    std::free(previous);
    std::free(report);
}

static void get_report_reply(uint32_t report_length, const uint8_t* device, size_t device_size) {
    if (!report_length || report_length > i2chid::MaxGetReportLength)
        return;
    const uint32_t read_length = report_length + 2;
    uint8_t* buffer = static_cast<uint8_t*>(std::malloc(read_length));
    std::memset(buffer, 0xff, read_length);
    std::memcpy(buffer, device, device_size < read_length ? device_size : read_length);
    const uint16_t response_length = i2chid::le16(buffer);
    if (i2chid::validResponseLength(response_length, read_length)) {
        // report->writeBytes(0, buffer + 2, response_length - 2) into the caller's report_length bytes
        const uint32_t copy = response_length - 2u;
        REQUIRE(copy <= report_length);
        uint8_t* client = static_cast<uint8_t*>(std::malloc(report_length));
        std::memcpy(client, buffer + 2, copy);
        std::free(client);
    }
    std::free(buffer);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3)
        return 0;
    const uint16_t capacity = i2chid::le16(data);
    const uint32_t report_length = data[2] + 1u;
    // Keep allocations small enough for a fast fuzz loop
    input_report(static_cast<uint16_t>(capacity % 4097), data + 3, size - 3);
    get_report_reply(report_length, data + 3, size - 3);

    // The descriptor check never reads beyond the 30-byte descriptor
    if (size >= 3 + sizeof(VoodooI2CHIDDeviceHIDDescriptor)) {
        VoodooI2CHIDDeviceHIDDescriptor descriptor;
        std::memcpy(&descriptor, data + 3, sizeof(descriptor));
        if (i2chid::descriptorValid(descriptor))
            REQUIRE(descriptor.bcdVersion == 0x0100 && descriptor.wHIDDescLength == 30);
    }
    return 0;
}
