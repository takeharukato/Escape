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

#include <sys/common.h>
#include <sys/task/proc.h>
#include <sys/task/sched.h>
#include <sys/task/signals.h>
#include <sys/mem/kheap.h>
#include <sys/vfs/vfs.h>
#include <sys/vfs/node.h>
#include <sys/vfs/real.h>
#include <sys/vfs/request.h>
#include <sys/kevent.h>
#include <sys/util.h>
#include <sys/video.h>
#include <esc/fsinterface.h>
#include <esc/messages.h>
#include <string.h>
#include <errors.h>


#define REQUEST_COUNT		1024
#define HANDLER_COUNT		32

/**
 * The internal function to get a request
 */
static sRequest *vfsreq_getReqIntern(tTid tid,void *buffer,u32 size,u32 *frameNos,
		u32 frameNoCount,u32 offset);

/* the vfs-driver-file */
static sRequest requests[REQUEST_COUNT];
static fReqHandler handler[HANDLER_COUNT] = {NULL};

void vfsreq_init(void) {
	u32 i;
	sRequest *req;

	req = requests;
	for(i = 0; i < REQUEST_COUNT; i++) {
		req->tid = INVALID_TID;
		req++;
	}
}

bool vfsreq_setHandler(tMsgId id,fReqHandler f) {
	if(id >= HANDLER_COUNT || handler[id] != NULL)
		return false;
	handler[id] = f;
	return true;
}

void vfsreq_sendMsg(tMsgId id,sVFSNode *node,tTid tid,const u8 *data,u32 size) {
	if(id < HANDLER_COUNT && handler[id])
		handler[id](tid,node,data,size);
}

sRequest *vfsreq_getRequest(tTid tid,void *buffer,u32 size) {
	return vfsreq_getReqIntern(tid,buffer,size,NULL,0,0);
}

sRequest *vfsreq_getReadRequest(tTid tid,u32 bufSize,u32 *frameNos,u32 frameNoCount,u32 offset) {
	return vfsreq_getReqIntern(tid,NULL,bufSize,frameNos,frameNoCount,offset);
}

void vfsreq_waitForReply(sRequest *req,bool allowSigs) {
	/* wait */
	thread_wait(req->tid,0,EV_REQ_REPLY);
	if(allowSigs)
		thread_switch();
	else
		thread_switchNoSigs();
	/* if we waked up and the request is not finished, the driver probably died or we received
	 * a signal (if allowSigs is true) */
	if(req->state != REQ_STATE_FINISHED) {
		/* indicate an error */
		req->count = (allowSigs && sig_hasSignalFor(req->tid)) ? ERR_INTERRUPTED : ERR_DRIVER_DIED;
	}
}

static sRequest *vfsreq_getReqIntern(tTid tid,void *buffer,u32 size,u32 *frameNos,
		u32 frameNoCount,u32 offset) {
	u32 i;
	sRequest *req = requests;
	for(i = 0; i < REQUEST_COUNT; i++) {
		if(req->tid == INVALID_TID)
			break;
		req++;
	}
	if(i == REQUEST_COUNT)
		return NULL;

	req->tid = tid;
	req->state = REQ_STATE_WAITING;
	req->val1 = 0;
	req->val2 = 0;
	req->data = buffer;
	req->dsize = size;
	req->readFrNos = frameNos;
	req->readFrNoCount = frameNoCount;
	req->readOffset = offset;
	req->count = 0;
	return req;
}

sRequest *vfsreq_getRequestByTid(tTid tid) {
	u32 i;
	sRequest *req = requests;
	for(i = 0; i < REQUEST_COUNT; i++) {
		if(req->tid == tid) {
			/* the thread may have been terminated... */
			if(thread_getById(tid) == NULL) {
				vfsreq_remRequest(req);
				return NULL;
			}
			return req;
		}
		req++;
	}
	return NULL;
}

void vfsreq_remRequest(sRequest *r) {
	r->tid = INVALID_TID;
}

#if DEBUGGING

void vfsreq_dbg_print(sRequest *r) {
	vid_printf("Request:\n");
	vid_printf("	tid: %d\n",r->tid);
	vid_printf("	state: %d\n",r->state);
	vid_printf("	val1: %d\n",r->val1);
	vid_printf("	val2: %d\n",r->val2);
	vid_printf("	data: %d\n",r->data);
	vid_printf("	dsize: %d\n",r->dsize);
	vid_printf("	readFrNos: %d\n",r->readFrNos);
	vid_printf("	readFrNoCount: %d\n",r->readFrNoCount);
	vid_printf("	readOffset: %d\n",r->readOffset);
	vid_printf("	count: %d\n",r->count);
}

#endif
