/**
 * $Id$
 * Copyright (C) 2008 - 2014 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <esc/common.h>
#include <ipc/screendevice.h>
#include <stdlib.h>
#include <assert.h>

#define VIDEO_MEM				0x30100000

#define COLS					80
#define ROWS					30
#define MAX_COLS				128

using namespace ipc;

static uint32_t *vgaData;

class VGAScreenDevice : public ScreenDevice<> {
public:
	explicit VGAScreenDevice(const std::vector<Screen::Mode> &modes,const char *path,mode_t mode)
		: ScreenDevice(modes,path,mode), _lastCol(COLS), _lastRow(ROWS), _color() {
	}

	virtual void setScreenMode(ScreenClient *c,const char *shm,Screen::Mode *mode,int type,bool) {
		assert(mode->id == 3);
		if(type != ipc::Screen::MODE_TYPE_TUI)
			throw std::default_error("Invalid mode type");

		/* undo previous mapping */
		if(c->fb)
			delete c->fb;
		c->mode = NULL;
		c->fb = NULL;

		if(mode) {
			if(*shm)
				c->fb = new FrameBuffer(*mode,shm,type);
			c->mode = mode;
		}
	}

	virtual void setScreenCursor(ScreenClient *,gpos_t x,gpos_t y,int) {
		if(_lastRow < ROWS && _lastCol < COLS)
			drawCursor(_lastRow,_lastCol,_color);
		if(y < ROWS && x < COLS) {
			_color = *(vgaData + y * MAX_COLS + x) >> 8;
			drawCursor(y,x,0x78);
		}
		_lastCol = x;
		_lastRow = y;
	}

	virtual void updateScreen(ScreenClient *c,gpos_t x,gpos_t y,gsize_t width,gsize_t height) {
		if(!c->mode || !c->fb)
			throw std::default_error("No mode set");
		if((gpos_t)(x + width) < x || x + width > c->mode->cols ||
			(gpos_t)(y + height) < y || y + height > c->mode->rows) {
			VTHROW("Invalid VGA update: " << x << "," << y << ":" << width << "x" << height);
		}

		uint32_t *screen = vgaData + y * MAX_COLS + x;
		for(size_t h = 0; h < height; h++) {
			uint16_t *buf = (uint16_t*)(c->fb->addr() + (y + h) * c->mode->cols * 2 + x * 2);
			for(size_t w = 0; w < width; w++) {
				uint16_t d = *buf++;
				screen[w] = (d >> 8) | (d & 0xFF) << 8;
			}
			screen += MAX_COLS;
		}
		if(_lastCol < COLS && _lastRow < ROWS) {
			/* update color and draw the cursor again if it has been overwritten */
			if(_lastCol >= x && _lastCol < (gpos_t)(x + width) &&
				_lastRow >= y && _lastRow < (gpos_t)(y + height)) {
				_color = *(vgaData + _lastRow * MAX_COLS + _lastCol) >> 8;
				drawCursor(_lastRow,_lastCol,0x78);
			}
		}
	}

private:
	void drawCursor(uint row,uint col,uchar curColor) {
		uint32_t *pos = vgaData + row * MAX_COLS + col;
		*pos = (curColor << 8) | (*pos & 0xFF);
	}

	gpos_t _lastCol;
	gpos_t _lastRow;
	uchar _color;
};

int main(void) {
	/* map VGA memory */
	uintptr_t phys = VIDEO_MEM;
	vgaData = (uint32_t*)mmapphys(&phys,MAX_COLS * ROWS * sizeof(uint32_t),0);
	if(vgaData == NULL)
		error("Unable to acquire vga-memory (%p)",VIDEO_MEM);

	std::vector<Screen::Mode> modes;
	modes.push_back(((Screen::Mode){
		0x0003,COLS,ROWS,0,0,4,0,0,0,0,0,0,VIDEO_MEM,0,0,ipc::Screen::MODE_TEXT,ipc::Screen::MODE_TYPE_TUI
	}));

	VGAScreenDevice dev(modes,"/dev/vga",0111);
	dev.loop();

	/* clean up */
	munmap(vgaData);
	return EXIT_SUCCESS;
}
