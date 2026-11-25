//
//  VoodooI2CHIDDevice.cpp
//  VoodooI2CHID
//
//  Created by Alexandre on 25/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include <IOKit/hid/IOHIDDevice.h>
#include <kern/locks.h>
#include "VoodooI2CHIDDevice.hpp"
#include "../../../VoodooI2C/VoodooI2C/VoodooI2CDevice/VoodooI2CDeviceNub.hpp"

#define super IOHIDDevice
OSDefineMetaClassAndStructors(VoodooI2CHIDDevice, IOHIDDevice);


bool VoodooI2CHIDDevice::init(OSDictionary* properties) {
    if (!super::init(properties))
        return false;
    awake = true;
    reset_pending = false;
    reset_in_progress = false;
    memset(&hid_descriptor, 0, sizeof(VoodooI2CHIDDeviceHIDDescriptor));
    acpi_device = NULL;
    api = NULL;
    command_gate = NULL;
    interrupt_simulator = NULL;
    interrupt_source = NULL;
    ready_for_input = false;

    client_lock = IOLockAlloc();

    clients = OSArray::withCapacity(1);

    if (!client_lock || !clients) {
        OSSafeReleaseNULL(clients);
        return false;
    }

    return true;
}

void VoodooI2CHIDDevice::free() {
    OSSafeReleaseNULL(clients);
    if (client_lock)
        IOLockFree(client_lock);

    super::free();
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptor() {
    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor_register;

    if (api->writeReadI2C(command.data, 2, (UInt8*)&hid_descriptor, (UInt16)sizeof(VoodooI2CHIDDeviceHIDDescriptor)) != kIOReturnSuccess) {
        IOLog("%s::%s Request for HID descriptor failed\n", getName(), name);
        return kIOReturnIOError;
    }

    return parseHIDDescriptor();
}

IOReturn VoodooI2CHIDDevice::parseHIDDescriptor() {
    if (!i2chid::descriptorValid(hid_descriptor)) {
        IOLog("%s::%s Invalid HID descriptor (BCD version 0x%x, length %d)\n", getName(), name,
              hid_descriptor.bcdVersion, hid_descriptor.wHIDDescLength);
        return kIOReturnInvalid;
    }

    OSDictionary* property_array = OSDictionary::withCapacity(1);
    if (!property_array)
        return kIOReturnNoMemory;

    setOSDictionaryNumber(property_array, "HIDDescLength",      hid_descriptor.wHIDDescLength);
    setOSDictionaryNumber(property_array, "BCDVersion",         hid_descriptor.bcdVersion);
    setOSDictionaryNumber(property_array, "ReportDescLength",   hid_descriptor.wReportDescLength);
    setOSDictionaryNumber(property_array, "ReportDescRegister", hid_descriptor.wReportDescRegister);
    setOSDictionaryNumber(property_array, "MaxInputLength",     hid_descriptor.wMaxInputLength);
    setOSDictionaryNumber(property_array, "InputRegister",      hid_descriptor.wInputRegister);
    setOSDictionaryNumber(property_array, "MaxOutputLength",    hid_descriptor.wMaxOutputLength);
    setOSDictionaryNumber(property_array, "OutputRegister",     hid_descriptor.wOutputRegister);
    setOSDictionaryNumber(property_array, "CommandRegister",    hid_descriptor.wCommandRegister);
    setOSDictionaryNumber(property_array, "DataRegister",       hid_descriptor.wDataRegister);
    setOSDictionaryNumber(property_array, "VendorID",           hid_descriptor.wVendorID);
    setOSDictionaryNumber(property_array, "ProductID",          hid_descriptor.wProductID);
    setOSDictionaryNumber(property_array, "VersionID",          hid_descriptor.wVersionID);

    setProperty("HIDDescriptor", property_array);

    OSSafeReleaseNULL(property_array);

    return kIOReturnSuccess;
}

IOReturn VoodooI2CHIDDevice::getHIDDescriptorAddress() {
    IOReturn ret;
    OSObject *obj = nullptr;

    ret = api->evaluateDSM(I2C_DSM_HIDG, HIDG_DESC_INDEX, &obj);
    if (ret == kIOReturnSuccess) {
        OSNumber *number = OSDynamicCast(OSNumber, obj);
        if (number != nullptr) {
            hid_descriptor_register = number->unsigned16BitValue();
            setProperty("HIDDescriptorAddress", hid_descriptor_register, 16);
        } else {
            IOLog("%s::%s HID descriptor address invalid\n", getName(), name);
            ret = kIOReturnInvalid;
        }
    } else {
        IOLog("%s::%s unable to parse HID descriptor address\n", getName(), name);
        ret = kIOReturnNotFound;
    }
    if (obj) obj->release();
    return ret;
}

void VoodooI2CHIDDevice::getInputReport() {
    // Recheck inside the work-loop gate, including callbacks queued before sleep.
    if (!awake && !reset_pending)
        return;
    if (!input_storage || input_read_active)
        return;

    // A controller wait can open a shared work-loop gate. Keep the buffer
    // alive and discard the read if sleep or teardown intervened.
    auto* storage = input_storage;
    storage->retain();
    UInt8* report = static_cast<UInt8*>(storage->getBytesNoCopy());
    const unsigned generation = input_generation;
    input_read_active = true;
    IOReturn read_result = api->readI2C(report, input_capacity);
    input_read_active = false;
    storage->release();
    if (read_result != kIOReturnSuccess || generation != input_generation)
        return;

    const UInt16 length = i2chid::le16(report);
    if (!length && reset_pending) {
        reset_pending = false;
        command_gate->commandWakeup(&reset_pending);
    }
    if (!ready_for_input || !i2chid::validInputLength(length, input_capacity))
        return;

    if (previous_input && (length != previous_length || memcmp(previous_input->getBytesNoCopy(), report, length))) {
        clock_get_uptime(&polling_activity);
        previous_length = length;
        input_storage = previous_input;
        previous_input = storage;
    }

    // Clients can retain a delivered descriptor beyond handleReport(). Give
    // each valid report independent storage; empty polls need no allocation.
    auto* buffer = IOBufferMemoryDescriptor::withBytes(report + 2, length - 2, kIODirectionNone);
    if (!buffer)
        return;
    IOReturn ret = handleReport(buffer, kIOHIDReportTypeInput);
    if (ret != kIOReturnSuccess)
        IOLog("%s::%s Error handling input report: 0x%.8x\n", getName(), name, ret);
    buffer->release();
}

IOReturn VoodooI2CHIDDevice::getReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    // Deliberately not run through command_gate: IOHIDDevice calls getReport()/setReport() with its
    // element lock held, while getInputReport() holds the gate and then takes that same lock in
    // handleReport(). Gating here deadlocks as soon as an input report arrives during a feature
    // transfer. The I2C transfers themselves are serialised by the controller.
    if (!command_gate)
        return kIOReturnNotReady;
    return getReportGated(report, reportType, options);
}

