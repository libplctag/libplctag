.\"                                      Hey, EMACS: -*- nroff -*-
.\" (C) Copyright 2019 Kyle Hayes <kyle.hayes@gmail.com>,
.\"
.\" First parameter, NAME, should be all caps
.\" Second parameter, SECTION, should be 1-8, maybe w/ subsection
.\" other parameters are allowed: see man(7), man(1)
.TH Libplctag SECTION "April 13 2019"
.\" Please adjust this date whenever revising the manpage.
.\"
.\" Some roff macros, for reference:
.\" .nh        disable hyphenation
.\" .hy        enable hyphenation
.\" .ad l      left justify
.\" .ad b      justify to both left and right margins
.\" .nf        disable filling
.\" .fi        enable filling
.\" .br        insert line break
.\" .sp <n>    insert n+1 empty lines
.\" for manpage-specific macros, see man(7)
.SH NAME
libplctag, plc_tag_create, plc_tag_lock, plc_tag_unlock, plc_tag_abort, plc_tag_destroy, plc_tag_read, plc_tag_status, plc_tag_write, plc_tag_get_size, plc_tag_get_uint64, plc_tag_set_uint64, plc_tag_get_int64, plc_tag_set_int64, plc_tag_get_uint32, plc_tag_set_uint32, plc_tag_get_int32, plc_tag_set_int32, plc_tag_get_uint16, plc_tag_set_uint16, plc_tag_get_int16, plc_tag_set_int16, plc_tag_get_uint8, plc_tag_set_uint8, plc_tag_get_int8, plc_tag_set_int8, plc_tag_get_float64, plc_tag_set_float64, plc_tag_get_float32, plc_tag_set_float32 \- a library to communicate with Allen-Bradley/Rockwell PLCs.
.SH SYNOPSIS
.B #include <libplctag.h>
.br
Control/creation routines:
.br
.B  int32_t plc_tag_create(const char *attrib_str, int timeout);
.br
.B  int plc_tag_lock(int32_t tag_id);
.br
.B  int plc_tag_unlock(int32_t tag_d);
.br
.B  int plc_tag_abort(int32_t tag_id);
.br
.B  int plc_tag_destroy(int32_t tag_id);
.br
.B  int plc_tag_read(int32_t tag_id, int timeout);
.br
.B  int plc_tag_status(int32_t tag_id);
.br
.B  int plc_tag_write(int32_t tag_id, int timeout);
.br
.B  int plc_tag_get_size(int32_t tag_id);
.PP
Data access routines:
.PP
.B  uint64_t plc_tag_get_uint64(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_uint64(int32_t tag_id, int offset, uint64_t val);
.br
.B  int64_t plc_tag_get_int64(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_int64(int32_t tag_id, int offset, int64_t val);
.br
.B  uint32_t plc_tag_get_uint32(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_uint32(int32_t tag_id, int offset, uint32_t val);
.br
.B  int32_t plc_tag_get_int32(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_int32(int32_t tag_id, int offset, int32_t val);
.br
.B  uint16_t plc_tag_get_uint16(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_uint16(int32_t tag_id, int offset, uint16_t val);
.br
.B  int16_t plc_tag_get_int16(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_int16(int32_t tag_id, int offset, int16_t val);
.br
.B  uint8_t plc_tag_get_uint8(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_uint8(int32_t tag_id, int offset, uint8_t val);
.br
.B  int8_t plc_tag_get_int8(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_int8(int32_t tag_id, int offset, int8_t val);
.br
.B  double plc_tag_get_float64(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_float64(int32_t tag_id, int offset, double val);
.br
.B  float plc_tag_get_float32(int32_t tag_id, int offset);
.br
.B  int plc_tag_set_float32(int32_t tag_id, int offset, float val);
.PP
Error decoding:
.PP
.B const char *plc_tag_decode_error(int err_code);
.SH DESCRIPTION
This manual page documents briefly the
.B libplctag
library.   This library allows C programs to communicate with Allen-Bradley/Rockwell PLCs including PLC/5, SLC 500, MicroLogix, ControlLogix, CompactLogix, and Micro800.
.PP
The above functions operate on all types of tags. API functions that take a timeout argument can be used in a synchronous manner or asynchronous manner. Providing a timeout will cause the library to do the work of waiting and checking for status. Generally it is easier to start out with the synchronous versions of the API and move to the asynchronous calls when you understand it better and need the performance.
.PP
.SH SEE ALSO
See the main site, https://github.com/kyle-github/libplctag, for examples and more documentation as well as wrappers for other languages.
