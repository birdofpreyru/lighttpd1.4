#include "first.h"

/**
 * the network chunk-API
 *
 *
 */

#include "chunk.h"
#include "fdevent.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include "sys-mmap.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

/* default 1 MB */
#define DEFAULT_TEMPFILE_SIZE (1 * 1024 * 1024)

static size_t chunk_buf_sz = 8192;
static chunk *chunks, *chunks_oversized, *chunks_filechunk;
static chunk *chunk_buffers;
static int chunks_oversized_n;
static const array *chunkqueue_default_tempdirs = NULL;
static off_t chunkqueue_default_tempfile_size = DEFAULT_TEMPFILE_SIZE;

void chunkqueue_set_chunk_size (size_t sz)
{
    size_t x = 1024;
    while (x < sz && x < (1u << 30)) x <<= 1;
    chunk_buf_sz = sz > 0 ? x : 8192;
}

void chunkqueue_set_tempdirs_default_reset (void)
{
    chunk_buf_sz = 8192;
    chunkqueue_default_tempdirs = NULL;
    chunkqueue_default_tempfile_size = DEFAULT_TEMPFILE_SIZE;
}

chunkqueue *chunkqueue_init(chunkqueue *cq) {
	/* (if caller passes non-NULL cq, it must be 0-init) */
	if (NULL == cq) {
		cq = calloc(1, sizeof(*cq));
		force_assert(NULL != cq);
	}

	cq->first = NULL;
	cq->last = NULL;

	cq->tempdirs              = chunkqueue_default_tempdirs;
	cq->upload_temp_file_size = chunkqueue_default_tempfile_size;

	return cq;
}

__attribute_returns_nonnull__
static chunk *chunk_init(void) {
	chunk * const restrict c = calloc(1, sizeof(*c));
	force_assert(NULL != c);

      #if 0 /*(zeroed by calloc())*/
	c->type = MEM_CHUNK;
	c->next = NULL;
	c->offset = 0;
	c->file.length = 0;
	c->file.mmap.length = c->file.mmap.offset = 0;
	c->file.is_temp = 0;
      #endif
	c->file.fd = -1;
	c->file.mmap.start = MAP_FAILED;

	c->mem = buffer_init();
	return c;
}

__attribute_returns_nonnull__
static chunk *chunk_init_sz(size_t sz) {
	chunk * const restrict c = chunk_init();
	buffer_string_prepare_copy(c->mem, sz-1);
	return c;
}

static void chunk_reset_file_chunk(chunk *c) {
	if (c->file.is_temp) {
		c->file.is_temp = 0;
		if (!buffer_is_blank(c->mem))
			unlink(c->mem->ptr);
	}
	if (c->file.refchg) {
		c->file.refchg(c->file.ref, -1);
		c->file.refchg = 0; /* NULL fn ptr */
		c->file.ref = NULL;
	}
	else if (c->file.fd != -1) {
		close(c->file.fd);
	}
	if (MAP_FAILED != c->file.mmap.start) {
		munmap(c->file.mmap.start, c->file.mmap.length);
		c->file.mmap.start = MAP_FAILED;
		c->file.mmap.length = c->file.mmap.offset = 0;
	}
	c->file.fd = -1;
	c->file.length = 0;
	c->type = MEM_CHUNK;
}

static void chunk_reset(chunk *c) {
	if (c->type == FILE_CHUNK) chunk_reset_file_chunk(c);

	buffer_clear(c->mem);
	c->offset = 0;
}

static void chunk_free(chunk *c) {
	if (c->type == FILE_CHUNK) chunk_reset_file_chunk(c);
	buffer_free(c->mem);
	free(c);
}

static chunk * chunk_pop_oversized(size_t sz) {
    /* future: might have buckets of certain sizes, up to socket buf sizes */
    if (chunks_oversized && chunks_oversized->mem->size >= sz) {
        --chunks_oversized_n;
        chunk *c = chunks_oversized;
        chunks_oversized = c->next;
        return c;
    }
    return NULL;
}

static void chunk_push_oversized(chunk * const c, const size_t sz) {
    /* XXX: chunk_buffer_yield() may have removed need for list size limit */
    if (chunks_oversized_n < 64 && chunk_buf_sz >= 4096) {
        ++chunks_oversized_n;
        chunk **co = &chunks_oversized;
        while (*co && sz < (*co)->mem->size) co = &(*co)->next;
        c->next = *co;
        *co = c;
    }
    else {
        buffer * const tb = chunks_oversized ? chunks_oversized->mem : NULL;
        if (tb && tb->size < sz) {
            /* swap larger mem block onto head of list; free smaller mem */
            chunks_oversized->mem = c->mem;
            c->mem = tb;
        }
        chunk_free(c);
    }
}

__attribute_returns_nonnull__
static buffer * chunk_buffer_acquire_sz(const size_t sz) {
    chunk *c;
    buffer *b;
    if (sz <= (chunk_buf_sz|1)) {
        if (chunks) {
            c = chunks;
            chunks = c->next;
        }
        else
            c = chunk_init_sz(chunk_buf_sz);
    }
    else {
        c = chunk_pop_oversized(sz);
        if (NULL == c) {
            /*(round up to nearest chunk_buf_sz)*/
            /* NB: round down power-2 + 1 to avoid excess allocation
             * (sz & ~1uL) relies on buffer_realloc() adding +1 *and* on callers
             * of this func never passing power-2 + 1 sz unless direct caller
             * adds +1 for '\0', as is done in chunk_buffer_prepare_append() */
            c = chunk_init_sz(((sz&~1uL)+(chunk_buf_sz-1)) & ~(chunk_buf_sz-1));
        }
    }
    c->next = chunk_buffers;
    chunk_buffers = c;
    b = c->mem;
    c->mem = NULL;
    return b;
}

buffer * chunk_buffer_acquire(void) {
    return chunk_buffer_acquire_sz(chunk_buf_sz);
}

void chunk_buffer_release(buffer *b) {
    if (NULL == b) return;
    if (chunk_buffers) {
        chunk *c = chunk_buffers;
        chunk_buffers = c->next;
        c->mem = b;
        buffer_clear(b);
        if (b->size == (chunk_buf_sz|1)) {
            c->next = chunks;
            chunks = c;
        }
        else if (b->size > chunk_buf_sz)
            chunk_push_oversized(c, b->size);
        else
            chunk_free(c);
    }
    else {
        buffer_free(b);
    }
}

void chunk_buffer_yield(buffer *b) {
    if (b->size == (chunk_buf_sz|1)) return;

    buffer * const cb = chunk_buffer_acquire_sz(chunk_buf_sz);
    buffer tb = *b;
    *b = *cb;
    *cb = tb;
    chunk_buffer_release(cb);
}

size_t chunk_buffer_prepare_append(buffer * const b, size_t sz) {
    if (sz > buffer_string_space(b)) {
        sz += b->used ? b->used : 1;
        buffer * const cb = chunk_buffer_acquire_sz(sz);
        /* swap buffer contents and copy original b->ptr into larger b->ptr */
        /*(this does more than buffer_move())*/
        buffer tb = *b;
        *b = *cb;
        *cb = tb;
        if ((b->used = tb.used))
            memcpy(b->ptr, tb.ptr, tb.used);
        chunk_buffer_release(cb);
    }
    return buffer_string_space(b);
}