IOReturn VoodooI2CHIDDevice::getReportGated(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    // A feature transaction during RESET could consume the device's reset ACK, so refuse it.
    if (!awake || reset_in_progress)
        return kIOReturnNotReady;
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeInput)
        return kIOReturnBadArgument;

    const IOByteCount report_length = report ? report->getLength() : 0;
    if (!report_length || report_length > i2chid::MaxGetReportLength)
        return kIOReturnBadArgument;

    const UInt16 read_length = static_cast<UInt16>(report_length + 2);
    UInt8* buffer = static_cast<UInt8*>(IOMalloc(read_length));
    if (!buffer)
        return kIOReturnNoMemory;

    UInt8 command[i2chid::MaxReportCommandLength];
    const UInt8 raw_type = reportType == kIOHIDReportTypeFeature ? i2chid::ReportTypeFeature : i2chid::ReportTypeInput;
    const UInt16 command_length = i2chid::encodeReportCommand(command, hid_descriptor, i2chid::OpcodeGetReport, raw_type, options & 0xFF);
    IOReturn ret = api->writeReadI2C(command, command_length, buffer, read_length);
    if (ret == kIOReturnSuccess) {
        UInt16 response_length = i2chid::le16(buffer);
        if (!i2chid::validResponseLength(response_length, read_length))
            ret = kIOReturnIOError;
        else if (report->writeBytes(0, buffer + 2, response_length - 2) != static_cast<IOByteCount>(response_length - 2))
            ret = kIOReturnIOError;
    }

    IOFree(buffer, read_length);
    return ret;
}

void VoodooI2CHIDDevice::interruptOccured(OSObject* owner, IOInterruptEventSource* src, int intCount) {
    // The interrupt source and command gate are attached to the same work loop.
    if (is_interrupt_started)
        getInputReport();
}

