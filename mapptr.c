/*=                                     -*- c-file-style: "bsd" -*-
 *
 * $Id$
 * 
 * Copyright (C) 2000 by Martin Pool <mbp@humbug.org.au>
 * Copyright (C) 1998 by Andrew Tridgell <tridge@samba.org>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

                              /*
                               | It's evolution, baby!
                               */

/* Originally from rsync.  Thanks, tridge! */

/*
 * MAP POINTERS:
 * 
 * This provides functionality somewhat similar to mmap() but using read().
 * It gives sliding window access to a file. With certain constraints, this
 * is suitable for use on sockets and similar things that cannot normally
 * support seek or mmap. Specifically, the caller must never attempt to move
 * backwards or to skip forwards without reading.  Both of these are
 * implicitly true for libhsync when interacting with a socket.
 * 
 * It's not an error to try to map past the end of a file.  If you do this,
 * the map will run up to the end of the file, and a flag will be returned to 
 * indicate that EOF was observed.  This will be checked each time you try to 
 * map past the end, so something good will happen if the file grows
 * underneath you.
 * 
 * If the file is open with O_NONBLOCK, then the operating system may choose
 * to fail an attempt to read, saying that it would block.  In this case, the 
 * map will not not fail, but it will indicate that zero bytes are available. 
 * The caller should be smart about doing a select(2) on the fd and calling
 * back when more data is available. 
 */

/*
 * TODO: Optionally debug this by simulating short reads.
 *
 * TODO: Make the default buffer smaller and make sure we test what
 * happens when it grows.
 *
 * TODO: Add an option to say we will never seek backwards, and so old
 * data can be discarded immediately.  There are some notes towards
 * this in walker.c, but it seems better just to implement them as a
 * different method on mapptr rather than from scratch.
 *
 * TODO: Is it really worth the trouble of handling files that grow?
 * In other words, if we've seen EOF once then is it better just to
 * remember that and not try to read anymore?  Certainly at least in
 * rproxy we should never have to deal with growing files On the other
 * hand, I'm not sure it costs us anything: if the caller doesn't try
 * to read past the end of the file then all requests should be
 * satisfied from cache and we never will actually try a long read.
 * Also, I rather think try to read at EOF will be quite quick:
 * presumably the operating system can just compare the current
 * position to the length.
 *
 * TODO: Perhaps support different ways for choosing the new window
 * depending on whether we're reading from a socket or from a file, or
 * on whether we expect random or sequential access, or on how useful
 * random access is expected to be.
 *
 * TODO: What about a function that turns around the flow of control
 * and calls a callback for all the available data?  Silly?
 */

/* The Unix98 pread(2) function is pretty interesting: it reads data
 * at a given offset, but without moving the file offset and in only a
 * single call.  Cute, but probably pointless in this application. */

/* mapptr is fine, but it's not optimized for reading from a socket into
 * nad.
 *
 * What's wrong?
 *
 * mapptr has the problem in this situation that it will try to read
 * more data than is strictly required, and this damages liveness.
 * Also, though this is less important, it retains old data in the
 * buffer even when we know we won't use it, and this is bad.
 *
 * On the other hand perhaps having less code is more important than
 * all the code being optimal. */

/*
 * walker -- an alternative input method for mapptr.  This one is
 * optimized for reading from a socket, or something similar, where we
 * never seek forward (skip) or backward (reverse).
 *
 * This code uses the same data structure as mapptr, but manipulates
 * it according to a different algorithm.  You could switch between
 * them, though there doesn't seem much point.  As with mapptr,
 * map_walker is called with the desired offset and length of the data
 * to map.  It may indicate to the caller that more or less data is
 * available, and it also indicates whether end of file was observed.
 *
 * The goals are:
 * 
 *  - make as much input data as possible available to the caller
 *    program.
 *
 *  - allocate no more memory for the input buffer than is necessary.
 *
 *  - avoid copying data.
 */


