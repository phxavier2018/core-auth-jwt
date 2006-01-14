/* Copyright (c) 2002-2003 Timo Sirainen */

/* @UNSAFE: whole file */

#include "lib.h"
#include "ioloop.h"
#include "write-full.h"
#include "network.h"
#include "sendfile-util.h"
#include "istream.h"
#include "istream-internal.h"
#include "ostream-internal.h"

#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_UIO_H
#  include <sys/uio.h>
#endif

/* try to keep the buffer size within 4k..128k. ReiserFS may actually return
   128k as optimal size. */
#define DEFAULT_OPTIMAL_BLOCK_SIZE 4096
#define MAX_OPTIMAL_BLOCK_SIZE (128*1024)

#define IS_STREAM_EMPTY(fstream) \
	((fstream)->head == (fstream)->tail && !(fstream)->full)

#define MAX_SSIZE_T(size) \
	((size) < SSIZE_T_MAX ? (size_t)(size) : SSIZE_T_MAX)

struct file_ostream {
	struct _ostream ostream;

	int fd;
	struct io *io;

	unsigned char *buffer; /* ring-buffer */
	size_t buffer_size, max_buffer_size, optimal_block_size;
	size_t head, tail; /* first unsent/unused byte */

	unsigned int full:1; /* if head == tail, is buffer empty or full? */
	unsigned int file:1;
	unsigned int corked:1;
	unsigned int flush_pending:1;
	unsigned int no_socket_cork:1;
	unsigned int no_sendfile:1;
	unsigned int autoclose_fd:1;
};

static void stream_send_io(void *context);

static void stream_closed(struct file_ostream *fstream)
{
	if (fstream->autoclose_fd && fstream->fd != -1) {
		if (close(fstream->fd) < 0)
			i_error("file_ostream.close() failed: %m");
		fstream->fd = -1;
	}

	if (fstream->io != NULL)
		io_remove(&fstream->io);

	fstream->ostream.ostream.closed = TRUE;
}

static void _close(struct _iostream *stream)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;

	/* flush output before really closing it */
	o_stream_flush(&fstream->ostream.ostream);

	stream_closed(fstream);
}

static void _destroy(struct _iostream *stream)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;

	p_free(fstream->ostream.iostream.pool, fstream->buffer);
}

static void _set_max_buffer_size(struct _iostream *stream, size_t max_size)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;

	fstream->max_buffer_size = max_size;
}

static void update_buffer(struct file_ostream *fstream, size_t size)
{
	size_t used;

	if (IS_STREAM_EMPTY(fstream) || size == 0)
		return;

	if (fstream->head < fstream->tail) {
		/* ...HXXXT... */
		used = fstream->tail - fstream->head;
		i_assert(size <= used);
		fstream->head += size;
	} else {
		/* XXXT...HXXX */
		used = fstream->buffer_size - fstream->head;
		if (size > used) {
			size -= used;
			i_assert(size <= fstream->tail);
			fstream->head = size;
		} else {
			fstream->head += size;
		}

		fstream->full = FALSE;
	}

	if (fstream->head == fstream->tail)
		fstream->head = fstream->tail = 0;

	if (fstream->head == fstream->buffer_size)
		fstream->head = 0;
}

static ssize_t o_stream_writev(struct file_ostream *fstream,
			       const struct const_iovec *iov, int iov_size)
{
	ssize_t ret;
	size_t size, sent;
	int i;

	if (iov_size == 1)
		ret = write(fstream->fd, iov->iov_base, iov->iov_len);
	else {
		sent = 0;
		while (iov_size > IOV_MAX) {
			size = 0;
			for (i = 0; i < IOV_MAX; i++)
				size += iov[i].iov_len;

			ret = writev(fstream->fd, (const struct iovec *)iov,
				     IOV_MAX);
			if (ret != (ssize_t)size)
				break;

			sent += ret;
			iov += IOV_MAX;
			iov_size -= IOV_MAX;
		}

		if (iov_size <= IOV_MAX) {
			ret = writev(fstream->fd, (const struct iovec *)iov,
				     iov_size);
		}
		if (ret > 0)
			ret += sent;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		fstream->ostream.ostream.stream_errno = errno;
		stream_closed(fstream);
		return -1;
	}

	return ret;
}

/* returns how much of vector was used */
static int o_stream_fill_iovec(struct file_ostream *fstream,
			       struct const_iovec iov[2])
{
	if (IS_STREAM_EMPTY(fstream))
		return 0;

