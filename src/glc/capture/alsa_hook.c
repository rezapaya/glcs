/**
 * \file glc/capture/alsa_hook.c
 * \brief audio capture hooks adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup alsa_hook
 *  \{
 */

/**
 * \note this has some threading bugs, but async alsa uses signals,
 *       so some tradeoffs are required
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <packetstream.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>

#include <glc/common/glc.h>
#include <glc/common/core.h>
#include <glc/common/thread.h>
#include <glc/common/log.h>
#include <glc/common/state.h>
#include <glc/common/util.h>

#include "alsa_hook.h"
#include "optimization.h"

#define ALSA_HOOK_CAPTURING    0x1
#define ALSA_HOOK_ALLOW_SKIP   0x2

struct alsa_hook_stream_s {
	alsa_hook_t alsa_hook;
	glc_state_audio_t state_audio;
	glc_stream_id_t id;
	glc_audio_format_t format;

	snd_pcm_t *pcm;
	int mode;
	const snd_pcm_channel_area_t *mmap_areas;
	snd_pcm_uframes_t frames, offset;

	unsigned int channels;
	unsigned int rate;
	glc_flags_t flags;
	int complex;

	int fmt, initialized;

	ps_packet_t packet;

	/* thread-related */
	glc_simple_thread_t thread;

	/* for communicating with capture thread */
	sem_t capture_empty, capture_full;

	/* for locking access */
	pthread_mutex_t write_mutex;
	pthread_spinlock_t write_spinlock;

	/* for busy waiting */
	volatile int capture_ready;

	char *capture_data;
	ssize_t capture_size, capture_data_size;
	glc_utime_t capture_time;

	struct alsa_hook_stream_s *next;
};

struct alsa_hook_s {
	glc_t *glc;
	glc_flags_t flags;
	ps_buffer_t *to;

	int started;

	struct alsa_hook_stream_s *stream;
};

static int alsa_hook_init_streams(alsa_hook_t alsa_hook);
static int alsa_hook_get_stream(alsa_hook_t alsa_hook, snd_pcm_t *pcm,
				struct alsa_hook_stream_s **stream);
static int alsa_hook_stream_init(alsa_hook_t alsa_hook,
				struct alsa_hook_stream_s *stream);
static int alsa_hook_stream_wait(struct alsa_hook_stream_s *stream);
static void *alsa_hook_mmap_pos(const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset);
static int alsa_hook_complex_to_interleaved(struct alsa_hook_stream_s *stream,
				const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
				snd_pcm_uframes_t frames, char *to);

static int alsa_hook_wait_for_thread(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream);
static int alsa_hook_lock_write(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream);
static int alsa_hook_unlock_write(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream);
static int alsa_hook_realloc_capture_buf(struct alsa_hook_stream_s *stream, ssize_t size);
static int alsa_hook_set_data_size(struct alsa_hook_stream_s *stream, ssize_t size);
static void *alsa_hook_thread(void *argptr);

static glc_audio_format_t pcm_fmt_to_glc_fmt(snd_pcm_format_t pcm_fmt);

int alsa_hook_init(alsa_hook_t *alsa_hook, glc_t *glc)
{
	*alsa_hook = (alsa_hook_t) calloc(1, sizeof(struct alsa_hook_s));
	(*alsa_hook)->glc = glc;
	return 0;
}

int alsa_hook_set_buffer(alsa_hook_t alsa_hook, ps_buffer_t *buffer)
{
	if (unlikely(alsa_hook->to))
		return EALREADY;

	alsa_hook->to = buffer;
	return 0;
}

int alsa_hook_allow_skip(alsa_hook_t alsa_hook, int allow_skip)
{
	if (allow_skip)
		alsa_hook->flags |= ALSA_HOOK_ALLOW_SKIP;
	else
		alsa_hook->flags &= ~ALSA_HOOK_ALLOW_SKIP;

	return 0;
}

