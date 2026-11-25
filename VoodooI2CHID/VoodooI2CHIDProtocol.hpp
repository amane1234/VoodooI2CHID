/* I2C-HID wire protocol helpers used by VoodooI2CHIDDevice: the HID descriptor, GET_REPORT/SET_REPORT
 * command encoding, length checks on device replies and the polling interval policy.
 *
 * Plain data processing with no IOKit dependency, so the kext and the host tests and fuzzers under Tests/
 * compile the same code. Everything a device returns is treated as untrusted. */

#ifndef VoodooI2CHIDProtocol_hpp
#define VoodooI2CHIDProtocol_hpp

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Polling intervals (ms) when the device has no usable interrupt */
#define INTERRUPT_SIMULATOR_TIMEOUT 5
#define INTERRUPT_SIMULATOR_TIMEOUT_BUSY 2
#define INTERRUPT_SIMULATOR_TIMEOUT_IDLE 50

typedef union {
    uint8_t data[4];
    struct __attribute__((__packed__)) cmd {
        uint16_t reg;
        uint8_t report_type_id;
        uint8_t opcode;
    } c;
} VoodooI2CHIDDeviceCommand;

typedef struct __attribute__((__packed__)) {
    uint16_t wHIDDescLength;
    uint16_t bcdVersion;
    uint16_t wReportDescLength;
    uint16_t wReportDescRegister;
    uint16_t wInputRegister;
    uint16_t wMaxInputLength;
    uint16_t wOutputRegister;
    uint16_t wMaxOutputLength;
    uint16_t wCommandRegister;
    uint16_t wDataRegister;
    uint16_t wVendorID;
    uint16_t wProductID;
    uint16_t wVersionID;
    uint32_t reserved;
} VoodooI2CHIDDeviceHIDDescriptor;

static_assert(sizeof(VoodooI2CHIDDeviceCommand) == 4, "I2C-HID command header is 4 bytes");
static_assert(sizeof(VoodooI2CHIDDeviceHIDDescriptor) == 30, "I2C-HID descriptor is 30 bytes");

namespace i2chid {

/* Report types in the command's report_type_id field */
static constexpr uint8_t ReportTypeInput = 0x01;
static constexpr uint8_t ReportTypeOutput = 0x02;
static constexpr uint8_t ReportTypeFeature = 0x03;

/* Opcodes */
static constexpr uint8_t OpcodeGetReport = 0x02;
static constexpr uint8_t OpcodeSetReport = 0x03;

/* Command register, type/ID, opcode, optional extended ID and data register */
static constexpr unsigned MaxReportCommandLength = sizeof(VoodooI2CHIDDeviceCommand) + 3;

/* Largest report a GET_REPORT/SET_REPORT may carry: the 16-bit length fields must also hold the
 * 2-byte size prefix, plus command, extended ID and data register for SET_REPORT */
static constexpr uint32_t MaxGetReportLength = 0xFFFF - 2;
static constexpr uint32_t MaxSetReportLength = 0xFFFF - 10;

inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

/* Only version 1.00 descriptors of the expected size are accepted */
inline bool descriptorValid(const VoodooI2CHIDDeviceHIDDescriptor& descriptor) {
    return descriptor.bcdVersion == 0x0100 && descriptor.wHIDDescLength == sizeof(VoodooI2CHIDDeviceHIDDescriptor);
}

/* Encode the command part of GET_REPORT / SET_REPORT into bytes (at least MaxReportCommandLength).
 * Report IDs of 15 and above do not fit the 4-bit field; they are sent as 0xF plus an extra byte. */
inline uint16_t encodeReportCommand(uint8_t* bytes, const VoodooI2CHIDDeviceHIDDescriptor& descriptor,
                                    uint8_t opcode, uint8_t report_type, uint8_t report_id) {
    VoodooI2CHIDDeviceCommand command;
    command.c.reg = descriptor.wCommandRegister;
    command.c.report_type_id = static_cast<uint8_t>((report_type << 4) | (report_id < 0x0F ? report_id : 0x0F));
    command.c.opcode = opcode;
    memcpy(bytes, command.data, sizeof(command));

    uint16_t length = sizeof(command);
    if (report_id >= 0x0F)
        bytes[length++] = report_id;
    bytes[length++] = static_cast<uint8_t>(descriptor.wDataRegister & 0xFF);
    bytes[length++] = static_cast<uint8_t>(descriptor.wDataRegister >> 8);
    return length;
}

/* An input report read from the input register starts with its total length (including the 2 length
 * bytes). 0 is the reset acknowledgement or "no report"; a usable report has payload and fits the buffer. */
inline bool validInputLength(uint16_t length, uint16_t capacity) {
    return length > 2 && length <= capacity;
}

/* A GET_REPORT reply into read_length bytes: its length field covers the 2 length bytes and must fit */
inline bool validResponseLength(uint16_t response_length, uint32_t read_length) {
    return response_length >= 2 && response_length <= read_length;
}

/* SET_REPORT framing.
 *
 * The caller allocates setReportCapacity() bytes and reads the report into the last report_length bytes,
 * so the report is copied only once. frameSetReport() then prepends the report ID if the caller left it
 * out, the 2-byte size and the command header, and returns where the wire bytes start and how many there
 * are. Wire layout: header (command, [extended ID], data register), size (LE16), [report ID], report. The
 * size counts itself, the report ID and the report, as in the I2C-HID specification. */
inline uint32_t setReportCapacity(uint16_t header_length, uint8_t report_id, uint32_t report_length) {
    return header_length + 2u + (report_id != 0) + report_length;
}

struct WireSpan {
    uint32_t offset;
    uint32_t length;
};

inline bool frameSetReport(uint8_t* storage, uint32_t capacity, const uint8_t* header, uint16_t header_length,
                           uint32_t report_length, uint8_t report_id, WireSpan& span) {
    if (!report_length || report_length > MaxSetReportLength ||
        capacity != setReportCapacity(header_length, report_id, report_length))
        return false;
    uint8_t* payload = storage + (capacity - report_length);
    uint32_t wire_size = 2 + report_length;
    if (report_id && payload[0] != report_id) {
        --payload;
        *payload = report_id;
        ++wire_size;
    }
    uint8_t* command = payload - 2 - header_length;
    memcpy(command, header, header_length);
    command[header_length] = static_cast<uint8_t>(wire_size & 0xFF);
    command[header_length + 1] = static_cast<uint8_t>(wire_size >> 8);
    span.offset = static_cast<uint32_t>(command - storage);
    span.length = header_length + wire_size;
    return true;
}

/* Polling interval after a poll: fast while a reset is pending or the report changed in the last 250 ms,
 * back off to the idle interval after 1.5 s without a change */
inline unsigned pollingIntervalMs(bool reset_pending, bool seen_change, uint64_t idle_ns) {
    if (reset_pending || (seen_change && idle_ns < 250000000ULL))
        return INTERRUPT_SIMULATOR_TIMEOUT_BUSY;
    if (idle_ns >= 1500000000ULL)
        return INTERRUPT_SIMULATOR_TIMEOUT_IDLE;
    return INTERRUPT_SIMULATOR_TIMEOUT;
}

} // namespace i2chid

#endif /* VoodooI2CHIDProtocol_hpp */
