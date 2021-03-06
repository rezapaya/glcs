/**
 * \file glc/common/util.c
 * \brief utility functions adapted from original work (glc) from Pyry Haulos <pyry.haulos@gmail.com>
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
 * \addtogroup util
 *  \{
 */
#include <stdlib.h> // for atoi()
#include <stdio.h>
#include <stddef.h> // for offsetof()
#include <alloca.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>

#include "glc.h"
#include "core.h"
#include "log.h"
#include "util.h"
#include "optimization.h"

/**
 * \brief util private structure
 */
struct glc_util_s {
	double fps;
	int pid;
};

/**
 * \brief acquire application name
 *
 * Currently this function resolves /proc/self/exe.
 * \param glc glc
 * \param path returned application name
 * \param path_size size of name string, including 0
 * \return 0 on success otherwise an error code
 */
static int glc_util_app_name(glc_t *glc, char **path, u_int32_t *path_size);

/**
 * \brief acquire current date as UTC string
 * \param glc glc
 * \param date returned date
 * \param date_size size of date string, including 0
 * \return 0 on success otherwise an error code
 */
static int glc_util_utc_date(glc_t *glc, char *date, u_int32_t *date_size);

int glc_util_init(glc_t *glc)
{
	glc->util = (glc_util_t) calloc(1, sizeof(struct glc_util_s));

	glc->util->fps = 30.0;
	glc->util->pid = getpid();

	return 0;
}

int glc_util_destroy(glc_t *glc)
{
	free(glc->util);
	return 0;
}

int glc_util_info_fps(glc_t *glc, double fps)
{
	glc->util->fps = fps;
	return 0;
}

int glc_util_info_create(glc_t *glc, glc_stream_info_t **stream_info,
			 char **info_name, char *info_date)
{
	*stream_info = (glc_stream_info_t *) calloc(1, sizeof(glc_stream_info_t));

	(*stream_info)->signature = GLC_SIGNATURE;
	(*stream_info)->version = GLC_STREAM_VERSION;
	(*stream_info)->flags = 0;
	(*stream_info)->pid = glc->util->pid;
	(*stream_info)->fps = glc->util->fps;

	glc_util_app_name(glc, info_name, &(*stream_info)->name_size);
	glc_util_utc_date(glc, info_date, &(*stream_info)->date_size);

	return 0;
}

int glc_util_app_name(glc_t *glc, char **path, u_int32_t *path_size)
{
	*path = (char *) malloc(1024);
	ssize_t len;

	if ((len = readlink("/proc/self/exe", *path, 1023)) != -1) {
		(*path)[len] = '\0';
		*path_size = len;
	} else {
		*path_size = 0;
		(*path)[0] = '\0';
	}

	(*path_size)++;

	return 0;
}

/*
 * date buffer must have space for at least 26 chars.
 */
int glc_util_utc_date(glc_t *glc, char *date, u_int32_t *date_size)
{
	time_t t = time(NULL);
	ctime_r(&t,date);
	/* trim trailing line feed */
	date[24] = '\0';
	*date_size = 25;
	return 0;
}

int glc_util_write_end_of_stream(glc_t *glc, ps_buffer_t *to)
{
	int ret = 0;
	ps_packet_t packet;
	glc_message_header_t header;
	header.type = GLC_MESSAGE_CLOSE;

	if (unlikely((ret = ps_packet_init(&packet, to))))
		goto finish;
	if (unlikely((ret = ps_packet_open(&packet, PS_PACKET_WRITE))))
		goto finish;
	if (unlikely((ret = ps_packet_write(&packet,
			&header, sizeof(glc_message_header_t)))))
		goto finish;
	if (unlikely((ret = ps_packet_close(&packet))))
		goto finish;
	if (unlikely((ret = ps_packet_destroy(&packet))))
		goto finish;

finish:
	return ret;
}

int glc_util_log_info(glc_t *glc)
{
	char *name;
	char date[26];
	u_int32_t unused;
	glc_util_app_name(glc, &name, &unused);
	glc_util_utc_date(glc,  date, &unused);

	glc_log(glc, GLC_INFO, "util", "system information\n" \
		"  threads hint = %ld", glc_threads_hint(glc));

	glc_log(glc, GLC_INFO, "util", "stream information\n" \
		"  signature    = 0x%08x\n" \
		"  version      = 0x%02x\n" \
		"  flags        = %d\n" \
		"  fps          = %f\n" \
		"  pid          = %d\n" \
		"  name         = %s\n" \
		"  date         = %s",
		GLC_SIGNATURE, GLC_STREAM_VERSION, 0, glc->util->fps,
		glc->util->pid, name, date);

	free(name);
	return 0;
}