__attribute_returns_nonnull__
static chunk * chunk_acquire(size_t sz) {
    if (sz <= (chunk_buf_sz|1)) {
        if (chunks) {
            chunk *c = chunks;
            chunks = c->next;
            return c;
        }
        sz = chunk_buf_sz;
    }
    else {
        /*(round up to nearest chunk_buf_sz)*/
        sz = (sz + (chunk_buf_sz-1)) & ~(chunk_buf_sz-1);
        chunk *c = chunk_pop_oversized(sz);
        if (c) return c;
    }

    return chunk_init_sz(sz);
}

static void chunk_release(chunk *c) {
    const size_t sz = c->mem->size;
    if (sz == (chunk_buf_sz|1)) {
        chunk_reset(c);
        c->next = chunks;
        chunks = c;
    }
    else if (sz > chunk_buf_sz) {
        chunk_reset(c);
        chunk_push_oversized(c, sz);
    }
    else if (c->type == FILE_CHUNK) {
        chunk_reset(c);
        c->next = chunks_filechunk;
        chunks_filechunk = c;
    }
    else {
        chunk_free(c);
    }
}

__attribute_returns_nonnull__
static chunk * chunk_acquire_filechunk(void) {
    if (chunks_filechunk) {
        chunk *c = chunks_filechunk;
        chunks_filechunk = c->next;
        return c;
    }
    return chunk_init();
}

void chunkqueue_chunk_pool_clear(void)
{
    for (chunk *next, *c = chunks; c; c = next) {
        next = c->next;
        chunk_free(c);
    }
    chunks = NULL;
    for (chunk *next, *c = chunks_oversized; c; c = next) {
        next = c->next;
        chunk_free(c);
    }
    chunks_oversized = NULL;
    chunks_oversized_n = 0;
    for (chunk *next, *c = chunks_filechunk; c; c = next) {
        next = c->next;
        chunk_free(c);
    }
    chunks_filechunk = NULL;
}

void chunkqueue_chunk_pool_free(void)
{
    chunkqueue_chunk_pool_clear();
    for (chunk *next, *c = chunk_buffers; c; c = next) {
        next = c->next;
      #if 1 /*(chunk_buffers contains MEM_CHUNK with (c->mem == NULL))*/
        free(c);
      #else /*(c->mem = buffer_init() is no longer necessary below)*/
        c->mem = buffer_init(); /*(chunk_reset() expects c->mem != NULL)*/
        chunk_free(c);
      #endif
    }
    chunk_buffers = NULL;
}

__attribute_pure__
static off_t chunk_remaining_length(const chunk *c) {
    /* MEM_CHUNK or FILE_CHUNK */
    return (c->type == MEM_CHUNK
              ? (off_t)buffer_clen(c->mem)
              : c->file.length)
           - c->offset;
}

static void chunkqueue_release_chunks(chunkqueue *cq) {
    cq->last = NULL;
    for (chunk *c; (c = cq->first); ) {
        cq->first = c->next;
        chunk_release(c);
    }
}

void chunkqueue_free(chunkqueue *cq) {
    if (NULL == cq) return;
    chunkqueue_release_chunks(cq);
    free(cq);
}

static void chunkqueue_prepend_chunk(chunkqueue * const restrict cq, chunk * const restrict c) {
    if (NULL == (c->next = cq->first)) cq->last = c;
    cq->first = c;
}

static void chunkqueue_append_chunk(chunkqueue * const restrict cq, chunk * const restrict c) {
    c->next = NULL;
    *(cq->last ? &cq->last->next : &cq->first) = c;
    cq->last = c;
}

__attribute_returns_nonnull__
static chunk * chunkqueue_prepend_mem_chunk(chunkqueue *cq, size_t sz) {
    chunk *c = chunk_acquire(sz);
    chunkqueue_prepend_chunk(cq, c);
    return c;
}

__attribute_returns_nonnull__
static chunk * chunkqueue_append_mem_chunk(chunkqueue *cq, size_t sz) {
    chunk *c = chunk_acquire(sz);
    chunkqueue_append_chunk(cq, c);
    return c;
}

__attribute_nonnull__()
__attribute_returns_nonnull__
static chunk * chunkqueue_append_file_chunk(chunkqueue * const restrict cq, const buffer * const restrict fn, off_t offset, off_t len) {
    chunk * const c = chunk_acquire_filechunk();
    chunkqueue_append_chunk(cq, c);
    c->type = FILE_CHUNK;
    c->offset = offset;
    c->file.length = offset + len;
    cq->bytes_in += len;
    buffer_copy_buffer(c->mem, fn);
    return c;
}

void chunkqueue_reset(chunkqueue *cq) {
    chunkqueue_release_chunks(cq);
    cq->bytes_in = 0;
    cq->bytes_out = 0;
    cq->tempdir_idx = 0;
}

void chunkqueue_append_file_fd(chunkqueue * const restrict cq, const buffer * const restrict fn, int fd, off_t offset, off_t len) {
    if (len > 0) {
        (chunkqueue_append_file_chunk(cq, fn, offset, len))->file.fd = fd;
    }
    else {
        close(fd);
    }
}

void chunkqueue_append_file(chunkqueue * const restrict cq, const buffer * const restrict fn, off_t offset, off_t len) {
    if (len > 0) {
        chunkqueue_append_file_chunk(cq, fn, offset, len);
    }
}


static int chunkqueue_append_mem_extend_chunk(chunkqueue * const restrict cq, const char * const restrict mem, size_t len) {
	chunk *c = cq->last;
	if (0 == len) return 1;
	if (c != NULL && c->type == MEM_CHUNK
	    && buffer_string_space(c->mem) >= len) {
		buffer_append_string_len(c->mem, mem, len);
		cq->bytes_in += len;
		return 1;
	}
	return 0;
}


void chunkqueue_append_buffer(chunkqueue * const restrict cq, buffer * const restrict mem) {
	chunk *c;
	const size_t len = buffer_clen(mem);
	if (len < 1024 && chunkqueue_append_mem_extend_chunk(cq, mem->ptr, len)) {
		buffer_clear(mem);
		return;
	}

	c = chunkqueue_append_mem_chunk(cq, chunk_buf_sz);
	cq->bytes_in += len;
	buffer_move(c->mem, mem);
}


void chunkqueue_append_mem(chunkqueue * const restrict cq, const char * const restrict mem, size_t len) {
	chunk *c;
	if (len < chunk_buf_sz && chunkqueue_append_mem_extend_chunk(cq, mem, len))
		return;

	c = chunkqueue_append_mem_chunk(cq, len+1);
	cq->bytes_in += len;
	buffer_copy_string_len(c->mem, mem, len);
}


void chunkqueue_append_mem_min(chunkqueue * const restrict cq, const char * const restrict mem, size_t len) {
	chunk *c;
	if (len < chunk_buf_sz && chunkqueue_append_mem_extend_chunk(cq, mem, len))
		return;

	c = chunk_init_sz(len+1);
	chunkqueue_append_chunk(cq, c);
	cq->bytes_in += len;
	buffer_copy_string_len(c->mem, mem, len);
}


void chunkqueue_append_chunkqueue(chunkqueue * const restrict cq, chunkqueue * const restrict src) {
	if (NULL == src->first) return;

	if (NULL == cq->first) {
		cq->first = src->first;
	} else {
		cq->last->next = src->first;
	}
	cq->last = src->last;
	cq->bytes_in += chunkqueue_length(src);

	src->first = NULL;
	src->last = NULL;
	src->bytes_out = src->bytes_in;
}