int alsa_hook_start(alsa_hook_t alsa_hook)
{
	if (unlikely(!alsa_hook->to)) {
		glc_log(alsa_hook->glc, GLC_ERROR, "alsa_hook",
			 "target buffer not specified");
		return EAGAIN;
	}

	if (!alsa_hook->started)
		alsa_hook_init_streams(alsa_hook);

	if (alsa_hook->flags & ALSA_HOOK_CAPTURING)
		glc_log(alsa_hook->glc, GLC_WARN, "alsa_hook",
			 "capturing is already active");
	else
		glc_log(alsa_hook->glc, GLC_INFO, "alsa_hook",
			 "starting capturing");

	alsa_hook->flags |= ALSA_HOOK_CAPTURING;
	return 0;
}

int alsa_hook_stop(alsa_hook_t alsa_hook)
{
	if (alsa_hook->flags & ALSA_HOOK_CAPTURING)
		glc_log(alsa_hook->glc, GLC_INFO, "alsa_hook",
			 "stopping capturing");
	else
		glc_log(alsa_hook->glc, GLC_WARN, "alsa_hook",
			 "capturing is already stopped");

	alsa_hook->flags &= ~ALSA_HOOK_CAPTURING;
	return 0;
}

int alsa_hook_init_streams(alsa_hook_t alsa_hook)
{
	struct alsa_hook_stream_s *stream = alsa_hook->stream;

	if (unlikely(!alsa_hook->to))
		return EAGAIN;

	if (unlikely(alsa_hook->started))
		return EALREADY;

	/* initialize all pending streams */
	while (stream != NULL) {
		if ((stream->fmt) && (!stream->initialized))
			alsa_hook_stream_init(alsa_hook, stream);
		stream = stream->next;
	}

	alsa_hook->started = 1;
	return 0;
}

int alsa_hook_stream_wait(struct alsa_hook_stream_s *stream)
{
	if (stream->thread.running) {
		stream->thread.running = 0;

		/* tell thread to quit */
		sem_post(&stream->capture_full);
		pthread_join(stream->thread.thread, NULL);
	}
	return 0;
}

int alsa_hook_destroy(alsa_hook_t alsa_hook)
{
	struct alsa_hook_stream_s *del;

	if (unlikely(alsa_hook == NULL))
		return EINVAL;

	while (alsa_hook->stream != NULL) {
		del = alsa_hook->stream;
		alsa_hook->stream = alsa_hook->stream->next;

		alsa_hook_stream_wait(del);

		sem_destroy(&del->capture_full);
		sem_destroy(&del->capture_empty);

		pthread_mutex_destroy(&del->write_mutex);
		pthread_spin_destroy(&del->write_spinlock);

		if (del->capture_data)
			free(del->capture_data);
		if (del->initialized)
			ps_packet_destroy(&del->packet);
		free(del);
	}

	free(alsa_hook);
	return 0;
}

glc_audio_format_t pcm_fmt_to_glc_fmt(snd_pcm_format_t pcm_fmt)
{
	switch (pcm_fmt) {
	case SND_PCM_FORMAT_S16_LE:
		return GLC_AUDIO_S16_LE;
	case SND_PCM_FORMAT_S24_LE:
		return GLC_AUDIO_S24_LE;
	case SND_PCM_FORMAT_S32_LE:
		return GLC_AUDIO_S32_LE;
	default:
		return 0;
	}
}

int alsa_hook_get_stream(alsa_hook_t alsa_hook, snd_pcm_t *pcm, struct alsa_hook_stream_s **stream)
{
	struct alsa_hook_stream_s *find = alsa_hook->stream;

	while (find != NULL) {
		if (find->pcm == pcm)
			break;
		find = find->next;
	}

	if (find == NULL) {
		find = (struct alsa_hook_stream_s *) calloc(1, sizeof(struct alsa_hook_stream_s));
		find->pcm = pcm;

		find->id = 0; /* zero until it is initialized */

		sem_init(&find->capture_full, 0, 0);
		sem_init(&find->capture_empty, 0, 1);

		pthread_mutex_init(&find->write_mutex, NULL);
		pthread_spin_init(&find->write_spinlock, 0);

		find->alsa_hook     = alsa_hook;
		find->thread.ask_rt = 1;
		find->next          = alsa_hook->stream;
		alsa_hook->stream = find;
	}

	*stream = find;
	return 0;
}

/*
 * The purpose of this thread is to make this module async signal safe.
 * ie: it couldn't be called safely from a sighandler if host process
 * use ALSA async mode and write to ALSA API from a sighandler.
 */