int glc_util_log_version(glc_t *glc)
{
	glc_log(glc, GLC_INFO, "util",
		"version %s", GLC_VERSION);
	glc_log(glc, GLC_DEBUG, "util",
		"%s %s, %s", __DATE__, __TIME__, __VERSION__);
	return 0;
}

char *glc_util_str_replace(const char *str, const char *find, const char *replace)
{
	/* calculate destination string size */
	size_t replace_len = strlen(replace);
	size_t find_len = strlen(find);
	ssize_t copy, add_per_replace = (ssize_t) replace_len - (ssize_t) find_len;
	ssize_t size = strlen(str) + 1;
	const char* p = str;
	while ((p = strstr(p, find)) != NULL) {
		size += add_per_replace;
		p = &p[find_len];
	}

	if (size < 0)
		return NULL;

	char *result = (char *) malloc(size * sizeof(char));
	p = str;
	const char *s = str;
	char *r = result;

	while ((p = strstr(p, find)) != NULL) {
		copy = (size_t) p - (size_t) s; /* naughty casts */

		/* copy string before previous replace (or start) */
		if (copy > 0) {
			memcpy(r, s, copy * sizeof(char));
			r = &r[copy];
		}

		/* copy replace string */
		memcpy(r, replace, replace_len * sizeof(char));
		r = &r[replace_len];

		p = &p[find_len];
		s = p;
	}

	copy = (size_t) str + (size_t) strlen(str) - (size_t) s;
	if (copy > 0)
		memcpy(r, s, copy * sizeof(char));

	result[size-1] = '\0';

	return result;
}

char *glc_util_format_filename(const char *fmt, unsigned int capture)
{
	char tmp[256];
	size_t fmt_size = strlen(fmt) + 1;
	char *old, *filename = (char *) malloc(sizeof(char) * fmt_size);
	memcpy(filename, fmt, fmt_size);

	/* %app% */
	if (strstr(filename, "%app%") != NULL) {
		/** \todo nicer way to determine */
		char *path;
		u_int32_t path_size;
		glc_util_app_name(NULL, &path, &path_size);
		char *p, *app = path;
		if ((p = strrchr(app, '/')) != NULL) {
			p++;
			app = p;
		}

		old = filename;
		filename = glc_util_str_replace(old, "%app%", app);
		free(path);
		free(old);
	}

#define NUM_REPLACE(tag, fmt, val) \
	if (strstr(filename, tag) != NULL) { \
		snprintf(tmp, sizeof(tmp), fmt, (val)); \
		old = filename; \
		filename = glc_util_str_replace(old, tag, tmp); \
		free(old); \
	}

	/* time */
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	NUM_REPLACE("%pid%", "%d", getpid())
	NUM_REPLACE("%capture%", "%u", capture)
	NUM_REPLACE("%year%", "%04d", tm.tm_year + 1900)
	NUM_REPLACE("%month%", "%02d", tm.tm_mon + 1)
	NUM_REPLACE("%day%", "%02d", tm.tm_mday)
	NUM_REPLACE("%hour%", "%02d", tm.tm_hour)
	NUM_REPLACE("%min%", "%02d", tm.tm_min)
	NUM_REPLACE("%sec%", "%02d", tm.tm_sec)

	return filename;
}

int glc_util_setflag( int fd, int flag )
{
	int val;
	if (unlikely((val = fcntl(fd, F_GETFL, 0)) < 0))
		return val;
	val |= flag;
	val = fcntl(fd, F_SETFL, val);
	return val;
}

int glc_util_clearflag( int fd, int flag )
{
	int val;
	if (unlikely((val = fcntl(fd, F_GETFL, 0)) < 0))
		return val;
	val &= ~flag;
	val = fcntl(fd, F_SETFL, val);
	return val;
}

int glc_util_set_nonblocking(int fd)
{
	return glc_util_setflag(fd, O_NONBLOCK);
}

#define PMS_FILE "/proc/sys/fs/pipe-max-size"

