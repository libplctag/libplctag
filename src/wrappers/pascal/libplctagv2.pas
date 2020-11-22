unit libplctagv2;


{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, ctypes;

const
  {$IFDEF WINDOWS}
  LibPLCTagLibName            = 'plctag.dll';
  {$ELSE}
  LibPLCTagLibName            = 'plctag';
  {$ENDIF}


  // library internal status.
  const PLCTAG_STATUS_PENDING       = (1);
  const PLCTAG_STATUS_OK            = (0);
  const PLCTAG_ERR_ABORT            = (-1);
  const PLCTAG_ERR_BAD_CONFIG       = (-2);
  const PLCTAG_ERR_BAD_CONNECTION   = (-3);
  const PLCTAG_ERR_BAD_DATA         = (-4);
  const PLCTAG_ERR_BAD_DEVICE       = (-5);
  const PLCTAG_ERR_BAD_GATEWAY      = (-6);
  const PLCTAG_ERR_BAD_PARAM        = (-7);
  const PLCTAG_ERR_BAD_REPLY        = (-8);
  const PLCTAG_ERR_BAD_STATUS       = (-9);
  const PLCTAG_ERR_CLOSE            = (-10);
  const PLCTAG_ERR_CREATE           = (-11);
  const PLCTAG_ERR_DUPLICATE        = (-12);
  const PLCTAG_ERR_ENCODE           = (-13);
  const PLCTAG_ERR_MUTEX_DESTROY    = (-14);
  const PLCTAG_ERR_MUTEX_INIT       = (-15);
  const PLCTAG_ERR_MUTEX_LOCK       = (-16);
  const PLCTAG_ERR_MUTEX_UNLOCK     = (-17);
  const PLCTAG_ERR_NOT_ALLOWED      = (-18);
  const PLCTAG_ERR_NOT_FOUND        = (-19);
  const PLCTAG_ERR_NOT_IMPLEMENTED  = (-20);
  const PLCTAG_ERR_NO_DATA          = (-21);
  const PLCTAG_ERR_NO_MATCH         = (-22);
  const PLCTAG_ERR_NO_MEM           = (-23);
  const PLCTAG_ERR_NO_RESOURCES     = (-24);
  const PLCTAG_ERR_NULL_PTR         = (-25);
  const PLCTAG_ERR_OPEN             = (-26);
  const PLCTAG_ERR_OUT_OF_BOUNDS    = (-27);
  const PLCTAG_ERR_READ             = (-28);
  const PLCTAG_ERR_REMOTE_ERR       = (-29);
  const PLCTAG_ERR_THREAD_CREATE    = (-30);
  const PLCTAG_ERR_THREAD_JOIN      = (-31);
  const PLCTAG_ERR_TIMEOUT          = (-32);
  const PLCTAG_ERR_TOO_LARGE        = (-33);
  const PLCTAG_ERR_TOO_SMALL        = (-34);
  const PLCTAG_ERR_UNSUPPORTED      = (-35);
  const PLCTAG_ERR_WINSOCK          = (-36);
  const PLCTAG_ERR_WRITE            = (-37);
  const PLCTAG_ERR_PARTIAL          = (-38);
  const PLCTAG_ERR_BUSY             = (-39);




  {*
   * helper function for errors.
   *
   * This takes one of the above errors and turns it into a const char * suitable
   * for printing.
   *}

  //LIB_EXPORT const char *plc_tag_decode_error(int err);
  function plc_tag_decode_error(err:cint):PChar; cdecl; external LibPLCTagLibName;


  {*
   * tag functions
   *
   * The following is the public API for tag operations.
   *
   * These are implemented in a protocol-specific manner.
   *}

  {*
   * plc_tag_create
   *
   * Create a new tag based on the passed attributed string.  The attributes
   * are protocol-specific.  The only required part of the string is the key-
   * value pair "protocol=XXX" where XXX is one of the supported protocol
   * types.
   *
   * Wait for timeout milliseconds for the tag to finish the creation process.
   * If this is zero, return immediately.  The application program will need to
   * poll the tag status with plc_tag_status() while the status is PLCTAG_STATUS_PENDING
   * until the status changes to PLCTAG_STATUS_OK if the creation was successful or
   * another PLCTAG_ERR_xyz if it was not.
   *
   * An opaque handle is returned. If the value is greater than zero, then
   * the operation was a success.  If the value is less than zero then the
   * tag was not created and the failure error is one of the PLCTAG_ERR_xyz
   * errors.
   *}
  //LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout);
  function plc_tag_create(const attrib_str:PChar; timeout:cint):cint32; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_lock
   *
   * Lock the tag against use by other threads.  Because operations on a tag are
   * very much asynchronous, actions like getting and extracting the data from
   * a tag take more than one API call.  If more than one thread is using the same tag,
   * then the internal state of the tag will get broken and you will probably experience
   * a crash.
   *
   * This should be used to initially lock a tag when starting operations with it
   * followed by a call to plc_tag_unlock when you have everything you need from the tag.
   *}
  //LIB_EXPORT int plc_tag_lock(int32_t tag);
  function plc_tag_lock(tag:cint32):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_unlock
   *
   * The opposite action of plc_tag_unlock.  This allows other threads to access the
   * tag.
   *}

  //LIB_EXPORT int plc_tag_unlock(int32_t tag);
  function plc_tag_unlock(tag:cint32):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_abort
   *
   * Abort any outstanding IO to the PLC.  If there is something in flight, then
   * it is marked invalid.  Note that this does not abort anything that might
   * be still processing in the report PLC.
   *
   * The status will be PLCTAG_STATUS_OK unless there is an error such as
   * a null pointer.
   *
   * This is a function provided by the underlying protocol implementation.
   */
  LIB_EXPORT int plc_tag_abort(int32_t tag);}
  function plc_tag_abort(tag:cint32):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_destroy
   *
   * This frees all resources associated with the tag.  Internally, it may result in closed
   * connections etc.   This calls through to a protocol-specific function.
   *
   * This is a function provided by the underlying protocol implementation.
   */
  LIB_EXPORT int plc_tag_destroy(int32_t tag);}
  function plc_tag_destroy(tag:cint32):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_read
   *
   * Start a read.  If the timeout value is zero, then wait until the read
   * returns or the timeout occurs, whichever is first.  Return the status.
   * If the timeout value is zero, then plc_tag_read will normally return
   * PLCTAG_STATUS_PENDING.
   *
   * This is a function provided by the underlying protocol implementation.
   */
  LIB_EXPORT int plc_tag_read(int32_t tag, int timeout);}
  function plc_tag_read(tag:cint32; timeout: cint):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_status
   *
   * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
   * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
   * errors will be returned as appropriate.
   *
   * This is a function provided by the underlying protocol implementation.
   */
  LIB_EXPORT int plc_tag_status(int32_t tag);}
  function plc_tag_status(tag:cint32):cint; cdecl; external LibPLCTagLibName;


  {*
   * plc_tag_write
   *
   * Start a write.  If the timeout value is zero, then wait until the write
   * returns or the timeout occurs, whichever is first.  Return the status.
   * If the timeout value is zero, then plc_tag_write will usually return
   * PLCTAG_STATUS_PENDING.  The write is considered done
   * when it has been written to the socket.
   *
   * This is a function provided by the underlying protocol implementation.
   */
  LIB_EXPORT int plc_tag_write(int32_t tag, int timeout);}
  function plc_tag_write(tag:cint32; timeout:cint):cint; cdecl; external LibPLCTagLibName;


  {*
   * Tag data accessors.
   *}

  //LIB_EXPORT int plc_tag_get_size(int32_t tag);
  function plc_tag_get_size(tag:cint32):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT uint64_t plc_tag_get_uint64(int32_t tag, int offset);
  function plc_tag_get_uint64(tag:cint32; offset:cint):cuint64; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_uint64(int32_t tag, int offset, uint64_t val);
  function plc_tag_set_uint64(tag:cint32; offset:cint; val:cuint64):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT int64_t plc_tag_get_int64(int32_t tag, int offset);
  function plc_tag_get_int64(tag:cint32; offset:cint):cint64; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_int64(int32_t, int offset, int64_t val);
  function plc_tag_set_int64(tag:cint32; offset:cint; val:cint64):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT uint32_t plc_tag_get_uint32(int32_t tag, int offset);
  function plc_tag_get_uint32(tag:cint32; offset:cint):cuint32; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_uint32(int32_t tag, int offset, uint32_t val);
  function plc_tag_set_uint32(tag:cint32; offset:cint; val:cuint32):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT int32_t plc_tag_get_int32(int32_t tag, int offset);
  function plc_tag_get_int32(tag:cint32; offset:cint):cint32; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_int32(int32_t, int offset, int32_t val);
  function plc_tag_set_int32(tag:cint32; offset:cint; val:cint32):cint; cdecl; external LibPLCTagLibName;


  //LIB_EXPORT uint16_t plc_tag_get_uint16(int32_t tag, int offset);
  function plc_tag_get_uint16(tag:cint32; offset:cint):cuint16; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_uint16(int32_t tag, int offset, uint16_t val);
  function plc_tag_set_uint16(tag:cint32; offset:cint; val:cuint16):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT int16_t plc_tag_get_int16(int32_t tag, int offset);
  function plc_tag_get_int16(tag:cint32; offset:cint):cint16; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_int16(int32_t, int offset, int16_t val);
  function plc_tag_set_int16(tag:cint32; offset:cint; val:cint16):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT uint8_t plc_tag_get_uint8(int32_t tag, int offset);
  function plc_tag_get_uint8(tag:cint32; offset:cint):cuint8; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_uint8(int32_t tag, int offset, uint8_t val);
  function plc_tag_set_uint8(tag:cint32; offset:cint; val:cuint8):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT int8_t plc_tag_get_int8(int32_t tag, int offset);
  function plc_tag_get_int8(tag:cint32; offset:cint):cint8; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_int8(int32_t, int offset, int8_t val);
  function plc_tag_set_int8(tag:cint32; offset:cint; val:cint8):cint; cdecl; external LibPLCTagLibName;


  //LIB_EXPORT double plc_tag_get_float64(int32_t tag, int offset);
  function plc_tag_get_float64(tag:cint32; offset:cint):cdouble; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_float64(int32_t tag, int offset, double val);
  function plc_tag_set_float64(tag:cint32; offset:cint; val:cdouble):cint; cdecl; external LibPLCTagLibName;

  //LIB_EXPORT float plc_tag_get_float32(int32_t tag, int offset);
  function plc_tag_get_float32(tag:cint32; offset:cint):cfloat; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_float32(int32_t tag, int offset, float val);
  function plc_tag_set_float32(tag:cint32; offset:cint; val:cfloat):cint; cdecl; external LibPLCTagLibName;

implementation

end.