VoodooI2CHIDDevice* VoodooI2CHIDDevice::probe(IOService* provider, SInt32* score) {
    if (!super::probe(provider, score))
        return NULL;

    name = getMatchedName(provider);

    acpi_device = OSDynamicCast(IOACPIPlatformDevice, provider->getProperty("acpi-device"));
    //acpi_device->retain();

    if (!acpi_device) {
        IOLog("%s::%s Could not get ACPI device\n", getName(), name);
        return NULL;
    }

    // Sometimes an I2C HID will have power state methods, lets turn it on in case

    acpi_device->evaluateObject("_PS0");

    api = OSDynamicCast(VoodooI2CDeviceNub, provider);
    //api->retain();

    if (!api) {
        IOLog("%s::%s Could not get VoodooI2C API access\n", getName(), name);
        return NULL;
    }

    if (getHIDDescriptorAddress() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }

    if (getHIDDescriptor() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get HID descriptor\n", getName(), name);
        return NULL;
    }

    return this;
}

void VoodooI2CHIDDevice::releaseResources() {
    ready_for_input = false;
    awake = false;
    stopInterrupt();

    if (interrupt_simulator) {
        if (interrupt_simulator->getWorkLoop())
            interrupt_simulator->getWorkLoop()->removeEventSource(interrupt_simulator);
        OSSafeReleaseNULL(interrupt_simulator);
    }
    if (interrupt_source) {
        if (interrupt_source->getWorkLoop())
            interrupt_source->getWorkLoop()->removeEventSource(interrupt_source);
        OSSafeReleaseNULL(interrupt_source);
    }
    if (command_gate) {
        command_gate->disable();
        if (command_gate->getWorkLoop())
            command_gate->getWorkLoop()->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }
    OSSafeReleaseNULL(input_storage);
    OSSafeReleaseNULL(previous_input);
    OSSafeReleaseNULL(work_loop);
    OSSafeReleaseNULL(acpi_device);
    if (api) {
        if (api->isOpen(this))
            api->close(this);
        OSSafeReleaseNULL(api);
    }
}

IOReturn VoodooI2CHIDDevice::resetHIDDevice() {
    if (!command_gate)
        return kIOReturnNotReady;
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CHIDDevice::resetHIDDeviceGated));
}

IOReturn VoodooI2CHIDDevice::resetHIDDeviceGated() {
    if (reset_in_progress)
        return kIOReturnBusy;

    reset_in_progress = true;
    const bool was_ready = ready_for_input;
    ready_for_input = false;
    IOReturn ret = setHIDPowerState(kVoodooI2CStateOn);
    VoodooI2CHIDDeviceCommand command;
    if (ret != kIOReturnSuccess)
        goto done;

    command.c.reg = hid_descriptor.wCommandRegister;
    command.c.opcode = 0x01;
    command.c.report_type_id = 0;
    // Establish the predicate before RESET can produce an interrupt.
    reset_pending = true;
    ret = api->writeI2C(command.data, 4);
    if (ret != kIOReturnSuccess)
        goto done;

    if (quirks & I2C_HID_QUIRK_NO_IRQ_AFTER_RESET) {
        IOSleep(100);
        reset_pending = false;
    } else {
        // Preserve this driver's 12-second allowance for slow firmware.
        AbsoluteTime absolute_time, deadline;
        nanoseconds_to_absolutetime(12000000000ULL, &absolute_time);
        clock_absolutetime_interval_to_deadline(absolute_time, &deadline);
        while (reset_pending) {
            IOReturn result = command_gate->commandSleep(&reset_pending, deadline, THREAD_UNINT);
            if (result == THREAD_TIMED_OUT && reset_pending) {
                // Like Linux i2c-hid (and this driver before), a missing reset ACK is not fatal:
                // plenty of devices never send it but work fine afterwards.
                IOLog("%s::%s Timeout waiting for device to complete host initiated reset, continuing\n", getName(), name);
                break;
            }
            if (result != THREAD_AWAKENED && result != THREAD_TIMED_OUT) {
                ret = kIOReturnAborted;
                break;
            }
        }
    }

done:
    reset_pending = false;
    if (ret != kIOReturnSuccess) {
        IOLog("%s::%s HID reset failed: 0x%.8x\n", getName(), name, ret);
        // Best effort only; preserve the original failure for the caller.
        setHIDPowerState(kVoodooI2CStateOff);
        awake = false;
        stopInterrupt();
    }
    ready_for_input = was_ready && ret == kIOReturnSuccess;
    reset_in_progress = false;
    command_gate->commandWakeup(&reset_in_progress);
    return ret;
}