	if (fstream->head < fstream->tail) {
		iov[0].iov_base = fstream->buffer + fstream->head;
		iov[0].iov_len = fstream->tail - fstream->head;
		return 1;
	} else {
		iov[0].iov_base = fstream->buffer + fstream->head;
		iov[0].iov_len = fstream->buffer_size - fstream->head;
		if (fstream->tail == 0)
			return 1;
		else {
			iov[1].iov_base = fstream->buffer;
			iov[1].iov_len = fstream->tail;
			return 2;
		}
	}
}

static int buffer_flush(struct file_ostream *fstream)
{
	struct const_iovec iov[2];
	int iov_len;
	ssize_t ret;

	iov_len = o_stream_fill_iovec(fstream, iov);
	if (iov_len > 0) {
		ret = o_stream_writev(fstream, iov, iov_len);
		if (ret < 0)
			return -1;

		update_buffer(fstream, ret);
	}

	return IS_STREAM_EMPTY(fstream) ? 1 : 0;
}

static void _cork(struct _ostream *stream, bool set)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;
	int ret;

	if (fstream->corked != set && !stream->ostream.closed) {
		if (set && fstream->io != NULL)
			io_remove(&fstream->io);
		else if (!set) {
			ret = buffer_flush(fstream);
			if (fstream->io == NULL &&
			    (ret == 0 || fstream->flush_pending)) {
				fstream->io = io_add(fstream->fd, IO_WRITE,
						     stream_send_io, fstream);
			}
		}

		if (!fstream->no_socket_cork) {
			if (net_set_cork(fstream->fd, set) < 0)
				fstream->no_socket_cork = TRUE;
		}
		fstream->corked = set;
	}
}

static int _flush(struct _ostream *stream)
{
	struct file_ostream *fstream = (struct file_ostream *) stream;

	return buffer_flush(fstream);
}

static void _flush_pending(struct _ostream *stream, bool set)
{
	struct file_ostream *fstream = (struct file_ostream *) stream;

	fstream->flush_pending = set;
	if (set && !fstream->corked && fstream->io == NULL) {
		fstream->io = io_add(fstream->fd, IO_WRITE,
				     stream_send_io, fstream);
	}
}

static size_t get_unused_space(struct file_ostream *fstream)
{
	if (fstream->head > fstream->tail) {
		/* XXXT...HXXX */
		return fstream->head - fstream->tail;
	} else if (fstream->head < fstream->tail) {
		/* ...HXXXT... */
		return (fstream->buffer_size - fstream->tail) + fstream->head;
	} else {
		/* either fully unused or fully used */
		return fstream->full ? 0 : fstream->buffer_size;
	}
}

static size_t _get_used_size(struct _ostream *stream)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;

	return fstream->buffer_size - get_unused_space(fstream);
}

static int _seek(struct _ostream *stream, uoff_t offset)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;
	off_t ret;

	if (offset > OFF_T_MAX) {
		stream->ostream.stream_errno = EINVAL;
		return -1;
	}

	if (buffer_flush(fstream) < 0)
		return -1;

	ret = lseek(fstream->fd, (off_t)offset, SEEK_SET);
	if (ret < 0) {
		stream->ostream.stream_errno = errno;
		return -1;
	}

	if (ret != (off_t)offset) {
		stream->ostream.stream_errno = EINVAL;
		return -1;
	}

	stream->ostream.stream_errno = 0;
	stream->ostream.offset = offset;
	return 1;
}

static void o_stream_grow_buffer(struct file_ostream *fstream, size_t bytes)
{
	size_t size, new_size, end_size;

	size = pool_get_exp_grown_size(fstream->ostream.iostream.pool,
				       fstream->buffer_size,
                                       fstream->buffer_size + bytes);
	if (size > fstream->max_buffer_size) {
		/* limit the size */
		size = fstream->max_buffer_size;
	} else if (fstream->corked) {
		/* try to use optimal buffer size with corking */
		new_size = I_MIN(fstream->optimal_block_size,
				 fstream->max_buffer_size);
		if (new_size > size)
			size = new_size;
	}

	if (size <= fstream->buffer_size)
		return;

	fstream->buffer = p_realloc(fstream->ostream.iostream.pool,
				    fstream->buffer,
				    fstream->buffer_size, size);

	if (fstream->tail <= fstream->head && !IS_STREAM_EMPTY(fstream)) {
		/* move head forward to end of buffer */
		end_size = fstream->buffer_size - fstream->head;
		memmove(fstream->buffer + size - end_size,
			fstream->buffer + fstream->head, end_size);
		fstream->head = size - end_size;
	}

	fstream->full = FALSE;
	fstream->buffer_size = size;
}