buffer * chunkqueue_prepend_buffer_open_sz(chunkqueue *cq, size_t sz) {
	chunk * const c = chunkqueue_prepend_mem_chunk(cq, sz);
	return c->mem;
}


buffer * chunkqueue_prepend_buffer_open(chunkqueue *cq) {
	return chunkqueue_prepend_buffer_open_sz(cq, chunk_buf_sz);
}


void chunkqueue_prepend_buffer_commit(chunkqueue *cq) {
	cq->bytes_in += buffer_clen(cq->first->mem);
}


buffer * chunkqueue_append_buffer_open_sz(chunkqueue *cq, size_t sz) {
	chunk * const c = chunkqueue_append_mem_chunk(cq, sz);
	return c->mem;
}


buffer * chunkqueue_append_buffer_open(chunkqueue *cq) {
	return chunkqueue_append_buffer_open_sz(cq, chunk_buf_sz);
}


void chunkqueue_append_buffer_commit(chunkqueue *cq) {
	cq->bytes_in += buffer_clen(cq->last->mem);
}


char * chunkqueue_get_memory(chunkqueue * const restrict cq, size_t * const restrict len) {
	size_t sz = *len ? *len : (chunk_buf_sz >> 1);
	buffer *b;
	chunk *c = cq->last;
	if (NULL != c && MEM_CHUNK == c->type) {
		/* return pointer into existing buffer if large enough */
		size_t avail = buffer_string_space(c->mem);
		if (avail >= sz) {
			*len = avail;
			b = c->mem;
			return b->ptr + buffer_clen(b);
		}
	}

	/* allocate new chunk */
	b = chunkqueue_append_buffer_open_sz(cq, sz);
	*len = buffer_string_space(b);
	return b->ptr;
}

void chunkqueue_use_memory(chunkqueue * const restrict cq, chunk *ckpt, size_t len) {
    buffer *b = cq->last->mem;

    if (len > 0) {
        buffer_commit(b, len);
        cq->bytes_in += len;
        if (cq->last == ckpt || NULL == ckpt || MEM_CHUNK != ckpt->type
            || len > buffer_string_space(ckpt->mem)) return;

        buffer_append_string_buffer(ckpt->mem, b);
    }
    else if (!buffer_is_blank(b)) { /*(cq->last == ckpt)*/
        return; /* last chunk is not empty */
    }

    /* remove empty last chunk */
    chunk_release(cq->last);
    cq->last = ckpt;
    *(ckpt ? &ckpt->next : &cq->first) = NULL;
}

void chunkqueue_update_file(chunkqueue * const restrict cq, chunk *c, off_t len) {
    /*assert(c->type == FILE_CHUNK);*/
    c->file.length += len;
    cq->bytes_in += len;
    if (0 == chunk_remaining_length(c))
        chunkqueue_remove_empty_chunks(cq);
}

void chunkqueue_set_tempdirs_default (const array *tempdirs, off_t upload_temp_file_size) {
    if (upload_temp_file_size == 0)
        upload_temp_file_size = DEFAULT_TEMPFILE_SIZE;
    chunkqueue_default_tempdirs = tempdirs;
    chunkqueue_default_tempfile_size = upload_temp_file_size;
}

void chunkqueue_set_tempdirs(chunkqueue * const restrict cq, const array * const restrict tempdirs, off_t upload_temp_file_size) {
    if (upload_temp_file_size == 0)
        upload_temp_file_size = chunkqueue_default_tempfile_size;
    cq->tempdirs = tempdirs;
    cq->upload_temp_file_size = upload_temp_file_size;
    cq->tempdir_idx = 0;
}

__attribute_noinline__
static void chunkqueue_dup_file_chunk_fd (chunk * const restrict d, const chunk * const restrict c) {
    /*assert(d != c);*/
    /*assert(d->type == FILE_CHUNK);*/
    /*assert(c->type == FILE_CHUNK);*/
    if (c->file.fd >= 0) {
        if (c->file.refchg) {
            d->file.fd = c->file.fd;
            d->file.ref = c->file.ref;
            d->file.refchg = c->file.refchg;
            d->file.refchg(d->file.ref, 1);
        }
        else
            d->file.fd = fdevent_dup_cloexec(c->file.fd);
    }
}

__attribute_noinline__
static void chunkqueue_steal_partial_file_chunk(chunkqueue * const restrict dest, const chunk * const restrict c, const off_t len) {
    chunkqueue_append_file(dest, c->mem, c->offset, len);
    chunkqueue_dup_file_chunk_fd(dest->last, c);
}

void chunkqueue_steal(chunkqueue * const restrict dest, chunkqueue * const restrict src, off_t len) {
	for (off_t clen; len > 0; len -= clen) {
		chunk * const c = src->first;
		if (__builtin_expect( (NULL == c), 0)) break;

		clen = chunk_remaining_length(c);

		if (len >= clen) {
			/* move complete chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;

			if (__builtin_expect( (0 != clen), 1)) {
				chunkqueue_append_chunk(dest, c);
				dest->bytes_in += clen;
			}
			else /* drop empty chunk */
				chunk_release(c);
		} else {
			/* copy partial chunk */

			switch (c->type) {
			case MEM_CHUNK:
				chunkqueue_append_mem(dest, c->mem->ptr + c->offset, len);
				break;
			case FILE_CHUNK:
				/* tempfile flag is in "last" chunk after the split */
				chunkqueue_steal_partial_file_chunk(dest, c, len);
				break;
			}

			c->offset += len;
			clen = len;
		}

		src->bytes_out += clen;
	}
}

static int chunkqueue_get_append_mkstemp(buffer * const b, const char *path, const uint32_t len) {
    buffer_copy_path_len2(b,path,len,CONST_STR_LEN("lighttpd-upload-XXXXXX"));
  #if defined(HAVE_SPLICE) && defined(HAVE_PWRITE)
    /*(splice() rejects O_APPEND target; omit flag if also using pwrite())*/
    return fdevent_mkostemp(b->ptr, 0);
  #else
    return fdevent_mkostemp(b->ptr, O_APPEND);
  #endif
}

static chunk *chunkqueue_get_append_newtempfile(chunkqueue * const restrict cq, log_error_st * const restrict errh) {
    static const buffer emptyb = { "", 0, 0 };
    chunk * const restrict last = cq->last;
    chunk * const restrict c = chunkqueue_append_file_chunk(cq, &emptyb, 0, 0);
    buffer * const restrict template = c->mem;
    c->file.is_temp = 1;

    if (cq->tempdirs && cq->tempdirs->used) {
        /* we have several tempdirs, only if all of them fail we jump out */
        for (errno = EIO; cq->tempdir_idx < cq->tempdirs->used; ++cq->tempdir_idx) {
            data_string *ds = (data_string *)cq->tempdirs->data[cq->tempdir_idx];
            c->file.fd =
              chunkqueue_get_append_mkstemp(template, BUF_PTR_LEN(&ds->value));
            if (-1 != c->file.fd) return c;
        }
    }
    else {
        c->file.fd =
          chunkqueue_get_append_mkstemp(template, CONST_STR_LEN("/var/tmp"));
        if (-1 != c->file.fd) return c;
    }

    /* (report only last error to mkstemp() even if multiple temp dirs tried) */
    log_perror(errh, __FILE__, __LINE__,
      "opening temp-file failed: %s", template->ptr);
    /* remove (failed) final chunk */
    c->file.is_temp = 0;
    if ((cq->last = last))
        last->next = NULL;
    else
        cq->first = NULL;
    chunk_release(c);
    return NULL;
}

