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
#include <sys/vfs/vfs.h>
#include <sys/vfs/node.h>
#include <sys/vfs/info.h>
#include <sys/vfs/devmsgs.h>
#include <sys/vfs/file.h>
#include <sys/vfs/dir.h>
#include <sys/vfs/link.h>
#include <sys/mem/paging.h>
#include <sys/mem/cache.h>
#include <sys/mem/pmem.h>
#include <sys/mem/dynarray.h>
#include <sys/task/env.h>
#include <sys/task/groups.h>
#include <sys/util.h>
#include <sys/video.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

static int vfs_node_createFile(pid_t pid,const char *path,sVFSNode *dir,inode_t *nodeNo,
		bool *created);
static void vfs_node_doDestroy(sVFSNode *n,bool remove);
static sVFSNode *vfs_node_requestNode(void);
static void vfs_node_releaseNode(sVFSNode *node);
static void vfs_node_dbg_doPrintTree(size_t level,sVFSNode *parent);

/* all nodes (expand dynamically) */
static sDynArray nodeArray;
/* a pointer to the first free node (which points to the next and so on) */
static sVFSNode *freeList = NULL;
static uint nextUsageId = 0;
static klock_t nodesLock;

void vfs_node_init(void) {
	dyna_start(&nodeArray,sizeof(sVFSNode),VFSNODE_AREA,VFSNODE_AREA_SIZE);
}

bool vfs_node_isValid(inode_t nodeNo) {
	return nodeNo >= 0 && nodeNo < (inode_t)nodeArray.objCount;
}

inode_t vfs_node_getNo(const sVFSNode *node) {
	return (inode_t)dyna_getIndex(&nodeArray,node);
}

sVFSNode *vfs_node_request(inode_t nodeNo) {
	sVFSNode *n = (sVFSNode*)dyna_getObj(&nodeArray,nodeNo);
	spinlock_aquire(&n->lock);
	if(n->name)
		return n;
	spinlock_release(&n->lock);
	return NULL;
}

void vfs_node_release(sVFSNode *node) {
	spinlock_release(&node->lock);
}

sVFSNode *vfs_node_get(inode_t nodeNo) {
	return (sVFSNode*)dyna_getObj(&nodeArray,nodeNo);
}

static sVFSNode *vfs_node_openDirOf(inode_t nodeNo,bool *isValid) {
	sVFSNode *dir = vfs_node_request(nodeNo);
	sVFSNode *parent;
	if(dir) {
		if(!S_ISLNK(dir->mode))
			parent = dir;
		else {
			parent = vfs_link_resolve(dir);
			vfs_node_release(dir);
			spinlock_aquire(&parent->lock);
			*isValid = parent->name != NULL;
		}
		return parent->firstChild;
	}
	return NULL;
}

sVFSNode *vfs_node_openDir(sVFSNode *dir,bool locked,bool *isValid) {
	sVFSNode *parent;
	if(!S_ISLNK(dir->mode))
		parent = dir;
	else
		parent = vfs_link_resolve(dir);
	if(locked)
		spinlock_aquire(&parent->lock);
	*isValid = parent->name != NULL;
	return parent->firstChild;
}

void vfs_node_closeDir(sVFSNode *dir,bool locked) {
	sVFSNode *parent;
	if(!S_ISLNK(dir->mode))
		parent = dir;
	else
		parent = vfs_link_resolve(dir);
	if(locked)
		spinlock_release(&parent->lock);
}

int vfs_node_isEmptyDir(sVFSNode *node) {
	bool valid;
	if(!S_ISDIR(node->mode))
		return -ENOTDIR;
	sVFSNode *c = vfs_node_openDir(node,false,&valid);
	if(valid) {
		bool res = c->next && !c->next->next;
		vfs_node_closeDir(node,false);
		return res ? 0 : -ENOTEMPTY;
	}
	return -EDESTROYED;
}