static void stream_send_io(void *context)
{
	struct file_ostream *fstream = context;
	struct ostream *ostream = &fstream->ostream.ostream;
	int ret;

	/* Set flush_pending = FALSE first before calling the flush callback,
	   and change it to TRUE only if callback returns 0. That way the
	   callback can call o_stream_set_flush_pending() again and we don't
	   forget it even if flush callback returns 1. */
	fstream->flush_pending = FALSE;

	o_stream_ref(ostream);
	if (fstream->ostream.callback != NULL)
		ret = fstream->ostream.callback(fstream->ostream.context);
	else
		ret = _flush(&fstream->ostream);

	if (ret == 0)
		fstream->flush_pending = TRUE;

	if (!fstream->flush_pending && IS_STREAM_EMPTY(fstream)) {
		if (fstream->io != NULL) {
			/* all sent */
			io_remove(&fstream->io);
		}
	} else {
		/* Add the IO handler if it's not there already. Callback
		   might have just returned 0 without there being any data
		   to be sent. */
		if (fstream->io == NULL) {
			fstream->io = io_add(fstream->fd, IO_WRITE,
					     stream_send_io, fstream);
		}
	}

	o_stream_unref(&ostream);
}

static size_t o_stream_add(struct file_ostream *fstream,
			   const void *data, size_t size)
{
	size_t unused, sent;
	int i;

	unused = get_unused_space(fstream);
	if (unused < size)
		o_stream_grow_buffer(fstream, size-unused);

	sent = 0;
	for (i = 0; i < 2 && sent < size && !fstream->full; i++) {
		unused = fstream->tail >= fstream->head ?
			fstream->buffer_size - fstream->tail :
			fstream->head - fstream->tail;

		if (unused > size-sent)
			unused = size-sent;
		memcpy(fstream->buffer + fstream->tail,
		       CONST_PTR_OFFSET(data, sent), unused);
		sent += unused;

		fstream->tail += unused;
		if (fstream->tail == fstream->buffer_size)
			fstream->tail = 0;

		if (fstream->head == fstream->tail)
			fstream->full = TRUE;
	}

	if (sent != 0 && fstream->io == NULL &&
	    !fstream->corked && !fstream->file) {
		fstream->io = io_add(fstream->fd, IO_WRITE, stream_send_io,
				     fstream);
	}

	return sent;
}

static ssize_t _sendv(struct _ostream *stream, const struct const_iovec *iov,
		      unsigned int iov_count)
{
	struct file_ostream *fstream = (struct file_ostream *)stream;
	size_t size, added, optimal_size;
	unsigned int i;
	ssize_t ret = 0;

	stream->ostream.stream_errno = 0;

	for (i = 0, size = 0; i < iov_count; i++)
		size += iov[i].iov_len;

	if (size > get_unused_space(fstream) && !IS_STREAM_EMPTY(fstream)) {
		if (_flush(stream) < 0)
			return -1;
	}

	optimal_size = I_MIN(fstream->optimal_block_size,
			     fstream->max_buffer_size);
	if (IS_STREAM_EMPTY(fstream) &&
	    (!fstream->corked || size >= optimal_size)) {
		/* send immediately */
		ret = o_stream_writev(fstream, iov, iov_count);
		if (ret < 0)
			return -1;

		size = ret;
		while (size > 0 && size >= iov[0].iov_len) {
			size -= iov[0].iov_len;
			iov++;
			iov_count--;
		}

		if (iov_count > 0) {
			added = o_stream_add(fstream,
					CONST_PTR_OFFSET(iov[0].iov_base, size),
					iov[0].iov_len - size);
			ret += added;

			if (added != iov[0].iov_len - size) {
				/* buffer full */
				stream->ostream.offset += ret;
				return ret;
			}

			iov++;
			iov_count--;
		}
	}

	/* buffer it, at least partly */
	for (i = 0; i < iov_count; i++) {
		added = o_stream_add(fstream, iov[i].iov_base, iov[i].iov_len);
		ret += added;
		if (added != iov[i].iov_len)
			break;
	}
	stream->ostream.offset += ret;
	return ret;
}