IOReturn VoodooI2CHIDDevice::setHIDPowerState(VoodooI2CState state) {
    VoodooI2CHIDDeviceCommand command;
    IOReturn ret = kIOReturnSuccess;
    int attempts = 5;
    do {
        command.c.reg = hid_descriptor.wCommandRegister;
        command.c.opcode = 0x08;
        command.c.report_type_id = state ? I2C_HID_PWR_ON : I2C_HID_PWR_SLEEP;

        ret = api->writeI2C(command.data, 4);
        IOSleep(100);
    } while (ret != kIOReturnSuccess && --attempts > 0);
    return ret;
}

IOReturn VoodooI2CHIDDevice::setReport(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    // Not gated, for the same lock-order reason as getReport()
    if (!command_gate)
        return kIOReturnNotReady;
    return setReportGated(report, reportType, options);
}

IOReturn VoodooI2CHIDDevice::setReportGated(IOMemoryDescriptor* report, IOHIDReportType reportType, IOOptionBits options) {
    // A feature transaction during RESET could consume the device's reset ACK, so refuse it.
    if (!awake || reset_in_progress)
        return kIOReturnNotReady;
    if (reportType != kIOHIDReportTypeFeature && reportType != kIOHIDReportTypeOutput)
        return kIOReturnBadArgument;

    const IOByteCount report_length = report ? report->getLength() : 0;
    // Reserve room for command, extended ID, data register and length.
    if (!report_length || report_length > i2chid::MaxSetReportLength)
        return kIOReturnBadArgument;

    const UInt8 report_id = options & 0xFF;
    const UInt8 raw_type = reportType == kIOHIDReportTypeFeature ? i2chid::ReportTypeFeature : i2chid::ReportTypeOutput;
    UInt8 header[i2chid::MaxReportCommandLength];
    const UInt16 header_length = i2chid::encodeReportCommand(header, hid_descriptor, i2chid::OpcodeSetReport, raw_type, report_id);
    const UInt32 capacity = i2chid::setReportCapacity(header_length, report_id, static_cast<UInt32>(report_length));
    UInt8* storage = static_cast<UInt8*>(IOMalloc(capacity));
    if (!storage)
        return kIOReturnNoMemory;

    // Read the descriptor once into the end of the buffer; frameSetReport() puts the report ID (if the
    // caller omitted it), the size and the command in front of it.
    if (report->readBytes(0, storage + (capacity - report_length), report_length) != report_length) {
        IOFree(storage, capacity);
        return kIOReturnIOError;
    }
    i2chid::WireSpan wire;
    if (!i2chid::frameSetReport(storage, capacity, header, header_length, static_cast<UInt32>(report_length), report_id, wire)) {
        IOFree(storage, capacity);
        return kIOReturnBadArgument;
    }
    IOReturn ret = api->writeI2C(storage + wire.offset, static_cast<UInt16>(wire.length));
    IOSleep(10);
    IOFree(storage, capacity);
    return ret;
}

void VoodooI2CHIDDevice::lookupQuirks() {
    quirks = 0;
    for (size_t n = 0; i2c_hid_quirks[n].idVendor; n++) {
        if (i2c_hid_quirks[n].idVendor == hid_descriptor.wVendorID &&
            (i2c_hid_quirks[n].idProduct == HID_ANY_ID ||
             i2c_hid_quirks[n].idProduct == hid_descriptor.wProductID)) {
            quirks = i2c_hid_quirks[n].quirks;
        }
    }
}

IOReturn VoodooI2CHIDDevice::setPowerState(unsigned long whichState, IOService* whatDevice) {
    // The return value is an acknowledgement delay in microseconds, not an IOReturn
    if (whatDevice != this)
        return kIOPMAckImplied;
    if (!command_gate)
        return kIOPMAckImplied;

    IOReturn ret = command_gate->runAction(
        [](OSObject* owner, void* state, void*, void*, void*) -> IOReturn {
            return static_cast<VoodooI2CHIDDevice*>(owner)->setPowerStateGated(
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(state)));
        }, reinterpret_cast<void*>(whichState));
    setProperty("LastHIDPowerError", static_cast<UInt32>(ret), 32);
    if (ret != kIOReturnSuccess)
        IOLog("%s::%s HID power transition failed: 0x%.8x\n", getName(), name, ret);
    // IOKit interprets positive return values as acknowledgement delays,
    // not transport errors. The transition is synchronous even on failure.
    return kIOPMAckImplied;
}