static chunk *chunkqueue_get_append_tempfile(chunkqueue * const restrict cq, log_error_st * const restrict errh) {
    /*
     * if the last chunk is
     * - smaller than cq->upload_temp_file_size
     * -> append to it (and it then might exceed cq->upload_temp_file_size)
     * otherwise
     * -> create a new chunk
     */

    chunk * const c = cq->last;
    if (NULL != c && c->file.is_temp && c->file.fd >= 0) {

        if (c->file.length < (off_t)cq->upload_temp_file_size)
            return c; /* ok, take the last chunk for our job */

        /* the chunk is too large now, close it */
        force_assert(0 == c->file.refchg); /*(else should not happen)*/
        int rc = close(c->file.fd);
        c->file.fd = -1;
        if (0 != rc) {
            log_perror(errh, __FILE__, __LINE__,
              "close() temp-file %s failed", c->mem->ptr);
            return NULL;
        }
    }
    return chunkqueue_get_append_newtempfile(cq, errh);
}

__attribute_cold__
static int chunkqueue_append_tempfile_err(chunkqueue * const cq, log_error_st * const restrict errh, chunk * const c) {
    const int errnum = errno;
    if (errnum == EINTR) return 1; /* retry */

    int retry = (errnum == ENOSPC && cq->tempdirs
                 && ++cq->tempdir_idx < cq->tempdirs->used);
    if (!retry)
        log_perror(errh, __FILE__, __LINE__,
          "write() temp-file %s failed", c->mem->ptr);

    if (0 == chunk_remaining_length(c)) {
        /*(remove empty chunk and unlink tempfile)*/
        chunkqueue_remove_empty_chunks(cq);
    }
    else {/*(close tempfile; avoid later attempts to append)*/
        force_assert(0 == c->file.refchg); /*(else should not happen)*/
        int rc = close(c->file.fd);
        c->file.fd = -1;
        if (0 != rc) {
            log_perror(errh, __FILE__, __LINE__,
              "close() temp-file %s failed", c->mem->ptr);
            retry = 0;
        }
    }
    return retry;
}

__attribute_cold__
__attribute_noinline__
static int chunkqueue_to_tempfiles(chunkqueue * const restrict dest, log_error_st * const restrict errh) {
    /* transfer chunks from dest to src, adjust dest->bytes_in, and then call
     * chunkqueue_steal_with_tempfiles() to write chunks from src back into
     * dest, but into tempfiles.   chunkqueue_steal_with_tempfiles() calls back
     * into chunkqueue_append_mem_to_tempfile(), but will not re-enter this func
     * since chunks moved to src, and dest made empty before recursive call */
    const off_t cqlen = chunkqueue_length(dest);
    chunkqueue src = *dest; /*(copy struct)*/
    dest->first = dest->last = NULL;
    dest->bytes_in -= cqlen;
    if (0 == chunkqueue_steal_with_tempfiles(dest, &src, cqlen, errh))
        return 0;
    else {
        const int errnum = errno;
        chunkqueue_release_chunks(&src);
        return -errnum;
    }
}

int chunkqueue_append_mem_to_tempfile(chunkqueue * const restrict dest, const char * restrict mem, size_t len, log_error_st * const restrict errh) {
	chunk *dst_c = dest->first;

	/* check if prior MEM_CHUNK(s) exist and write to tempfile
	 * (check first chunk only, since if we are using tempfiles, then
	 *  we expect further chunks to be tempfiles after starting tempfiles)*/
	if (dst_c && dst_c->type == MEM_CHUNK
	    && 0 != chunkqueue_to_tempfiles(dest, errh)) {
		return -1;
	}

	do {
		/*(aside: arg len is permitted to be 0 and creates tempfile as a
		 * side effect.  This is used by mod_ssi for ssi exec, as the func
		 * chunkqueue_get_append_tempfile() is not public.  The result is
		 * an empty chunk at the end of the chunkqueue, which typically
		 * should be avoided)*/
		dst_c = chunkqueue_get_append_tempfile(dest, errh);
		if (NULL == dst_c)
			return -1;
	      #ifdef __COVERITY__
		if (dst_c->file.fd < 0) return -1;
	      #endif
	      #ifdef HAVE_PWRITE
		/* coverity[negative_returns : FALSE] */
		const ssize_t written =pwrite(dst_c->file.fd, mem, len, dst_c->file.length);
	      #else
		/* coverity[negative_returns : FALSE] */
		const ssize_t written = write(dst_c->file.fd, mem, len);
	      #endif

		if ((size_t) written == len) {
			dst_c->file.length += len;
			dest->bytes_in += len;
			return 0;
		} else if (written >= 0) {
			/*(assume EINTR if partial write and retry write();
			 * retry write() might fail with ENOSPC if no more space on volume)*/
			dest->bytes_in += written;
			mem += written;
			len -= (size_t)written;
			dst_c->file.length += (size_t)written;
			/* continue; retry */
		} else if (!chunkqueue_append_tempfile_err(dest, errh, dst_c)) {
			break; /* return -1; */
		} /* else continue; retry */
	} while (len);

	return -1;
}

#ifdef HAVE_PWRITEV

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

__attribute_cold__
__attribute_noinline__
static ssize_t chunkqueue_append_cqmem_to_tempfile_partial(chunkqueue * const dest, chunk * const c, ssize_t wr, log_error_st * const restrict errh) {
    /* recover from partial write of existing dest MEM_CHUNK to tempfile */
    chunk *ckpt = dest->first;
    while (ckpt->next != c) ckpt = ckpt->next;
    ckpt->next = NULL;
    dest->last = ckpt;
    dest->bytes_in  -= wr; /*(avoid double count in dest cq)*/
    dest->bytes_out -= wr;
    chunkqueue_mark_written(dest, wr);/*(remove MEM_CHUNK written to tempfile)*/

    c->next = dest->first; /*(place tempfile at beginning of dest cq)*/
    dest->first = c;
    return (0 == chunkqueue_to_tempfiles(dest, errh)) ? 0 : -1;
}

