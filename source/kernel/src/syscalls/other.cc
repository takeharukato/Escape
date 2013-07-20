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

#include <sys/common.h>
#include <sys/boot.h>
#include <sys/intrpt.h>
#include <sys/task/timer.h>
#include <sys/mem/paging.h>
#include <sys/task/thread.h>
#include <sys/task/lock.h>
#include <sys/dbg/console.h>
#include <sys/syscalls/other.h>
#include <sys/syscalls.h>
#include <sys/log.h>
#include <sys/config.h>
#include <sys/video.h>
#include <errno.h>

int sysc_loadmods(A_UNUSED Thread *t,sIntrptStackFrame *stack) {
	int res = boot_loadModules(stack);
	SYSC_RET1(stack,res);
}

int sysc_debugc(A_UNUSED Thread *t,sIntrptStackFrame *stack) {
	char c = (char)SYSC_ARG1(stack);
	vid_setTargets(TARGET_LOG);
	vid_printf("%c",c);
	vid_setTargets(TARGET_SCREEN | TARGET_LOG);
	SYSC_RET1(stack,0);
}

int sysc_debug(A_UNUSED Thread *t,A_UNUSED sIntrptStackFrame *stack) {
#if 0
	static size_t foo = 0;
	cache_dbg_setAaFEnabled(foo == 0);
	foo = foo ? 0 : 1;
#else
	cons_start(NULL);
#endif
	SYSC_RET1(stack,0);
}

int sysc_sysconf(A_UNUSED Thread *t,sIntrptStackFrame *stack) {
	int id = SYSC_ARG1(stack);
	long res = conf_get(id);
	if(res < 0)
		SYSC_ERROR(stack,res);
	SYSC_RET1(stack,res);
}

int sysc_tsctotime(A_UNUSED Thread *t,sIntrptStackFrame *stack) {
	uint64_t *tsc = (uint64_t*)SYSC_ARG1(stack);
	if(!paging_isInUserSpace((uintptr_t)tsc,sizeof(uint64_t)))
		SYSC_ERROR(stack,-EINVAL);
	*tsc = timer_cyclesToTime(*tsc);
	SYSC_RET1(stack,0);
}