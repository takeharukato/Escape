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

#include <sys/common.h>
#include <sys/conf.h>
#include <sys/esccodes.h>
#include <sys/proc.h>
#include <sys/sync.h>
#include <sys/thread.h>
#include <usergroup/usergroup.h>
#include <stdio.h>
#include <stdlib.h>

#include "jobmng.h"
#include "keystrokes.h"
#include "screens.h"

const int Keystrokes::VGA_MODE			= 3;

const char *Keystrokes::VTERM_PROG		= "/sbin/vterm";
const char *Keystrokes::LOGIN_PROG		= "/bin/login";

const char *Keystrokes::WINMNG_PROG		= "/sbin/winmng";
const char *Keystrokes::GLOGIN_PROG		= "/bin/glogin";

const char *Keystrokes::TUI_DEF_COLS	= "100";
const char *Keystrokes::TUI_DEF_ROWS	= "37";

const char *Keystrokes::GUI_DEF_RES_X	= "800";
const char *Keystrokes::GUI_DEF_RES_Y	= "600";

const char *Keystrokes::GROUP_NAME		= "ui";

void Keystrokes::createConsole(const char *mng,const char *cols,const char *rows,const char *login,
							   const char *termVar) {
	char name[SSTRLEN("ui") + 11];
	char path[SSTRLEN("/dev/ui") + 11];

	int maxfds = sysconf(CONF_MAX_FDS);

	int id = JobMng::requestId();
	if(id < 0) {
		printe("Maximum number of clients reached");
		return;
	}

	snprintf(name,sizeof(name),"ui%d",id);
	snprintf(path,sizeof(path),"/dev/%s",name);

	print("Starting %s %s %s %s",mng,cols,rows,name);

	// determine groups, gid and uid in parent process (we can't use global stuff in the child)
	size_t groupCount;
	gid_t *groups = usergroup_collectGroupsFor(GROUP_NAME,1,&groupCount);
	if(!groups) {
		printe("Unable to collect groups of ui");
		return;
	}
	int gid = usergroup_getGid(GROUP_NAME);
	if(gid < 0) {
		printe("Unable to get group of ui");
		return;
	}
	int uid = usergroup_nameToId(USERS_PATH,GROUP_NAME);
	if(uid < 0) {
		printe("Unable to get user id of ui");
		return;
	}

	int mngPid = fork();
	if(mngPid < 0) {
		printe("fork failed");
		return;
		JobMng::releaseId(id);
	}
	if(mngPid == 0) {
		/* ATTENTION: since we have multiple threads, we have to be REALLY careful what we do here.
		 * we basically can only do simple system calls, because we cannot be sure that our program
		 * state is consistent. This is because other threads could have been at an arbitrary
		 * position while we forked, so that, for example, locks could be taken, data structures
		 * could be inconsistent and so on. */

		/* close all but stdin, stdout, stderr, strace */
		for(int i = 4; i < maxfds; ++i)
			close(i);

		if(setgid(gid) < 0)
			error("Unable to set gid");
		if(setuid(uid) < 0)
			error("Unable to set uid");
		if(setgroups(groupCount,groups) < 0)
			error("Unable to set groups");

		const char *args[] = {mng,cols,rows,name,NULL};
		execv(mng,args);
		error("exec with %s failed",mng);
	}

	JobMng::setTermPid(id,mngPid);

	print("Waiting for %s",path);
	/* TODO not good */
	int fd;
	while((fd = open(path,O_MSGS)) < 0) {
		if(fd != -ENOENT)
			printe("Unable to open '%s'",path);
		if(!JobMng::exists(id))
			return;
		usleep(1000 * 20);
	}
	close(fd);

	print("Starting %s",login);

	/* we can't do that in the child (see ATTENTION comment above), so do it here */
	/* we don't need the env var in uimng anyway, so it doesn't hurt to set it */
	setenv(termVar,path);

	int loginPid = fork();
	if(loginPid < 0) {
		printe("fork failed");
		return;
	}
	if(loginPid == 0) {
		/* close all; login will open different streams */
		for(int i = 0; i < maxfds; ++i) {
			if(i != STRACE_FILENO)
				close(i);
		}

		const char *args[] = {login,NULL};
		execv(login,args);
		error("exec with %s failed",login);
	}

	JobMng::setLoginPid(id,loginPid);
}

void Keystrokes::switchToVGA() {
	esc::Screen *scr;
	esc::Screen::Mode mode;
	if(ScreenMng::find(VGA_MODE,&mode,&scr)) {
		try {
			scr->setMode(esc::Screen::MODE_TYPE_TUI,VGA_MODE,-1,true);
		}
		catch(const std::exception &e) {
			printe("Unable to switch to VGA: %s",e.what());
		}
	}
	else
		printe("Unable to find screen for mode %d",VGA_MODE);
}
