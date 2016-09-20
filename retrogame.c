/*
ADAFRUIT RETROGAME UTILITY: remaps buttons on Raspberry Pi GPIO header to
virtual USB keyboard presses.  Great for classic game emulators!  Retrogame
is interrupt-driven and efficient (typically < 0.3% CPU use, even w/heavy
button-mashing) and debounces inputs for glitch-free gaming.

****** IF YOU ARE SEARCHING FOR THE ioStandard[] OR ioTFT[] TABLES: ******
GPIO pin and key mapping is now set in a configuration file; there's no
fixed table to edit in this code (as in earlier releases).  An example
config file is provided in retrogame.cfg.  By default, retrogame looks for
this file in /boot, but an alternate (full pathname) can be passed as a
command line argument.

Connect one side of button(s) to GND pin (there are several on the GPIO
header, but see later notes) and the other side to GPIO pin of interest.
Internal pullups are used; no resistors required.

Must be run as root, i.e. 'sudo ./retrogame &' or edit /etc/rc.local to
launch automatically at system startup.

Early Raspberry Pi Linux distributions might not have the uinput kernel
module installed by default.  To enable this, add a line to /etc/modules:

    uinput

Written by Phil Burgess for Adafruit Industries, distributed under BSD
License.  Adafruit invests time and resources providing this open source
code, please support Adafruit and open-source hardware by purchasing
products from Adafruit!

Copyright (c) 2013, 2016 Adafruit Industries.
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "keyTable.h"

// Global variables and such -----------------------------------------------

bool
   running      = true;              // Signal handler will set false (exit)
extern char
  *__progname,                       // Program name (for error reporting)
  *program_invocation_name;          // Full name as invoked (path, etc.)
char
   sysfs_root[] = "/sys/class/gpio", // Location of Sysfs GPIO files
  *cfgPath,                          // Directory containing config file
  *cfgName      = NULL,              // Name (no path) of config
  *cfgPathname,                      // Full path/name to config file
   board;                            // 0=Pi1Rev1, 1=Pi1Rev2, 2=Pi2/Pi3
int
   key[32],                          // Keycodes assigned to GPIO pins
   fileWatch,                        // inotify file descriptor
   intstate[32],                     // Button last-read state
   extstate[32],                     // Button debounced state
   keyfd1       = -1,                // /dev/uinput file descriptor
   keyfd2       = -1,                // /dev/input/eventX file descriptor
   keyfd        = -1,                // = (keyfd2 >= 0) ? keyfd2 : keyfd1;
   vulcanKey    = KEY_RESERVED,      // 'Vulcan pinch' keycode to send
   vulcanTime   = 1500,              // Pinch time in milliseconds
   debounceTime = 20,                // 20 ms for button debouncing
   repTime1     = 500,               // Key hold time to begin repeat
   repTime2     = 100;               // Time between key repetitions
   // Note: auto-repeat is for navigating the game-selection menu using the
   // 'gamera' utility; MAME disregards key repeat events (as it should).
uint32_t
   vulcanMask   = 0;                 // Bitmask of 'Vulcan nerve pinch' combo
volatile unsigned int
  *gpio;                             // GPIO register table
struct pollfd
   p[35];                            // File descriptors for poll()

enum commandNum {
	CMD_NONE, // Used during config file scanning (no command ID'd yet)
	CMD_KEY,  // Key-to-GPIO mapping command
	CMD_GND   // Pin-to-ground assignment
};

// dict of config file commands that AREN'T keys (KEY_*)
dict command[] = { // Struct is defined in keyTable.h
	{ "GND"   , CMD_GND },
	{ "GROUND", CMD_GND },
	// Might add commands here for fine-tuning debounce & repeat settings
	{  NULL   , -1      } // END-OF-LIST
};

#define PI1_BCM2708_PERI_BASE 0x20000000
#define PI1_GPIO_BASE         (PI1_BCM2708_PERI_BASE + 0x200000)
#define PI2_BCM2708_PERI_BASE 0x3F000000
#define PI2_GPIO_BASE         (PI2_BCM2708_PERI_BASE + 0x200000)
#define BLOCK_SIZE            (4*1024)
#define GPPUD                 (0x94 / 4)
#define GPPUDCLK0             (0x98 / 4)

#define GND                   KEY_CNT


// Some utility functions --------------------------------------------------

// Detect Pi board type.  Not detailed, just enough for GPIO compatibilty:
// 0 = Pi 1 Model B revision 1
// 1 = Pi 1 Model B revision 2, Model A, Model B+, Model A+
// 2 = Pi 2 Model B or Pi 3
static int boardType(void) {
	FILE *fp;
	char  buf[1024], *ptr;
	int   n, board = 1; // Assume Pi1 Rev2 by default

	// Relies on info in /proc/cmdline.  If this becomes unreliable
	// in the future, alt code below uses /proc/cpuinfo if any better.
#if 1
	if((fp = fopen("/proc/cmdline", "r"))) {
		while(fgets(buf, sizeof(buf), fp)) {
			if((ptr = strstr(buf, "mem_size=")) &&
			   (sscanf(&ptr[9], "%x", &n) == 1) &&
			   (n == 0x3F000000)) {
				board = 2; // Appears to be a Pi 2
				break;
			} else if((ptr = strstr(buf, "boardrev=")) &&
			          (sscanf(&ptr[9], "%x", &n) == 1) &&
			          ((n == 0x02) || (n == 0x03))) {
				board = 0; // Appears to be an early Pi
				break;
			}
		}
		fclose(fp);
	}
#else
	char s[8];
	if((fp = fopen("/proc/cpuinfo", "r"))) {
		while(fgets(buf, sizeof(buf), fp)) {
			if((ptr = strstr(buf, "Hardware")) &&
			   (sscanf(&ptr[8], " : %7s", s) == 1) &&
			   (!strcmp(s, "BCM2709"))) {
				board = 2; // Appears to be a Pi 2
				break;
			} else if((ptr = strstr(buf, "Revision")) &&
			          (sscanf(&ptr[8], " : %x", &n) == 1) &&
			          ((n == 0x02) || (n == 0x03))) {
				board = 0; // Appears to be an early Pi
				break;
			}
		}
		fclose(fp);
	}
#endif

	return board;
}

// Set one GPIO pin attribute through the Sysfs interface.
static int pinSetup(int pin, char *attr, char *value) {
	char filename[50];
	int  fd, w, len = strlen(value);
	sprintf(filename, "%s/gpio%d/%s", sysfs_root, pin, attr);
	if((fd = open(filename, O_WRONLY)) < 0) return -1;
	w = write(fd, value, len);
	close(fd);
	return (w != len); // 0 = success
}

// Configure GPIO internal pull up/down/none
static void pull(int bitmask, int state) {
	volatile unsigned char shortWait;
	gpio[GPPUD]     = state;         // 2=up, 1=down, 0=none
	for(shortWait=150;--shortWait;); // Min 150 cycle wait
	gpio[GPPUDCLK0] = bitmask;       // Set pullup mask
	for(shortWait=150;--shortWait;); // Wait again
	gpio[GPPUD]     = 0;             // Reset pullup registers
	gpio[GPPUDCLK0] = 0;
}

// Restore GPIO and uinput to startup state; un-export any Sysfs pins used,
// don't leave any filesystem cruft; restore any GND pins to inputs and
// disable previously-set pull-ups.  Write errors are ignored as pins may be
// in a partially-initialized state.
static void pinConfigUnload() {
	char buf[50];
	int  fd, i;

	// Close GPIO file descriptors
	for(i=0; i<32; i++) {
		if(p[i].fd >= 0) {
			close(p[i].fd);
			p[i].fd = -1;
		}
		p[i].events = p[i].revents = 0;
	}

	// Close uinput file descriptors
	keyfd = -1;
	if(keyfd2 >= 0) {
		close(keyfd2);
		keyfd2 = -1;
	}
	if(keyfd1 >= 0) {
		ioctl(keyfd1, UI_DEV_DESTROY);
		close(keyfd1);
		keyfd1 = -1;
	}

	// Un-export pins
	sprintf(buf, "%s/unexport", sysfs_root);
	if((fd = open(buf, O_WRONLY)) >= 0) {
		for(i=0; i<32; i++) {
			// Restore GND items to inputs
			if(key[i] >= GND) pinSetup(i, "direction", "in");
			// And un-export all items regardless
			sprintf(buf, "%d", i);
			write(fd, buf, strlen(buf));
		}
		close(fd);
	}

	for(i=0; i<32; i++) {
		if((key[i] > KEY_RESERVED) && (key[i] < GND))
			vulcanMask |= (1 << i);
	}
	pull(vulcanMask, 0); // Disable pullups

	// Reset pin-and-key-related globals
	for(i=0; i<32; i++) key[i] = KEY_RESERVED;
	memset(intstate, 0, sizeof(intstate));
	memset(extstate, 0, sizeof(intstate));
	vulcanMask = 0;
	vulcanKey  = KEY_RESERVED;
}

// Quick-n-dirty error reporter; print message, clean up and exit.
static void err(char *msg) {
	printf("%s: %s.  Try 'sudo %s'.\n", __progname, msg,
	  program_invocation_name);
	pinConfigUnload();
	exit(1);
}

// Filter function for scandir(), identifies possible device candidates for
// simulated keypress events (distinct from actual USB keyboard(s)).
static int filter1(const struct dirent *d) {
	if(!strncmp(d->d_name, "input", 5)) { // Name usu. 'input' + #
		// Read contents of 'name' file inside this subdirectory,
		// if it matches the retrogame executable, that's probably
		// the device we want...
		char  filename[100], line[100];
		FILE *fp;
		sprintf(filename, "/sys/devices/virtual/input/%s/name",
		  d->d_name);
		memset(line, 0, sizeof(line));
		if((fp = fopen(filename, "r"))) {
			fgets(line, sizeof(line), fp);
			fclose(fp);
		}
		if(!strncmp(line, __progname, strlen(__progname))) return 1;
	}
        return 0;
}

// A second scandir() filter, checks for filename of 'event' + #
static int filter2(const struct dirent *d) {
	return !strncmp(d->d_name, "event", 5);
}

// Search for name in dictionary, return assigned value (-1 = not found)
static int dictSearch(char *str, dict *d) {
	int i;
	for(i=0; d[i].name && strcasecmp(str, d[i].name); i++);
	return d[i].value;
}


// Config file handlage ----------------------------------------------------

// Load pin/key configuration from cfgPathname.
static void pinConfigLoad() {

	// Config file format is super simple, just per-line keyword and
	// argument(s) with whitespace delimiters...can parse it ourselves
	// here.  Configuration libraries such as libconfig, libconfuse
	// have some potent features but enforce a correspondingly more
	// exacting syntax on the user; do not want if we can avoid it.

	FILE            *fp;
	char             buf[50];
	enum commandNum  cmd = CMD_NONE;
	int              stringLen      = 0,
	                 wordCount      = 0,
	                 keyCode        = KEY_RESERVED,
	                 i, c, k, fd, bitmask;
	bool             readingString  = false,
	                 isComment      = false;
	uint32_t         pinMask        = 0;

	// Read config file into key[] table -------------------------------

	if(NULL == (fp = fopen(cfgPathname, "r"))) {
		printf("%s: could not open config file '%s'\n",
		   __progname, cfgPathname);
		return; // Not fatal; file might be created later
	}

	do {
		c = getc(fp);
		if(isspace(c) || (c <= 0)) { // If whitespace char...
			if(readingString && !isComment) {
				// Switching from string-reading to
				// whitespace-skipping.  Cap off and process
				// current string, reset readingString flag.
				buf[stringLen] = 0;
				if(wordCount == 1) {
					// First word on line.  Look it up
					// in key dict, then command dict
					pinMask = 0;
					keyCode = KEY_RESERVED;
					if((k = dictSearch(
					  buf, keyTable)) >= 0) {
						// Start of key command
						cmd     = CMD_KEY;
						keyCode = k;
					} else if((k = dictSearch(buf,
					  command)) >= 0) {
						// Not a key, is other
						// command (e.g. GND)
						cmd = k;
					} else {
						printf("%s: unknown key or "
						  "command '%s' (not fatal, "
						  "continuing)\n",
						  __progname, buf);
					}
				} else {
					// Word #2+ on line;
					// Certain commands may accumulate
					// values.  At the moment, just pin
					// numbers; this code will need
					// revision if other argument types
					// happen later (e.g. timeouts).
					char *endptr;
					int   pinNum;
					pinNum = strtol(buf, &endptr, 0);
					if((*endptr) || (pinNum < 0) ||
					  (pinNum > 31)) {
						// Non-NUL character
						// indicates not entire
						// string was translated,
						// i.e. bad numeric input.
						printf("%s: invalid pin "
						  "'%s' (not fatal, "
						  "continuing)\n",
						  __progname, buf);
					} else {
						// Add pin # to list
						pinMask |= (1 << pinNum);
					}
				}
				readingString = false;
			}
			if((c == '\n') || (c <= 0)) { // If EOF or EOL
				// Execute last line if useful command
				switch(cmd) {
				   case CMD_KEY:
					// Count number of pins on line
					for(k=i=0; i<32; i++) {
						if(pinMask & (1 << i)) k++;
					}
					if(k == 1) {
						for(i=0;!(pinMask&(1<<i));i++);
						key[i] = keyCode;
					} else if(k > 1) {
						vulcanMask = pinMask;
						vulcanKey  = keyCode;
					}
					break;
				   case CMD_GND:
					// One or more GND pins
					for(i=0; i<32; i++) {
						if(pinMask & (1 << i)) {
							key[i] = GND;
						}
					}
					vulcanMask &= ~pinMask;
					break;
				   default:
					break;
				}
				// Reset ALL string-reading flags
				readingString = false;
				stringLen     = 0;
				wordCount     = 0;
				isComment     = false;
				cmd           = CMD_NONE;
			}
		} else {                        // Non-whitespace char
			if(isComment) continue;
			if(!readingString) {
				// Switching from whitespace-skipping
				// to string-reading.  Reset string.
				readingString = true;
				stringLen     = 0;
				// Is it beginning of a comment?
				// If so, ignore chars to next EOL.
				if(c == '#') {
					isComment = true;
					continue;
				}
				wordCount++;
			}
			// Append characer to current string
			if(stringLen < (sizeof(buf) - 1)) {
				buf[stringLen++] = c;
			}
		}
	} while(c > 0);

	fclose(fp);

	// If this is a "Revision 1" Pi board (no mounting holes),
	// remap certain pin numbers for compatibility.  Can then use
	// 'modern' pin numbers regardless of board type.
	if(board == 0) {
		key[0]  = key[2];  // GPIO2 -> GPIO0
		key[1]  = key[3];  // etc.
		key[21] = key[27];
		key[2]  = key[3] = key[27] = KEY_RESERVED;
	}

	// Set up GPIO from key[] table ------------------------------------

	bitmask = vulcanMask;
	for(i=0; i<32; i++) {
		if((key[i] > KEY_RESERVED) && (key[i] < GND))
			bitmask |= (1 << i);
	}
	pull(bitmask, 2); // Enable pullups on input pins
	if(!vulcanMask) vulcanKey = KEY_RESERVED;

	// All other GPIO config is handled through the sysfs interface.

	sprintf(buf, "%s/export", sysfs_root);
	if((fd = open(buf, O_WRONLY)) < 0) // Open Sysfs export file
		err("Can't open GPIO export file");
	for(i=0; i<32; i++) {
		if((key[i] == KEY_RESERVED) && !(vulcanMask & (1<<i)))
			continue;
		sprintf(buf, "%d", i);
		write(fd, buf, strlen(buf));    // Export pin
		pinSetup(i, "active_low", "0"); // Don't invert
		if(key[i] >= GND) {
			// Set pin to output, value 0 (ground)
			if(pinSetup(i, "direction", "out") ||
			   pinSetup(i, "value"    , "0"))
				err("Pin config failed (GND)");
		} else {
			// Set pin to input, detect rise+fall events
			char x;
			if(pinSetup(i, "direction", "in") ||
			   pinSetup(i, "edge"     , "both"))
				err("Pin config failed");
			// Get initial pin value
			sprintf(buf, "%s/gpio%d/value", sysfs_root, i);
			if((p[i].fd = open(buf, O_RDONLY)) < 0)
				err("Can't access pin value");
			intstate[i] = 0;
			if((read(p[i].fd, &x, 1) == 1) && (x == '0'))
				intstate[i] = 1;
			extstate[i]  = intstate[i];
			p[i].events  = POLLPRI; // Set up poll() events
			p[i].revents = 0;
		}
	}
	close(fd); // Done w/Sysfs exporting

	// Set up uinput

	// Attempt to create uidev virtual keyboard
	if((keyfd1 = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) >= 0) {
		(void)ioctl(keyfd1, UI_SET_EVBIT, EV_KEY);
		for(i=0; i<32; i++) {
			if(((key[i] >= KEY_RESERVED) && (key[i] < GND)) ||
			   (vulcanMask & (1 << i))) {
				(void)ioctl(keyfd1, UI_SET_KEYBIT, key[i]);
			}
		}
		(void)ioctl(keyfd1, UI_SET_KEYBIT, vulcanKey);
		struct uinput_user_dev uidev;
		memset(&uidev, 0, sizeof(uidev));
		snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "retrogame");
		uidev.id.bustype = BUS_USB;
		uidev.id.vendor  = 0x1;
		uidev.id.product = 0x1;
		uidev.id.version = 1;
		if(write(keyfd1, &uidev, sizeof(uidev)) < 0)
			err("write failed");
		if(ioctl(keyfd1, UI_DEV_CREATE) < 0)
			err("DEV_CREATE failed");
	}

	// SDL2 (used by some newer emulators) wants /dev/input/eventX
	// instead -- BUT -- this only exists if there's a physical USB
	// keyboard attached or if the above code has run and created a
	// virtual keyboard.  On older systems this method doesn't apply,
	// events can be sent to the keyfd1 virtual keyboard above...so,
	// this code looks for an eventX device and (if present) will use
	// that as the destination for events, else fallback on keyfd1.

	// The 'X' in eventX is a unique identifier (typically a numeric
	// digit or two) for each input device, dynamically assigned as
	// USB input devices are plugged in or disconnected (or when the
	// above code runs, creating a virtual keyboard).  As it's
	// dynamically assigned, we can't rely on a fixed number -- it
	// will vary if there's a keyboard connected at startup.

	struct dirent **namelist;
	int             n;
	char            evName[100] = "";

	if((n = scandir("/sys/devices/virtual/input",
	  &namelist, filter1, NULL)) > 0) {
		// Got a list of device(s).  In theory there should
		// be only one that makes it through the filter (name
		// matches retrogame)...if there's multiples, only
		// the first is used.  (namelist can then be freed)
		char path[100];
		sprintf(path, "/sys/devices/virtual/input/%s",
		  namelist[0]->d_name);
		for(i=0; i<n; i++) free(namelist[i]);
		free(namelist);
		// Within the given device path should be a subpath with
		// the name 'eventX' (X varies), again theoretically
		// should be only one, first in list is used.
		if((n = scandir(path, &namelist, filter2, NULL)) > 0) {
			sprintf(evName, "/dev/input/%s",
			  namelist[0]->d_name);
			for(i=0; i<n; i++) free(namelist[i]);
			free(namelist);
		}
	}

	if(!evName[0]) { // Nothing found?  Use fallback method...
		// Kinda lazy skim for last item in /dev/input/event*
		// This is NOT guaranteed to be retrogame, but if the
		// above method fails for some reason, this may be
		// adequate.  If there's a USB keyboard attached at
		// boot, it usually instantiates in /dev/input before
		// retrogame, so even if it's then removed, the index
		// assigned to retrogame stays put...thus the last
		// index mmmmight be what we need.
		struct stat st;
		for(i=99; i>=0; i--) {
			sprintf(buf, "/dev/input/event%d", i);
			if(!stat(buf, &st)) break; // last valid device
		}
		strcpy(evName, (i >= 0) ? buf : "/dev/input/event0");
	}

	keyfd2 = open(evName, O_WRONLY | O_NONBLOCK);
	keyfd  = (keyfd2 >= 0) ? keyfd2 : keyfd1;
	// keyfd1 and 2 are global and are held open (as a destination for
	// key events) until pinConfigUnload() is called.
}

// Handle signal events (i=32), config file change events (33) or
// config directory contents change events (34).
// CONFIGURATION FILES AREN'T YET IMPLEMENTED, but this is a vital
// part of that, monitoring for config changes so new settings can
// be loaded synamically without a kill/restart/whatev.
static void pollHandler(int i) {

	if(i == 32) { // Signal event
		struct signalfd_siginfo info;
		read(p[i].fd, &info, sizeof(info));
		if(info.ssi_signo == SIGHUP) { // kill -1 = force reload
			// printf("SIGHUP\n");
			// Reload config
			pinConfigUnload();
			pinConfigLoad();
		} else { // Other signal = abort program
			running = false;
		}
	} else { // Change in config file or directory contents
		char evBuf[1000];
		//int  evCount = 0;
		int  bufPos = 0,
		     bytesRead = read(p[i].fd, evBuf, sizeof(evBuf));
		while(bufPos < bytesRead) {
			struct inotify_event *ev =
			  (struct inotify_event *)&evBuf[bufPos];

			//printf("EVENT %d:\n", evCount++);
			//printf("\tinotify event mask: %08x\n", ev->mask);
			//printf("\tlen: %d\n", ev->len);
			//if(ev->len > 0)
				//printf("\tname: '%s'\n", ev->name);

			if(ev->mask & IN_MODIFY) {
				//puts("\tConfig file changed (reloading)");
				pinConfigUnload();
				pinConfigLoad();
			} else if(ev->mask & IN_IGNORED) {
				// Config file deleted -- stop watching it
				//puts("\tConfig file removed");
				inotify_rm_watch(p[1].fd, fileWatch);
				// Closing the descriptor turns out to be
				// important, as removing the watch itself
				// creates another IN_IGNORED event.
				// Avoids turtles all the way down.
				close(p[33].fd);
				p[33].fd     = -1;
				p[33].events =  0;
				// Pin config is NOT unloaded...
				// keep using prior values for now.
			} else if(ev->mask & IN_MOVED_FROM) {
				// File moved/renamed from directory...
				// check if it's the one we're monitoring.
				//puts("\tFile moved or renamed");
				if(!strcmp(ev->name, cfgName)) {
					// It's our file -- stop watching it
					//puts("\tEffectively removed");
					inotify_rm_watch(p[33].fd, fileWatch);
					close(p[33].fd);
					p[33].fd     = -1;
					p[33].events =  0;
					// Pin config is NOT unloaded...
					// keep using prior values for now.
				} else {
					// Some other file -- disregard
					//puts("\tNot the file we're watching");
				}
			} else if(ev->mask & (IN_CREATE | IN_MOVED_TO)) {
				// File moved/renamed to directory...
				// check if it's the one we're monitoring for.
				//puts("\tNew file in directory...");
				if(!strcmp(ev->name, cfgName)) {
					// It's our file -- start watching it!
					//puts("\tFile created/moved-to!");
					if(p[33].fd >= 0) { // Existing file?
						inotify_rm_watch(
						  p[33].fd, fileWatch);
						close(p[33].fd);
					}
					p[33].fd   = inotify_init();
					fileWatch = inotify_add_watch(
					  p[33].fd, cfgPathname,
					  IN_MODIFY | IN_IGNORED);
					p[33].events = POLLIN;
					pinConfigUnload();
					pinConfigLoad();
				} else {
					// Some other file -- disregard
					//puts("\tNot the config file.");
				}
			}

			bufPos += sizeof(struct inotify_event) + ev->len;
		}
	}
}


// Init and main loop ------------------------------------------------------

int main(int argc, char *argv[]) {

	char               c;            // Pin input value ('0'/'1')
	int                fd,           // For mmap, sysfs
	                   i,            // Generic counter
	                   timeout = -1, // poll() timeout
	                   lastKey = -1; // Last key down (for repeat)
	unsigned long      pressMask;    // For Vulcan pinch detect
	struct input_event keyEv, synEv; // uinput events
	sigset_t           sigset;       // Signal mask

	// Locate configuration file (if any) and path ---------------------

	if(argc > 1) { // First argument (if given) is config file name
		char *ptr = strrchr(argv[1], '/'); // Full pathname given?
		if(ptr) { // Pathname given; separate into path & name
			if(!(cfgPathname = strdup(argv[1])))
				err("malloc() fail");
			int len = ptr - argv[1]; // Length of path component
			if(!len) { // Root path?
				cfgPath = "/";
				cfgName = &cfgPathname[1];
			} else {
				if(!(cfgPath = (char *)malloc(len + 1)))
					err("malloc() fail");
				memcpy(cfgPath, argv[1], len);
				cfgPath[len] = 0;
			}
		} else { // No path given; use /boot directory.
			cfgPath = "/boot";
			if(!(cfgPathname = (char *)malloc(
			  strlen(cfgPath) + strlen(argv[1]) + 2)))
				err("malloc() fail");
			sprintf(cfgPathname, "%s/%s", cfgPath, argv[1]);
		}
	} else {
		// No argument passed -- config file is located in /boot,
		// name is "[program name].cfg" (e.g. /boot/retrogame.cfg)
		cfgPath = "/boot";
		if(!(cfgPathname = (char *)malloc(
		  strlen(cfgPath) + strlen(__progname) + 6)))
			err("malloc() fail");
		sprintf(cfgPathname, "%s/%s.cfg", cfgPath, __progname);
	}
	if(!cfgName) cfgName = &cfgPathname[strlen(cfgPath) + 1];

	// Catch signals, config file changes ------------------------------

	// Clear all descriptors and GPIO state, init input event structures
	memset(p, 0, sizeof(p));
	for(i=0; i<35; i++) p[i].fd = -1;
	for(i=0; i<32; i++) key[i] = KEY_RESERVED;
	memset(intstate, 0, sizeof(intstate));
	memset(extstate, 0, sizeof(extstate));
	memset(&keyEv  , 0, sizeof(keyEv));
	memset(&synEv  , 0, sizeof(synEv));
	keyEv.type  = EV_KEY;
	synEv.type  = EV_SYN;
	synEv.code  = SYN_REPORT;
	vulcanMask  = 0;
	vulcanKey   = KEY_RESERVED;

	sigfillset(&sigset);
	sigprocmask(SIG_BLOCK, &sigset, NULL);
	// pollfd #32 catches signals, so GPIO cleanup on exit is possible
	p[32].fd     = signalfd(-1, &sigset, 0);
	p[32].events = POLLIN;

	// pollfd #33 and #34 will be used for detecting changes in the
	// config file and its parent directory.  This will let you edit
	// the config and have immediate feedback without needing to kill
	// the process or reboot the system.
	for(i=33; i<=34; i++) {
		p[i].fd     = inotify_init();
		p[i].events = POLLIN;
	}
	fileWatch = inotify_add_watch(p[33].fd, cfgPathname,
	  IN_MODIFY | IN_IGNORED);
	inotify_add_watch(p[34].fd, cfgPath,
	  IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO);

	// p[0-31] are related to GPIO states, and will be reconfigured
	// each time the config file is loaded.

	// GPIO startup ----------------------------------------------------

	board = boardType();

	// Although Sysfs provides solid GPIO interrupt handling, there's
	// no interface to the internal pull-up resistors (this is by
	// design, being a hardware-dependent feature).  It's necessary to
	// grapple with the GPIO configuration registers directly to enable
	// the pull-ups.  Based on GPIO example code by Dom and Gert van
	// Loo on elinux.org
	if((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		err("Can't open /dev/mem");
	gpio = mmap(            // Memory-mapped I/O
	  NULL,                 // Any adddress will do
	  BLOCK_SIZE,           // Mapped block length
	  PROT_READ|PROT_WRITE, // Enable read+write
	  MAP_SHARED,           // Shared with other processes
	  fd,                   // File to map
	  (board == 2) ?
	   PI2_GPIO_BASE :      // -> GPIO registers
	   PI1_GPIO_BASE);
	close(fd);              // Not needed after mmap()
	if(gpio == MAP_FAILED) err("Can't mmap()");

	pinConfigLoad();

	// Main loop -------------------------------------------------------

	// Monitor GPIO file descriptors for button events.  The poll()
	// function watches for GPIO IRQs in this case; it is NOT
	// continually polling the pins!  Processor load is near zero.

	while(running) { // Signal handler will set this to 0 to exit
		// Wait for IRQ on pin (or timeout for button debounce)
		if(poll(p, 35, timeout) > 0) { // If IRQ...
			for(i=0; i<32; i++) {  // For each GPIO bit...
				if(p[i].revents) { // Event received?
					timeout = debounceTime;
					// Read current pin state, store in
					// internal state flag, flag, but
					// don't issue to uinput yet -- must
					// wait for debounce!
					lseek(p[i].fd, 0, SEEK_SET);
					read(p[i].fd, &c, 1);
					if(c == '0')      intstate[i] = 1;
					else if(c == '1') intstate[i] = 0;
					p[i].revents = 0;
				}
			}
			for(; i<35; i++) { // Check signals, etc.
				if(p[i].revents) { // Event received?
					pollHandler(i);
					p[i].revents = 0;
				}
			}
			c = 0; // Don't issue SYN event
			// Else timeout occurred
		} else if(timeout == debounceTime) { // Debounce timeout
			for(pressMask=i=0; i<32; i++) {
				if((key[i] > KEY_RESERVED) &&
				   (key[i] < GND)) {
					// Compare internal state against
					// previously-issued value.  Send
					// keys only for changed states.
					if(intstate[i] != extstate[i]) {
						extstate[i] = intstate[i];
						keyEv.code  = key[i];
						keyEv.value = intstate[i];
						write(keyfd, &keyEv,
						  sizeof(keyEv));
						c = 1; // Follow w/SYN event
						if(intstate[i]) { // Press?
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
							lastKey = timeout =
							  -1;
						}
					}
					if(intstate[i]) pressMask |= (1<<i);
				}
			}

			// If the "Vulcan nerve pinch" buttons are pressed,
			// set long timeout -- if this time elapses without
			// a button state change, esc keypress will be sent.
			if((pressMask & vulcanMask) == vulcanMask)
				timeout = vulcanTime;
		} else if(timeout == vulcanTime) { // Vulcan key timeout
			// Send keycode (MAME exits or displays exit menu)
			keyEv.code = vulcanKey;
			for(i=1; i>= 0; i--) { // Press, release
				keyEv.value = i;
				write(keyfd, &keyEv, sizeof(keyEv));
				usleep(10000); // Be slow, else MAME flakes
				write(keyfd, &synEv, sizeof(synEv));
				usleep(10000);
			}
			timeout = -1; // Return to normal processing
			c       = 0;  // No add'l SYN required
		} else if(lastKey >= 0) { // Else key repeat timeout
			if(timeout == repTime1) timeout = repTime2;
			else if(timeout > 30)   timeout -= 5; // Accelerate
			c           = 1; // Follow w/SYN event
			keyEv.code  = key[lastKey];
			keyEv.value = 2; // Key repeat event
			write(keyfd, &keyEv, sizeof(keyEv));
		}
		if(c) write(keyfd, &synEv, sizeof(synEv));
	}

	// Clean up --------------------------------------------------------

	pinConfigUnload(); // Close uinput, un-export pins

	puts("Done.");

	return 0;
}
