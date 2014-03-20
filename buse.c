/*
 * buse - block-device userspace extensions
 * Copyright (C) 2013 Adam Cozzette
 *
 * This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <pthread.h>
#include <sys/poll.h>
#include "threadpool.h"

#include "buse.h"
//using shared thread pool across all devices
threadpool_t *pool;
#define THREAD 32
#define QUEUE  256
static pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
static int devices;
/*
 * These helper functions were taken from cliserv.h in the nbd distribution.
 */
#ifdef WORDS_BIGENDIAN
u_int64_t ntohll(u_int64_t a) {
	return a;
}
#else
u_int64_t ntohll(u_int64_t a) {
	u_int32_t lo = a & 0xffffffff;
	u_int32_t hi = a >> 32U;
	lo = ntohl(lo);
	hi = ntohl(hi);
	return ((u_int64_t) lo) << 32U | hi;
}
#endif
#define htonll ntohll

static int read_all(int fd, char* buf, size_t count) {
	int bytes_read;

	while (count > 0) {
		bytes_read = read(fd, buf, count);

		if (bytes_read > 0) {
			buf += bytes_read;
			count -= bytes_read;
		} else {
			if (errno != EAGAIN)
				fprintf(stderr, "not able to read %s\n", strerror(errno));
		}
	}
	assert(count == 0);

	return 0;
}

static int write_all(int fd, char* buf, size_t count) {
	int bytes_written;

	while (count > 0) {
		bytes_written = write(fd, buf, count);
		if (bytes_written > 0) {
			buf += bytes_written;
			count -= bytes_written;
		} else {
			if (errno != EAGAIN)
				fprintf(stderr, "not able to write %s\n", strerror(errno));
		}
	}
	assert(count == 0);

	return 0;
}

static void list_add_worker(struct cmd_request *w, struct cmd_request *next) {
	pthread_mutex_lock(&(w->session->llock));
	struct cmd_request *prev = next->prev;
	w->next = next;
	w->prev = prev;
	prev->next = w;
	next->prev = w;
	w->session->numrunning++;
	pthread_mutex_unlock(&(w->session->llock));
}

static void list_del_worker(struct cmd_request *w) {
	pthread_mutex_lock(&(w->session->llock));
	struct cmd_request *prev = w->prev;
	struct cmd_request *next = w->next;
	prev->next = next;
	next->prev = prev;
	w->session->numrunning--;
	pthread_mutex_unlock(&(w->session->llock));
}

void mt_do_work(void *data) {
	struct cmd_request *w = (struct cmd_request *) data;
	struct buse_session *mt = w->session;
	u_int64_t from;
	u_int32_t len;
	//request.magic == htonl(NBD_REQUEST_MAGIC);
	struct nbd_reply reply;
	reply.magic = htonl(NBD_REPLY_MAGIC);
	reply.error = htonl(0);

	memcpy(reply.handle, w->handle, sizeof(w->handle));
	switch (ntohl(w->type)) {
	/* I may at some point need to deal with the the fact that the
	 * official nbd server has a maximum buffer size, and divides up
	 * oversized requests into multiple pieces. This applies to reads
	 * and writes.
	 */
	case NBD_CMD_READ:
		len = ntohl(w->len);
		from = ntohll(w->from);
		//fprintf(stderr, "Request for read of size %d\n", len);
		//assert(aop->read);
		w->buf = malloc(len);
		reply.error = mt->aop->read(w->buf, len, from, mt->userdata);
		pthread_mutex_lock(&(mt->wlock));
		write_all(mt->sk, (char*) &reply, sizeof(struct nbd_reply));
		if (w->reply.error == 0)
			write_all(mt->sk, (char*) w->buf, len);
		
		pthread_mutex_unlock(&(mt->wlock));
		break;
	case NBD_CMD_WRITE:
		//fprintf(stderr, "Request for write of size %d\n", len);
		len = ntohl(w->len);
		from = ntohll(w->from);
		reply.error = mt->aop->write(w->buf, len, from, mt->userdata);
			//fprintf(stderr, "Request err write %d\n", reply.error);
		pthread_mutex_lock(&(mt->wlock));
		write_all(mt->sk, (char*) &reply, sizeof(struct nbd_reply));
		pthread_mutex_unlock(&(mt->wlock));
		break;
	case NBD_CMD_DISC:
		/* Handle a disconnect request. */
		// assert(aop->disc);
		//fprintf(stderr, "Received Disconnect!\n");
		mt->aop->disc(mt->userdata);
		mt->go_on = false;
		break;
#ifdef NBD_FLAG_SEND_FLUSH
	case NBD_CMD_FLUSH:
		//assert(aop->flush);
		reply.error = mt->aop->flush(mt->userdata);
			//fprintf(stderr, "Request err flush %d\n", reply.error);
		pthread_mutex_lock(&(mt->wlock));
		write_all(mt->sk, (char*) &reply, sizeof(struct nbd_reply));
		pthread_mutex_unlock(&(mt->wlock));
		break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
	case NBD_CMD_TRIM:
		len = ntohl(w->len);
		from = ntohll(w->from);
		//fprintf(stderr, "Request err trim %d\n", reply.error);
		//fprintf(stderr, "Request for trim of size %d\n", len);
		//assert(aop->trim);
		reply.error = mt->aop->trim(from, len, mt->userdata);
		pthread_mutex_lock(&(mt->wlock));
		write_all(mt->sk, (char*) &reply, sizeof(struct nbd_reply));
		pthread_mutex_unlock(&(mt->wlock));
		break;
#endif
	}
	list_del_worker(w);
	if (w->buf)
		free(w->buf);
	free(w);
}