void *alsa_hook_thread(void *argptr)
{
	struct alsa_hook_stream_s *stream = (struct alsa_hook_stream_s *) argptr;
	glc_audio_data_header_t hdr;
	glc_message_header_t msg_hdr;
	int ret = 0;

	msg_hdr.type = GLC_MESSAGE_AUDIO_DATA;
	hdr.id = stream->id;

	stream->capture_ready = 1;
	while (1) {
		sem_wait(&stream->capture_full);
		stream->capture_ready = 0;

		if (unlikely(!stream->thread.running))
			break;

		if (unlikely(stream->capture_size < 0)) {
			alsa_hook_realloc_capture_buf(stream,-stream->capture_size);
			goto capture_ready;
		}

		hdr.time = stream->capture_time;
		hdr.size = stream->capture_size;

		if (unlikely((ret = ps_packet_open(&stream->packet, PS_PACKET_WRITE))))
			break;
		if (unlikely((ret = ps_packet_setsize(&stream->packet, hdr.size
					+ sizeof(glc_message_header_t)
					+ sizeof(glc_audio_data_header_t)))))
			break;
		if (unlikely((ret = ps_packet_write(&stream->packet, &msg_hdr,
					sizeof(glc_message_header_t)))))
			break;
		if (unlikely((ret = ps_packet_write(&stream->packet, &hdr,
					sizeof(glc_audio_data_header_t)))))
			break;
		if (unlikely((ret = ps_packet_write(&stream->packet,
					stream->capture_data, hdr.size))))
			break;
		if (unlikely((ret = ps_packet_close(&stream->packet))))
			break;

		if (!(stream->mode & SND_PCM_ASYNC))
			sem_post(&stream->capture_empty);
capture_ready:
		stream->capture_ready = 1;
	}

	if (ret != 0)
		glc_log(stream->alsa_hook->glc, GLC_ERROR, "alsa_hook",
			"thread failed: %s (%d)", strerror(ret), ret);

	return NULL;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_wait_for_thread(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream)
{
	if (stream->mode & SND_PCM_ASYNC) {
		/**
		* \note this is ugly, but snd_pcm_...() functions can be called from
		*       signal handler (f.ex. async mode)
		*/
		while (!stream->capture_ready) {
			if (alsa_hook->flags & ALSA_HOOK_ALLOW_SKIP)
				goto busy;
			sched_yield();
		}
	} else
		sem_wait(&stream->capture_empty);

	return 0;
busy:
	return EBUSY;
}

int alsa_hook_lock_write(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream)
{
	int ret = 0;
	if (stream->mode & SND_PCM_ASYNC)
		ret = pthread_spin_lock(&stream->write_spinlock);
	else
		ret = pthread_mutex_lock(&stream->write_mutex);
	return ret;
}

int alsa_hook_unlock_write(alsa_hook_t alsa_hook, struct alsa_hook_stream_s *stream)
{
	int ret = 0;
	if (stream->mode & SND_PCM_ASYNC)
		ret = pthread_spin_unlock(&stream->write_spinlock);
	else
		ret = pthread_mutex_unlock(&stream->write_mutex);
	return ret;
}

int alsa_hook_realloc_capture_buf(struct alsa_hook_stream_s *stream, ssize_t size)
{
	char *op = stream->capture_data;
	int ret = 0;
	stream->capture_data_size = size;
	stream->capture_data = (char *) realloc(stream->capture_data,
						stream->capture_data_size);

	if (unlikely(!stream->capture_data)) {
		glc_log(stream->alsa_hook->glc, GLC_ERROR, "alsa_hook",
			"realloc() error");
		free(op);
		stream->capture_data_size = 0;
		ret = ENOMEM;
	}
	return ret;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_set_data_size(struct alsa_hook_stream_s *stream, ssize_t size)
{
	int ret = 0;
	stream->capture_size = size;
	if (size <= stream->capture_data_size)
		return 0;

	if (!(stream->mode & SND_PCM_ASYNC)) {
		ret = alsa_hook_realloc_capture_buf(stream,size);
	} else {
		/*
		 * realloc is not an async-signal-safe function.
		 * we ask the thread to enlarge the buffer for the next time
		 * on our behalf.
		 */
		stream->capture_size = -size;
		ret = EBUSY;
		sem_post(&stream->capture_full);
	}

	return ret;
}

int alsa_hook_open(alsa_hook_t alsa_hook, snd_pcm_t *pcm, const char *name,
			 snd_pcm_stream_t pcm_stream, int mode)
{
	struct alsa_hook_stream_s *stream;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);

	stream->mode = mode;

	glc_log(alsa_hook->glc, GLC_INFO, "alsa_hook",
		 "%p: opened device \"%s\" with mode is 0x%02x (async=%s, nonblock=%s)",
		 stream->pcm, name, mode,
		 mode & SND_PCM_ASYNC ? "yes" : "no",
		 mode & SND_PCM_NONBLOCK ? "yes" : "no");

	return 0;
}

int alsa_hook_close(alsa_hook_t alsa_hook, snd_pcm_t *pcm)
{
	struct alsa_hook_stream_s *stream;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);
	glc_log(alsa_hook->glc, GLC_INFO, "alsa_hook", "%p: closing stream %d",
		 pcm, stream->id);
	stream->fmt = 0; /* no format -> do not initialize */

	return 0;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_writei(alsa_hook_t alsa_hook, snd_pcm_t *pcm,
		     const void *buffer, snd_pcm_uframes_t size)
{
	struct alsa_hook_stream_s *stream;
	int ret = 0;
	int savedErrno = errno;

	if (!(alsa_hook->flags & ALSA_HOOK_CAPTURING))
		goto leave;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);

	if (unlikely(!stream->initialized)) {
		ret = EINVAL;
		goto leave;
	}

	if (unlikely((ret = alsa_hook_lock_write(alsa_hook, stream))))
		goto leave;

	if (unlikely((ret = alsa_hook_wait_for_thread(alsa_hook, stream))))
		goto unlock;

	if (unlikely((ret = alsa_hook_set_data_size(stream,
				snd_pcm_frames_to_bytes(pcm, size)))))
		goto unlock;

	stream->capture_time = glc_state_time(alsa_hook->glc);
	memcpy(stream->capture_data, buffer, stream->capture_size);
	sem_post(&stream->capture_full);

unlock:
	alsa_hook_unlock_write(alsa_hook, stream);
leave:
	errno = savedErrno;
	return ret;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_writen(alsa_hook_t alsa_hook, snd_pcm_t *pcm,
		     void **bufs, snd_pcm_uframes_t size)
{
	struct alsa_hook_stream_s *stream;
	int c, ret = 0;
	int savedErrno = errno;

	if (!(alsa_hook->flags & ALSA_HOOK_CAPTURING))
		goto leave;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);

	if (unlikely(!stream->initialized)) {
		ret = EINVAL;
		goto leave;
	}

	if (unlikely((ret = alsa_hook_lock_write(alsa_hook, stream))))
		goto leave;

	if (unlikely(stream->flags & GLC_AUDIO_INTERLEAVED)) {
		if (!(stream->mode & SND_PCM_ASYNC))
			glc_log(alsa_hook->glc, GLC_ERROR, "alsa_hook",
				 "stream format (interleaved) incompatible with snd_pcm_writen()");
		ret = EINVAL;
		goto unlock;
	}

	if (unlikely((ret = alsa_hook_wait_for_thread(alsa_hook, stream))))
		goto unlock;

	if (unlikely((ret = alsa_hook_set_data_size(stream,
				snd_pcm_frames_to_bytes(pcm, size)))))
		goto unlock;

	stream->capture_time = glc_state_time(alsa_hook->glc);
	for (c = 0; c < stream->channels; c++)
		memcpy(&stream->capture_data[c * snd_pcm_samples_to_bytes(pcm, size)], bufs[c],
		       snd_pcm_samples_to_bytes(pcm, size));

	sem_post(&stream->capture_full);

unlock:
	alsa_hook_unlock_write(alsa_hook, stream);
leave:
	errno = savedErrno;
	return ret;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_mmap_begin(alsa_hook_t alsa_hook, snd_pcm_t *pcm,
			       const snd_pcm_channel_area_t *areas,
			       snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
	struct alsa_hook_stream_s *stream;
	int ret = 0;
	int savedErrno = errno;

	if (!(alsa_hook->flags & ALSA_HOOK_CAPTURING))
		goto leave;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);

	if (unlikely(!stream->initialized)) {
		ret = EINVAL;
		goto leave;
	}

	if (unlikely((ret = alsa_hook_lock_write(alsa_hook, stream))))
		goto leave;

	stream->mmap_areas = areas;
	stream->frames = frames;
	stream->offset = offset;

	alsa_hook_unlock_write(alsa_hook, stream);
leave:
	errno = savedErrno;
	return ret;
}