int glc_util_set_pipe_size(glc_t *glc, int fd, int size)
{
	int pipe_max_size;
	int ret = 0;
	int cursize;

	if (unlikely(!size))
		return EINVAL;

	FILE *pms_file = fopen(PMS_FILE, "r");
	if (unlikely(!pms_file)) {
		glc_log(glc, GLC_ERROR, "util",
			"failed to open '%s", PMS_FILE);
		return 1;
	}
	if (unlikely(fscanf(pms_file,"%d", &pipe_max_size) != 1)) {
		glc_log(glc, GLC_ERROR, "util", "error reading '%s'",
			PMS_FILE);
		goto nocheck;
	}
	if (unlikely(size > pipe_max_size))
	{
		glc_log(glc, GLC_WARN, "util",
			"requested pipe buffer size of %d bytes "
			"exceeding system maximum value of %d."
			" Consider increase it by doing 'echo %d > %s'",
			size, pipe_max_size, size, PMS_FILE);
		size = pipe_max_size;
	}
nocheck:
	if (unlikely((cursize = fcntl(fd, F_GETPIPE_SZ)) < 0)) {
		glc_log(glc, GLC_WARN, "util",
			"error getting the pipe buffer current size: %s (%d)",
			strerror(errno), errno);
		cursize = 0;
	}
	glc_log(glc, GLC_DEBUG, "util", "pipe buffer size: current %d requested %d",
		cursize, size);
	if (size > cursize)
		if (unlikely((ret = fcntl(fd, F_SETPIPE_SZ, size)) < 0))
			glc_log(glc, GLC_ERROR, "util",
				"error setting the pipe buffer size: %s (%d)",
				strerror(errno), errno);
	fclose(pms_file);
	return ret;
}

void glc_util_empty_pipe(int fd)
{
	char buf[256];
	while (read(fd,buf,256) > 0);
}

const char *glc_util_msgtype_to_str(glc_message_type_t type)
{
	const char *res;
	switch(type)
	{
	case GLC_MESSAGE_CLOSE:
		res = "GLC_MESSAGE_CLOSE";
		break;
	case GLC_MESSAGE_VIDEO_FRAME:
		res = "GLC_MESSAGE_VIDEO_FRAME";
		break;
	case GLC_MESSAGE_VIDEO_FORMAT:
		res = "GLC_MESSAGE_VIDEO_FORMAT";
		break;
	case GLC_MESSAGE_LZO:
		res = "GLC_MESSAGE_LZO";
		break;
	case GLC_MESSAGE_AUDIO_FORMAT:
		res = "GLC_MESSAGE_AUDIO_FORMAT";
		break;
	case GLC_MESSAGE_AUDIO_DATA:
		res = "GLC_MESSAGE_AUDIO_DATA";
		break;
	case GLC_MESSAGE_QUICKLZ:
		res = "GLC_MESSAGE_QUICKLZ";
		break;
	case GLC_MESSAGE_COLOR:
		res = "GLC_MESSAGE_COLOR";
		break;
	case GLC_MESSAGE_CONTAINER:
		res = "GLC_MESSAGE_CONTAINER";
		break;
	case GLC_MESSAGE_LZJB:
		res = "GLC_MESSAGE_LZJB";
		break;
	case GLC_CALLBACK_REQUEST:
		res = "GLC_CALLBACK_REQUEST";
		break;
	default:
		res = "unknown";
		break;
	}
	return res;
}

const char *glc_util_videofmt_to_str(glc_video_format_t fmt)
{
	const char *res;
	switch(fmt)
	{
	case GLC_VIDEO_BGR:
		res = "bgr24";
		break;
	case GLC_VIDEO_BGRA:
		res = "bgra";
		break;
	case GLC_VIDEO_YCBCR_420JPEG:
		res = "YV12_420";
		break;
	case GLC_VIDEO_RGB:
		res = "rgb24";
		break;
	default:
		res = "unknown";
		break;
	}
	return res;
}

int glc_util_get_videofmt_bpp(glc_video_format_t fmt)
{
	int res;
	switch(fmt)
	{
	case GLC_VIDEO_BGR:
	case GLC_VIDEO_RGB:
		res = 3;
		break;
	case GLC_VIDEO_BGRA:
		res = 4;
		break;
	default:
		res = -1;
		break;
	}
	return res;
}

/*
 * Implementation based on discussion held at:
 *
 * http://stackoverflow.com/questions/1676522/delete-files-while-reading-directory-with-readdir/
 *
 * Note: Knowing that /dev/fd dir only contains files whose name can only be integers, we can
 *       take a shortcut to determine the name_max.
 */
void glc_util_close_fds(int start_fd)
{
	DIR *dirp     = opendir("/dev/fd");
	long name_max = 15;
	size_t len;
	struct dirent *entryp, *result;
	len = offsetof(struct dirent, d_name) + name_max + 1;
	entryp = (struct dirent *)alloca(len);
	while (!readdir_r(dirp, entryp, &result) && result) {
		int fd;
		if (unlikely(result->d_name[0] == '.'))
			continue;
		fd = atoi(result->d_name);
		if (fd < start_fd || fd == dirfd(dirp))
			continue;
		close(fd);
	}
	closedir(dirp);
}

/**  \} */