static ssize_t chunkqueue_append_cqmem_to_tempfile(chunkqueue * const restrict dest, chunkqueue * const restrict src, off_t len, log_error_st * const restrict errh) {
    /* write multiple MEM_CHUNKs to tempfile in single pwritev() syscall */
    /*(could lseek() and writev() if pwritev() is not available,
     * but if writev() is available, pwritev() is likely available,
     * e.g. any modern Linux or *BSD, and possibly anything not Windows)*/
    unsigned int iovcnt = 0;
    struct iovec iov[16];

    off_t dlen = 0;
    chunk *c;
    for (c = dest->first; c && c->type == MEM_CHUNK; c = c->next) {
        const off_t clen = chunk_remaining_length(c);
        iov[iovcnt].iov_base = c->mem->ptr + c->offset;
        iov[iovcnt].iov_len  = (size_t)clen;
        dlen += clen;
        ++iovcnt;
        if (__builtin_expect( (iovcnt == sizeof(iov)/sizeof(*iov)), 0))
            break; /*(not expecting large number of MEM_CHUNK)*/
    }
    if (__builtin_expect( (c != NULL), 0) && dest->first->type == MEM_CHUNK) {
        /*(expecting only MEM_CHUNK if dest cq starts w/ MEM_CHUNK)*/
        /*(use less efficient fallback if that assumption does not hold true)*/
        if (0 != chunkqueue_to_tempfiles(dest, errh))
            return -1;
        dlen = 0;
        iovcnt = 0;
    }

    if (__builtin_expect( (iovcnt < sizeof(iov)/sizeof(*iov)), 1)) {
        for (c = src->first; c && c->type == MEM_CHUNK; c = c->next) {
            off_t clen = chunk_remaining_length(c);
            if (clen > len) clen = len;
            iov[iovcnt].iov_base = c->mem->ptr + c->offset;
            iov[iovcnt].iov_len  = (size_t)clen;
            len -= clen;
            ++iovcnt;
            if (0 == len) break;
            if (__builtin_expect( (iovcnt == sizeof(iov)/sizeof(*iov)), 0))
                break; /*(not expecting large number of MEM_CHUNK)*/
        }
    }

    if (__builtin_expect( (0 == iovcnt), 0)) return 0; /*(should not happen)*/

    c = chunkqueue_get_append_tempfile(dest, errh);
    if (NULL == c)
        return -1;
  #ifdef __COVERITY__
    if (c->file.fd < 0) return -1;
  #endif
    /* coverity[negative_returns : FALSE] */
    ssize_t wr = pwritev(c->file.fd, iov, (int)iovcnt, c->file.length);

    /*(memory use in chunkqueues is expected to be limited before spilling
     * to tempfiles, so common case will write entire iovec to tempfile,
     * and we return amount written *from src cq*, even if partial write;
     * (not looping here to retry writing more, but caller might loop))*/

    if (wr >= 0) {
        c->file.length += wr;
        dest->bytes_in += wr;
        if (dlen) {
            if (__builtin_expect( (wr < dlen), 0))
                return
                  chunkqueue_append_cqmem_to_tempfile_partial(dest,c,wr,errh);
            wr -= (ssize_t)dlen;
            dest->bytes_in  -= dlen; /*(avoid double count in dest cq)*/
            dest->bytes_out -= dlen;
            chunkqueue_mark_written(dest, dlen);
        }
    }

    return wr;
}

#endif /* HAVE_PWRITEV */

#ifdef HAVE_SPLICE

__attribute_cold__
__attribute_noinline__
static ssize_t chunkqueue_append_drain_pipe_tempfile(chunkqueue * const restrict cq, const int fd, unsigned int len, log_error_st * const restrict errh) {
    /* attempt to drain full 'len' from pipe
     * (even if len not reduced to opts->max_per_read limit)
     * since data may have already been moved from socket to pipe
     *(returns 0 on success, or -errno (negative errno) if error,
     * even if partial write occurred)*/
    char buf[16384];
    ssize_t rd;
    do {
        do {
            rd = read(fd, buf, sizeof(buf));
        } while (rd < 0 && errno == EINTR);
        if (rd < 0) break;
        if (0 != chunkqueue_append_mem_to_tempfile(cq, buf, (size_t)rd, errh))
            break;
    } while ((len -= (unsigned int)rd));

    if (0 == len)
        return 0;
    else {
        const int errnum = errno;
        if (cq->last && 0 == chunk_remaining_length(cq->last)) {
            /*(remove empty chunk and unlink tempfile)*/
            chunkqueue_remove_empty_chunks(cq);
        }
        return -errnum;
    }
}