IOReturn VoodooI2CHIDDevice::setPowerStateGated(unsigned long whichState) {
    // Let an existing RESET finish before changing its power state.
    while (reset_in_progress) {
        if (command_gate->commandSleep(&reset_in_progress, THREAD_UNINT) != THREAD_AWAKENED)
            return kIOReturnAborted;
    }
    if (!whichState) {
        if (!awake)
            return kIOReturnSuccess;
        ready_for_input = false;
        IOReturn ret = kIOReturnSuccess;
        if (!(quirks & I2C_HID_QUIRK_NO_SLEEP_ON_SUSPEND))
            ret = setHIDPowerState(kVoodooI2CStateOff);
        // The provider may power down even if SET_POWER failed.
        stopInterrupt();
        awake = false;
        return ret;
    }
    if (awake)
        return kIOReturnSuccess;

    ready_for_input = false;
    if (quirks & I2C_HID_QUIRK_DELAY_WAKEUP_AFTER_RESUME)
        IOSleep(1500);
    startInterrupt(); // Arm input after the firmware delay, before RESET.
    IOReturn ret = (quirks & I2C_HID_QUIRK_RESET_ON_RESUME) ?
        resetHIDDeviceGated() : setHIDPowerState(kVoodooI2CStateOn);
    if (ret != kIOReturnSuccess) {
        stopInterrupt();
        return ret;
    }
    awake = true;
    ready_for_input = true;
    return kIOReturnSuccess;
}

bool VoodooI2CHIDDevice::handleStart(IOService* provider) {
    if (!IOHIDDevice::handleStart(provider)) {
        return false;
    }

    // releaseResources also runs on early startup failures.
    acpi_device->retain();
    api->retain();

    work_loop = getWorkLoop();

    if (!work_loop) {
        IOLog("%s::%s Could not get work loop\n", getName(), name);
        goto exit;
    }

    work_loop->retain();

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s::%s Could not open command gate\n", getName(), name);
        goto exit;
    }

    if (!api->open(this)) {
        IOLog("%s::%s Could not open API\n", getName(), name);
        goto exit;
    }

    interrupt_source = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &VoodooI2CHIDDevice::interruptOccured), api, 0);
    if (interrupt_source && work_loop->addEventSource(interrupt_source) != kIOReturnSuccess)
        OSSafeReleaseNULL(interrupt_source);
    if (!interrupt_source) {
        IOLog("%s::%s Warning: Could not get interrupt event source, using polling instead\n", getName(), name);
        interrupt_simulator = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &VoodooI2CHIDDevice::simulateInterrupt));

        if (!interrupt_simulator) {
            IOLog("%s::%s Could not get timer event source\n", getName(), name);
            goto exit;
        }

        if (work_loop->addEventSource(interrupt_simulator) != kIOReturnSuccess) {
            IOLog("%s::%s Could not attach polling timer\n", getName(), name);
            goto exit;
        }
    }
    input_capacity = hid_descriptor.wMaxInputLength;
    if (input_capacity < 2)
        goto exit;
    input_storage = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, input_capacity);
    if (!input_storage)
        goto exit;
    if (interrupt_simulator) {
        previous_input = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, input_capacity);
        if (!previous_input)
            goto exit;
    }
    startInterrupt();

    lookupQuirks();
    if (resetHIDDevice() != kIOReturnSuccess)
        goto exit;


    PMinit();
    api->joinPMtree(this);
    registerPowerDriver(this, VoodooI2CIOPMPowerStates, kVoodooI2CIOPMNumberPowerStates);

    // Give the reset a bit of time so that IOHIDDevice doesnt happen to start requesting the report
    // descriptor before the driver is ready

    IOSleep(100);

    return true;
exit:
    releaseResources();
    return false;
}

bool VoodooI2CHIDDevice::start(IOService* provider) {
    if (!super::start(provider))
        return false;

    ready_for_input = true;

    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);

    return true;
}