int vfs_node_getInfo(pid_t pid,inode_t nodeNo,USER sFileInfo *info) {
	sVFSNode *n = vfs_node_request(nodeNo);
	if(n == NULL)
		return -ENOENT;

	/* some infos are not available here */
	/* TODO needs to be completed */
	info->device = VFS_DEV_NO;
	info->accesstime = 0;
	info->modifytime = 0;
	info->createtime = 0;
	info->blockCount = 0;
	info->blockSize = 512;
	info->inodeNo = nodeNo;
	info->linkCount = 1;
	info->uid = n->uid;
	info->gid = n->gid;
	info->mode = n->mode;
	info->size = n->getSize ? n->getSize(pid,n) : 0;
	vfs_node_release(n);
	return 0;
}

int vfs_node_chmod(pid_t pid,inode_t nodeNo,mode_t mode) {
	int res = 0;
	const Proc *p = pid == KERNEL_PID ? NULL : Proc::getByPid(pid);
	sVFSNode *n = vfs_node_request(nodeNo);
	if(n == NULL)
		return -ENOENT;
	/* root can chmod everything; others can only chmod their own files */
	if(p && p->getEUid() != n->uid && p->getEUid() != ROOT_UID)
		res = -EPERM;
	else
		n->mode = (n->mode & ~MODE_PERM) | (mode & MODE_PERM);
	vfs_node_release(n);
	return res;
}

int vfs_node_chown(pid_t pid,inode_t nodeNo,uid_t uid,gid_t gid) {
	int res = 0;
	const Proc *p = pid == KERNEL_PID ? NULL : Proc::getByPid(pid);
	sVFSNode *n = vfs_node_request(nodeNo);
	if(n == NULL)
		return -ENOENT;

	/* root can chown everything; others can only chown their own files */
	if(p && p->getEUid() != n->uid && p->getEUid() != ROOT_UID)
		res = -EPERM;
	else if(p && p->getEUid() != ROOT_UID) {
		/* users can't change the owner */
		if(uid != (uid_t)-1 && uid != n->uid && uid != p->getEUid())
			res = -EPERM;
		/* users can change the group only to a group they're a member of */
		else if(gid != (gid_t)-1 && gid != n->gid && gid != p->getEGid() &&
				!groups_contains(p->getPid(),gid))
			res = -EPERM;
	}

	if(res == 0) {
		if(uid != (uid_t)-1)
			n->uid = uid;
		if(gid != (gid_t)-1)
			n->gid = gid;
	}
	vfs_node_release(n);
	return res;
}

const char *vfs_node_getPath(inode_t nodeNo) {
	static char path[MAX_PATH_LEN];
	size_t nlen,len = 0,total = 0;
	sVFSNode *node = vfs_node_get(nodeNo);
	sVFSNode *n = node;
	if(n->name == NULL)
		return "<destroyed>";

	while(n->parent != NULL) {
		/* name + slash */
		total += n->nameLen + 1;
		n = n->parent;
	}

	/* not nice, but ensures that we don't overwrite something :) */
	if(total > MAX_PATH_LEN)
		total = MAX_PATH_LEN;

	n = node;
	len = total;
	while(n->parent != NULL) {
		nlen = n->nameLen + 1;
		/* insert the new element */
		*(path + total - nlen) = '/';
		memcpy(path + total + 1 - nlen,n->name,nlen - 1);
		total -= nlen;
		n = n->parent;
	}

	/* terminate */
	*(path + len) = '\0';
	return (char*)path;
}