/*
 * Might be called from signal handlers.
 */
int alsa_hook_mmap_commit(alsa_hook_t alsa_hook, snd_pcm_t *pcm,
				snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
	struct alsa_hook_stream_s *stream;
	unsigned int c;
	int ret = 0;
	int savedErrno = errno;

	if (!(alsa_hook->flags & ALSA_HOOK_CAPTURING))
		goto leave;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);

	if (unlikely((ret = alsa_hook_lock_write(alsa_hook, stream))))
		goto leave;

	if (unlikely(stream->channels == 0))
		goto unlock; /* 0 channels :P */

	if (unlikely(!stream->mmap_areas)) {
		/* this might actually happen */
		if (!(stream->mode & SND_PCM_ASYNC))
			glc_log(alsa_hook->glc, GLC_WARN, "alsa_hook",
				 "snd_pcm_mmap_commit() before snd_pcm_mmap_begin()");
		goto unlock;
	}

	if (unlikely(offset != stream->offset))
		if (!(stream->mode & SND_PCM_ASYNC))
			glc_log(alsa_hook->glc, GLC_WARN, "alsa_hook",
				 "offset=%lu != stream->offset=%lu", offset, stream->offset);

	if (unlikely((ret = alsa_hook_wait_for_thread(alsa_hook, stream))))
		goto unlock;

	if (unlikely((ret = alsa_hook_set_data_size(stream,
			snd_pcm_frames_to_bytes(pcm, frames)))))
		goto unlock;

	stream->capture_time = glc_state_time(alsa_hook->glc);

	if (stream->flags & GLC_AUDIO_INTERLEAVED) {
		memcpy(stream->capture_data,
		       alsa_hook_mmap_pos(stream->mmap_areas, offset),
		       stream->capture_size);
	} else if (stream->complex) {
		alsa_hook_complex_to_interleaved(stream, stream->mmap_areas, offset,
		                                  frames, stream->capture_data);
	} else {
		for (c = 0; c < stream->channels; c++)
			memcpy(&stream->capture_data[c * snd_pcm_samples_to_bytes(stream->pcm, frames)],
			       alsa_hook_mmap_pos(&stream->mmap_areas[c], offset),
			       snd_pcm_samples_to_bytes(stream->pcm, frames));
	}

	sem_post(&stream->capture_full);

