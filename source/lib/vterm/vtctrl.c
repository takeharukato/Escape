/**
 * $Id$
 * Copyright (C) 2008 - 2011 Nils Asmussen
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
#include <esc/io.h>
#include <esc/proc.h>
#include <esc/keycodes.h>
#include <esc/driver.h>
#include <esc/thread.h>
#include <esc/messages.h>
#include <esc/ringbuffer.h>
#include <esc/driver/screen.h>
#include <esc/driver/uimng.h>
#include <esc/conf.h>
#include <esc/mem.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <vterm/vtctrl.h>

/* the number of chars to keep in history */
#define INITIAL_RLBUF_SIZE	50

bool vtctrl_init(sVTerm *vt,sScreenMode *mode) {
	size_t i,len;
	uchar color;
	char *ptr;
	vt->cols = mode->cols;
	vt->rows = mode->rows;

	/* by default we have no handlers for that */
	vt->setCursor = NULL;

	/* init state */
	if(crtlocku(&vt->lock) < 0) {
		printe("Unable to create vterm lock");
		return false;
	}
	vt->col = 0;
	vt->row = vt->rows - 1;
	vt->lastCol = 0;
	vt->lastRow = 0;
	vt->upCol = vt->cols;
	vt->upRow = vt->rows;
	vt->upWidth = 0;
	vt->upHeight = 0;
	vt->upScroll = 0;
	vt->foreground = vt->defForeground;
	vt->background = vt->defBackground;
	/* start on first line of the last page */
	vt->firstLine = HISTORY_SIZE * vt->rows - vt->rows;
	vt->currLine = HISTORY_SIZE * vt->rows - vt->rows;
	vt->firstVisLine = HISTORY_SIZE * vt->rows - vt->rows;
	/* default behaviour */
	vt->echo = true;
	vt->readLine = true;
	vt->navigation = true;
	vt->printToRL = false;
	vt->printToCom1 = sysconf(CONF_LOG);
	vt->escapePos = -1;
	vt->rlStartCol = 0;
	vt->shellPid = 0;
	vt->screenBackup = NULL;
	vt->buffer = (char*)malloc(vt->rows * HISTORY_SIZE * vt->cols * 2);
	if(vt->buffer == NULL) {
		printe("Unable to allocate mem for vterm-buffer");
		return false;
	}
	vt->rlBufSize = INITIAL_RLBUF_SIZE;
	vt->rlBufPos = 0;
	vt->rlBuffer = (char*)malloc(vt->rlBufSize * sizeof(char));
	if(vt->rlBuffer == NULL) {
		printe("Unable to allocate memory for readline-buffer");
		return false;
	}

	vt->inbufEOF = false;
	vt->inbuf = rb_create(sizeof(char),INPUT_BUF_SIZE,RB_OVERWRITE);
	if(vt->inbuf == NULL) {
		printe("Unable to allocate memory for ring-buffer");
		return false;
	}

	/* fill buffer with spaces to ensure that the cursor is visible (spaces, white on black) */
	ptr = vt->buffer;
	color = (vt->background << 4) | vt->foreground;
	for(i = 0, len = vt->rows * HISTORY_SIZE * vt->cols * 2; i < len; i += 2) {
		*ptr++ = ' ';
		*ptr++ = color;
	}
	return true;
}

void vtctrl_destroy(sVTerm *vt) {
	rb_destroy(vt->inbuf);
	free(vt->buffer);
	free(vt->rlBuffer);
}