/*----------------------------------------------------------------------
 *
 *  ====================================================  file
 *     ||||||||||||||||||||||||||||||||||||||||||         buffer
 *             $$$$$$$$$$$$$$$$$$$$$$$$$$                 window
 *
 * We have three overlapping extents here: the file is the sequence of
 * bytes from the stream.  The buffer covers a certain region of it,
 * but not all of the buffer is necessarily valid.  The window is the
 * section of the buffer that contains valid data. */

/* TODO: Run this whole algorithm past tridge. */

#include "includes.h"

#include <unistd.h>
#include <string.h>
#include <sys/file.h>

#include "mapptr.h"
#include "map_p.h"

/* These values are intentionally small at the moment.  It would be
 * more efficient to make them larger, but these are more likely to
 * tease bugs out into the open. */
#define CHUNK_SIZE (1024)

/* We'll read data in windows of this size, unless otherwise indicated. */
#ifdef HS_BIG_WINDOW
static ssize_t const DEFAULT_WINDOW_SIZE = ((ssize_t) (256 * 1024));
#else
static ssize_t const DEFAULT_WINDOW_SIZE = ((ssize_t) (16 * 1024));
#endif


/*
 * Set up a new file mapping.
 * 
 * The file cursor is assumed to be at position 0 when this is
 * called. For nonseekable files this is arbitrary; for seekable files
 * bad things will happen if that's not true and we later have to
 * seek.
 */
hs_map_t *
hs_map_file(int fd)
{
    hs_map_t       *map;

    map = _hs_alloc_struct(hs_map_t);

    /* TODO: Perhaps use fcntl(fd, F_GETFL, 0) to check whether the
     * file has O_NONBLOCK set, and remember that for later? */

    map->godtag = HS_MAP_TAG;
    map->fd = fd;
    map->p = NULL;
    map->p_size = 0;
    map->p_offset = 0;
    map->p_fd_offset = 0;
    map->p_len = 0;

    return map;
}


/*
 * Read data into MAP at &p[READ_OFFSET].  Return the number of bytes added
 * to the buffer, and set REACHED_EOF if appropriate.
 * 
 * The amount of data is specified in an opportunistic, lazy way, with the
 * idea being that we make IO operations as large as possible without
 * blocking for any longer than is necessary when waiting for data from a
 * network.
 * 
 * Therefore, the function tries to read at least MIN_SIZE bytes, unless it
 * encounters an EOF or error.  It reads up to MAX_SIZE bytes, and there must 
 * be that much space in the buffer.  Once MIN_SIZE bytes have been received, 
 * no new IO operations will start. 
 */
static ssize_t
_hs_map_do_read(hs_map_t *map,
                off_t const read_offset,
                ssize_t const max_size, ssize_t const min_size,
                int *reached_eof)
{
    ssize_t total_read = 0;     /* total amount read in */
    ssize_t nread;
    ssize_t buf_remain = max_size; /* buffer space left */
    byte_t *p = map->p + read_offset;

    assert(max_size > 0);
    assert(min_size >= 0);
    assert(read_offset >= 0);
    assert(map->godtag == HS_MAP_TAG);
    
    do {
        nread = read(map->fd, p, (size_t) buf_remain);

        _hs_trace("tried to read %ld bytes, result %ld",
                  (long) buf_remain, (long) nread);

        if (nread < 0  &&  errno == EWOULDBLOCK) {
            _hs_trace("input from this file would block");
            break; /* go now */
        } else if (nread < 0) {
            _hs_error("read error in hs_mapptr: %s", strerror(errno));
            /* Should we return null here?  We ought to tell the
               caller about this somehow, but at the same time we
               don't want to discard the data we have already
               received. */
            break;
        } else if (nread == 0) {
            /* GNU libc manual: A value of zero indicates end-of-file
             * (except if the value of the SIZE argument is also
             * zero).  This is not considered an error.  If you keep
             * *calling `read' while at end-of-file, it will keep
             * returning zero and doing nothing else.  */
            *reached_eof = 1;
            break;
        }

        total_read += nread;
        p += nread;
        buf_remain -= nread;
        map->p_fd_offset += nread;

        /* TODO: If we know we're in nonblocking mode, then perhaps we
         * should keep reading data until we either run out of space
         * or we know we're about to block. */
    } while (total_read < min_size);

    _hs_trace("wanted %ld to %ld bytes, read %ld bytes, fd now at %ld%s",
              (long) min_size, (long) max_size, (long) total_read,
              (long) map->p_fd_offset,
              *reached_eof ? " which is eof" : "");

    return total_read;
}