int vfs_node_resolvePath(const char *path,inode_t *nodeNo,bool *created,uint flags) {
	sVFSNode *dir,*n = vfs_node_get(0);
	const Thread *t = Thread::getRunning();
	/* at the beginning, t might be NULL */
	pid_t pid = t ? t->getProc()->getPid() : KERNEL_PID;
	int pos = 0,err,depth,lastdepth;
	bool isValid;
	if(created)
		*created = false;
	/* not initialized? */
	if(nodeArray.objSize == 0)
		return -ENOTSUP;

	/* no absolute path? */
	if(*path != '/')
		return -EINVAL;

	/* skip slashes */
	while(*path == '/')
		path++;

	/* root/current node requested? */
	if(!*path) {
		*nodeNo = vfs_node_getNo(n);
		return 0;
	}

	depth = 0;
	lastdepth = -1;
	dir = n;
	n = vfs_node_openDir(dir,true,&isValid);
	if(isValid) {
		while(n != NULL) {
			/* go to next '/' and check for invalid chars */
			if(depth != lastdepth) {
				char c;
				/* check if we can access this directory */
				if((err = vfs_hasAccess(pid,dir,VFS_EXEC)) < 0)
					goto done;

				pos = 0;
				while((c = path[pos]) && c != '/') {
					if((c != ' ' && isspace(c)) || !isprint(c)) {
						err = -EINVAL;
						goto done;
					}
					pos++;
				}
				lastdepth = depth;
			}

			if((int)n->nameLen == pos && strncmp(n->name,path,pos) == 0) {
				path += pos;
				/* finished? */
				if(!*path)
					break;

				/* skip slashes */
				while(*path == '/')
					path++;
				/* "/" at the end is optional */
				if(!*path)
					break;

				if(IS_DEVICE(n->mode))
					break;

				/* move to childs of this node */
				vfs_node_closeDir(dir,true);
				dir = n;
				n = vfs_node_openDir(dir,true,&isValid);
				if(!isValid) {
					err = -EDESTROYED;
					goto done;
				}
				depth++;
				continue;
			}
			n = n->next;
		}
	}

	err = 0;
	if(n == NULL) {
		/* not existing file/dir in root-directory means that we should ask fs */
		/* Note: this means that no one can create (additional) virtual nodes in the root-directory,
		 * which is intended. The existing virtual nodes in the root-directory, of course, hide
		 * possibly existing directory-entries in the real filesystem with the same name. */
		if(depth == 0)
			err = -EREALPATH;
		/* should we create a default-file? */
		else if((flags & VFS_CREATE) && S_ISDIR(dir->mode))
			err = vfs_node_createFile(pid,path,dir,nodeNo,created);
		else
			err = -ENOENT;
	}
	else {
		/* resolve link */
		if(!(flags & VFS_NOLINKRES) && S_ISLNK(n->mode))
			n = vfs_link_resolve(n);

		/* virtual node */
		*nodeNo = vfs_node_getNo(n);
	}
done:
	vfs_node_closeDir(dir,true);
	return err;
}

char *vfs_node_basename(char *path,size_t *len) {
	char *p = path + *len - 1;
	while(*p == '/') {
		p--;
		(*len)--;
	}
	*(p + 1) = '\0';
	if((p = strrchr(path,'/')) == NULL)
		return path;
	return p + 1;
}

void vfs_node_dirname(char *path,size_t len) {
	char *p = path + len - 1;
	/* remove last '/' */
	while(*p == '/') {
		p--;
		len--;
	}

	/* nothing to remove? */
	if(len == 0)
		return;

	/* remove last path component */
	while(*p != '/')
		p--;

	/* set new end */
	*(p + 1) = '\0';
}

sVFSNode *vfs_node_findInDir(sVFSNode *dir,const char *name,size_t nameLen) {
	bool isValid = false;
	sVFSNode *res = NULL;
	sVFSNode *n = vfs_node_openDir(dir,false,&isValid);
	if(isValid) {
		while(n != NULL) {
			if(n->nameLen == nameLen && strncmp(n->name,name,nameLen) == 0) {
				res = n;
				break;
			}
			n = n->next;
		}
	}
	vfs_node_closeDir(dir,false);
	return res;
}

sVFSNode *vfs_node_findInDirOf(inode_t nodeNo,const char *name,size_t nameLen) {
	bool isValid = false;
	sVFSNode *res = NULL;
	sVFSNode *n = vfs_node_openDirOf(nodeNo,&isValid);
	if(isValid) {
		while(n != NULL) {
			if(n->nameLen == nameLen && strncmp(n->name,name,nameLen) == 0) {
				res = n;
				break;
			}
			n = n->next;
		}
	}
	vfs_node_closeDir(vfs_node_get(nodeNo),true);
	return res;
}

