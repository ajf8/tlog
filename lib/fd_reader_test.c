/*
 * Tlog tlog_fd_reader test.
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
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "tlog/fd_reader.h"
#include "tlog/misc.h"
#include "test.h"

enum op_type {
    OP_TYPE_NONE,
    OP_TYPE_READ,
    OP_TYPE_LOC_GET,
    OP_TYPE_NUM
};

static const char*
op_type_to_str(enum op_type t)
{
    switch (t) {
        case OP_TYPE_NONE:
            return "none";
        case OP_TYPE_READ:
            return "read";
        case OP_TYPE_LOC_GET:
            return "loc_get";
        default:
            return "<unkown>";
    }
}

struct op_data_loc_get {
    size_t exp_loc;
};

struct op_data_read {
    int         exp_rc;
    char       *exp_string;
};

struct op {
    enum op_type type;
    union {
        struct op_data_loc_get  loc_get;
        struct op_data_read     read;
    } data;
};

struct test {
    const char     *input;
    struct op       op_list[16];
};

static bool
test(const char *n, const struct test t)
{
    bool passed = true;
    int fd = -1;
    int rc;
    const struct tlog_reader_type *reader_type = &tlog_fd_reader_type;
    struct tlog_reader *reader = NULL;
    char filename[] = "tlog_fd_reader_test.XXXXXX";
    const struct op *op;
    struct json_object *object = NULL;
    size_t input_len = strlen(t.input);
    size_t exp_string_len;
    size_t res_string_len;
    const char *res_string;
    size_t loc;

    fd = mkstemp(filename);
    if (fd < 0) {
        fprintf(stderr, "Failed opening a temporary file: %s\n",
                strerror(errno));
        exit(1);
    }
    if (unlink(filename) < 0) {
        fprintf(stderr, "Failed unlinking the temporary file: %s\n",
                strerror(errno));
        exit(1);
    }
    if (write(fd, t.input, input_len) != (ssize_t)input_len) {
        fprintf(stderr, "Failed writing the temporary file: %s\n",
                strerror(errno));
        exit(1);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "Failed rewinding the temporary file: %s\n",
                strerror(errno));
        exit(1);
    }
    rc = tlog_reader_create(&reader, reader_type, fd);
    if (rc != 0) {
        fprintf(stderr, "Failed creating FD reader: %s\n",
                tlog_reader_type_strerror(reader_type, rc));
        exit(1);
    }

#define FAIL(_fmt, _args...) \
    do {                                                \
        fprintf(stderr, "%s: " _fmt "\n", n, ##_args);  \
        passed = false;                                 \
    } while (0)

#define FAIL_OP(_fmt, _args...) \
    FAIL("op #%zd (%s): " _fmt,                                 \
         op - t.op_list + 1, op_type_to_str(op->type), ##_args)

    for (op = t.op_list; op->type != OP_TYPE_NONE; op++) {
        switch (op->type) {
            case OP_TYPE_READ:
                rc = tlog_reader_read(reader, &object);
                if (rc != op->data.read.exp_rc) {
                    const char *res_str;
                    const char *exp_str;
                    res_str = tlog_reader_strerror(reader, rc);
                    exp_str = tlog_reader_strerror(reader,
                                                   op->data.read.exp_rc);
                    FAIL_OP("rc: %s (%d) != %s (%d)",
                            res_str, rc,
                            exp_str, op->data.read.exp_rc);
                }
                if ((object == NULL) != (op->data.read.exp_string == NULL))
                    FAIL_OP("object: %s != %s",
                            (object ? "!NULL" : "NULL"),
                            (op->data.read.exp_string ? "!NULL" : "NULL"));
                else if (object != NULL) {
                    res_string = json_object_to_json_string(object);
                    res_string_len = strlen(res_string);
                    exp_string_len = strlen(op->data.read.exp_string);
                    if (res_string_len != exp_string_len ||
                        memcmp(res_string, op->data.read.exp_string,
                               res_string_len) != 0) {
                        FAIL_OP("object mismatch:");
                        tlog_test_diff(
                                stderr,
                                (const uint8_t *)res_string,
                                res_string_len,
                                (const uint8_t *)op->data.read.exp_string,
                                exp_string_len);
                    }
                }
                if (object != NULL)
                    json_object_put(object);
                break;
            case OP_TYPE_LOC_GET:
                loc = tlog_reader_loc_get(reader);
                if (loc != op->data.loc_get.exp_loc) {
                    char *res_str;
                    char *exp_str;
                    res_str = tlog_reader_loc_fmt(reader, loc);
                    exp_str = tlog_reader_loc_fmt(reader,
                                                  op->data.loc_get.exp_loc);
                    FAIL_OP("loc: %s (%zu) != %s (%zu)",
                            res_str, loc,
                            exp_str, op->data.loc_get.exp_loc);
                }
                break;
            default:
                fprintf(stderr, "Unknown operation type: %d\n", op->type);
                exit(1);
        }
    }

#undef FAIL_OP
#undef FAIL

    fprintf(stderr, "%s: %s\n", n, (passed ? "PASS" : "FAIL"));

    tlog_reader_destroy(reader);
    if (fd >= 0)
        close(fd);
    return passed;
}

int
main(void)
{
    bool passed = true;

#define OP_NONE {.type = OP_TYPE_NONE}

#define OP_READ(_exp_rc, _exp_string) \
    {.type = OP_TYPE_READ,                                              \
     .data = {.read = {.exp_rc = _exp_rc, .exp_string = _exp_string}}}

#define OP_LOC_GET(_exp_loc) \
    {.type = OP_TYPE_LOC_GET,                       \
     .data = {.loc_get = {.exp_loc = _exp_loc}}}

#define TEST(_name_token, _input, _op_list_init_args...) \
    passed = test(#_name_token,                                 \
                  (struct test){                                \
                    .input = _input,                            \
                    .op_list = {_op_list_init_args, OP_NONE}    \
                  }                                             \
                 ) && passed


    TEST(null,
         "",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(null_repeat_eof,
         "",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(single_space,
         " ",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(single_space_repeat_eof,
         " ",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(two_spaces,
         " ",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(empty_line,
         "\n",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(2));

    TEST(single_space_line,
         " \n",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(2));

    TEST(two_single_space_lines,
         " \n \n",
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(3));

    TEST(empty_object,
         "{}",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(empty_object_repeat_eof,
         "{}",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(empty_object_space_pad_before,
         " {}",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(1));

    TEST(empty_object_space_pad_after,
         "{} ",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(1));

    TEST(empty_object_space_pad_both,
         " {} ",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(1));

    TEST(empty_object_newline_pad_before,
         "\n{}",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2));

    TEST(empty_object_newline_pad_after,
         "{}\n",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2));

    TEST(empty_object_newline_pad_both,
         "\n{}\n",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(3));

    TEST(two_empty_objects_hanging,
         "{}\n{}",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2),
         OP_READ(0, NULL),
         OP_LOC_GET(2));

    TEST(two_empty_objects_complete,
         "{}\n{}\n",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2),
         OP_READ(0, "{ }"),
         OP_LOC_GET(3),
         OP_READ(0, NULL),
         OP_LOC_GET(3));

    TEST(two_empty_objects_apart,
         "{}\n  \n{}\n",
         OP_LOC_GET(1),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2),
         OP_READ(0, "{ }"),
         OP_LOC_GET(4),
         OP_READ(0, NULL),
         OP_LOC_GET(4));

    TEST(one_deep_object,
         "{\"x\": 1}",
         OP_LOC_GET(1),
         OP_READ(0, "{ \"x\": 1 }"),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    TEST(two_deep_object,
         "[{\"x\": 1}]",
         OP_LOC_GET(1),
         OP_READ(json_tokener_error_depth, NULL),
         OP_LOC_GET(1));

    TEST(object_after_err,
         "[{\"x\": 1}]\n{}",
         OP_LOC_GET(1),
         OP_READ(json_tokener_error_depth, NULL),
         OP_LOC_GET(2),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2));

    TEST(eof_after_err,
         "[{\"x\": 1}]\n{}",
         OP_LOC_GET(1),
         OP_READ(json_tokener_error_depth, NULL),
         OP_LOC_GET(2),
         OP_READ(0, "{ }"),
         OP_LOC_GET(2),
         OP_READ(0, NULL),
         OP_LOC_GET(2));

    TEST(premature_eof,
         "{\"x\": 1",
         OP_LOC_GET(1),
         OP_READ(TLOG_FD_READER_ERROR_INCOMPLETE_LINE, NULL),
         OP_LOC_GET(1));

    TEST(premature_newline,
         "{\"x\": 1\n",
         OP_LOC_GET(1),
         OP_READ(TLOG_FD_READER_ERROR_INCOMPLETE_LINE, NULL),
         OP_LOC_GET(2));

    TEST(multiproperty_object,
         "{\"abc\": 123, \"def\": 456, \"ghi\": 789, "
         "\"bool\": true, \"string\": \"wool\"}",
         OP_LOC_GET(1),
         OP_READ(0, 
                 "{ \"abc\": 123, \"def\": 456, \"ghi\": 789, "
                 "\"bool\": true, \"string\": \"wool\" }"),
         OP_LOC_GET(1),
         OP_READ(0, NULL),
         OP_LOC_GET(1));

    return !passed;
}