void VoodooI2CHIDDevice::stop(IOService* provider) {
    // Give the clients time to close, but never wait forever: a client that does not close would
    // otherwise hang termination, and with it sleep or shutdown.
    AbsoluteTime deadline;
    clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
    IOLockLock(client_lock);
    while (clients && clients->getCount()) {
        if (IOLockSleepDeadline(client_lock, &client_lock, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            IOLog("%s::%s %u client(s) still open, stopping anyway\n", getName(), name, clients->getCount());
            break;
        }
    }
    IOLockUnlock(client_lock);

    PMstop();
    releaseResources();
    OSSafeReleaseNULL(clients);
    super::stop(provider);
}

IOReturn VoodooI2CHIDDevice::newReportDescriptor(IOMemoryDescriptor** descriptor) const {
    if (!hid_descriptor.wReportDescLength) {
        IOLog("%s::%s Invalid report descriptor size\n", getName(), name);
        return kIOReturnDeviceError;
    }

    VoodooI2CHIDDeviceCommand command;
    command.c.reg = hid_descriptor.wReportDescRegister;

    UInt8* buffer = reinterpret_cast<UInt8*>(IOMalloc(hid_descriptor.wReportDescLength));
    if (!buffer)
        return kIOReturnNoMemory;
    memset(buffer, 0, hid_descriptor.wReportDescLength);

    if (api->writeReadI2C(command.data, 2, buffer, hid_descriptor.wReportDescLength) != kIOReturnSuccess) {
        IOLog("%s::%s Could not get report descriptor\n", getName(), name);
        IOFree(buffer, hid_descriptor.wReportDescLength);
        return kIOReturnIOError;
    }

    IOBufferMemoryDescriptor* report_descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, hid_descriptor.wReportDescLength);

    if (!report_descriptor) {
        IOLog("%s::%s Could not allocated buffer for report descriptor\n", getName(), name);
        IOFree(buffer, hid_descriptor.wReportDescLength);
        return kIOReturnNoResources;
    }

    report_descriptor->writeBytes(0, buffer, hid_descriptor.wReportDescLength);
    *descriptor = report_descriptor;

    IOFree(buffer, hid_descriptor.wReportDescLength);

    return kIOReturnSuccess;
}

OSNumber* VoodooI2CHIDDevice::newVendorIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVendorID, 16);
}

OSNumber* VoodooI2CHIDDevice::newProductIDNumber() const {
    return OSNumber::withNumber(hid_descriptor.wProductID, 16);
}

OSNumber* VoodooI2CHIDDevice::newVersionNumber() const {
    return OSNumber::withNumber(hid_descriptor.wVersionID, 16);
}

OSString* VoodooI2CHIDDevice::newTransportString() const {
    return OSString::withCString("I2C");
}

OSString* VoodooI2CHIDDevice::newManufacturerString() const {
    return OSString::withCString("Apple");
}

void VoodooI2CHIDDevice::simulateInterrupt(OSObject* owner, IOTimerEventSource* timer) {
    if (!is_interrupt_started)
        return;
    getInputReport();
    if (!is_interrupt_started)
        return;

    AbsoluteTime now;
    UInt64 idle_ns;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - polling_activity, &idle_ns);
    timer->setTimeoutMS(i2chid::pollingIntervalMs(reset_pending, previous_length != 0, idle_ns));
}

bool VoodooI2CHIDDevice::open(IOService *forClient, IOOptionBits options, void *arg) {
    // Only track clients whose open succeeded, and each one once: stop() waits until every tracked
    // client has closed, so a failed or repeated open used to leave an entry behind and hang termination.
    if (!super::open(forClient, options, arg))
        return false;

    IOLockLock(client_lock);
    if (clients && clients->getNextIndexOfObject(forClient, 0) == (unsigned int)-1)
        clients->setObject(forClient);
    IOLockUnlock(client_lock);

    return true;
}

void VoodooI2CHIDDevice::close(IOService *forClient, IOOptionBits options) {
    IOLockLock(client_lock);

    for(int i = 0; i < clients->getCount(); i++) {
        OSObject* service = clients->getObject(i);

        if (service == forClient) {
            clients->removeObject(i);
            break;
        }
    }

    IOLockUnlock(client_lock);

    IOLockWakeup(client_lock, &client_lock, true);

    super::close(forClient, options);
}

void VoodooI2CHIDDevice::startInterrupt() {
    if (is_interrupt_started) {
        return;
    }

    if (interrupt_simulator) {
        previous_length = 0;
        clock_get_uptime(&polling_activity);
        is_interrupt_started = true;
        interrupt_simulator->enable();
        interrupt_simulator->setTimeoutMS(INTERRUPT_SIMULATOR_TIMEOUT);
    } else if (interrupt_source) {
        is_interrupt_started = true;
        interrupt_source->enable();
    }
}

void VoodooI2CHIDDevice::stopInterrupt() {
    if (!is_interrupt_started) {
        return;
    }

    ++input_generation;
    is_interrupt_started = false;
    if (interrupt_simulator) {
        interrupt_simulator->cancelTimeout();
        interrupt_simulator->disable();
    } else if (interrupt_source) {
        interrupt_source->disable();
    }
}
