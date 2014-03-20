#ifndef BUSE_H_INCLUDED
#define BUSE_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Most of this file was copied from nbd.h in the nbd distribution. */
#include <linux/types.h>
#include <sys/types.h>
#include <linux/nbd.h>
#include <stdbool.h>
#include <pthread.h>

struct buse_operations {
	int (*read)(void *buf, u_int32_t len, u_int64_t offset, void *userdata);
	int (*write)(const void *buf, u_int32_t len, u_int64_t offset,
			void *userdata);
	void (*disc)(void *userdata);
	int (*flush)(void *userdata);
	int (*trim)(u_int64_t from, u_int32_t len, void *userdata);
	int blocksize;
	u_int64_t size;
	bool readonly;
	int nbd;
};

typedef struct cmd_request {
	struct nbd_request request;
	struct nbd_reply reply;
	char *buf;
	struct buse_session *session;
	struct cmd_request *prev;
	struct cmd_request *next;
	 __be32 magic;
	 __be32 type;    /* == READ || == WRITE  */
	 char handle[8];
	 __be64 from;
	 __be32 len;
}cmd_request;

typedef struct buse_session {
	int sk;
	void *userdata;
	pthread_mutex_t wlock;
	pthread_mutex_t rlock;
	pthread_mutex_t llock;
	bool go_on;
	struct buse_operations *aop;
	int numworker;
	int numavail;
	int numrunning;
	struct cmd_request main_pool;
	struct cmd_request main_running;

}buse_session;

int buse_main(const char* dev_file, struct buse_operations *bop,
		void *userdata);
void disconnectDev(char* device);
void setSize(char* device,u_int64_t size);

#ifdef __cplusplus
}
#endif

#endif /* BUSE_H_INCLUDED */