sVFSNode *vfs_node_create(pid_t pid,char *name) {
	sVFSNode *node;
	size_t nameLen = strlen(name);
	const Proc *p = pid != INVALID_PID ? Proc::getByPid(pid) : NULL;
	vassert(name != NULL,"name == NULL");

	if(nameLen > MAX_NAME_LEN)
		return NULL;

	node = vfs_node_requestNode();
	if(node == NULL)
		return NULL;

	/* ensure that all values are initialized properly */
	*(pid_t*)&node->owner = pid;
	node->uid = p ? p->getEUid() : ROOT_UID;
	node->gid = p ? p->getEGid() : ROOT_GID;
	*(char**)&node->name = name;
	*(size_t*)&node->nameLen = nameLen;
	node->mode = 0;
	node->refCount = 1;
	node->next = NULL;
	node->prev = NULL;
	node->firstChild = NULL;
	node->lastChild = NULL;
	*(sVFSNode**)&node->parent = NULL;
	return node;
}

void vfs_node_append(sVFSNode *parent,sVFSNode *node) {
	if(parent != NULL) {
		/* we assume here that the parent is locked */
		if(parent->firstChild == NULL)
			parent->firstChild = node;
		if(parent->lastChild != NULL)
			parent->lastChild->next = node;
		node->prev = parent->lastChild;
		parent->lastChild = node;
	}
	*(sVFSNode**)&node->parent = parent;
}

void vfs_node_destroy(sVFSNode *n) {
	vfs_node_doDestroy(n,false);
}

void vfs_node_destroyNow(sVFSNode *n) {
	vfs_node_doDestroy(n,true);
}

static void vfs_node_doDestroy(sVFSNode *n,bool remove) {
	/* remove childs */
	sVFSNode *parent;
	sVFSNode *tn;
	sVFSNode *child;
	bool norefs;
	spinlock_aquire(&n->lock);
	norefs = --n->refCount == 0;
	spinlock_release(&n->lock);

	if(norefs || remove) {
		child = n->firstChild;
		while(child != NULL) {
			tn = child->next;
			vfs_node_doDestroy(child,remove);
			child = tn;
		}
	}

	/* aquire both locks before n->destroy(). we can't destroy the node-data without lock because
	 * otherwise one could access it before the node is removed from the tree */
	spinlock_aquire(&n->lock);
	parent = n->parent;
	if(parent)
		spinlock_aquire(&parent->lock);

	/* take care that we don't destroy the node twice */
	if(norefs) {
		/* let the node clean up */
		if(n->destroy)
			n->destroy(n);
	}
	if((norefs || remove) && n->name) {
		/* free name */
		if(IS_ON_HEAP(n->name))
			cache_free((void*)n->name);
		*(char**)&n->name = NULL;

		/* remove from parent and release (attention: maybe its not yet in the tree) */
		if(n->prev)
			n->prev->next = n->next;
		else if(parent)
			parent->firstChild = n->next;
		if(n->next)
			n->next->prev = n->prev;
		else if(parent)
			parent->lastChild = n->prev;
		n->prev = NULL;
		n->next = NULL;
		n->firstChild = NULL;
		n->lastChild = NULL;
		*(sVFSNode**)&n->parent = NULL;
	}

	if(parent)
		spinlock_release(&parent->lock);
	spinlock_release(&n->lock);

	/* if there are no references anymore, we can put the node on the freelist */
	if(norefs)
		vfs_node_releaseNode(n);
}

char *vfs_node_getId(pid_t pid) {
	char *name;
	size_t len;
	uint id;
	/* we want a id in the form <pid>.<x>, i.e. 2 ints, a '.' and '\0'. thus, allowing up to 31
	 * digits per int is enough, even for 64-bit ints */
	const size_t size = 64;
	name = (char*)cache_alloc(size);
	if(name == NULL)
		return NULL;

	/* create usage-node */
	itoa(name,size,pid);
	len = strlen(name);
	*(name + len) = '.';
	spinlock_aquire(&nodesLock);
	id = nextUsageId++;
	spinlock_release(&nodesLock);
	itoa(name + len + 1,size - (len + 1),id);
	return name;
}

