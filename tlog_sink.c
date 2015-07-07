/*
 * Tlog sink.
 *
 * Copyright (C) 2015 Red Hat
 *
 * This file is part of tlog.
 *
 * Tlog is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Tlog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with tlog; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <syslog.h>
#include "tlog_sink.h"
#include "tlog_misc.h"

void
tlog_sink_cleanup(struct tlog_sink *sink)
{
    assert(sink != NULL);
    tlog_io_cleanup(&sink->io);
    free(sink->message_buf);
    sink->message_buf = NULL;
    free(sink->username);
    sink->username = NULL;
    free(sink->hostname);
    sink->hostname = NULL;
}

tlog_rc
tlog_sink_init(struct tlog_sink *sink,
               struct tlog_writer *writer,
               const char *hostname,
               const char *username,
               unsigned int session_id,
               size_t io_size,
               const struct timespec *timestamp)
{
    int orig_errno;

    assert(sink != NULL);
    assert(tlog_writer_is_valid(writer));
    assert(hostname != NULL);
    assert(io_size >= TLOG_IO_SIZE_MIN);
    assert(timestamp != NULL);

    memset(sink, 0, sizeof(*sink));

    sink->writer = writer;

    sink->hostname = strdup(hostname);
    if (sink->hostname == NULL)
        goto error;

    sink->username = strdup(username);
    if (sink->username == NULL)
        goto error;

    sink->session_id = session_id;

    sink->message_id = 0;

    sink->start = *timestamp;

    /* NOTE: approximate size */
    sink->message_len = io_size + 1024;
    sink->message_buf = malloc(sink->message_len);
    if (sink->message_buf == NULL)
        goto error;

    if (tlog_io_init(&sink->io, io_size) != TLOG_RC_OK)
        goto error;

    return TLOG_RC_OK;

error:
    orig_errno = errno;
    tlog_sink_cleanup(sink);
    errno = orig_errno;
    return TLOG_RC_FAILURE;
}

tlog_rc
tlog_sink_create(struct tlog_sink **psink,
                 struct tlog_writer *writer,
                 const char *hostname,
                 const char *username,
                 unsigned int session_id,
                 size_t io_size,
                 const struct timespec *timestamp)
{
    struct tlog_sink *sink;

    assert(tlog_writer_is_valid(writer));
    assert(hostname != NULL);
    assert(io_size >= TLOG_IO_SIZE_MIN);
    assert(timestamp != NULL);

    sink = malloc(sizeof(*sink));
    if (sink == NULL)
        return TLOG_RC_FAILURE;

    if (tlog_sink_init(sink, writer, hostname, username,
                       session_id, io_size, timestamp) != TLOG_RC_OK) {
        free(sink);
        return TLOG_RC_FAILURE;
    }

    if (psink == NULL)
        tlog_sink_destroy(sink);
    else
        *psink = sink;
    return TLOG_RC_OK;
};

void
tlog_sink_destroy(struct tlog_sink *sink)
{
    if (sink != NULL) {
        tlog_sink_cleanup(sink);
        free(sink);
    }
}

bool
tlog_sink_is_valid(const struct tlog_sink *sink)
{
    return sink != NULL &&
           tlog_writer_is_valid(sink->writer) &&
           sink->hostname != NULL &&
           sink->username != NULL &&
           sink->message_buf != NULL &&
           tlog_io_is_valid(&sink->io);
}