static off_t io_stream_sendfile(struct _ostream *outstream,
				struct istream *instream,
				int in_fd, uoff_t in_size)
{
	struct file_ostream *foutstream = (struct file_ostream *)outstream;
	uoff_t start_offset;
	uoff_t offset, send_size, v_offset;
	ssize_t ret;

	/* flush out any data in buffer */
	if ((ret = buffer_flush(foutstream)) <= 0)
		return ret;

        start_offset = v_offset = instream->v_offset;
	do {
		offset = instream->real_stream->abs_start_offset + v_offset;
		send_size = in_size - v_offset;

		ret = safe_sendfile(foutstream->fd, in_fd, &offset,
				    MAX_SSIZE_T(send_size));
		if (ret <= 0) {
			if (ret == 0 || errno == EINTR || errno == EAGAIN) {
				ret = 0;
				break;
			}

			outstream->ostream.stream_errno = errno;
			if (errno != EINVAL) {
				/* close only if error wasn't because
				   sendfile() isn't supported */
				stream_closed(foutstream);
			}
			break;
		}

		v_offset += ret;
		outstream->ostream.offset += ret;
	} while ((uoff_t)ret != send_size);

	i_stream_seek(instream, v_offset);
	return ret < 0 ? -1 : (off_t)(instream->v_offset - start_offset);
}

static off_t io_stream_copy(struct _ostream *outstream,
			    struct istream *instream, uoff_t in_size)
{
	struct file_ostream *foutstream = (struct file_ostream *)outstream;
	uoff_t start_offset;
	struct const_iovec iov[3];
	int iov_len;
	const unsigned char *data;
	size_t size, skip_size, block_size;
	ssize_t ret;
	int pos;

	iov_len = o_stream_fill_iovec(foutstream, iov);

        skip_size = 0;
	for (pos = 0; pos < iov_len; pos++)
		skip_size += iov[pos].iov_len;

	start_offset = instream->v_offset;
	in_size -= instream->v_offset;
	while (in_size > 0) {
		block_size = I_MIN(foutstream->optimal_block_size, in_size);
		(void)i_stream_read_data(instream, &data, &size, block_size-1);
		in_size -= size;

		if (size == 0) {
			/* all sent */
			break;
		}

		pos = iov_len++;
		iov[pos].iov_base = (void *) data;
		iov[pos].iov_len = size;

		ret = o_stream_writev(foutstream, iov, iov_len);
		if (ret < 0)
			return -1;

		if (skip_size > 0) {
			if ((size_t)ret < skip_size) {
				update_buffer(foutstream, ret);
				skip_size -= ret;
				ret = 0;
			} else {
				update_buffer(foutstream, skip_size);
				ret -= skip_size;
				skip_size = 0;
			}
		}
		outstream->ostream.offset += ret;
		i_stream_skip(instream, ret);

		if ((size_t)ret != iov[pos].iov_len)
			break;

		i_assert(skip_size == 0);
		iov_len = 0;
	}

	return (off_t) (instream->v_offset - start_offset);
}

static off_t io_stream_copy_backwards(struct _ostream *outstream,
				      struct istream *instream, uoff_t in_size)
{
	struct file_ostream *foutstream = (struct file_ostream *)outstream;
	uoff_t in_start_offset, in_offset, in_limit, out_offset;
	const unsigned char *data;
	size_t buffer_size, size, read_size;
	ssize_t ret;

	i_assert(IS_STREAM_EMPTY(foutstream));

	/* figure out optimal buffer size */
	buffer_size = instream->real_stream->buffer_size;
	if (buffer_size == 0 || buffer_size > foutstream->buffer_size) {
		if (foutstream->optimal_block_size > foutstream->buffer_size) {
			o_stream_grow_buffer(foutstream,
					     foutstream->optimal_block_size -
					     foutstream->buffer_size);
		}

		buffer_size = foutstream->buffer_size;
	}

	in_start_offset = instream->v_offset;
	in_offset = in_limit = in_size;
	out_offset = outstream->ostream.offset + (in_offset - in_start_offset);

	while (in_offset > in_start_offset) {
		if (in_offset - in_start_offset <= buffer_size)
			read_size = in_offset - in_start_offset;
		else
			read_size = buffer_size;
		in_offset -= read_size;
		out_offset -= read_size;

		for (;;) {
			i_assert(in_offset <= in_limit);

			i_stream_seek(instream, in_offset);
			read_size = in_limit - in_offset;

			(void)i_stream_read_data(instream, &data, &size,
						 read_size-1);
			if (size >= read_size) {
				size = read_size;
				if (instream->mmaped) {
					/* we'll have to write it through
					   buffer or the file gets corrupted */
					i_assert(size <=
						 foutstream->buffer_size);
					memcpy(foutstream->buffer, data, size);
					data = foutstream->buffer;
				}
				break;
			}

			/* buffer too large probably, try with smaller */
			read_size -= size;
			in_offset += read_size;
			out_offset += read_size;
			buffer_size -= read_size;
		}
		in_limit -= size;

		if (o_stream_seek(&outstream->ostream, out_offset) < 0)
			return -1;

		ret = write_full(foutstream->fd, data, size);
		if (ret < 0) {
			/* error */
			outstream->ostream.stream_errno = errno;
			return -1;
		}
	}