void vfs_node_printTree(void) {
	vid_printf("VFS:\n");
	vid_printf("/\n");
	vfs_node_dbg_doPrintTree(1,vfs_node_get(0));
}

void vfs_node_printNode(const sVFSNode *node) {
	vid_printf("VFSNode @ %p:\n",node);
	if(node) {
		vid_printf("\tname: %s\n",node->name ? node->name : "NULL");
		vid_printf("\tfirstChild: %p\n",node->firstChild);
		vid_printf("\tlastChild: %p\n",node->lastChild);
		vid_printf("\tnext: %p\n",node->next);
		vid_printf("\tprev: %p\n",node->prev);
		vid_printf("\towner: %d\n",node->owner);
	}
}

static int vfs_node_createFile(pid_t pid,const char *path,sVFSNode *dir,inode_t *nodeNo,
		bool *created) {
	size_t nameLen;
	sVFSNode *child;
	char *nameCpy;
	char *nextSlash;
	int err;
	/* can we create files in this directory? */
	if((err = vfs_hasAccess(pid,dir,VFS_WRITE)) < 0)
		return err;

	nextSlash = strchr(path,'/');
	if(nextSlash) {
		/* if there is still a slash in the path, we can't create the file */
		if(*(nextSlash + 1) != '\0')
			return -EINVAL;
		*nextSlash = '\0';
		nameLen = nextSlash - path;
	}
	else
		nameLen = strlen(path);
	/* copy the name because vfs_file_create() will store the pointer */
	nameCpy = (char*)cache_alloc(nameLen + 1);
	if(nameCpy == NULL)
		return -ENOMEM;
	memcpy(nameCpy,path,nameLen + 1);
	/* now create the node and pass the node-number back */
	if((child = vfs_file_create(pid,dir,nameCpy,vfs_file_read,vfs_file_write)) == NULL) {
		cache_free(nameCpy);
		return -ENOMEM;
	}
	if(created)
		*created = true;
	*nodeNo = vfs_node_getNo(child);
	return 0;
}

static sVFSNode *vfs_node_requestNode(void) {
	sVFSNode *node = NULL;
	spinlock_aquire(&nodesLock);
	if(freeList == NULL) {
		size_t i,oldCount = nodeArray.objCount;
		if(!dyna_extend(&nodeArray)) {
			spinlock_release(&nodesLock);
			return NULL;
		}
		freeList = vfs_node_get(oldCount);
		for(i = oldCount; i < nodeArray.objCount - 1; i++) {
			node = vfs_node_get(i);
			node->next = vfs_node_get(i + 1);
		}
		node->next = NULL;
	}

	node = freeList;
	freeList = freeList->next;
	spinlock_release(&nodesLock);
	return node;
}

static void vfs_node_releaseNode(sVFSNode *node) {
	vassert(node != NULL,"node == NULL");
	/* mark unused */
	*(char**)&node->name = NULL;
	*(size_t*)&node->nameLen = 0;
	*(pid_t*)&node->owner = INVALID_PID;
	spinlock_aquire(&nodesLock);
	node->next = freeList;
	freeList = node;
	spinlock_release(&nodesLock);
}

static void vfs_node_dbg_doPrintTree(size_t level,sVFSNode *parent) {
	size_t i;
	bool isValid;
	sVFSNode *n = vfs_node_openDir(parent,true,&isValid);
	if(isValid) {
		while(n != NULL) {
			for(i = 0;i < level;i++)
				vid_printf(" |");
			vid_printf("- %s\n",n->name);
			/* don't recurse for "." and ".." */
			if(strncmp(n->name,".",1) != 0 && strncmp(n->name,"..",2) != 0)
				vfs_node_dbg_doPrintTree(level + 1,n);
			n = n->next;
		}
	}
	vfs_node_closeDir(parent,true);
}