tlog_rc
tlog_sink_window_write(struct tlog_sink *sink,
                       const struct timespec *timestamp,
                       unsigned short int width,
                       unsigned short int height)
{
    struct timespec pos;
    int len;

    assert(tlog_sink_is_valid(sink));
    assert(timestamp != NULL);

    tlog_timespec_sub(timestamp, &sink->start, &pos);

    tlog_sink_io_flush(sink);

    len = snprintf(
        (char *)sink->message_buf, sink->message_len,
        "{"
            "\"type\":"     "\"window\","
            "\"host\":"     "\"%s\","
            "\"user\":"     "\"%s\","
            "\"session\":"  "%u,"
            "\"id\":"       "%zu,"
            "\"pos\":"      "%ld.%03ld,"
            "\"width\":"    "%hu,"
            "\"height\":"   "%hu"
        "}\n",
        sink->hostname,
        sink->username,
        sink->session_id,
        sink->message_id,
        pos.tv_sec, pos.tv_nsec / 1000000,
        width,
        height);
    if (len < 0)
        return TLOG_RC_FAILURE;
    if ((size_t)len >= sink->message_len) {
        errno = ENOMEM;
        return TLOG_RC_FAILURE;
    }

    sink->message_id++;

    return tlog_writer_write(sink->writer, sink->message_buf, len);
}

tlog_rc
tlog_sink_io_write(struct tlog_sink *sink,
                   const struct timespec *timestamp,
                   bool output, const uint8_t *buf, size_t len)
{
    assert(tlog_sink_is_valid(sink));
    assert(timestamp != NULL);
    assert(buf != NULL || len == 0);

    while (true) {
        tlog_io_write(&sink->io, timestamp, output, &buf, &len);
        if (len == 0) {
            break;
        } else {
            if (tlog_sink_io_flush(sink) != TLOG_RC_OK)
                return TLOG_RC_FAILURE;
        }
    }
    return TLOG_RC_OK;
}

tlog_rc
tlog_sink_io_cut(struct tlog_sink *sink)
{
    assert(tlog_sink_is_valid(sink));
    while (!tlog_io_cut(&sink->io)) {
        if (tlog_sink_io_flush(sink) != TLOG_RC_OK)
            return TLOG_RC_FAILURE;
    }
    return TLOG_RC_OK;
}

tlog_rc
tlog_sink_io_flush(struct tlog_sink *sink)
{
    int len;
    struct timespec pos;

    assert(tlog_sink_is_valid(sink));

    if (tlog_io_is_empty(&sink->io))
        return TLOG_RC_OK;

    /* Write terminating metadata records to reserved space */
    tlog_io_flush(&sink->io);

    tlog_timespec_sub(&sink->io.first, &sink->start, &pos);
    len = snprintf(
        (char *)sink->message_buf, sink->message_len,
        "{"
            "\"type\":"     "\"io\","
            "\"host\":"     "\"%s\","
            "\"user\":"     "\"%s\","
            "\"session\":"  "%u,"
            "\"id\":"       "%zu,"
            "\"pos\":"      "%ld.%03ld,"
            "\"timing\":"   "\"%.*s\","
            "\"in_txt\":"   "\"%.*s\","
            "\"in_bin\":"   "[%.*s],"
            "\"out_txt\":"  "\"%.*s\","
            "\"out_bin\":"  "[%.*s]"
        "}\n",
        sink->hostname,
        sink->username,
        sink->session_id,
        sink->message_id,
        pos.tv_sec, pos.tv_nsec / 1000000,
        (int)(sink->io.timing_ptr - sink->io.timing_buf), sink->io.timing_buf,
        (int)sink->io.input.txt_len, sink->io.input.txt_buf,
        (int)sink->io.input.bin_len, sink->io.input.bin_buf,
        (int)sink->io.output.txt_len, sink->io.output.txt_buf,
        (int)sink->io.output.bin_len, sink->io.output.bin_buf);
    if (len < 0)
        return TLOG_RC_FAILURE;
    if ((size_t)len >= sink->message_len) {
        errno = ENOMEM;
        return TLOG_RC_FAILURE;
    }

    if (tlog_writer_write(sink->writer, sink->message_buf, len) !=
            TLOG_RC_OK)
        return TLOG_RC_FAILURE;

    sink->message_id++;
    tlog_io_empty(&sink->io);

    return TLOG_RC_OK;
}
