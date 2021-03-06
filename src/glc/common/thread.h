/**
 * \file glc/common/thread.c
 * \brief generic stream processor thread adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
 * \author Olivier Langlois <olivier@trillion01.com>
 * \date 2014

    Copyright 2014 Olivier Langlois

    This file is part of glcs.

    glcs is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    glcs is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with glcs.  If not, see <http://www.gnu.org/licenses/>.

 */

/**
 * \addtogroup common
 *  \{
 * \defgroup thread generic thread
 *  \{
 */

#ifndef _THREAD_H
#define _THREAD_H

#include <packetstream.h>
#include <glc/common/glc.h>

#ifdef __cplusplus
extern "C" {
#endif

/** currently unused legacy */
#define GLC_THREAD_UNUSED1                    1
/** currently unused legacy */
#define GLC_THREAD_UNUSED2                    2
/** thread does not yet know final packet size, so write dma
    is not acquired */
#define GLC_THREAD_STATE_UNKNOWN_FINAL_SIZE   4
/** thread wants to skip reading a packet */
#define GLC_THREAD_STATE_SKIP_READ            8
/** thread wants to skip writing a packet */
#define GLC_THREAD_STATE_SKIP_WRITE          16
/** just copy data to write packet, skip write callback */
#define GLC_THREAD_COPY                      32
/** thread wants to stop */
#define GLC_THREAD_STOP                      64

/**
 * \brief thread state
 *
 * Thread state structure holds information about
 * current read and write packet and thread state.
 * Callback functions modify data and thread state
 * using this structure.
 */
typedef struct {
	/** flags */
	glc_flags_t flags;
	/** current message header */
	glc_message_header_t header;
	/** read data excluding header */
	char *read_data;
	/** data to be written excluding header */
	char *write_data;
	/** read data size */
	size_t read_size;
	/** write data size, read callback should set this
	    so thread knows how big dma to request */
	size_t write_size;
	/** global argument pointer */
	void *ptr;
	/** per-thread argument pointer */
	void *threadptr;
} glc_thread_state_t;

/** thread does read operations */
#define GLC_THREAD_READ                       1
/** thread does write operations */
#define GLC_THREAD_WRITE                      2
/**
 * \brief thread vtable
 *
 * glc_thread_t holds information about thread callbacks
 * and features. Mandatory values are flags, and threads.
 * If callback is NULL, it is ignored.
 */
typedef struct {
	/** flags, GLC_THREAD_READ or GLC_THREAD_WRITE or both */
	glc_flags_t flags;
	/** global argument pointer */
	void *ptr;
	/** number of threads to create */
	size_t threads;
	/** flag to indicate that rt prio is desired. */
	int    ask_rt;
	/** implementation specific */
	void *priv;

	/** thread create callback is called when a thread starts */
	int (*thread_create_callback)(void *, void **);
	/** thread finish callback is called when a thread is finished */
	void (*thread_finish_callback)(void *, void *, int);
	/** open callback is called before thread tries to
	    open read (or write if GLC_THREAD_READ is not specified)
	    packet */
	int (*open_callback)(glc_thread_state_t *);
	/** header callback is called when thread has read
	    header from packet */
	int (*header_callback)(glc_thread_state_t *);
	/** read callback is called when thread has read the
	    whole packet */
	int (*read_callback)(glc_thread_state_t *);
	/** write callback is called when thread has opened
	    dma to write packet */
	int (*write_callback)(glc_thread_state_t *);
	/** close callback is called when both packets are closed */
	int (*close_callback)(glc_thread_state_t *);
	/** finish callback is called only once, when all threads have
	    finished */
	void (*finish_callback)(void *, int);
} glc_thread_t;

/**
 * \brief create thread
 *
 * Creates thread.threads threads (glc_thread()).
 * \param glc glc
 * \param thread thread information structure
 * \param from buffer where data is read from
 * \param to buffer where data is written to
 * \return 0 on success otherwise an error code
 */
__PUBLIC int glc_thread_create(glc_t *glc, glc_thread_t *thread,
				ps_buffer_t *from, ps_buffer_t *to);

/**
 * \brief block until threads have finished and clean up
 * \param thread thread
 * \return 0 on success otherwise an error code
 */
__PUBLIC int glc_thread_wait(glc_thread_t *thread);

typedef struct {
	pthread_t thread;
	/** flag to indicate that rt prio is desired. */
	int ask_rt;
	int running;
} glc_simple_thread_t;

__PUBLIC int glc_simple_thread_create(glc_t *glc, glc_simple_thread_t *thread,
				void *(*start_routine) (void *), void *arg);

__PUBLIC int glc_simple_thread_wait(glc_t *glc, glc_simple_thread_t *thread);

#ifdef __cplusplus
}
#endif

#endif

/**  \} */
/**  \} */