ssize_t chunkqueue_append_splice_pipe_tempfile(chunkqueue * const restrict cq, const int fd, unsigned int len, log_error_st * const restrict errh) {
    /* check if prior MEM_CHUNK(s) exist and write to tempfile
     * (check first chunk only, since if we are using tempfiles, then
     *  we expect further chunks to be tempfiles after starting tempfiles)*/
    if (cq->first && cq->first->type == MEM_CHUNK) {
        int rc = chunkqueue_to_tempfiles(cq, errh);
        if (__builtin_expect( (0 != rc), 0)) return rc;
    }

    /*(returns num bytes written, or -errno (negative errno) if error)*/
    ssize_t total = 0;
    do {
        chunk * const c = chunkqueue_get_append_tempfile(cq, errh);
        if (__builtin_expect( (NULL == c), 0)) return -errno;

        loff_t off = c->file.length;
        ssize_t wr = splice(fd, NULL, c->file.fd, &off, len,
                            SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

        if (__builtin_expect(((size_t)wr == len), 1)) {
            c->file.length += len;
            cq->bytes_in += len;
            return total + len;
        }
        else if (wr >= 0) {
            /*(assume EINTR if partial write and retry;
             * retry might fail with ENOSPC if no more space on volume)*/
            cq->bytes_in += wr;
            total += wr;
            len -= (size_t)wr;
            c->file.length += (size_t)wr;
            /* continue; retry */
        }
        else {
            const int errnum = errno;
            switch (errnum) {
              case EAGAIN:
             #ifdef EWOULDBLOCK
             #if EWOULDBLOCK != EAGAIN
              case EWOULDBLOCK:
             #endif
             #endif
                if (0 == chunk_remaining_length(c)) {
                    /*(remove empty chunk and unlink tempfile)*/
                    chunkqueue_remove_empty_chunks(cq);
                }
                return total;
              case EINVAL: /*(assume total == 0 if EINVAL)*/
                wr = chunkqueue_append_drain_pipe_tempfile(cq, fd, len, errh);
                return (0 == wr) ? total + (ssize_t)len : wr;
              default:
                if (!chunkqueue_append_tempfile_err(cq, errh, c))
                    return -errnum;
                break; /* else continue; retry */
            }
        }
    } while (len);
    return -EIO; /*(not reached)*/
}

static int cqpipes[2] = { -1, -1 };

__attribute_cold__
__attribute_noinline__
void chunkqueue_internal_pipes(int init) {
    /*(intended for internal use within a single lighttpd process;
     * must be initialized after fork() and graceful-restart to avoid
     * sharing pipes between processes)*/
    if (-1 != cqpipes[0]) { close(cqpipes[0]); cqpipes[0] = -1; }
    if (-1 != cqpipes[1]) { close(cqpipes[1]); cqpipes[1] = -1; }
    if (init)
        if (0 != fdevent_pipe_cloexec(cqpipes, 262144)) { } /*(ignore error)*/
}

__attribute_cold__
__attribute_noinline__
static void chunkqueue_pipe_read_discard (void) {
    char buf[16384];
    ssize_t rd;
    do {
        rd = read(cqpipes[0], buf, sizeof(buf));
    } while (rd > 0 || (rd < 0 && errno == EINTR));
    if (rd < 0
      #ifdef EWOULDBLOCK
      #if EWOULDBLOCK != EAGAIN
        && errno != EWOULDBLOCK
      #endif
      #endif
        && errno != EAGAIN) {
        chunkqueue_internal_pipes(1); /*(close() and re-initialize)*/
    }
}

ssize_t chunkqueue_append_splice_sock_tempfile(chunkqueue * const restrict cq, const int fd, unsigned int len, log_error_st * const restrict errh) {
    /*(returns num bytes written, or -errno (negative errno) if error)*/
    int * const pipes = cqpipes;
    if (-1 == pipes[1])
        return -EINVAL; /*(not configured; not handled here)*/

    /* splice() socket data to intermediate pipe */
    ssize_t wr = splice(fd, NULL, pipes[1], NULL, len,
                        SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
    if (__builtin_expect( (wr <= 0), 0))
        return -EINVAL; /*(reuse to indicate not handled here)*/
    len = (unsigned int)wr;

    /* splice() data from intermediate pipe to tempfile */
    wr = chunkqueue_append_splice_pipe_tempfile(cq, pipes[0], len, errh);
    if (wr < 0) /* expect (wr == (ssize_t)len) or (wr == -1) */
        chunkqueue_pipe_read_discard();/* discard data from intermediate pipe */
    return wr;
}

#endif /* HAVE_SPLICE */

int chunkqueue_steal_with_tempfiles(chunkqueue * const restrict dest, chunkqueue * const restrict src, off_t len, log_error_st * const restrict errh) {
	for (off_t clen; len > 0; len -= clen) {
		chunk * const c = src->first;
		if (__builtin_expect( (NULL == c), 0)) break;

	  #ifdef HAVE_PWRITEV

		if (c->type == MEM_CHUNK) {
			clen = chunkqueue_append_cqmem_to_tempfile(dest, src, len, errh);
			if (__builtin_expect( (clen < 0), 0)) return -1;
			chunkqueue_mark_written(src, clen); /*(updates src->bytes_out)*/
		}
		else { /* (c->type == FILE_CHUNK) */
			clen = chunk_remaining_length(c);
			if (len < clen) clen = len;
			chunkqueue_steal(dest, src, clen);/*(same as below for FILE_CHUNK)*/
		}

	  #else

		clen = chunk_remaining_length(c);
		if (__builtin_expect( (0 == clen), 0)) {
			/* drop empty chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunk_release(c);
			continue;
		}

		switch (c->type) {
		case FILE_CHUNK:
			if (len >= clen) {
				/* move complete chunk */
				src->first = c->next;
				if (c == src->last) src->last = NULL;
				chunkqueue_append_chunk(dest, c);
				dest->bytes_in += clen;
			} else {
				/* copy partial chunk */
				/* tempfile flag is in "last" chunk after the split */
				chunkqueue_steal_partial_file_chunk(dest, c, len);
				c->offset += len;
				clen = len;
			}
			break;

		case MEM_CHUNK:
			/* store bytes from memory chunk in tempfile */
			if (0 != chunkqueue_append_mem_to_tempfile(dest, c->mem->ptr + c->offset,
			                                           (len >= clen) ? clen : len, errh)) {
				return -1;
			}

			if (len >= clen) {
				/* finished chunk */
				src->first = c->next;
				if (c == src->last) src->last = NULL;
				chunk_release(c);
			} else {
				/* partial chunk */
				c->offset += len;
				clen = len;
			}
			break;
		}

		src->bytes_out += clen;

	  #endif
	}

	return 0;
}

void chunkqueue_append_cq_range (chunkqueue * const dst, const chunkqueue * const src, off_t offset, off_t len) {
    /* similar to chunkqueue_steal() but copy and append src range to dst cq */
    /* (dst cq and src cq can be the same cq, so neither is marked restrict) */

    /* copy and append range len from src to dst */
    for (const chunk *c = src->first; len > 0 && c != NULL; c = c->next) {
        /* scan into src to range offset (also skips empty chunks) */
        off_t clen = chunk_remaining_length(c);
        if (offset >= clen) {
            offset -= clen;
            continue;
        }
        clen -= offset;
        if (len < clen) clen = len;
        len -= clen;

        if (c->type == FILE_CHUNK) {
            chunkqueue_append_file(dst, c->mem, c->offset + offset, clen);
            chunkqueue_dup_file_chunk_fd(dst->last, c);
        }
        else { /*(c->type == MEM_CHUNK)*/
            /*(string refs would reduce copying,
             * but this path is not expected to be hot)*/
            chunkqueue_append_mem(dst, c->mem->ptr + c->offset + offset, clen);
        }
        offset = 0;
    }
}

void chunkqueue_mark_written(chunkqueue *cq, off_t len) {
    cq->bytes_out += len;

    for (chunk *c = cq->first; c; ) {
        off_t c_len = chunk_remaining_length(c);
        if (len >= c_len) { /* chunk got finished */
            chunk * const x = c;
            c = c->next;
            len -= c_len;
            chunk_release(x);
        }
        else { /* partial chunk */
            c->offset += len;
            cq->first = c;
            return; /* chunk not finished */
        }
    }
    cq->first = cq->last = NULL;
}

void chunkqueue_remove_finished_chunks(chunkqueue *cq) {
    for (chunk *c; (c = cq->first) && 0 == chunk_remaining_length(c); ){
        if (NULL == (cq->first = c->next)) cq->last = NULL;
        chunk_release(c);
    }
}

void chunkqueue_remove_empty_chunks(chunkqueue *cq) {
	chunk *c;
	chunkqueue_remove_finished_chunks(cq);

	for (c = cq->first; c && c->next; c = c->next) {
		if (0 == chunk_remaining_length(c->next)) {
			chunk *empty = c->next;
			c->next = empty->next;
			if (empty == cq->last) cq->last = c;
			chunk_release(empty);
		}
	}
}

void chunkqueue_compact_mem_offset(chunkqueue * const cq) {
    chunk * const restrict c = cq->first;
    if (0 == c->offset) return;
    if (c->type != MEM_CHUNK) return; /*(should not happen)*/

    buffer * const restrict b = c->mem;
    size_t len = buffer_clen(b) - c->offset;
    memmove(b->ptr, b->ptr+c->offset, len);
    c->offset = 0;
    buffer_truncate(b, len);
}

void chunkqueue_compact_mem(chunkqueue *cq, size_t clen) {
    /* caller must guarantee that chunks in chunkqueue are MEM_CHUNK,
     * which is currently always true when reading input from client */
    chunk *c = cq->first;
    buffer *b = c->mem;
    size_t len = buffer_clen(b) - c->offset;
    if (len >= clen) return;
    if (b->size > clen) {
        if (buffer_string_space(b) < clen - len)
            chunkqueue_compact_mem_offset(cq);
    }
    else {
        b = chunkqueue_prepend_buffer_open_sz(cq, clen+1);
        buffer_append_string_len(b, c->mem->ptr + c->offset, len);
        cq->first->next = c->next;
        if (NULL == c->next) cq->last = cq->first;
        chunk_release(c);
        c = cq->first;
    }

    for (chunk *fc = c; ((clen -= len) && (c = fc->next)); ) {
        len = buffer_clen(c->mem) - c->offset;
        if (len > clen) {
            buffer_append_string_len(b, c->mem->ptr + c->offset, clen);
            c->offset += clen;
            break;
        }
        buffer_append_string_len(b, c->mem->ptr + c->offset, len);
        fc->next = c->next;
        if (NULL == c->next) cq->last = fc;
        chunk_release(c);
    }
    /* chunkqueue_prepend_buffer_commit() is not called here;
     * no data added/removed from chunkqueue; consolidated only */
}

static int chunk_open_file_chunk(chunk * const restrict c, log_error_st * const restrict errh) {
	if (-1 == c->file.fd) {
		/* (permit symlinks; should already have been checked.  However, TOC-TOU remains) */
		if (-1 == (c->file.fd = fdevent_open_cloexec(c->mem->ptr, 1, O_RDONLY, 0))) {
			log_perror(errh, __FILE__, __LINE__, "open failed: %s",c->mem->ptr);
			return -1;
		}
	}

	/*(skip file size checks if file is temp file created by lighttpd)*/
	if (c->file.is_temp) return 0;

	struct stat st;
	if (-1 == fstat(c->file.fd, &st)) {
		log_perror(errh, __FILE__, __LINE__, "fstat failed");
		return -1;
	}

	const off_t offset = c->offset;
	const off_t len = c->file.length - c->offset;
	force_assert(offset >= 0 && len >= 0);
	if (offset > st.st_size - len) {
		log_error(errh, __FILE__, __LINE__, "file shrunk: %s", c->mem->ptr);
		return -1;
	}

	return 0;
}

int chunkqueue_open_file_chunk(chunkqueue * const restrict cq, log_error_st * const restrict errh) {
    return chunk_open_file_chunk(cq->first, errh);
}


static ssize_t
chunkqueue_write_data (const int fd, const void *buf, size_t count)
{
    ssize_t wr;
    do { wr = write(fd, buf, count); } while (-1 == wr && errno == EINTR);
    return wr;
}


#if defined(HAVE_MMAP) || defined(_WIN32) /*(see local sys-mmap.h)*/
__attribute_cold__
#endif
__attribute_noinline__
static ssize_t
chunkqueue_write_chunk_file_intermed (const int fd, chunk * const restrict c, log_error_st * const errh)
{
    char buf[16384];
    char *data = buf;
    const off_t count = c->file.length - c->offset;
    uint32_t dlen = count < (off_t)sizeof(buf) ? (uint32_t)count : sizeof(buf);
    chunkqueue cq = {c,c,0,0,0,0,0}; /*(fake cq for chunkqueue_peek_data())*/
    if (0 != chunkqueue_peek_data(&cq, &data, &dlen, errh) && 0 == dlen)
        return -1;
    return chunkqueue_write_data(fd, data, dlen);
}


#if defined(HAVE_MMAP) || defined(_WIN32) /*(see local sys-mmap.h)*/

/*(improved from network_write_mmap.c)*/
static off_t
mmap_align_offset (off_t start)
{
    static off_t pagemask = 0;
    if (0 == pagemask) {
      #ifndef _WIN32
        long pagesize = sysconf(_SC_PAGESIZE);
      #else
        long pagesize = -1; /*(not implemented (yet))*/
      #endif
        if (-1 == pagesize) pagesize = 4096;
        pagemask = ~((off_t)pagesize - 1); /* pagesize always power-of-2 */
    }
    return (start & pagemask);
}


__attribute_noinline__
static char *
chunkqueue_mmap_chunk_len (chunk * const c, off_t len)
{
    /* (re)mmap the buffer to file length if range is not covered completely */
    /*(caller is responsible for handling SIGBUS if chunkqueue might contain
     * untrusted file, i.e. any file other than lighttpd-created tempfile)*/
    /*(tempfiles are expected for input, MAP_PRIVATE used for portability)*/
    /*(mmaps and writes complete chunk instead of only small parts; files
     * are expected to be temp files with reasonable chunk sizes)*/
    if (MAP_FAILED == c->file.mmap.start
        || c->offset < c->file.mmap.offset
        || c->offset+len > (off_t)(c->file.mmap.offset + c->file.mmap.length)) {

        if (MAP_FAILED != c->file.mmap.start) {
            munmap(c->file.mmap.start, c->file.mmap.length);
            /*c->file.mmap.start = MAP_FAILED;*//*(assigned below)*/
        }

        c->file.mmap.offset = mmap_align_offset(c->offset);
        c->file.mmap.length = c->file.length - c->file.mmap.offset;
        c->file.mmap.start  =
          mmap(NULL, c->file.mmap.length, PROT_READ, MAP_PRIVATE,
               c->file.fd, c->file.mmap.offset);
        if (MAP_FAILED == c->file.mmap.start) return NULL;

      #if 0 /*(review callers before changing; some expect open file)*/
        /* close() fd as soon as fully mmap() rather than when done w/ chunk
         * (possibly worthwhile to keep active fd count lower) */
        if (c->file.is_temp && !c->file.refchg) {
            close(c->file.fd);
            c->file.fd = -1;
        }
      #endif
    }
    return c->file.mmap.start + c->offset - c->file.mmap.offset;
}

#endif


#if defined HAVE_SYS_SENDFILE_H && defined HAVE_SENDFILE \
 && (!defined _LARGEFILE_SOURCE || defined HAVE_SENDFILE64) \
 && defined(__linux__) && !defined HAVE_SENDFILE_BROKEN
#include <sys/sendfile.h>
#include <stdint.h>
#endif
static ssize_t
chunkqueue_write_chunk_file (const int fd, chunk * const restrict c, log_error_st * const errh)
{
    /*(similar to network_write_file_chunk_mmap(), but does not use send() on
    *  Windows because fd is expected to be file or pipe here, not socket)*/

    if (0 != chunk_open_file_chunk(c, errh))
        return -1;

    const off_t count = c->file.length - c->offset;
    if (0 == count) return 0; /*(sanity check)*/

  #if defined HAVE_SYS_SENDFILE_H && defined HAVE_SENDFILE \
   && (!defined _LARGEFILE_SOURCE || defined HAVE_SENDFILE64) \
   && defined(__linux__) && !defined HAVE_SENDFILE_BROKEN
    /* Linux kernel >= 2.6.33 supports sendfile() between most fd types */
    off_t offset = c->offset;
    const ssize_t wr =
      sendfile(fd, c->file.fd, &offset, count<INT32_MAX ? count : INT32_MAX);
    if (__builtin_expect( (wr >= 0), 1) || (errno != EINVAL && errno != ENOSYS))
        return wr;
  #endif

  #if defined(HAVE_MMAP) || defined(_WIN32) /*(see local sys-mmap.h)*/
    const char * const data = chunkqueue_mmap_chunk_len(c, count);
    if (NULL != data)
        return chunkqueue_write_data(fd, data, count);
  #endif

    return chunkqueue_write_chunk_file_intermed(fd, c, errh);
}


static ssize_t
chunkqueue_write_chunk_mem (const int fd, const chunk * const restrict c)
{
    const void * const buf = c->mem->ptr + c->offset;
    const size_t count = buffer_clen(c->mem) - (size_t)c->offset;
    ssize_t wr;
    do { wr = write(fd, buf, count); } while (-1 == wr && errno == EINTR);
    return wr;
}


ssize_t
chunkqueue_write_chunk (const int fd, chunkqueue * const restrict cq, log_error_st * const restrict errh)
{
    /*(note: expects non-empty cq->first)*/
    chunk * const c = cq->first;
    switch (c->type) {
      case MEM_CHUNK:
        return chunkqueue_write_chunk_mem(fd, c);
      case FILE_CHUNK:
        return chunkqueue_write_chunk_file(fd, c, errh);
      default:
        errno = EINVAL;
        return -1;
    }
}


ssize_t
chunkqueue_write_chunk_to_pipe (const int fd, chunkqueue * const restrict cq, log_error_st * const restrict errh)
{
    /*(note: expects non-empty cq->first)*/
  #ifdef HAVE_SPLICE /* splice() temp files to pipe on Linux */
    chunk * const c = cq->first;
    if (c->type == FILE_CHUNK) {
        loff_t abs_offset = c->offset;
        return (0 == chunk_open_file_chunk(c, errh))
          ? splice(c->file.fd, &abs_offset, fd, NULL,
                   (size_t)(c->file.length - c->offset), SPLICE_F_NONBLOCK)
          : -1;
    }
  #endif
    return chunkqueue_write_chunk(fd, cq, errh);
}


void
chunkqueue_small_resp_optim (chunkqueue * const restrict cq)
{
    /*(caller must verify response is small (and non-empty) before calling)*/

    /*(optimization to use fewer syscalls to send a small response by reading
     * small files into memory, thereby avoiding use of sendfile() and multiple
     * calls to writev()  (benefit for cleartext (non-TLS) and <= HTTP/1.1))
     *(If TLS, then will shortly need to be in memory for encryption anyway)*/

    /*assert(cq->first);*/
    chunk * restrict c = cq->first;
    chunk * const restrict filec = c->next;  /*(require file already be open)*/
    if (filec != cq->last || filec->type != FILE_CHUNK || filec->file.fd < 0)
        return;

  #ifndef HAVE_PREAD
    if (-1 == lseek(filec->file.fd, filec->offset, SEEK_SET)) return;
  #endif

    /* Note: there should be no size change in chunkqueue,
     * so cq->bytes_in and cq->bytes_out should not be modified */

    off_t len = filec->file.length - filec->offset;
    if (c->type != MEM_CHUNK || (size_t)len > buffer_string_space(c->mem)) {
        c->next = chunk_acquire((size_t)len+1);
        c = c->next;
        /*c->next = filec;*/
    }
    /* detach filec from chunkqueue; file expected to be read fully */
    c->next = NULL;
    cq->last = c;

    ssize_t rd;
    off_t offset = 0;
    char * const ptr = buffer_extend(c->mem, len);
    do {
      #ifdef HAVE_PREAD
        rd =pread(filec->file.fd, ptr+offset, (size_t)len,filec->offset+offset);
      #else
        rd = read(filec->file.fd, ptr+offset, (size_t)len);
      #endif
    } while (rd > 0 ? (offset += rd, len -= rd) : errno == EINTR);
    /*(contents of chunkqueue kept valid even if error reading from file)*/
    if (__builtin_expect( (0 == len), 1))
        chunk_release(filec);
    else { /*(unexpected; error recovery)*/
        buffer_truncate(c->mem, (uint32_t)(ptr + offset - c->mem->ptr));
        cq->last = c->next = filec;
        if (offset)
            filec->offset += offset;
        else if (__builtin_expect( (cq->first != c), 0)) {
            cq->first->next = filec;
            chunk_release(c);
        }
    }
}


int
chunkqueue_peek_data (chunkqueue * const cq,
                      char ** const data, uint32_t * const dlen,
                      log_error_st * const errh)
{
    char * const data_in = *data;
    const uint32_t data_insz = *dlen;
    *dlen = 0;

    for (chunk *c = cq->first; c; ) {
        uint32_t space = data_insz - *dlen;
        switch (c->type) {
          case MEM_CHUNK:
            {
                uint32_t have = buffer_clen(c->mem) - (uint32_t)c->offset;
                if (have > space)
                    have = space;
                if (*dlen)
                    memcpy(data_in + *dlen, c->mem->ptr + c->offset, have);
                else
                    *data = c->mem->ptr + c->offset; /*(reference; defer copy)*/
                *dlen += have;
                break;
            }

          case FILE_CHUNK:
            if (c->file.fd >= 0 || 0 == chunk_open_file_chunk(c, errh)) {
                off_t offset = c->offset;
                off_t len = c->file.length - c->offset;
                if (len > (off_t)space)
                    len = (off_t)space;
                if (0 == len)
                    break;

            #if 0 /* XXX: might improve performance on some system workloads */
              #if defined(_LP64) || defined(__LP64__) || defined(_WIN64)
              #if defined(HAVE_MMAP) || defined(_WIN32) /*see local sys-mmap.h*/
                /* mmap file to access data
                 * (Only consider temp files here since not catching SIGBUS)
                 * (For now, also limit to 64-bit to avoid address space issues)
                 * If temp file is used, data should be large enough that mmap
                 * is worthwhile.  fd need not be kept open for the mmap once
                 * the mmap has been created, but is currently kept open for
                 * other pre-existing logic which checks fd and opens file,
                 * such as the condition for entering this code block above. */
                /* Note: under heavy load (or microbenchmark), system-reported
                 * memory use for RSS can be very, very large, due to presence
                 * of lots and lots of temp file read-only memory maps.
                 * pmap -X and exclude lighttpd temporary files to get a better
                 * view of memory use */
                char *mdata;
                if (c->file.is_temp
                    && (mdata = chunkqueue_mmap_chunk_len(c, len))) {
                    if (*dlen) {
                        if (*data != data_in) {
                            memcpy(data_in, *data, *dlen);
                            *data = data_in;
                        }
                        memcpy(data_in+*dlen, mdata, (size_t)len);
                    }
                    else {
                        *data = mdata;
                    }
                    *dlen += (uint32_t)len;
                    break;
                }
              #endif
              #endif
            #endif

              #ifndef HAVE_PREAD
                if (-1 == lseek(c->file.fd, offset, SEEK_SET)) {
                    log_perror(errh, __FILE__, __LINE__, "lseek(\"%s\")",
                               c->mem->ptr);
                    return -1;
                }
              #endif
                ssize_t rd;
                do {
                  #ifdef HAVE_PREAD
                    rd =pread(c->file.fd, data_in + *dlen, (size_t)len, offset);
                  #else
                    rd = read(c->file.fd, data_in + *dlen, (size_t)len);
                  #endif
                } while (-1 == rd && errno == EINTR);
                if (rd <= 0) { /* -1 error; 0 EOF (unexpected) */
                    log_perror(errh, __FILE__, __LINE__, "read(\"%s\")",
                               c->mem->ptr);
                    return -1;
                }

                *dlen += (uint32_t)rd;
                break;
            }
            return -1;

          default:
            return -1;
        }

        if (*dlen == data_insz)
            break;

        c = c->next;
        if (NULL == c)
            break;

        if (*dlen && *data != data_in) {
            memcpy(data_in, *data, *dlen);
            *data = data_in;
        }
    }

    return 0;
}


int
chunkqueue_read_data (chunkqueue * const cq,
                      char * const data, const uint32_t dlen,
                      log_error_st * const errh)
{
    char *ptr = data;
    uint32_t len = dlen;
    if (chunkqueue_peek_data(cq, &ptr, &len, errh) < 0 || len != dlen)
        return -1;
    if (data != ptr) memcpy(data, ptr, len);
    chunkqueue_mark_written(cq, len);
    return 0;
}


buffer *
chunkqueue_read_squash (chunkqueue * const restrict cq, log_error_st * const restrict errh)
{
    /* read and replace chunkqueue contents with single MEM_CHUNK.
     * cq->bytes_out is not modified */

    off_t cqlen = chunkqueue_length(cq);
    if (cqlen >= UINT32_MAX) return NULL;

    if (cq->first && NULL == cq->first->next && cq->first->type == MEM_CHUNK)
        return cq->first->mem;

    chunk * const c = chunk_acquire((uint32_t)cqlen+1);
    char *data = c->mem->ptr;
    uint32_t dlen = (uint32_t)cqlen;
    int rc = chunkqueue_peek_data(cq, &data, &dlen, errh);
    if (rc < 0) {
        chunk_release(c);
        return NULL;
    }
    buffer_truncate(c->mem, dlen);

    chunkqueue_release_chunks(cq);
    chunkqueue_append_chunk(cq, c);
    return c->mem;
}
