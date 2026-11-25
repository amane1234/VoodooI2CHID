// GET_REPORT/SET_REPORT framing of VoodooI2CHIDDevice at d8e8bf6, before the 851fcb3 rewrite, minus the
// I/O. Used by the unit tests and fuzz_set_report as the reference the current encoder must match.

#ifndef I2CHID_TESTS_REFERENCE_HPP
#define I2CHID_TESTS_REFERENCE_HPP

#include "VoodooI2CHIDProtocol.hpp"

#include <cstring>
#include <vector>

namespace reference {

// VoodooI2CHIDDevice::setReportGated at d8e8bf6, minus the I/O: returns the bytes passed to writeI2C()
inline std::vector<uint8_t> set_report(const VoodooI2CHIDDeviceHIDDescriptor& hid_descriptor, bool feature,
                                       uint8_t report_id, const std::vector<uint8_t>& report) {
    uint16_t data_register = hid_descriptor.wDataRegister;
    uint8_t raw_report_type = feature ? 0x03 : 0x02;
    uint16_t report_length = static_cast<uint16_t>(report.size());
    const uint8_t* buffer = report.data();
    if (report_id && report_id == buffer[0]) {
        buffer++;
        report_length--;
    }
    uint16_t size = 2 + (report_id ? 1 : 0) + report_length;
    std::vector<uint8_t> arguments;
    if (report_id >= 0x0F)
        arguments.push_back(report_id);
    arguments.push_back(data_register & 0xFF);
    arguments.push_back(data_register >> 8);
    arguments.push_back(size & 0xFF);
    arguments.push_back(size >> 8);
    if (report_id)
        arguments.push_back(report_id);
    arguments.insert(arguments.end(), buffer, buffer + report_length);

    const uint8_t command_report_id = report_id >= 0x0F ? 0x0F : report_id;
    VoodooI2CHIDDeviceCommand command;
    std::memset(&command, 0, sizeof(command));
    command.c.reg = hid_descriptor.wCommandRegister;
    command.c.opcode = 0x03;
    command.c.report_type_id = command_report_id | raw_report_type << 4;
    std::vector<uint8_t> wire(command.data, command.data + 4);
    wire.insert(wire.end(), arguments.begin(), arguments.end());
    return wire;
}

// The GET_REPORT command of VoodooI2CHIDDevice::getReportGated at d8e8bf6
inline std::vector<uint8_t> get_report_command(const VoodooI2CHIDDeviceHIDDescriptor& hid_descriptor, bool feature,
                                               uint8_t options_report_id) {
    uint8_t args[3];
    int args_len = 0;
    uint16_t read_register = hid_descriptor.wDataRegister;
    uint8_t report_id = options_report_id;
    uint8_t raw_report_type = feature ? 0x03 : 0x01;
    if (report_id >= 0x0F) {
        args[args_len++] = report_id;
        report_id = 0x0F;
    }
    args[args_len++] = read_register & 0xFF;
    args[args_len++] = read_register >> 8;
    VoodooI2CHIDDeviceCommand command;
    std::memset(&command, 0, sizeof(command));
    command.c.reg = hid_descriptor.wCommandRegister;
    command.c.opcode = 0x02;
    command.c.report_type_id = report_id | raw_report_type << 4;
    std::vector<uint8_t> wire(command.data, command.data + 4);
    wire.insert(wire.end(), args, args + args_len);
    return wire;
}

} // namespace reference

#endif
