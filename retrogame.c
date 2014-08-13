/*
ADAFRUIT RETROGAME UTILITY: remaps buttons on Raspberry Pi GPIO header
to virtual USB keyboard presses.  Great for classic game emulators!
Retrogame is interrupt-driven and efficient (usually under 0.3% CPU use)
and debounces inputs for glitch-free gaming.

Connect one side of button(s) to GND pin (there are several on the GPIO
header, but see later notes) and the other side to GPIO pin of interest.
Internal pullups are used; no resistors required.  Avoid pins 8 and 10;
these are configured as a serial port by default on most systems (this
can be disabled but takes some doing).  Pin configuration is currently
set in global table; no config file yet.  See later comments.

Must be run as root, i.e. 'sudo ./retrogame &' or configure init scripts
to launch automatically at system startup.

Requires uinput kernel module.  This is typically present on popular
Raspberry Pi Linux distributions but not enabled on some older varieties.
To enable, either type:

    sudo modprobe uinput

Or, to make this persistent between reboots, add a line to /etc/modules:

    uinput

Prior versions of this code, when being compiled for use with the Cupcade
or PiGRRL projects, required CUPCADE to be #defined.  This is no longer
the case; instead a test is performed to see if a PiTFT is connected, and
one of two I/O tables is automatically selected.

Written by Phil Burgess for Adafruit Industries, distributed under BSD
License.  Adafruit invests time and resources providing this open source
code, please support Adafruit and open-source hardware by purchasing
products from Adafruit!


Copyright (c) 2013 Adafruit Industries.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/i2c-dev.h>

// Defines to change limits and compiled behavior.
#define MAX_MCP  4   // Maximum number of MCP23017 devices.
#define MAX_PINS 64  // Maximum number of pins that can be monitored.
#define DEBUG 1      // Set to 1 to output debug messages, otherwise 0 to disable.

// START HERE ------------------------------------------------------------
// This table remaps GPIO inputs to keyboard values.  In this initial
// implementation there's a 1:1 relationship (can't attach multiple keys
// to a button) and the list is fixed in code; there is no configuration
// file.  Buttons physically connect between GPIO pins and ground.  There
// are only a few GND pins on the GPIO header, so a breakout board is
// often needed.  If you require just a couple extra ground connections
// and have unused GPIO pins, set the corresponding key value to GND to
// create a spare ground point.

#define GND -1
#define SOURCE_GPIO -1
#define SOURCE_MCP1  0
#define SOURCE_MCP2  1
struct {
	int pin;
	int key;
	int source;
} *io, // In main() this pointer is set to one of the two tables below.
   ioTFT[] = {
	// This pin/key table is used if an Adafruit PiTFT display
	// is detected (e.g. Cupcade or PiGRRL).
	// Input   Output (from /usr/include/linux/input.h)
	{   2,     KEY_LEFT,    SOURCE_GPIO },   // Joystick (4 pins)
	{   3,     KEY_RIGHT,   SOURCE_GPIO },
	{   4,     KEY_DOWN,    SOURCE_GPIO },
	{  17,     KEY_UP,      SOURCE_GPIO },
	{  27,     KEY_Z,       SOURCE_GPIO },   // A/Fire/jump/primary
	{  22,     KEY_X,       SOURCE_GPIO },   // B/Bomb/secondary
	{  23,     KEY_R,       SOURCE_GPIO },   // Credit
	{  18,     KEY_Q,       SOURCE_GPIO },   // Start 1P
	{  -1,     -1           -1          } }, // END OF LIST, DO NOT CHANGE
	// MAME must be configured with 'z' & 'x' as buttons 1 & 2 -
	// this was required for the accompanying 'menu' utility to
	// work (catching crtl/alt w/ncurses gets totally NASTY).
	// Credit/start are likewise moved to 'r' & 'q,' reason being
	// to play nicer with certain emulators not liking numbers.
	// GPIO options are 'maxed out' with PiTFT + above table.
	// If additional buttons are desired, will need to disable
	// serial console and/or use P5 header.  Or use keyboard.
   ioStandard[] = {
	// This pin/key table is used when the PiTFT isn't found
	// (using HDMI or composite instead), as with our original
	// retro gaming guide.
	// Input   Output (from /usr/include/linux/input.h)
	{  25,     KEY_LEFT,     SOURCE_GPIO },   // Joystick (4 pins)
	{   9,     KEY_RIGHT,    SOURCE_GPIO },
	{  10,     KEY_UP,       SOURCE_GPIO },
	{  17,     KEY_DOWN,     SOURCE_GPIO },
	{  23,     KEY_LEFTCTRL, SOURCE_GPIO },   // A/Fire/jump/primary
	{   7,     KEY_LEFTALT,  SOURCE_GPIO },   // B/Bomb/secondary
	{   0,     KEY_X,        SOURCE_MCP1 },   // MCP Example configuration
	{   1,     GND,          SOURCE_MCP1 },   // Move this to comments!
	{   2,     KEY_Z,        SOURCE_MCP1 },
	// For credit/start/etc., use USB keyboard or add more buttons.
	{  -1,     -1            -1          } }; // END OF LIST, DO NOT CHANGE

// A "Vulcan nerve pinch" (holding down a specific button combination
// for a few seconds) issues an 'esc' keypress to MAME (which brings up
// an exit menu or quits the current game).  The button combo is
// configured with a bitmask corresponding to elements in the above io[]
// array.  The default value here uses elements 6 and 7 (credit and start
// in the Cupcade pinout).  If you change this, make certain it's a combo
// that's not likely to occur during actual gameplay (i.e. avoid using
// joystick directions or hold-for-rapid-fire buttons).
// Also key auto-repeat times are set here.  This is for navigating the
// game menu using the 'gamera' utility; MAME disregards key repeat
// events (as it should).
const unsigned long vulcanMask = (1L << 6) | (1L << 7);
const int           vulcanKey  = KEY_ESC, // Keycode to send
                    vulcanTime = 1500,    // Pinch time in milliseconds
                    repTime1   = 500,     // Key hold time to begin repeat
                    repTime2   = 100;     // Time between key repetitions

// Define list of MCP devices.
// Must set the path to the I2C bus, the I2C bus address, and the GPIO
// which is connected to an interrupt pin on the MCP device.
struct {
	char* path;
	uint16_t address;
	int pin;
} *mcp,
   mcpList[] = {
	{ "/dev/i2c-1", 0x20, 18 },
	//{ "/dev/i2c-1", 0x21, 23 },
	{ 0,               0,  0 } // Sentinel to signal end of MCP list.
};

// A few globals ---------------------------------------------------------

char
  *progName,                         // Program name (for error reporting)
   sysfs_root[] = "/sys/class/gpio", // Location of Sysfs GPIO files
   running      = 1;                 // Signal handler will set to 0 (exit)
volatile unsigned int
  *gpio;                             // GPIO register table
const int
   debounceTime = 20;                // 20 ms for button debouncing
int
   mcplen = 0,                       // Number of MCP23017 devices.
   mcpfd[MAX_MCP];                   // MCP23017 devices file descriptors.


// Some utility functions ------------------------------------------------

#define DEBUG_PRINT(...) { if (DEBUG) printf(__VA_ARGS__); }

// Set one GPIO pin attribute through the Sysfs interface.
int pinConfig(int pin, char *attr, char *value) {
	char filename[50];
	int  fd, w, len = strlen(value);
	sprintf(filename, "%s/gpio%d/%s", sysfs_root, pin, attr);
	if((fd = open(filename, O_WRONLY)) < 0) return -1;
	w = write(fd, value, len);
	close(fd);
	return (w != len); // 0 = success
}

// Un-export any Sysfs pins used; don't leave filesystem cruft.  Also
// restores any GND pins to inputs.  Write errors are ignored as pins
// may be in a partially-initialized state.
void cleanup() {
	char buf[50];
	int  fd, i;
	sprintf(buf, "%s/unexport", sysfs_root);
	if((fd = open(buf, O_WRONLY)) >= 0) {
		for(i=0; io[i].pin >= 0; i++) {
			// Restore GND items to inputs
			if(io[i].key == GND)
				pinConfig(io[i].pin, "direction", "in");
			// And un-export all items regardless
			sprintf(buf, "%d", io[i].pin);
			write(fd, buf, strlen(buf));
		}
		close(fd);
	}
	// Close MCP device files.
	for (i=0; i<mcplen; ++i) {
		close(mcpfd[i]);
	}
}

// Quick-n-dirty error reporter; print message, clean up and exit.
void err(char *msg) {
	printf("%s: %s.\n", progName, msg);
	cleanup();
	exit(1);
}

// Interrupt handler -- set global flag to abort main loop.
void signalHandler(int n) {
	DEBUG_PRINT("Received INT or KILL signal.\n");
	running = 0;
}

// Returns 1 if running on early Pi board, 0 otherwise.
// Relies on info in /proc/cmdline by default; if this is
// unreliable in the future, easy change to /proc/cpuinfo.
int isRevOnePi(void) {
	FILE *fp;
	char  buf[1024], *ptr;
	int   n, rev = 0;
#if 1
	char *filename = "/proc/cmdline",
	     *token    = "boardrev=",
	     *fmt      = "%x";
#else
	char *filename = "/proc/cpuinfo",
	     *token    = "Revision", // Capital R!
	     *fmt      = " : %x";
#endif

	if((fp = fopen(filename, "r"))) {
		if((n = fread(buf, 1, sizeof(buf)-1, fp)) > 0) {
			buf[n] = 0;
			if((ptr = strstr(buf, token))) {
				sscanf(&ptr[strlen(token)], fmt, &rev);
			}
		}
		fclose(fp);
	}

	return ((rev == 0x02) || (rev == 0x03));
}

// Read a 16 bit register value from an MCP device.
uint16_t mcpReadReg16(int fd, uint8_t reg) {
	uint16_t value = 0;
	uint8_t buffer[2];
	buffer[0] = reg;
	if (write(fd, buffer, 1) != 1) 
		err("Can't write MCP register!");
	if (read(fd, buffer, 2) != 2) 
		err("Can't read MCP register!");
	value = (buffer[1] << 8) | buffer[0];
	DEBUG_PRINT("Read 0x%04X from register 0x%02X on I2C device %d\n", value, reg, fd);
	return value;
}

// Read a 8 bit register value from an MCP device.
uint8_t mcpReadReg8(int fd, uint8_t reg) {
	uint8_t buffer = reg;
	if (write(fd, &buffer, 1) != 1) 
		err("Can't write MCP register!");
	if (read(fd, &buffer, 1) != 1) 
		err("Can't read MCP register!");
	DEBUG_PRINT("Read 0x%02X from register 0x%02X on I2C device %d\n", buffer, reg, fd);
	return buffer;
}

// Write a 16 bit register value to an MCP device.
void mcpWriteReg16(int fd, uint8_t reg, uint16_t value) {
	uint8_t buffer[3];
	buffer[0] = reg;
	buffer[1] = value & 0xFF;
	buffer[2] = (value >> 8) & 0xFF;
	if (write(fd, buffer, 3) != 3) 
		err("Can't write MCP register!");
	DEBUG_PRINT("Wrote 0x%04X to register 0x%02X on I2C device %d\n", value, reg, fd);
}

// Write a 8 bit register value to an MCP device.
void mcpWriteReg8(int fd, uint8_t reg, uint8_t value) {
	uint8_t buffer[2];
	buffer[0] = reg;
	buffer[1] = value;
	if (write(fd, buffer, 2) != 2) 
		err("Can't write MCP register!");
	DEBUG_PRINT("Wrote 0x%02X to register 0x%02X on I2C device %d\n", value, reg, fd);
}

// Main stuff ------------------------------------------------------------

#define BCM2708_PERI_BASE 0x20000000
#define GPIO_BASE         (BCM2708_PERI_BASE + 0x200000)
#define BLOCK_SIZE        (4*1024)
#define GPPUD             (0x94 / 4)
#define GPPUDCLK0         (0x98 / 4)

// MCP23017 register addresses
#define IODIRA		0x00	// bank A GPIO pin address direction
#define IODIRB		0x01	// bank B GPIO pin address direction
#define GPINTENA	0x04	// enable interrupt on change of GPIOA register
#define GPINTENB	0x05	// enable interrupt on change of GPIOB register
#define GPPUA		0x0C	// bank A pull-up register address
#define GPPUB		0x0D	// bank B pull-up register address
#define INTCAPA		0x10	// store GPIOA register values on interrupt
#define INTCAPB		0x11	// store GPIOB register values on interrupt
#define GPIOA		0x12	// bank A GPIO pin address
#define GPIOB		0x13	// bank B GPIO pin address
#define IOCON       0x0A    // control register address

int main(int argc, char *argv[]) {

	DEBUG_PRINT("Starting...\n");

	// A few arrays here are declared with 32 elements, even though
	// values aren't needed for io[] members where the 'key' value is
	// GND.  This simplifies the code a bit -- no need for mallocs and
	// tests to create these arrays -- but may waste a handful of
	// bytes for any declared GNDs.
	char                   buf[50],      // For sundry filenames
	                       c;            // Pin input value ('0'/'1')
	int                    fd,           // For mmap, sysfs, uinput
	                       i, j, k,      // Asst. counter
	                       bitmask,      // Pullup enable bitmask
	                       timeout = -1, // poll() timeout
	                       intstate[MAX_PINS], // Last-read state
	                       extstate[MAX_PINS], // Debounced state
	                       lastKey = -1, // Last key down (for repeat)
	                       curpin,       // Current MCP pin value
	                       oldpin;       // Old MCP pin value
	unsigned long          bitMask, bit; // For Vulcan pinch detect
	volatile unsigned char shortWait;    // Delay counter
	struct input_event     keyEv, synEv; // uinput events
	struct pollfd          p[MAX_PINS];  // GPIO file descriptors
	uint16_t               reg,          // MCP register value.
	                       mcpOld[MAX_MCP]; // MCP chip last read states.

	progName = argv[0];             // For error reporting
	signal(SIGINT , signalHandler); // Trap basic signals (exit cleanly)
	signal(SIGKILL, signalHandler);

	// Default to MCP list in code.
	mcp = mcpList;

	// Count the number of MCP devices and initialize MCP last input buffers.
	mcplen = 0;
	while (mcp[mcplen].path != 0) {
		mcpOld[mcplen] = 0;
		mcplen++;
	}
	DEBUG_PRINT("Using %d MCP23017 devices.\n", mcplen);

	// Select io[] table for Cupcade (TFT) or 'normal' project.
	io = (access("/etc/modprobe.d/adafruit.conf", F_OK) ||
	      access("/dev/fb1", F_OK)) ? ioStandard : ioTFT;

	// If this is a "Revision 1" Pi board (no mounting holes),
	// remap certain pin numbers in the io[] array for compatibility.
	// This way the code doesn't need modification for old boards.
	if(isRevOnePi()) {
		for(i=0; io[i].pin >= 0; i++) {
			if(     io[i].pin ==  2 && io[i].source == SOURCE_GPIO) io[i].pin = 0;
			else if(io[i].pin ==  3 && io[i].source == SOURCE_GPIO) io[i].pin = 1;
			else if(io[i].pin == 27 && io[i].source == SOURCE_GPIO) io[i].pin = 21;
		}
	}

	// ----------------------------------------------------------------
	// Initialize MCP23017 devices.

	for (i=0; i<mcplen; ++i) {
		// Open I2C bus.
		if ((mcpfd[i] = open(mcp[i].path, O_RDWR)) < 0)
			err("Can't open I2C bus!");
		// Set I2C slave address.
		if (ioctl(mcpfd[i], I2C_SLAVE, mcp[i].address) < 0)
			err("Can't set I2C address");
		// Set IOCON interrupt mirroring bit to 1, all others to 0.
		mcpWriteReg8(mcpfd[i], IOCON, 0x40);
	}

	// ----------------------------------------------------------------
	// Although Sysfs provides solid GPIO interrupt handling, there's
	// no interface to the internal pull-up resistors (this is by
	// design, being a hardware-dependent feature).  It's necessary to
	// grapple with the GPIO configuration registers directly to enable
	// the pull-ups.  Based on GPIO example code by Dom and Gert van
	// Loo on elinux.org

	if((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		err("Can't open /dev/mem  Make sure to run with sudo, like 'sudo ./retrogame'");
	gpio = mmap(            // Memory-mapped I/O
	  NULL,                 // Any adddress will do
	  BLOCK_SIZE,           // Mapped block length
	  PROT_READ|PROT_WRITE, // Enable read+write
	  MAP_SHARED,           // Shared with other processes
	  fd,                   // File to map
	  GPIO_BASE );          // Offset to GPIO registers
	close(fd);              // Not needed after mmap()
	if(gpio == MAP_FAILED) err("Can't mmap()");
	// Make combined bitmap of pullup-enabled pins:
	for(bitmask=i=0; io[i].pin >= 0; i++)
		if(io[i].key != GND && io[i].source == SOURCE_GPIO) bitmask |= (1 << io[i].pin);
	gpio[GPPUD]     = 2;                    // Enable pullup
	for(shortWait=150;--shortWait;);        // Min 150 cycle wait
	gpio[GPPUDCLK0] = bitmask;              // Set pullup mask
	for(shortWait=150;--shortWait;);        // Wait again
	gpio[GPPUD]     = 0;                    // Reset pullup registers
	gpio[GPPUDCLK0] = 0;
	(void)munmap((void *)gpio, BLOCK_SIZE); // Done with GPIO mmap()

	// ----------------------------------------------------------------
	// Initialize MCP23017 pins to inputs and outputs.

	for (i=0; io[i].pin >= 0; ++i) {
		// Enable input, pull-up resistors, and interrupts for input pins.
		if(io[i].key != GND && io[i].source != SOURCE_GPIO) {
			// Read the MCP direction register value, flip the appropriate
			// bit, and then write the new register value.  This isn't
			// the most efficient update, but it's only done once at the
			// start.
			reg = mcpReadReg16(mcpfd[io[i].source], IODIRA);
			reg |= (1 << io[i].pin);
			mcpWriteReg16(mcpfd[io[i].source], IODIRA, reg);
			// Enable pull-up resistor.
			reg = mcpReadReg16(mcpfd[io[i].source], GPPUA);
			reg |= (1 << io[i].pin);
			mcpWriteReg16(mcpfd[io[i].source], GPPUA, reg);
			// Enable interrupts on change.
			reg = mcpReadReg16(mcpfd[io[i].source], GPINTENA);
			reg |= (1 << io[i].pin);
			mcpWriteReg16(mcpfd[io[i].source], GPINTENA, reg);
		}
		// Set output to low/ground for ground pins.
		else if (io[i].key == GND && io[i].source != SOURCE_GPIO) {
			// Set pin as output.
			reg = mcpReadReg16(mcpfd[io[i].source], IODIRA);
			reg &= ~(1 << io[i].pin);
			mcpWriteReg16(mcpfd[io[i].source], IODIRA, reg);
			// Set low value for pin.
			reg = mcpReadReg16(mcpfd[io[i].source], GPIOA);
			reg &= ~(1 << io[i].pin);
			mcpWriteReg16(mcpfd[io[i].source], GPIOA, reg);
		}
	}

	// Get initial MCP input state and clear any pending interrupts.
	for (i=0; i<mcplen; ++i) {
		mcpOld[i] = mcpReadReg16(mcpfd[i], GPIOA);
		DEBUG_PRINT("MCP device %d has initial value %04X.\n", i, mcpOld[i]);
	}

	// ----------------------------------------------------------------
	// All other GPIO config is handled through the sysfs interface.

	sprintf(buf, "%s/export", sysfs_root);
	if((fd = open(buf, O_WRONLY)) < 0) // Open Sysfs export file
		err("Can't open GPIO export file");
	for(i=j=0; io[i].pin >= 0; i++) { // For each pin of interest...
		sprintf(buf, "%d", io[i].pin);
		write(fd, buf, strlen(buf));             // Export pin
		pinConfig(io[i].pin, "active_low", "0"); // Don't invert
		if(io[i].key == GND && io[i].source == SOURCE_GPIO) {
			// Set pin to output, value 0 (ground)
			if(pinConfig(io[i].pin, "direction", "out") ||
			   pinConfig(io[i].pin, "value"    , "0"))
				err("Pin config failed (GND)");
		} 
		else if (io[i].source == SOURCE_GPIO) {
			// Set pin to input, detect rise+fall events
			if(pinConfig(io[i].pin, "direction", "in") ||
			   pinConfig(io[i].pin, "edge"     , "both"))
				err("Pin config failed");
			// Get initial pin value
			sprintf(buf, "%s/gpio%d/value",
			  sysfs_root, io[i].pin);
			// The p[] file descriptor array isn't necessarily
			// aligned with the io[] array.  GND keys in the
			// latter are skipped, but p[] requires contiguous
			// entries for poll().  So the pins to monitor are
			// at the head of p[], and there may be unused
			// elements at the end for each GND.  Same applies
			// to the intstate[] and extstate[] arrays.
			// Note that p[] and intstate[]/extstate[] also
			// include MCP interrupt pins which are also
			// monitored for changes.
			if((p[j].fd = open(buf, O_RDONLY)) < 0)
				err("Can't access pin value");
			intstate[j] = 0;
			if((read(p[j].fd, &c, 1) == 1) && (c == '0'))
				intstate[j] = 1;
			extstate[j] = intstate[j];
			p[j].events  = POLLPRI; // Set up poll() events
			p[j].revents = 0;
			j++;
		}
	}
	// Add MCP device interrupt pins to monitored GPIO pins.
	for (i=0; i<mcplen; ++i) {
		// Set pin to input, detect rise+fall events
		sprintf(buf, "%d", mcp[i].pin);
		write(fd, buf, strlen(buf));  // Export pin
		if(pinConfig(mcp[i].pin, "direction", "in") ||
		   pinConfig(mcp[i].pin, "edge"     , "both"))
			err("Pin config failed");
		// Get initial pin value
		sprintf(buf, "%s/gpio%d/value",
		  sysfs_root, mcp[i].pin);
		if((p[j].fd = open(buf, O_RDONLY)) < 0)
			err("Can't access pin value");
		intstate[j] = 0;
		if((read(p[j].fd, &c, 1) == 1) && (c == '0'))
			intstate[j] = 1;
		extstate[j] = intstate[j];
		p[j].events  = POLLPRI; // Set up poll() events
		p[j].revents = 0;
		j++;
	}
	// 'j' is now count of monitorired pins in io[] table 
	// (non-GND items + MCP device interrupt pins)
	close(fd); // Done exporting

	// ----------------------------------------------------------------
	// Set up uinput

#if 1
	// Retrogame normally uses /dev/uinput for generating key events.
	// Cupcade requires this and it's the default.  SDL2 (used by
	// some newer emulators) doesn't like it, wants /dev/input/event0
	// instead.  Enable that code by changing to "#if 0" above.
	if((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0)
		err("Can't open /dev/uinput");
	if(ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		err("Can't SET_EVBIT");
	for(i=0; io[i].pin >= 0; i++) {
		if(io[i].key != GND) {
			if(ioctl(fd, UI_SET_KEYBIT, io[i].key) < 0)
				err("Can't SET_KEYBIT");
		}
	}
	if(ioctl(fd, UI_SET_KEYBIT, vulcanKey) < 0) err("Can't SET_KEYBIT");
	struct uinput_user_dev uidev;
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "retrogame");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor  = 0x1;
	uidev.id.product = 0x1;
	uidev.id.version = 1;
	if(write(fd, &uidev, sizeof(uidev)) < 0)
		err("write failed");
	if(ioctl(fd, UI_DEV_CREATE) < 0)
		err("DEV_CREATE failed");
#else // SDL2 prefers this event methodology
	if((fd = open("/dev/input/event0", O_WRONLY | O_NONBLOCK)) < 0)
		err("Can't open /dev/input/event0");
#endif

	// Initialize input event structures
	memset(&keyEv, 0, sizeof(keyEv));
	keyEv.type  = EV_KEY;
	memset(&synEv, 0, sizeof(synEv));
	synEv.type  = EV_SYN;
	synEv.code  = SYN_REPORT;
	synEv.value = 0;

	// 'fd' is now open file descriptor for issuing uinput events


	// ----------------------------------------------------------------
	// Monitor GPIO file descriptors for button events.  The poll()
	// function watches for GPIO IRQs in this case; it is NOT
	// continually polling the pins!  Processor load is near zero.

	DEBUG_PRINT("Waiting for input...\n");
	while(running) { // Signal handler can set this to 0 to exit
		// Wait for IRQ on pin (or timeout for button debounce)
		if(poll(p, j, timeout) > 0) { // If IRQ...
			for(i=0; i<j; i++) {       // Scan non-GND pins...
				if(p[i].revents) { // Event received?
					// Read current pin state, store
					// in internal state flag, but
					// don't issue to uinput yet --
					// must wait for debounce!
					lseek(p[i].fd, 0, SEEK_SET);
					read(p[i].fd, &c, 1);
					if(c == '0')      intstate[i] = 1;
					else if(c == '1') intstate[i] = 0;
					p[i].revents = 0; // Clear flag
				}
			}
			timeout = debounceTime; // Set timeout for debounce
			c       = 0;            // Don't issue SYN event
			// Else timeout occurred
		} else if(timeout == debounceTime) { // Button debounce timeout
			// 'j' (number of non-GNDs) is re-counted as
			// it's easier than maintaining an additional
			// remapping table or a duplicate key[] list.
			bitMask = 0L; // Mask of buttons currently pressed
			bit     = 1L;
			for(c=i=j=0; io[i].pin >= 0; i++, bit<<=1) {
				if(io[i].key != GND && io[i].source == SOURCE_GPIO) {
					// Handle GPIO pin changes.
					// Compare internal state against
					// previously-issued value.  Send
					// keystrokes only for changed states.
					if(intstate[j] != extstate[j]) {
						extstate[j] = intstate[j];
						keyEv.code  = io[i].key;
						keyEv.value = intstate[j];
						write(fd, &keyEv,
						  sizeof(keyEv));
						c = 1; // Follow w/SYN event
						DEBUG_PRINT("Input %d changed, sent key %d value %d.\n", i, keyEv.code, keyEv.value);
						if(intstate[j]) { // Press?
							// Note pressed key
							// and set initial
							// repeat interval.
							lastKey = i;
							timeout = repTime1;
						} else { // Release?
							// Stop repeat and
							// return to normal
							// IRQ monitoring
							// (no timeout).
							lastKey = timeout = -1;
						}
					}
					j++;
					if(intstate[i]) bitMask |= bit;
				}
			}
			// Handle MCP pin changes if the MCP interrupt pin changed.
			for (i=0; i<mcplen; ++i) {
				// Check if MCP state changed.
				if(intstate[j] != extstate[j]) {
					extstate[j] = intstate[j];
					if(intstate[j]) { // MCP interrupt occured (active low)
						// Grab the current input state from the chip.
						reg = mcpReadReg16(mcpfd[i], GPIOA);
						// Compare input state to previous for each of this
						// MCP chip's inputs.
						bit = 1L;
						for (k=0; io[k].pin >= 0; k++, bit<<=1) {
							// Filter to just the defined inputs for this MCP chip.
							if (io[k].source == i) {
								// Check if the pin changed state.
								oldpin = mcpOld[i] & (1 << io[k].pin);
								curpin = reg       & (1 << io[k].pin);
								if (oldpin != curpin) {
									// State changed, fire key event.
									keyEv.code  = io[k].key;
									// Note button is pressed when curpin is low
									// because buttons pull to ground when pressed.
									keyEv.value = !curpin ? 1 : 0;
									write(fd, &keyEv, sizeof(keyEv));
									c = 1; // Follow w/SYN event
									DEBUG_PRINT("Input %d changed, sent key %d value %d.\n", k, keyEv.code, keyEv.value);
									if(!curpin) { // Press?
										// Note pressed key
										// and set initial
										// repeat interval.
										lastKey = k;
										timeout = repTime1;
									} else { // Release?
										// Stop repeat and
										// return to normal
										// IRQ monitoring
										// (no timeout).
										lastKey = timeout = -1;
									}
								}
								// Update vulcan bitmask if key is pressed.
								if (!curpin) bitMask |= bit;
							}
						}
						// Fire appropriate key events. (set c=1, lastkey, timeout)
						// Set previous input state to current.
						mcpOld[i] = reg;
					}
				}
				j++;
			}

			// If the "Vulcan nerve pinch" buttons are pressed,
			// set long timeout -- if this time elapses without
			// a button state change, esc keypress will be sent.
			if((bitMask & vulcanMask) == vulcanMask)
				timeout = vulcanTime;
		} else if(timeout == vulcanTime) { // Vulcan timeout occurred
			// Send keycode (MAME exits or displays exit menu)
			keyEv.code = vulcanKey;
			for(i=1; i>= 0; i--) { // Press, release
				keyEv.value = i;
				write(fd, &keyEv, sizeof(keyEv));
				usleep(10000); // Be slow, else MAME flakes
				write(fd, &synEv, sizeof(synEv));
				usleep(10000);
				DEBUG_PRINT("Sent vulcan key press.\n");
			}
			timeout = -1; // Return to normal processing
			c       = 0;  // No add'l SYN required
		} else if(lastKey >= 0) { // Else key repeat timeout
			if(timeout == repTime1) timeout = repTime2;
			else if(timeout > 30)   timeout -= 5; // Accelerate
			c           = 1; // Follow w/SYN event
			keyEv.code  = io[lastKey].key;
			keyEv.value = 2; // Key repeat event
			write(fd, &keyEv, sizeof(keyEv));
		}
		if(c) write(fd, &synEv, sizeof(synEv));
	}

	// ----------------------------------------------------------------
	// Clean up

	ioctl(fd, UI_DEV_DESTROY); // Destroy and
	close(fd);                 // close uinput
	cleanup();                 // Un-export pins

	puts("Done.");

	return 0;
}
