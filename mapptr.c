/* -*- mode: c; c-file-style: "bsd" -*- */
/*--------------------------------------------------------------------
   $Id$
   
   Copyright (C) 2000 by Martin Pool
   Copyright (C) 1998 by Andrew Tridgell 
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* Originally from rsync.  Thanks, tridge! */

/* MAP POINTERS:
  
   This provides functionality somewhat similar to mmap() but using
   read(). It gives sliding window access to a file. With certain
   constraints, this is suitable for use on sockets and similar things
   that cannot normally support seek or mmap. Specifically, the caller
   must never attempt to move backwards or to skip forwards without
   reading.  Both of these are implicitly true for libhsync when
   interacting with a socket.

   It's not an error to try to map past the end of a file.  If you do
   this, the map will run up to the end of the file, and a flag will
   be returned to indicate that EOF was observed.  This will be
   checked each time you try to map past the end, so something good
   will happen if the file grows underneath you. */


/* TODO: When reading from a socket, it might happen that we've read
   enough data to satisfy the current mapping request, though not
   enough to completely full the buffers.  That's OK, though: the best
   thing to do in that case would be to return the current data, and
   trust that more network packets will arrive in their own good time.
   This means that in map_ptr we have to know the minimum satisfactory
   amount of input.

   TODO: Conversely: if it turns out that we read more data than the
   user really wants, then it would be nice to offer them the chance
   to use more of it.

   TODO: Optionally debug this by simulating short reads. */

#include "includes.h"

#ifndef DO_HS_TRACE
#define DO_HS_TRACE
#endif

#include "hsync.h"
#include "private.h"

#define CHUNK_SIZE (32*1024)
#define IO_BUFFER_SIZE (4092)

/* We'll read data in windows of this size, unless otherwise indicated. */
static ssize_t const DEFAULT_WINDOW_SIZE = (ssize_t) (256 * 1024);
static int const HS_MAP_TAG = 189900;


/* 
   HS_MAP structure:
  
   TAG is a dogtag, and always equal to HS_MAP_TAG if the structure is
   valid.  FD is of course the file descriptor we're using for input.
  
   P points to the start of the allocated data buffer.  P_SIZE is the
   amount of allocated buffer at P, not all of which necessarily
   contains valid file data.
  
   P_FD_OFFSET is the current absolute position of the file cursor.
   We use this to avoid doing seeks if we're already in the right
   position.  P_OFFSET is the absolute position in the file covered by
   P[0].  P_LEN is the number of bytes after that point that are valid
   in P.
 */
struct hs_map {
    int             tag;
    /*@null@*/ char *p;
    int             fd;
    ssize_t p_size, p_len;
    hs_off_t        p_offset, p_fd_offset;
};


/* Set up a new file mapping.
  
   The file cursor is assumed to be at position 0 when this is called.
   For nonseekable files this is arbitrary; for seekable files bad
   things will happen if that's not true and we later have to seek. */
hs_map_t       *
_hs_map_file(int fd)
{
    hs_map_t       *map;

    map = (hs_map_t *) malloc(sizeof(*map));
    if (!map) {
	_hs_fatal("map_file couldn't allocate memory for hs_map_t");
    }

    map->tag = HS_MAP_TAG;
    map->fd = fd;
    map->p = NULL;
    map->p_size = 0;
    map->p_offset = 0;
    map->p_fd_offset = 0;
    map->p_len = 0;

    return map;
}


/* Read data into MAP at &p[READ_OFFSET].  Return the number of bytes
   added to the buffer, and set REACHED_EOF if appropriate.

   The amount of data is specified in an opportunistic, lazy way, with
   the idea being that we make IO operations as large as possible
   without blocking for any longer than is necessary when waiting for
   data from a network.

   Therefore, the function tries to read at least MIN_SIZE bytes,
   unless it encounters an EOF or error.  It reads up to MAX_SIZE
   bytes, and there must be that much space in the buffer.  Once
   MIN_SIZE bytes have been received, no new IO operations will
   start. */
