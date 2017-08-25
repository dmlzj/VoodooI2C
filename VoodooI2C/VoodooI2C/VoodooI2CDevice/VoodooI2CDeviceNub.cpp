//
//  VoodooI2CDeviceNub.cpp
//  VoodooI2C
//
//  Created by Alexandre on 07/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include "VoodooI2CDeviceNub.hpp"
#include "VoodooI2CACPICRSParser.hpp"

#define super IOService
OSDefineMetaClassAndStructors(VoodooI2CDeviceNub, IOService);

bool VoodooI2CDeviceNub::attach(IOService* provider, IOService* child) {
    if (!super::attach(provider))
        return false;

    controller_name = provider->getName();
    setProperty("acpi-device", child);
    acpi_device = OSDynamicCast(IOACPIPlatformDevice, child);

    if (!acpi_device) {
        IOLog("%s::%s Could not get ACPI device\n", controller_name, child->getName());
        return false;
    }

    if (getDeviceResources() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get device resources\n", controller_name, child->getName());
        return false;
    }

    controller = OSDynamicCast(VoodooI2CControllerDriver, provider);

    if (has_gpio_interrupts) {
        gpio_controller = getGPIOController();
    }

    setName(child->getName());

    return true;
}

IOReturn VoodooI2CDeviceNub::disableInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->disableInterrupt(gpio_pin);
    } else {
        return acpi_device->disableInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::enableInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->enableInterrupt(gpio_pin);
    } else {
        return acpi_device->enableInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::getDeviceResources() {
    OSObject *result = NULL;
    acpi_device->evaluateObject("_CRS", &result);

    OSData *data = OSDynamicCast(OSData, result);
    if (!data) {
        IOLog("%s::%s Could not find or evaluate _CRS method", controller_name, getName());
        return kIOReturnNotFound;
    }

    const uint8_t *crs = reinterpret_cast<const uint8_t*>(data->getBytesNoCopy());
    VoodooI2CACPICRSParser crs_parser;
    crs_parser.parseACPICRS(crs, 0, data->getLength());

    if (crs_parser.found_i2c) {
        use_10bit_addressing = crs_parser.i2c_info.address_mode_10Bit;
        setProperty("addrWidth", OSNumber::withNumber(use_10bit_addressing ? 10 : 7, 8));

        i2c_address = crs_parser.i2c_info.address;
        setProperty("i2cAddress", OSNumber::withNumber(i2c_address, 16));

        setProperty("sclHz", OSNumber::withNumber(crs_parser.i2c_info.bus_speed, 32));
    } else {
        IOLog("%s::%s Could not find an I2C Serial Bus declaration\n", controller_name, getName());
        return kIOReturnNotFound;
    }

    if (crs_parser.found_gpio_interrupts) {
        setProperty("gpioPin", OSNumber::withNumber(crs_parser.gpio_interrupts.pin_number, 16));
        setProperty("gpioIRQ", OSNumber::withNumber(crs_parser.gpio_interrupts.irq_type, 16));

        has_gpio_interrupts = true;
        gpio_pin = crs_parser.gpio_interrupts.pin_number;
        gpio_irq = crs_parser.gpio_interrupts.irq_type;
    } else {
        has_gpio_interrupts = false;
    }

    data->release();
    return kIOReturnSuccess;
}

VoodooGPIO* VoodooI2CDeviceNub::getGPIOController() {
    VoodooGPIO* gpio_controller = NULL;

    OSDictionary *match = serviceMatching("VoodooGPIO");
    OSIterator *iterator = getMatchingServices(match);
    if (iterator) {
        gpio_controller = OSDynamicCast(VoodooGPIO, iterator->getNextObject());

        if (gpio_controller != NULL) {
            IOLog("%s::Got GPIO Controller! %s\n", getName(), gpio_controller->getName());
        }

        gpio_controller->retain();

        iterator->release();
    }

    return gpio_controller;
}

IOReturn VoodooI2CDeviceNub::getInterruptType(int source, int* interrupt_type) {
    if (has_gpio_interrupts) {
        return gpio_controller->getInterruptType(gpio_pin, interrupt_type);
    } else {
        return acpi_device->getInterruptType(source, interrupt_type);
    }
}

IOReturn VoodooI2CDeviceNub::readI2C(UInt8* values, UInt16 length) {
    UInt16 flags = I2C_M_RD;

    if (use_10bit_addressing)
        flags |= I2C_M_TEN;
    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = values,
            .flags = flags,
            .length = length,
        },
    };
    return controller->transferI2C(msgs, 1);
}

IOReturn VoodooI2CDeviceNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) {
    if (has_gpio_interrupts) {
        gpio_controller->setInterruptTypeForPin(gpio_pin, gpio_irq);
        return gpio_controller->registerInterrupt(gpio_pin, target, handler, refcon);
    } else {
        return acpi_device->registerInterrupt(source, target, handler, refcon);
    }
}

bool VoodooI2CDeviceNub::start(IOService* provider) {
    if (!super::start(provider))
        return false;

    registerService();

    return true;
}

void VoodooI2CDeviceNub::stop(IOService* provider) {
    if (gpio_controller) {
        gpio_controller->release();
        gpio_controller = NULL;
    }

    super::stop(provider);
}

IOReturn VoodooI2CDeviceNub::unregisterInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->unregisterInterrupt(gpio_pin);
    } else {
        return acpi_device->unregisterInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::writeI2C(UInt8* values, UInt16 length) {
    UInt16 flags = 0;
    if (use_10bit_addressing)
        flags = I2C_M_TEN;

    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = values,
            .flags = flags,
            .length = length,
        },
    };
    return controller->transferI2C(msgs, 1);
}

IOReturn VoodooI2CDeviceNub::writeReadI2C(UInt8* write_buffer, UInt16 write_length, UInt8* read_buffer, UInt16 read_length) {
    UInt16 read_flags = I2C_M_RD;
    if (use_10bit_addressing)
        read_flags |= I2C_M_TEN;

    UInt16 write_flags = 0;

    if (use_10bit_addressing)
        write_flags = I2C_M_TEN;
    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = write_buffer,
            .flags = write_flags,
            .length = write_length,
        },
        {
            .address = i2c_address,
            .buffer = read_buffer,
            .flags = read_flags,
            .length = read_length,
        }
    };
    return controller->transferI2C(msgs, 2);
}