/*
 * If we can satisfy this request from data already cached in MAP,
 * then update LEN and REACHED_EOF and return an appropriate pointer.
 * Otherwise, return NULL in which case the caller should arrange to
 * do some IO.
 */
void *
_hs_map_from_cache(hs_map_t * map, off_t offset, size_t *len)
{
    size_t		out_off; /* offset in window to return */

    if (offset < map->p_offset) {
        /* Requested region starts before the window. */
        return NULL;
    }
    if (offset > (off_t) (map->p_offset + map->p_len)) {
        /* Requested region starts after the window. */
        return NULL;
    }
    
    out_off = offset - map->p_offset;

    if (out_off + *len <= map->p_len) {
	/* Starts after the beginning of the window and fits in the
	 * window. */
	*len = map->p_len - out_off;
	_hs_trace("found %ld byte match in cache", (long) *len);
	return map->p + out_off;
    }            

    return NULL;
}


/*
 * Work out where to put the window to cover the requested region.
 */
static void
_hs_map_calc_window(off_t offset, size_t *len,
                    off_t *window_start, size_t *window_size)
{
    if (offset > (off_t) (2 * CHUNK_SIZE)) {
        /* On some systems, it's much faster to do reads aligned with
         * filesystem blocks.  This isn't the case on Linux, which has
         * a pretty efficient filesystem and kernel/app interface, but
         * we don't lose a lot by checking this. */
        *window_start = offset - 2 * CHUNK_SIZE;
        
        /* Include only higher-order bits; assumes power of 2 */
        *window_start &= ~((off_t) (CHUNK_SIZE - 1)); 
    } else {
        *window_start = 0;
    }
    *window_size = DEFAULT_WINDOW_SIZE;

    /* If the default window is not big enough to hold all the data, then
     * expand it. */
    if (offset + *len > *window_start + *window_size) {
        *window_size = (offset + *len) - *window_start;
    }
}


static void
_hs_map_ensure_allocated(hs_map_t *map, size_t window_size)
{
    /* make sure we have allocated enough memory for the window */
    if (!map->p) {
        assert(map->p_size == 0);
        _hs_trace("allocate initial %ld byte window", (long) window_size);
        map->p = (byte_t *) malloc((size_t) window_size);
        map->p_size = window_size;
    } else if (window_size > map->p_size) {
        _hs_trace("grow buffer to hold %ld byte window",
                  (long) window_size);
        map->p = (byte_t *) realloc(map->p, (size_t) window_size);
        map->p_size = window_size;
    }

    if (!map->p) {
        _hs_fatal("map_ptr: out of memory");
    }
}


/*
 * Return a pointer to a mapped region of a file, of at least LEN bytes.  You
 * can read from (but not write to) this region just as if it were mmap'd.
 * 
 * If the file reaches EOF, then the region mapped may be less than is
 * requested.  In this case, LEN will be reduced, and REACHED_EOF will
 * be set.  If EOF was seen, but not in the requested region, then
 * REACHED_EOF will not be set until you ask to map the area up to the
 * end of the file.
 * 
 * LEN may be increased if more data than you requested is available.
 *
 * If the file is nonblocking, then any data available will be
 * returned, and LEN will change to reflect this.
 * 
 * The buffer is only valid until the next call to hs_map_ptr on this map, or
 * until _hs_unmap_file.  You certainly MUST NOT free the buffer.
 * 
 * Iff an error occurs, returns NULL.
 */