static ssize_t
_hs_map_do_read(hs_map_t *map,
		hs_off_t const read_offset,
		ssize_t const max_size, ssize_t const min_size,
		int *reached_eof)
{
    ssize_t total_read = 0;	/* total amount read in */
    ssize_t nread;
    ssize_t buf_remain = max_size; /* buffer space left */
    char *p = map->p + read_offset;

    assert(max_size > 0);
    assert(min_size >= 0);
    assert(read_offset >= 0);
    assert(map->tag == HS_MAP_TAG);
    
    do {
	nread = read(map->fd, p, (size_t) buf_remain);

	if (nread < 0) {
	    _hs_error("read error in hs_mapptr: %s", strerror(errno));
	    /* Should we return null here? */
	    break;
	}

	if (nread == 0) {
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
    } while (total_read < min_size);

    _hs_trace("wanted %ld to %ld bytes, read %ld bytes%s",
	      (long) min_size, (long) max_size, (long) total_read,
	      *reached_eof ? ", now at eof" : "");

    return total_read;
}


/* Return a pointer to a mapped region of a file, of at least LEN
   bytes.  You can read from (but not write to) this region just as if
   it were mmap'd.
  
   If the file reaches EOF, then the region mapped may be less than is
   requested.  In this case, LEN will be reduced, and REACHED_EOF will
   be set.

   LEN may be increased if more data than you requested is
   available.

   The buffer is only valid until the next call to _hs_map_ptr on this
   map, or until _hs_unmap_file.  You certainly MUST NOT free the
   buffer. */
/*@null@*/ const char *
_hs_map_ptr(hs_map_t * map, hs_off_t offset, ssize_t *len, int *reached_eof)
{
    /* window_{start,size} define the part of the file that will in
       the future be covered by the map buffer, if we have our way.

       read_{start,size} describes the region of the file that we want
       to read; we'll put it into the buffer starting at
       &p[read_offset]. */
    hs_off_t        window_start, read_start;
    ssize_t window_size;
    ssize_t          read_max_size; /* space remaining */
    ssize_t read_min_size;	/* needed to fill this request */
    hs_off_t read_offset;
    ssize_t             total_read, avail;

    assert(map->tag == HS_MAP_TAG);
    assert(len != NULL);	/* check pointers */
    assert(reached_eof != NULL);
    assert(offset >= 0);
    assert(*len > 0);
    *reached_eof = 0;

    _hs_trace("map=<%p>, off=%ld, len=%ld",
	      map, (long) offset, (long) *len);

    /* in most cases the region will already be available */
    if (offset >= map->p_offset &&
	offset + *len <= map->p_offset + map->p_len) {
	_hs_trace("region is already in the buffer");
	return (map->p + (offset - map->p_offset));
    }


    /* nope, we are going to have to do a read. Work out our desired window */
    if (offset > (hs_off_t) (2 * CHUNK_SIZE)) {
	/* XXX: Is this useful?  If we're out of the first two blocks,
           then it tries to keep the start of the window block aligned
           in the file.  But why? */
	window_start = offset - 2 * CHUNK_SIZE;
	
	/* Include only higher-order bits; assumes power of 2 */
	window_start &= ~((hs_off_t) (CHUNK_SIZE - 1));	
    } else {
	window_start = 0;
    }
    window_size = DEFAULT_WINDOW_SIZE;

    /* If the default window is not big enough to hold all the data,
       then expand it. */
    if (offset + *len > window_start + window_size) {
	window_size = (offset + *len) - window_start;
    }

    /* make sure we have allocated enough memory for the window */
    if (!map->p) {
	assert(map->p_size == 0);
	map->p = (char *) malloc((size_t) window_size);
	map->p_size = window_size;
    } else if (window_size > map->p_size) {
	map->p = (char *) realloc(map->p, (size_t) window_size);
	map->p_size = window_size;
    }

    if (!map->p) {
	_hs_fatal("map_ptr: out of memory");
    }

    /* now try to avoid re-reading any bytes by reusing any bytes from the
     * previous buffer. */
    if (window_start >= map->p_offset &&
	window_start < map->p_offset + map->p_len &&
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

    if (read_max_size <= 0) {
	_hs_trace("Warning: unexpected read size of %d in map_ptr\n",
		  read_max_size);
	return NULL;
    }

    if (map->p_fd_offset != read_start) {
	if (lseek(map->fd, read_start, SEEK_SET) != read_start) {
	    _hs_trace("lseek failed in map_ptr\n");
	    abort();
	}
	map->p_fd_offset = read_start;
    }

    /* Work out the minimum number of bytes we must read to cover the
       requested region. */
    read_min_size = *len + (offset - map->p_offset) - read_offset;
    assert(read_min_size >= 0);

    /* read_min_size may be >*len when offset > map->p_offset, i.e. we
       have to read in some data before the stuff the caller wants to
       see.  We read it anyhow to avoid seeking (in the case of a
       pipe) or because they might want to go back and see it later
       (in a file). */

    if (read_min_size > read_max_size) {
	_hs_fatal("we really screwed up: minimum size is %ld, but remaining "
		  "buffer is just %ld",
		  (long) read_min_size, (long) read_max_size);
    }
    
    total_read = _hs_map_do_read(map, read_offset, read_max_size,
				 read_min_size, reached_eof);
    assert(*reached_eof  ||  total_read >= read_min_size);
    map->p_fd_offset += total_read;

    map->p_offset = window_start;

    /* If we didn't map all the data we wanted because we ran into * EOF,
     * then adjust everything so that the map doesn't hang out * over the end 
     * of the file.  */

    /* Amount of data now valid: the stuff at the start of the buffer * from
     * last time, plus the data now read in. */
    map->p_len = read_offset + total_read;

    if (total_read == read_max_size) {
	/* This was the formula before we worried about EOF, so * assert that 
	 * it's still the same. */
	assert(map->p_len == window_size);
    }

    /* Available data after the requested offset: we have p_len bytes *
     * altogether, but the client is interested in the ones starting * at
     * &p[offset - map->p_offset] */
    avail = map->p_len - (offset - map->p_offset);
    if (avail < *len)
	*len = avail;

    return map->p + (offset - map->p_offset);
}


/* Release a file mapping.  This does not close the underlying fd. */
void
_hs_unmap_file(hs_map_t * map)
{
    assert(map->tag == HS_MAP_TAG);
    if (map->p) {
	free(map->p);
	map->p = NULL;
    }
    memset(map, 0, sizeof(*map));
    free(map);
}
