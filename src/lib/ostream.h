#ifndef __OSTREAM_H
#define __OSTREAM_H

#include "ioloop.h"

struct ostream {
	uoff_t offset;

	int stream_errno;
	/* overflow is set when some of the data given to send()
	   functions was neither sent nor buffered. It's never unset inside
	   ostream code. */
	unsigned int overflow:1;
	unsigned int closed:1;

	struct _ostream *real_stream;
};

/* Returns 1 if all data is sent (not necessarily flushed), 0 if not.
   Pretty much the only real reason to return 0 is if you wish to send more
   data to client which isn't buffered, eg. o_stream_send_istream(). */
typedef int stream_flush_callback_t(void *context);

/* Create new output stream from given file descriptor.
   If max_buffer_size is 0, an "optimal" buffer size is used (max 128kB). */
struct ostream *
o_stream_create_file(int fd, pool_t pool, size_t max_buffer_size,
		     bool autoclose_fd);

/* Reference counting. References start from 1, so calling o_stream_unref()
   destroys the stream if o_stream_ref() is never used. */
void o_stream_ref(struct ostream *stream);
/* Unreferences the stream and sets stream pointer to NULL. */
void o_stream_unref(struct ostream **stream);

/* Mark the stream closed. Nothing will be sent after this call. */
void o_stream_close(struct ostream *stream);

/* Set IO_WRITE callback. Default will just try to flush the output and
   finishes when the buffer is empty.  */
void o_stream_set_flush_callback(struct ostream *stream,
				 stream_flush_callback_t *callback,
				 void *context);
/* Change the maximum size for stream's output buffer to grow. */
void o_stream_set_max_buffer_size(struct ostream *stream, size_t max_size);

/* Delays sending as far as possible, writing only full buffers. Also sets
   TCP_CORK on if supported. */
void o_stream_cork(struct ostream *stream);
void o_stream_uncork(struct ostream *stream);
/* Flush the output stream, blocks until everything is sent.
   Returns 1 if ok, -1 if error. */
int o_stream_flush(struct ostream *stream);
/* Set "flush pending" state of stream. If set, the flush callback is called
   when more data is allowed to be sent, even if the buffer itself is empty. */
void o_stream_set_flush_pending(struct ostream *stream, bool set);
/* Returns number of bytes currently in buffer. */
size_t o_stream_get_buffer_used_size(struct ostream *stream);

/* Seek to specified position from beginning of file. This works only for
   files. Returns 1 if successful, -1 if error. */
int o_stream_seek(struct ostream *stream, uoff_t offset);
/* Returns number of bytes sent, -1 = error */
ssize_t o_stream_send(struct ostream *stream, const void *data, size_t size);
ssize_t o_stream_sendv(struct ostream *stream, const struct const_iovec *iov,
		       unsigned int iov_count);
ssize_t o_stream_send_str(struct ostream *stream, const char *str);
/* Send data from input stream. Returns number of bytes sent, or -1 if error.
   Note that this function may block if either instream or outstream is
   blocking.

   It's also possible to use this function to copy data within same file
   descriptor. If the file must be grown, you have to do it manually before
   calling this function. */
off_t o_stream_send_istream(struct ostream *outstream,
			    struct istream *instream);

#endif