unlock:
	alsa_hook_unlock_write(alsa_hook, stream);
leave:
	errno = savedErrno;
	return ret;
}

void *alsa_hook_mmap_pos(const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset)
{
	/** \todo FIX: first or step not divisible by 8 */
	void *addr = &((unsigned char *) area->addr)[area->first / 8];
	addr = &((unsigned char *) addr)[offset * (area->step / 8)];
	return addr;
}

int alsa_hook_complex_to_interleaved(struct alsa_hook_stream_s *stream, const snd_pcm_channel_area_t *areas,
				snd_pcm_uframes_t offset, snd_pcm_uframes_t frames, char *to)
{
	/** \todo test this... :D */
	/** \note this is quite expensive operation */
	unsigned int c;
	size_t s, off, add, ssize;

	add = snd_pcm_frames_to_bytes(stream->pcm, 1);
	ssize = snd_pcm_samples_to_bytes(stream->pcm, 1);

	for (c = 0; c < stream->channels; c++) {
		off = add * c;
		for (s = 0; s < frames; s++) {
			memcpy(&to[off], alsa_hook_mmap_pos(&areas[c], offset + s), ssize);
			off += add;
		}
	}

	return 0;
}

int alsa_hook_hw_params(alsa_hook_t alsa_hook, snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	struct alsa_hook_stream_s *stream;

	snd_pcm_format_t format;
	snd_pcm_uframes_t period_size;
	snd_pcm_access_t access;
	int dir, ret;

	alsa_hook_get_stream(alsa_hook, pcm, &stream);
	if (unlikely((ret = alsa_hook_lock_write(alsa_hook, stream))))
		return ret;

	glc_log(alsa_hook->glc, GLC_DEBUG, "alsa_hook",
		 "%p: creating/updating configuration for stream %d",
		 stream->pcm, stream->id);

	/* extract information */
	if (unlikely((ret = snd_pcm_hw_params_get_format(params, &format)) < 0))
		goto err;
	stream->flags = 0; /* zero flags */
	stream->format = pcm_fmt_to_glc_fmt(format);
	if (unlikely(!stream->format)) {
		glc_log(alsa_hook->glc, GLC_ERROR, "alsa_hook",
			"%p: unsupported audio format 0x%02x", stream->pcm, format);
		ret = ENOTSUP;
		goto err;
	}
	if (unlikely((ret = snd_pcm_hw_params_get_rate(params, &stream->rate, &dir)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_get_channels(params, &stream->channels)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_get_period_size(params, &period_size, NULL)) < 0))
		goto err;
	if (unlikely((ret = snd_pcm_hw_params_get_access(params, &access)) < 0))
		goto err;
	if ((access == SND_PCM_ACCESS_RW_INTERLEAVED) ||
	    (access == SND_PCM_ACCESS_MMAP_INTERLEAVED))
		stream->flags |= GLC_AUDIO_INTERLEAVED;
	else if (access == SND_PCM_ACCESS_MMAP_COMPLEX) {
		stream->flags |= GLC_AUDIO_INTERLEAVED; /* convert to interleaved */
		stream->complex = 1; /* do conversion */
	} else {
		glc_log(alsa_hook->glc, GLC_ERROR, "alsa_hook",
			 "%p: unsupported access mode 0x%02x", stream->pcm, access);
		ret = ENOTSUP;
		goto err;
	}

	glc_log(alsa_hook->glc, GLC_DEBUG, "alsa_hook",
		 "%p: %d channels, rate %d, flags 0x%02x",
		 stream->pcm, stream->channels, stream->rate, stream->flags);

	stream->fmt = 1;
	if (alsa_hook->started) {
		if (unlikely((ret = alsa_hook_stream_init(alsa_hook, stream))))
			goto err;
	}

	alsa_hook_unlock_write(alsa_hook, stream);
	return 0;

err:
	glc_log(alsa_hook->glc, GLC_ERROR, "alsa_hook",
		 "%p: can't extract hardware configuration: %s (%d)",
		 stream->pcm, snd_strerror(ret), ret);

	alsa_hook_unlock_write(alsa_hook, stream);
	return ret;
}