void disconnectDev(char* device) {
	int nbd = open(device, O_RDWR);

	if (nbd < 0)
		fprintf(stderr,
				"Cannot open NBD: %m\nPlease ensure the 'nbd' module is loaded.");
	//printf("disconnect, ");
	if (ioctl(nbd, NBD_DISCONNECT) < 0)
		fprintf(stderr, "Ioctl failed: %m\n");
	//printf("sock, ");
	if (ioctl(nbd, NBD_CLEAR_SOCK) < 0)
		fprintf(stderr, "Ioctl failed: %m\n");
	close(nbd);
	fprintf(stderr, "disconnected [%s]\n", device);
}

void setSize(char* device,u_int64_t size) {
	int nbd = open(device, O_RDWR);

	if (nbd < 0)
		fprintf(stderr,
				"Cannot open NBD: %m\nPlease ensure the 'nbd' module is loaded.");
	if (ioctl(nbd, NBD_SET_SIZE,(unsigned long)size) < 0)
		fprintf(stderr, "Set Size Failed: %m\n");
	close(nbd);
}

int buse_main(const char* dev_file, struct buse_operations *aop, void *userdata) {
	int sp[2];
	int nbd, err;
	struct nbd_request request;
	buse_session ses;
	ssize_t bytes_read = 0;
	u_int32_t flags = NBD_FLAG_HAS_FLAGS;
	pthread_mutex_lock(&glock);
	bool ready = true;
	if (!pool) {
		fprintf(stderr,
				"Starting thread pool of size [%d] and queue size [%d]\n",
				THREAD, QUEUE);
		pool = threadpool_create(THREAD, QUEUE, 0);
		if (pool == NULL) {
			fprintf(stderr, "Could not start threadpool\n");
			ready = false;
		}
	}
	if (ready)
		devices++;
	fprintf(stderr, "Current number of devices mounted is [%d]\n", devices);
	pthread_mutex_unlock(&glock);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) {
		fprintf(stderr, "Could not open socket\n");
		ready = false;
	}

	nbd = open(dev_file, O_RDWR);
	int oflags = fcntl(sp[0], F_GETFL, 0);
	fcntl(sp[0], F_SETFL, oflags | O_NONBLOCK);
	int sflags = fcntl(sp[1], F_GETFL, 0);
	fcntl(sp[1], F_SETFL, sflags | O_NONBLOCK);

	if (aop->readonly) {
		//fprintf(stderr,"Readonly set\n");
		flags |= NBD_FLAG_READ_ONLY;
	}
#if defined NBD_SET_FLAGS && defined NBD_FLAG_SEND_TRIM
	flags |= NBD_FLAG_SEND_TRIM;
#endif
#if defined NBD_SET_FLAGS && defined NBD_FLAG_SEND_FLUSH
	flags |= NBD_FLAG_SEND_FLUSH;