	return (off_t) (in_size - in_start_offset);
}

static off_t _send_istream(struct _ostream *outstream, struct istream *instream)
{
	struct file_ostream *foutstream = (struct file_ostream *)outstream;
	const struct stat *st;
	uoff_t in_size;
	off_t ret;
	int in_fd, overlapping;

	st = i_stream_stat(instream, TRUE);
	if (st == NULL) {
		outstream->ostream.stream_errno = instream->stream_errno;
		return -1;
	}

	in_fd = i_stream_get_fd(instream);
	in_size = st->st_size;
	i_assert(instream->v_offset <= in_size);

	outstream->ostream.stream_errno = 0;
	if (in_fd != foutstream->fd)
		overlapping = 0;
	else {
		/* copying data within same fd. we'll have to be careful with
		   seeks and overlapping writes. */
		if (in_size == (uoff_t)-1) {
			outstream->ostream.stream_errno = EINVAL;
			return -1;
		}

		ret = (off_t)outstream->ostream.offset -
			(off_t)(instream->real_stream->abs_start_offset +
				instream->v_offset);
		if (ret == 0) {
			/* copying data over itself. we don't really
			   need to do that, just fake it. */
			return in_size - instream->v_offset;
		}
		overlapping = ret < 0 ? -1 : 1;
	}

	if (!foutstream->no_sendfile && in_fd != -1 && overlapping <= 0) {
		ret = io_stream_sendfile(outstream, instream, in_fd, in_size);
		if (ret >= 0 || outstream->ostream.stream_errno != EINVAL)
			return ret;

		/* sendfile() not supported (with this fd), fallback to
		   regular sending. */
		outstream->ostream.stream_errno = 0;
		foutstream->no_sendfile = TRUE;
	}

	if (overlapping <= 0)
		return io_stream_copy(outstream, instream, in_size);
	else
		return io_stream_copy_backwards(outstream, instream, in_size);
}

struct ostream *
o_stream_create_file(int fd, pool_t pool, size_t max_buffer_size,
		     bool autoclose_fd)
{
	struct file_ostream *fstream;
	struct ostream *ostream;
	struct stat st;
	off_t offset;

	fstream = p_new(pool, struct file_ostream, 1);
	fstream->fd = fd;
	fstream->max_buffer_size = max_buffer_size;
	fstream->autoclose_fd = autoclose_fd;
	fstream->optimal_block_size = DEFAULT_OPTIMAL_BLOCK_SIZE;

	fstream->ostream.iostream.close = _close;
	fstream->ostream.iostream.destroy = _destroy;
	fstream->ostream.iostream.set_max_buffer_size = _set_max_buffer_size;

	fstream->ostream.cork = _cork;
	fstream->ostream.flush = _flush;
	fstream->ostream.flush_pending = _flush_pending;
	fstream->ostream.get_used_size = _get_used_size;
	fstream->ostream.seek = _seek;
	fstream->ostream.sendv = _sendv;
	fstream->ostream.send_istream = _send_istream;

	ostream = _o_stream_create(&fstream->ostream, pool);

	offset = lseek(fd, 0, SEEK_CUR);
	if (offset >= 0) {
		ostream->offset = offset;

		if (fstat(fd, &st) == 0) {
			if ((uoff_t)st.st_blksize >
			    fstream->optimal_block_size) {
				/* use the optimal block size, but with a
				   reasonable limit */
				fstream->optimal_block_size =
					I_MIN(st.st_blksize,
					      MAX_OPTIMAL_BLOCK_SIZE);
			}

			if (S_ISREG(st.st_mode)) {
				fstream->no_socket_cork = TRUE;
				fstream->file = TRUE;
			}
		}
		fstream->no_sendfile = TRUE;
	} else {
		if (net_getsockname(fd, NULL, NULL) < 0) {
			fstream->no_sendfile = TRUE;
			fstream->no_socket_cork = TRUE;
		}
	}

	if (max_buffer_size == 0)
		fstream->max_buffer_size = fstream->optimal_block_size;

	return ostream;
}
