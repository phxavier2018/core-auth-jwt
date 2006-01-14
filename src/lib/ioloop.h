#ifndef __IOLOOP_H
#define __IOLOOP_H

#include <sys/time.h>
#include <time.h>

struct io;
struct timeout;
struct ioloop;

enum io_condition {
	IO_READ		= 0x01,
	IO_WRITE	= 0x02,
	IO_ERROR	= 0x04,
	
	/* internal */
	IO_NOTIFY	= 0x04,
};

typedef void io_callback_t(void *context);
typedef void timeout_callback_t(void *context);

/* Time when the I/O loop started calling handlers.
   Can be used instead of time(NULL). */
extern time_t ioloop_time;
extern struct timeval ioloop_timeval;
extern struct timezone ioloop_timezone;

extern struct ioloop *current_ioloop;

/* I/O listeners - you can create different handlers for IO_READ and IO_WRITE,
   but make sure you don't create multiple handlers of same type, it's not
   checked and removing one will stop the other from working as well.

 */
struct io *io_add(int fd, enum io_condition condition,
		  io_callback_t *callback, void *context);
struct io *io_add_notify(const char *path, io_callback_t *callback,
			 void *context);
/* Remove I/O handler, and set io pointer to NULL. */
void io_remove(struct io **io);

/* Timeout handlers */
struct timeout *timeout_add(unsigned int msecs, timeout_callback_t *callback,
			    void *context);
/* Remove timeout handler, and set timeout pointer to NULL. */
void timeout_remove(struct timeout **timeout);

void io_loop_run(struct ioloop *ioloop);
void io_loop_stop(struct ioloop *ioloop); /* safe to run in signal handler */

bool io_loop_is_running(struct ioloop *ioloop);

/* call these if you wish to run the iteration only once */
void io_loop_set_running(struct ioloop *ioloop);
void io_loop_handler_run(struct ioloop *ioloop);

struct ioloop *io_loop_create(pool_t pool);
/* Destroy I/O loop and set ioloop pointer to NULL. */
void io_loop_destroy(struct ioloop **ioloop);

#endif