bool vtctrl_resize(sVTerm *vt,size_t cols,size_t rows) {
	bool res = false;
	locku(&vt->lock);
	if(vt->cols != cols || vt->rows != rows) {
		size_t c,r,color;
		size_t ccols = MIN(cols,vt->cols);
		char *buf,*oldBuf,*old = vt->buffer;
		vt->buffer = (char*)malloc(rows * HISTORY_SIZE * cols * 2);
		if(vt->buffer == NULL) {
			vt->buffer = old;
			unlocku(&vt->lock);
			return false;
		}

		buf = vt->buffer;
		color = (vt->background << 4) | vt->foreground;
		r = 0;
		if(rows > vt->rows) {
			size_t limit = (rows - vt->rows) * HISTORY_SIZE;
			for(; r < limit; r++) {
				for(c = 0; c < cols; c++) {
					*buf++ = ' ';
					*buf++ = color;
				}
			}
			oldBuf = old;
		}
		else
			oldBuf = old + (vt->rows - rows) * HISTORY_SIZE * vt->cols * 2;
		for(; r < rows * HISTORY_SIZE; r++) {
			memcpy(buf,oldBuf,ccols * 2);
			buf += ccols * 2;
			oldBuf += vt->cols * 2;
			for(c = ccols; c < cols; c++) {
				*buf++ = ' ';
				*buf++ = color;
			}
		}

		if(vt->rows * HISTORY_SIZE - vt->firstLine >= rows * HISTORY_SIZE)
			vt->firstLine = 0;
		else
			vt->firstLine = rows * HISTORY_SIZE - (vt->rows * HISTORY_SIZE - vt->firstLine);
		vt->firstLine += vt->rows - rows;
		if(vt->rows * HISTORY_SIZE - vt->currLine >= rows * HISTORY_SIZE)
			vt->currLine = 0;
		else
			vt->currLine = rows * HISTORY_SIZE - (vt->rows * HISTORY_SIZE - vt->currLine);
		vt->currLine += vt->rows - rows;
		if(vt->rows * HISTORY_SIZE - vt->firstVisLine >= rows * HISTORY_SIZE)
			vt->firstVisLine = 0;
		else
			vt->firstVisLine = rows * HISTORY_SIZE - (vt->rows * HISTORY_SIZE - vt->firstVisLine);
		vt->firstVisLine += vt->rows - rows;

		/* TODO update screenbackup */
		vt->col = MIN(vt->col,cols - 1);
		vt->row = MIN(rows - 1,rows - (vt->rows - vt->row));
		vt->cols = cols;
		vt->rows = rows;
		vt->upCol = vt->cols;
		vt->upRow = vt->rows;
		vt->upWidth = 0;
		vt->upHeight = 0;
		vtctrl_markScrDirty(vt);
		free(old);
		res = true;
	}
	unlocku(&vt->lock);
	return res;
}

int vtctrl_control(sVTerm *vt,uint cmd,void *data) {
	int res = 0;
	locku(&vt->lock);
	switch(cmd) {
		case MSG_VT_SHELLPID:
			vt->shellPid = *(pid_t*)data;
			break;
		case MSG_VT_EN_ECHO:
			vt->echo = true;
			break;
		case MSG_VT_DIS_ECHO:
			vt->echo = false;
			break;
		case MSG_VT_EN_RDLINE:
			vt->readLine = true;
			/* reset reading */
			vt->rlBufPos = 0;
			vt->rlStartCol = vt->col;
			break;
		case MSG_VT_DIS_RDLINE:
			vt->readLine = false;
			break;
		case MSG_VT_EN_NAVI:
			vt->navigation = true;
			break;
		case MSG_VT_DIS_NAVI:
			vt->navigation = false;
			break;
		case MSG_VT_BACKUP:
			if(!vt->screenBackup)
				vt->screenBackup = (char*)malloc(vt->rows * vt->cols * 2);
			memcpy(vt->screenBackup,
					vt->buffer + vt->firstVisLine * vt->cols * 2,
					vt->rows * vt->cols * 2);
			vt->backupCol = vt->col;
			vt->backupRow = vt->row;
			break;
		case MSG_VT_RESTORE:
			if(vt->screenBackup) {
				memcpy(vt->buffer + vt->firstVisLine * vt->cols * 2,
						vt->screenBackup,
						vt->rows * vt->cols * 2);
				free(vt->screenBackup);
				vt->screenBackup = NULL;
				vt->col = vt->backupCol;
				vt->row = vt->backupRow;
				vtctrl_markScrDirty(vt);
			}
			break;
		case MSG_VT_ISVTERM:
			res = 1;
			break;
	}
	unlocku(&vt->lock);
	return res;
}

void vtctrl_scroll(sVTerm *vt,int lines) {
	size_t old;
	old = vt->firstVisLine;
	if(lines > 0) {
		/* ensure that we don't scroll above the first line with content */
		vt->firstVisLine = MAX((int)vt->firstLine,(int)vt->firstVisLine - lines);
	}
	else {
		/* ensure that we don't scroll behind the last line */
		vt->firstVisLine = MIN(HISTORY_SIZE * vt->rows - vt->rows,vt->firstVisLine - lines);
	}

	if(old != vt->firstVisLine)
		vt->upScroll -= lines;
}

void vtctrl_markScrDirty(sVTerm *vt) {
	vtctrl_markDirty(vt,0,0,vt->cols,vt->rows);
}

void vtctrl_markDirty(sVTerm *vt,uint col,uint row,size_t width,size_t height) {
	int x = vt->upCol, y = vt->upRow;
	vt->upCol = MIN(vt->upCol,col);
	vt->upRow = MIN(vt->upRow,row);
	vt->upWidth = MAX(x + vt->upWidth,col + width) - vt->upCol;
	vt->upHeight = MAX(y + vt->upHeight,row + height) - vt->upRow;
	assert(vt->upWidth <= vt->cols);
	assert(vt->upHeight <= vt->rows);
}