int alsa_hook_stream_init(alsa_hook_t alsa_hook,
			struct alsa_hook_stream_s *stream)
{
	int ret;
	glc_message_header_t msg_hdr;
	glc_audio_format_message_t fmt_msg;

	if (unlikely(!stream->fmt))
		return EINVAL;

	/* we need proper id for the stream */
	if (stream->id < 1)
		glc_state_audio_new(alsa_hook->glc, &stream->id,
					&stream->state_audio);

	glc_log(alsa_hook->glc, GLC_INFO, "alsa_hook",
		 "%p: initializing stream %d", stream->pcm, stream->id);

	/* init packet */
	if (stream->initialized)
		ps_packet_destroy(&stream->packet);
	ps_packet_init(&stream->packet, alsa_hook->to);

	/* prepare audio format message */
	msg_hdr.type = GLC_MESSAGE_AUDIO_FORMAT;
	fmt_msg.id = stream->id;
	fmt_msg.flags = stream->flags;
	fmt_msg.rate = stream->rate;
	fmt_msg.channels = stream->channels;
	fmt_msg.format = stream->format;
	ps_packet_open(&stream->packet, PS_PACKET_WRITE);
	ps_packet_write(&stream->packet, &msg_hdr,
			sizeof(glc_message_header_t));
	ps_packet_write(&stream->packet, &fmt_msg,
			sizeof(glc_audio_format_message_t));
	ps_packet_close(&stream->packet);

	alsa_hook_stream_wait(stream);

	ret = glc_simple_thread_create(alsa_hook->glc, &stream->thread,
				alsa_hook_thread, stream);

	stream->initialized = 1;
	return ret;
}

/**  \} */
