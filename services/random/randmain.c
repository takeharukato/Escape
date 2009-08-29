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
#include <esc/service.h>
#include <esc/io.h>
#include <esc/fileio.h>
#include <esc/date.h>
#include <stdlib.h>
#include <messages.h>
#include <errors.h>

static sMsg msg;

int main(void) {
	tServ id,client;
	tMsgId mid;

	id = regService("random",SERV_DRIVER);
	if(id < 0) {
		printe("Unable to register service 'random'");
		return EXIT_FAILURE;
	}

	/* random numbers are always available ;) */
	setDataReadable(id,true);
	srand(getTime());

    /* wait for commands */
	while(1) {
		tFD fd = getClient(&id,1,&client);
		if(fd < 0)
			wait(EV_CLIENT);
		else {
			while(receive(fd,&mid,&msg,sizeof(msg)) > 0) {
				switch(mid) {
					case MSG_DRV_OPEN:
						msg.args.arg1 = 0;
						send(fd,MSG_DRV_OPEN_RESP,&msg,sizeof(msg.args));
						break;
					case MSG_DRV_READ: {
						/* offset is ignored here */
						u32 count = msg.args.arg2;
						u32 *data = (u32*)malloc(count);
						msg.args.arg1 = 0;
						if(data) {
							u32 *d = data;
							msg.args.arg1 = count;
							count /= sizeof(u32);
							while(count-- > 0)
								*d++ = rand();
						}
						msg.args.arg2 = true;
						send(fd,MSG_DRV_READ_RESP,&msg,sizeof(msg.args));
						if(data) {
							send(fd,MSG_DRV_READ_RESP,data,msg.args.arg1 / sizeof(u32));
							free(data);
						}
					}
					break;
					case MSG_DRV_WRITE:
						msg.args.arg1 = ERR_UNSUPPORTED_OPERATION;
						send(fd,MSG_DRV_WRITE_RESP,&msg,sizeof(msg.args));
						break;
					case MSG_DRV_IOCTL: {
						msg.data.arg1 = ERR_UNSUPPORTED_OPERATION;
						send(fd,MSG_DRV_IOCTL_RESP,&msg,sizeof(msg.data));
					}
					break;
					case MSG_DRV_CLOSE:
						break;
				}
			}
			close(fd);
		}
	}

	/* clean up */
	unregService(id);
	return EXIT_SUCCESS;
}