#endif
	if (nbd == -1) {
		fprintf(stderr,
				"Cannot open NBD: %m\nPlease ensure the 'nbd' module is loaded.\n");
		ready = false;
	}
	if (ioctl(nbd, NBD_SET_BLKSIZE, (unsigned long) aop->blocksize) < 0)
		fprintf(stderr, "Ioctl/1.1a failed: %m\n");
	if (ioctl(nbd, NBD_SET_SIZE_BLOCKS,
			aop->size / (unsigned long) aop->blocksize) < 0)
		fprintf(stderr, "Ioctl/1.1b failed: %m\n");

	ioctl(nbd, NBD_CLEAR_SOCK);
	if (!fork()) {
		/* The child needs to continue setting things up. */
		close(sp[0]);
		ses.sk = sp[1];
		if (ioctl(nbd, NBD_SET_SOCK, ses.sk) == -1) {
			fprintf(stderr, "ioctl(nbd, NBD_SET_SOCK, sk) failed.[%s]\n",
					strerror(errno));
		}
#if defined NBD_SET_FLAGS
		else if (ioctl(nbd, NBD_SET_FLAGS, flags) == -1) {
			fprintf(stderr,
					"ioctl(nbd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM) failed.[%s]\n",
					strerror(errno));
		}
#endif
		else {
			err = ioctl(nbd, NBD_DO_IT);
			fprintf(stderr, "nbd device [%s] terminated with code %d\n",
					dev_file, err);
			if (err == -1)
				fprintf(stderr, "%s\n", strerror(errno));
		}
		ioctl(nbd, NBD_CLEAR_QUE);
		ioctl(nbd, NBD_CLEAR_SOCK);

	}

	/* The parent opens the device file at least once, to make sure the
	 * partition table is updated. Then it closes it and starts serving up
	 * requests. */

	close(sp[1]);
	//sk = sp[0];

	//fprintf(stderr, "waiting from requests on [%s]\n",dev_file);
	ses.sk = sp[0];
	ses.userdata = userdata;
	ses.aop = aop;
	ses.numworker = 0;
	ses.numavail = 0;
	ses.numrunning = 0;
	struct pollfd ufds[1];
	ufds[0].fd = ses.sk;
	ufds[0].events = POLLIN | POLLPRI; // check for normal or out-of-band
	ses.main_running.prev = ses.main_running.next = &ses.main_running;
	if (pthread_mutex_init(&(ses.rlock), NULL) != 0) {
		fprintf(stderr, "unable to init ses.rlock \n");
	}
	if (pthread_mutex_init(&(ses.wlock), NULL) != 0) {
		fprintf(stderr, "unable to init ses.wlock \n");
	}
	if (pthread_mutex_init(&(ses.llock), NULL) != 0) {
		fprintf(stderr, "unable to init ses.llock \n");
	}
	ses.go_on = ready;
	int rv;
	while (ses.go_on) {
		rv = poll(ufds, 1, -1);

		if (rv == -1) {
			perror("poll"); // error occurred in poll()
			ses.go_on = false;
		} else if (rv == 0) {
			//printf("Timeout occurred! Not Sure why\n");
		} else {
			pthread_mutex_lock(&(ses.rlock));
			bytes_read = read(ses.sk, &request, sizeof(request));
			if (bytes_read > 0) {
				struct cmd_request *cr = calloc(1, sizeof(struct cmd_request));
				//assert(bytes_read == sizeof(request));

				memcpy(cr->handle, request.handle, sizeof(cr->handle));
				cr->from = request.from;
				cr->len = request.len;
				cr->magic = request.magic;
				cr->type = request.type;
				cr->session = &ses;
				u_int32_t len;
				if (ntohl(cr->type) == NBD_CMD_WRITE) {
					len = ntohl(cr->len);
					//assert(aop->write);
					cr->buf = malloc(len);
					read_all(ses.sk, cr->buf, len);
				}
				if (ntohl(cr->type) == NBD_CMD_DISC) { /* Handle a disconnect request. */
					// assert(aop->disc);
					fprintf(stderr, "Received Disconnect!\n");
					ses.aop->disc(ses.userdata);
					ses.go_on = false;
					free(cr);
				}
				if (ses.go_on)
					list_add_worker(cr, &ses.main_running);
				pthread_mutex_unlock(&(ses.rlock));
				if (ses.go_on) {
					if (threadpool_add(pool, &mt_do_work, cr, 0) != 0) {
						/*
						fprintf(stderr,
								"thread pool full\n");
						*/
						mt_do_work(cr);
					}
				}
			} else {
				pthread_mutex_unlock(&(ses.rlock));
				if (errno != EAGAIN)
					ses.go_on = false;
			}
		}

	}
	fprintf(stderr, "%s closing\n", dev_file);
	while (ses.numrunning > 0) {
		sleep(1);
	}
	ioctl(nbd, NBD_CLEAR_SOCK);
	pthread_mutex_lock(&glock);
	devices--;
	pthread_mutex_unlock(&glock);
	close(ses.sk);
	close(nbd);
	pthread_mutex_destroy(&(ses.llock));
	pthread_mutex_destroy(&(ses.rlock));
	pthread_mutex_destroy(&(ses.wlock));
	if (bytes_read == -1)
		fprintf(stderr, "error %s\n", strerror(errno));
	fprintf(stderr, "%s exited\n", dev_file);
	return 0;
}

