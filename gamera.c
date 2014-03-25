/*
gamera (Game Rom Aggregator): provides a basic game selection interface
for advmame (or possibly other MAME variants).

If the file [base path]/advmame.xml exists, games will have 'human readable'
titles.  Otherwise the (sometimes cryptic) ROM filename will be used.  Use
the following command to generate the XML file:

    advmame -listxml > /boot/advmame/advmame.xml

MAME -must- be configured with 'z' and 'x' as buttons 1 & 2 (normally left
ctrl and alt) for a seamless retrogame/menu/advmame experience.  This is
because handling raw keycodes with ncurses is a Pandora's Box of pure evil.

 device_keyboard raw
 input_map[p1_button1] keyboard[0,lcontrol] or keyboard[0,z]
 input_map[p1_button2] keyboard[0,lalt] or keyboard[0,x]
 input_map[ui_select] keyboard[0,enter] or keyboard[0,lcontrol] or keyboard[0,z]

The command to launch MAME, the location of the ROM folder and the (optional)
XML file are currently all set in global variables near the top of the code.

The Pre-built executable should run as-is for most users.  If you need to
tweak and recompile, this requires the ncurses and expat C libraries:

    sudo apt-get install ncurses-dev libexpat1-dev

Written by Phil Burgess for Adafruit Industries, distributed under BSD
License.  Adafruit invests time and resources providing this open source
code, please support Adafruit and open-source hardware by purchasing
products from Adafruit!


Copyright (c) 2014 Adafruit Industries.
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

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ncurses.h>
#include <menu.h>
#include <expat.h>


// START HERE: configurable stuff ----------------------------------------

static const char
  mameCmd[]  = "advmame",              // Advance MAME executable
  basePath[] = "/boot/advmame",        // Path to advmame configs & ROM dir
  cfgTall[]  = "advmame.rc.portrait",  // Config for vertical screen
  cfgWide[]  = "advmame.rc.landscape", // Config for horizontal screen
  romPath[]  = "rom",                  // Subdirectory for ROMs
  xmlFile[]  = "advmame.xml";          // Name of game list file

// TFT rotation setting may be stored in different places depending
// on kernel vs. module usage.  Rather than require configuration or
// recompilation, this table lists all the likely culprits...
static const struct {
  const char *filename; // Name of config file
  const char *keyword;  // Rotation setting string
} tftCfg[] = {
  { "/etc/modprobe.d/adafruit.conf", "rotate"              },
  { "/boot/cmdline.txt"            , "fbtft_device.rotate" } };
#define N_TFT_FILES (sizeof(tftCfg) / sizeof(tftCfg[0]))


// A few globals ---------------------------------------------------------

// Linked list of Game structs is generated when scanning ROM folder
typedef struct Game {
	char        *name; // ROM filename (sans .zip)
	char        *desc; // From XML - verbose title, or NULL if none
	struct Game *next;
} Game;

static Game *gameList       = NULL, // Entry to Game linked list
            *gameToDescribe = NULL; // Used during XML cross-referencing

static unsigned char descFlag = 0;  // Also for XML cross-ref

WINDOW *mainWin  = NULL, // ncurses elements
       *noRomWin = NULL;
MENU   *menu     = NULL;
ITEM  **items    = NULL;

static char *xml, // Absolute path to XML file
            *rom; // Absolute path to ROM folder


// Some utility functions ------------------------------------------------

// Filter function for scandir().  Returns files & links ending in .zip
static int sel(const struct dirent *d) {
	char *ptr;
	if(((d->d_type == DT_REG) || (d->d_type == DT_LNK)) &&
	   (ptr = strrchr(d->d_name,'.')) && !strcasecmp(ptr,".zip")) {
		*ptr = 0; // Truncate .zip extender
		return 1;
	}
	return 0;
}

// Called at the start of each XML element: e.g. <foo>
static void XMLCALL startElement(
  void *depth, const char *name, const char **attr) {
	if((*(int *)depth += 1) == 2) { // If level 2 nesting...
		// If element is <game> and at least 2 attributes present...
		if(!strcmp(name, "game") && attr[0] && attr[1]) {
			// Compare attr[1] against list of game names...
			Game *g;
			for(g=gameList; g && !gameToDescribe; g=g->next) {
				if(!strcmp(attr[1], g->name)) {
					// Found match, save pointer to game
					gameToDescribe = g;
					// The element data parser, if
					// subsequently enabled,  may then
					// modify the desc for this game.
				}
			}
		}
	// else if element is '<description>' at level 3...
	} else if((*(int *)depth == 3) && !strcmp(name, "description")) {
		descFlag = 1; // Enable element data parser
	}
}

// Called at the end of each XML element: e.g. </foo>
static void XMLCALL endElement(void *depth, const char *name) {
	if((*(int *)depth -= 1) == 1) { // End of 'game' element?
		gameToDescribe = NULL;  // Deactivate description search
		descFlag       = 0;
	}
}

// Called for element data between start/end: e.g. <foo>DATA</foo>
static void elementData(void *data, const char *content, int length) {
	// gameToDescribe and descFlag must both be set; avoid false positives
	if(descFlag && gameToDescribe) {
		// Shouldn't be multiple descriptions, but just in case...
		if(gameToDescribe->desc) free(gameToDescribe->desc);
		if((gameToDescribe->desc = malloc(length + 1))) {
			strncpy(gameToDescribe->desc, content, length);
			gameToDescribe->desc[length] = 0;
		} // malloc() fail is non-fatal; will fall back on name alone
		descFlag = 0; // Found description, we're done
	}
}

// Scan MAME ROMs folder, cross-reference against XML (if present) for
// verbose descriptions, generate ncurses menu.
static void find_roms(void) {
	FILE           *fp;
	struct dirent **dirList;
	int             i, nFiles;
	Game           *g;       // For traversing Game linked list
	WINDOW         *scanWin; // Modal 'Scanning...' window

	if(noRomWin) { // Delete 'No ROMS' window if present
		delwin(noRomWin);
		noRomWin = NULL;
		werase(mainWin);
		box(mainWin, 0, 0);
	}

	if(items) { // Delete old ROM menu and contents, if any
		if(menu) {
			unpost_menu(menu);
			free_menu(menu);
			menu = NULL;
		}
		for(i=0; items[i]; i++) free_item(items[i]);
		free(items);
		items = NULL;
	}

	const char scanMsg[] = "Scanning ROM folder...";
	scanWin = newwin(3, strlen(scanMsg) + 4,
	  (LINES - 4) / 2 - 1, (COLS - strlen(scanMsg)) / 2 - 2);
	box(scanWin, 0, 0);
	mvwprintw(scanWin, 1, 2, scanMsg);

	wnoutrefresh(mainWin);
	wnoutrefresh(scanWin);
	doupdate();

	delwin(scanWin);
	werase(mainWin);
	box(mainWin, 0, 0);

	while(gameList) { // Delete existing gameList, if any
		g = gameList->next;
		if(gameList->name) free(gameList->name);
		if(gameList->desc) free(gameList->desc);
		free(gameList);
		gameList = g;
	}

	i = 0; // Count number of games found & successfully alloc'd
	if((nFiles = scandir(rom, &dirList, sel, alphasort)) > 0) {
		// Copy dirent array to a Game linked list
		while(nFiles--) { // List is assembled in reverse
			if((g = (Game *)malloc(sizeof(Game)))) {
				g->name  = strdup(dirList[nFiles]->d_name);
				g->desc  = NULL;
				g->next  = gameList;
				gameList = g;
				i++; // A winner is you
			}
			// dirList contents are freed as we go
			free(dirList[nFiles]);
		}
		free(dirList);
	}

	// Alloc, load, cross-reference XML file against ROM filenames
	if((fp = fopen(xml, "r"))) {
		fseek(fp, 0, SEEK_END);
		char *buf;
		int   len = ftell(fp);
		if((buf = (char *)malloc(len))) {
			int depth = 0;
			fseek(fp, 0, SEEK_SET);
			fread(buf, 1, len, fp);
			XML_Parser parser = XML_ParserCreate(NULL);
			XML_SetUserData(parser, &depth);
			XML_SetElementHandler(parser,
			  startElement, endElement);
			XML_SetCharacterDataHandler(parser, elementData);
			XML_Parse(parser, buf, len, 1);
			XML_ParserFree(parser);
			free(buf);
		}
		fclose(fp);
	}

	if((items = (ITEM**)malloc((i + 1) * sizeof(ITEM *)))) {
		for(i=0, g=gameList; g; g=g->next, i++) {
			items[i] = new_item(g->name, g->desc);
			set_item_userptr(items[i], g);
		}
		items[i] = NULL;
		menu = new_menu(items);
		set_menu_win(menu, mainWin);
		set_menu_sub(menu, derwin(mainWin, LINES-6, COLS-2, 1, 1));
		set_menu_format(menu, LINES-6, 1);
		set_menu_mark(menu, " ");
		post_menu(menu);
	}

	wrefresh(mainWin);

	if(!menu) { // If no ROMs, throw up a message to that effect
		const char noRomMsg[] = "No ROMs found";
		noRomWin = newwin(3, strlen(noRomMsg) + 4,
		  (LINES - 4) / 2 - 1, (COLS - strlen(noRomMsg)) / 2 - 2);
		box(noRomWin, 0, 0);
		mvwprintw(noRomWin, 1, 2, noRomMsg);
		wrefresh(noRomWin);
	}
}

// Get full path to file/directory (relative to baseName, unless item
// specifies absolute path).  Returns NULL on malloc error.  Return path
// *may* be same as item (if it's absolute), or a malloc'd buffer (if
// full path was constructed).
char *fullPath(const char *item) {
	char *ptr;

	if(item[0] == '/') return (char *)item;
	if((ptr = (char *)malloc(strlen(basePath) + strlen(item) + 2)))
		(void)sprintf(ptr, "%s/%s", basePath, item);

	return ptr;
}


// Main stuff ------------------------------------------------------------

int main(int argc, char *argv[]) {

	const char  title[] = "MAME YOUR POISON:";
	char       *cfg     = NULL, cmdline[1024];
	const char *c;
	Game       *g;
	int         i;

	if((NULL == (rom = fullPath(romPath))) ||
	   (NULL == (xml = fullPath(xmlFile)))) {
		(void)printf("%s: malloc() fail (rom/xml)\n", argv[0]);
		return 1;
	}

	// ncurses setup
	initscr();
	cbreak();
	noecho();
	set_escdelay(0);
	curs_set(0);

	// Determine if screen is in portrait or landscape mode, get
	// path to corresponding advmame config file.  Method is to
	// check for 'rotate=0' in TFT module config file.  If present
	// (system() returns 0), is portrait screen, else landscape.
	c = cfgWide; // Assume landscape screen to start
	for(i=0; i<N_TFT_FILES; i++) { // Check each TFT config location...
		(void)sprintf(cmdline, "grep %s=0 %s",
		  tftCfg[i].keyword, tftCfg[i].filename);
		if(!system(cmdline)) { // Found portrait reference!
			c = cfgTall;
			break;
		}
	}
	if(NULL == (cfg = fullPath(c))) {
		endwin();
		(void)printf("%s: malloc() fail (cfg)\n", argv[0]);
		return 1;
	}

	mvprintw(0, (COLS - strlen(title)) / 2, title);
	mvprintw(LINES-2, 0     , "Up/Down: Choose");
	mvprintw(LINES-1, 0     , "Enter  : Run game");
	mvprintw(LINES-2, COLS/2, "R  : Rescan ROMs");
	mvprintw(LINES-1, COLS/2, geteuid() ? "Esc: Quit" : "Esc: Shutdown");

	mainWin = newwin(LINES-3, COLS, 1, 0);
	keypad(mainWin, TRUE);
	box(mainWin, 0, 0);

	refresh();

	find_roms();

	for(;;) {
		switch(wgetch(mainWin)) {
		   case KEY_DOWN:
			menu_driver(menu, REQ_DOWN_ITEM);
			break;
		   case KEY_UP:
			menu_driver(menu, REQ_UP_ITEM);
			break;
		   case KEY_NPAGE:
			menu_driver(menu, REQ_SCR_DPAGE);
			break;
		   case KEY_PPAGE:
			menu_driver(menu, REQ_SCR_UPAGE);
			break;
		   case 'r': // Re-scan ROM folder
			find_roms();
			break;
		   case 'R': // Rotate-and-reboot
			if(!geteuid()) { // Must be root
				clear();
				refresh();
				endwin();
				for(i=0; i<N_TFT_FILES; i++) {
					(void)sprintf(cmdline, 
					  "sed -i 's/%s=90/Fo0BaR/;"
						  "s/%s=0/%s=90/;"
						  "s/Fo0BaR/%s=0/' %s",
					  tftCfg[i].keyword, tftCfg[i].keyword, 
					  tftCfg[i].keyword, tftCfg[i].keyword, 
					  tftCfg[i].filename);
					(void)system(cmdline);
				}
				(void)system("reboot");
			}
			break;
		   case 27: // Esc = shutdown (if run as root) or quit
			clear();
			refresh();
			endwin();
			if(geteuid()) return 0; // Not root, quit to console
			(void)system("shutdown -h now");
			break;
		   case '\n': // Enter
		   case 'z':
		   case 'x':
			if((g = item_userptr(current_item(menu)))) {
				(void)sprintf(cmdline, "%s -cfg %s %s",
				  mameCmd, cfg, g->name);
				def_prog_mode();
				endwin();
				i = system(cmdline);
				reset_prog_mode();
				if(i) { // If error message, wait for input
					(void)printf("Press any button...");
					fflush(stdout);
					while(!getch());
				}
			}
			break;
		}
		wrefresh(mainWin);
	}

	return 0;
}

