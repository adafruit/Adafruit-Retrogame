#!/usr/bin/python

"""Adafruit Arcade Bonnet handler

Somewhat minimal Adafruit Arcade Bonnet handler.  Runs in background,
translates inputs from MCP23017 port expander to virtual USB keyboard
events.  Not -quite- as efficient or featuretastic as retrogame, but
still reasonably lightweight and may be easier and/or more reliable than
retrogame for some users.  Supports ONE port expander, no regular GPIO
(non-port-expander) or "Vulcan nerve pinch" features.

Prerequisites:

i   sudo apt-get install python-pip python-smbus python-dev
    sudo pip install evdev

Be sure to enable I2C via raspi-config.  Also, udev rules will need to
be set up per retrogame directions.

Credit to Pimoroni for Picade HAT scripts as starting point.
"""

import os
import time
import RPi.GPIO as gpio
from evdev import UInput
from evdev import ecodes as e
from smbus import SMBus

key = [
    # EDIT KEYCODES IN THIS TABLE TO YOUR PREFERENCES:
    # See /usr/include/linux/input.h for keycode names
    # Keyboard        Bonnet        EmulationStation
    e.KEY_LEFTCTRL,   # 1A            'A' button
    e.KEY_LEFTALT,    # 1B            'B' button
    e.KEY_Z,          # 1C            'X' button
    e.KEY_X,          # 1D            'Y' button
    e.KEY_SPACE,      # 1E            'Select' button
    e.KEY_ENTER,      # 1F            'Start' button
    0,                # Bit 6 NOT CONNECTED on Bonnet
    0,                # Bit 7 NOT CONNECTED on Bonnet
    e.KEY_DOWN,       # 4-way down    D-pad down
    e.KEY_UP,         # 4-way up      D-pad up
    e.KEY_RIGHT,      # 4-way right   D-pad right
    e.KEY_LEFT,       # 4-way left    D-pad left
    e.KEY_L,          # Analog right
    e.KEY_H,          # Analog left
    e.KEY_J,          # Analog down
    e.KEY_K           # Analog up
]

addr = 0x26  # I2C address of Arcade bonnet MCP23017
# addr = 0x27  # Alternate I2C address of Arcade bonnet MCP23017
irqPin = 17  # IRQ pin for MCP23017

# MCP23017 register addresses, when in bank 0 (interleaved) mode

IODIRA = 0x00
IOCONA = 0x0A
INTCAPA = 0x10
INTCAPA = 0x11
GPIOA = 0x12
GPIOB = 0x13


def mcp_irq(pin):
    """Callback for MCP23017 interrupt request

    This routine is called to handle interrupt requests from the MCP23017
    device. Interrupts are generated when any of the GPIO ports change
    state. The port status is read, and key events are generated for each
    pin that changed state from the previous call. A simple de-dup is
    implemented by ignoring cases where the port status has not changed
    since the previous call

    """
    global oldState

    x = bus.read_i2c_block_data(addr, GPIOA, 2)
    newState = (x[1] << 8) | x[0]
    # quick event dedup - check for no change since last interrupt.
    if newState == oldState:
        return
    # something changed - figure out the differences and generate
    # key events.
    for i in range(16):
        bit = 1 << i
        lvl = newState & bit
        if lvl != (oldState & bit):
            ui.write(e.EV_KEY, key[i], 0 if lvl else 1)
    ui.syn()
    oldState = newState


# main program flow starts here

os.system("sudo modprobe uinput")

ui = UInput({e.EV_KEY: key}, name="retrogame", bustype=e.BUS_USB)
bus = SMBus(1)

# Configure the MCP23017 GPIO Extender
#
# We put the device into 'BANK=0' mode, which pairs the A/B device
# registers in sequential addresses. The address of the IOCON register
# changes depending on the bank mode. In bank=1 mode, the IOCON register
# is at address 0x05, so we write to that address.  If the device was
# already in bank=0 mode, the write to address 0x05 (GPINTENB) will be
# harmless since we reconfigure the whole device.

bus.write_byte_data(addr, 0x05, 0x00)  # If bank 1, switch to 0
bus.write_byte_data(addr, IOCONA, 0x44)  # Bank 0, INTB=A, seq, OD IRQ

# Read/modify/write remaining MCP23017 config.
# We enable the pins as inputs, enable pullups,
# enable interrupts on change, and set polarity to normal.

cfg = bus.read_i2c_block_data(addr, IODIRA, 14)
cfg[0] = 0xFF  # Input bits
cfg[1] = 0xFF
cfg[2] = 0x00  # Polarity
cfg[3] = 0x00
cfg[4] = 0xFF  # Interrupt pins
cfg[5] = 0xFF
cfg[12] = 0xFF  # Pull-ups
cfg[13] = 0xFF
bus.write_i2c_block_data(addr, IODIRA, cfg)

# GPIO init
gpio.setwarnings(False)
gpio.setmode(gpio.BCM)

oldState = 0  # used to detect changes since last event

# Enable pullup and callback on MCP23017 IRQ pin
# All initalization should be complete before this point,
# since our handler can now be called at any time

gpio.setup(irqPin, gpio.IN, pull_up_down=gpio.PUD_UP)
gpio.add_event_detect(irqPin, gpio.FALLING, callback=mcp_irq)

while True:
    # Run forever and wait for interrupts, which are handled by
    # mcp_irq() on  a separate thread.
    #
    # We regularly read the GPIOA registers (and ignore the result), which
    # re-enables interrupts if one was missed for any reason. This avoids
    # 'hangs' where the irq handler never gets called because interrupts are
    # disabled but interupts will never be enabled unless a read occurs.

    time.sleep(0.1)
    x = bus.read_i2c_block_data(addr, GPIOA, 2)
