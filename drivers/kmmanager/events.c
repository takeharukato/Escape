/**
 * $Id$
 * Copyright (C) 2008 - 2009 Nils Asmussen
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
#include <esc/driver.h>
#include <messages.h>
#include <sllist.h>
#include <stdlib.h>
#include <errors.h>
#include <string.h>
#include <assert.h>
#include "events.h"

/* a listener */
typedef struct {
	tTid tid;
	u8 flags;
	u8 key;
	u8 modifier;
} sEventListener;

/**
 * Searches for the given listener
 */
static sEventListener *events_find(tTid tid,u8 flags,u8 key,u8 modifier);

/* all announced listeners */
static sSLList *listener;

void events_init(void) {
	listener = sll_create();
	assert(listener);
}

bool events_send(tDrvId driver,sKmData *km) {
	sSLNode *n;
	sMsg msg;
	bool copied = false;
	for(n = sll_begin(listener); n != NULL; n = n->next) {
		sEventListener *l = (sEventListener*)n->data;
		/* modifiers equal and pressed/released? */
		if(l->modifier == km->modifier &&
			(((l->flags & KE_EV_RELEASED) && km->isBreak) ||
				((l->flags & KE_EV_PRESSED) && !km->isBreak))) {
			/* character/keycode equal? */
			if(((l->flags & KE_EV_KEYCODE) && l->key == km->keycode) ||
					((l->flags & KE_EV_CHARACTER) && l->key == km->character)) {
				tFD fd = getClientThread(driver,l->tid);
				if(fd >= 0) {
					if(!copied) {
						memcpy(&msg.data.d,km,sizeof(sKmData));
						copied = true;
					}
					send(fd,MSG_KM_EVENT,&msg,sizeof(msg.data));
					close(fd);
				}
			}
		}
	}
	return copied;
}

s32 events_add(tTid tid,u8 flags,u8 key,u8 modifier) {
	sEventListener *l;
	if(events_find(tid,flags,key,modifier) != NULL)
		return ERR_LISTENER_EXISTS;

	l = (sEventListener*)malloc(sizeof(sEventListener));
	l->tid = tid;
	l->flags = flags;
	l->key = key;
	l->modifier = modifier;
	if(!sll_append(listener,l)) {
		free(l);
		return ERR_NOT_ENOUGH_MEM;
	}
	return 0;
}

void events_remove(tTid tid,u8 flags,u8 key,u8 modifier) {
	sEventListener *l = events_find(tid,flags,key,modifier);
	if(l) {
		sll_removeFirst(listener,l);
		free(l);
	}
}

static sEventListener *events_find(tTid tid,u8 flags,u8 key,u8 modifier) {
	sSLNode *n;
	for(n = sll_begin(listener); n != NULL; n = n->next) {
		sEventListener *l = (sEventListener*)n->data;
		if(l->tid == tid && l->flags == flags && l->key == key && l->modifier == modifier)
			return l;
	}
	return NULL;
}