void const *
hs_map_ptr(hs_map_t * map, off_t offset, size_t *len, int *reached_eof)
{
    /* window_{start,size} define the part of the file that will in
       the future be covered by the map buffer, if we have our way.

       read_{start,size} describes the region of the file that we want
       to read; we'll put it into the buffer starting at
       &p[read_offset]. */
    off_t            window_start, read_start;
    size_t              window_size;
    ssize_t read_max_size;      /* space remaining */
    ssize_t read_min_size;      /* needed to fill this request */
    off_t read_offset;
    ssize_t total_read, avail;
    ssize_t		out_off; /* offset in window to return */
    void                *p;

    assert(map->godtag == HS_MAP_TAG);
    assert(len != NULL);        /* check pointers */
    assert(reached_eof != NULL);
    assert(offset >= 0);
    assert(*len > 0);
    *reached_eof = 0;

    _hs_trace("asked for off=%ld, len=%ld", (long) offset, (long) *len);

    out_off = (offset - map->p_offset);
    
    /* We hope that for many reads the required data will already be available
     * in the window, so we return it directly in that case.  Also, if the EOF
     * marker is in the requested region, we tell that to the client. */
    if ((p = _hs_map_from_cache(map, offset, len)))
        return p;

    _hs_map_calc_window(offset, len, &window_start, &window_size);
    _hs_map_ensure_allocated(map, window_size);

    /* now try to avoid re-reading any bytes by reusing any bytes from the
     * previous buffer. */
    if (window_start >= map->p_offset &&
        window_start < (off_t) (map->p_offset + map->p_len) &&
        window_start + window_size >= map->p_offset + map->p_len) {
        read_start = map->p_offset + map->p_len;
        read_offset = read_start - window_start;
        assert(read_offset >= 0);
        read_max_size = window_size - read_offset;
        memmove(map->p, map->p + (map->p_len - read_offset),
                (size_t) read_offset);
    } else {
        read_start = window_start;
        read_max_size = window_size;
        read_offset = 0;
    }

    map->p_offset = window_start;

    /* Work out the minimum number of bytes we must read to cover the
     * requested region. */
    read_min_size = *len + (offset - map->p_offset) - read_offset;

    if (read_min_size <= 0) {
        _hs_trace("no need to read after moving data; p_offset=%ld",
                  (long) map->p_offset);
        return map->p + (offset - map->p_offset);
    }

    if (map->p_fd_offset != read_start) {
        if (lseek(map->fd, read_start, SEEK_SET) != read_start) {
            _hs_error("lseek to %ld failed in map_ptr: %s",
                      (long) read_start, strerror(errno));
            abort();
        }
        map->p_fd_offset = read_start;
        _hs_trace("seek to %ld", (long) read_start);
    }

    /* read_min_size may be >*len when offset > map->p_offset, i.e. we
     * have to read in some data before the stuff the caller wants to
     * see.  We read it anyhow to avoid seeking (in the case of a
     * pipe) or because they might want to go back and see it later
     * (in a file). */

    if (read_min_size > read_max_size) {
        _hs_fatal("we really screwed up: minimum size is %ld, but remaining "
                  "buffer is just %ld",
                  (long) read_min_size, (long) read_max_size);
    }
    
    total_read = _hs_map_do_read(map, read_offset, read_max_size,
                                 read_min_size, reached_eof);

    /*
     * If we didn't map all the data we wanted because we ran into EOF, then
     * adjust everything so that the map doesn't hang out over the end of the
     * file.
     */

    /* Amount of data now valid: the stuff at the start of the buffer * from
     * last time, plus the data now read in. */
    map->p_len = read_offset + total_read;

    if (total_read == read_max_size) {
        /* This was the formula before we worried about EOF, so assert
         * that it's still the same. */
        assert(map->p_len == window_size);
    }

    /* Available data after the requested offset: we have p_len bytes *
     * altogether, but the client is interested in the ones starting * at
     * &p[offset - map->p_offset] */
    avail = map->p_len - (offset - map->p_offset);
    if (avail < 0)
        avail = 0;
    *len = avail;

    return map->p + (offset - map->p_offset);
}


/*
 * Release a file mapping.  This does not close the underlying fd. 
 */
void
_hs_unmap_file(hs_map_t * map)
{
    assert(map->godtag == HS_MAP_TAG);
    if (map->p) {
        free(map->p);
        map->p = NULL;
    }
    hs_bzero(map, sizeof *map);
    free(map);
}
