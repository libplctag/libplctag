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

type
  TLibPLCStatus    = -39..1;
  TLibPLCDebug     =   0..5;
  TLibPLCEvents    =   1..6;

  TTagCallbackFunc = procedure(tag_id:cint32; event, status:cint); cdecl;
  TLogCallbackFunc = procedure(tag_id:cint32; debug_level:cint; const message:PChar); cdecl;


//library internal status.

const
  PLCTAG_STATUS_PENDING      = TLibPLCStatus(  1);
  PLCTAG_STATUS_OK           = TLibPLCStatus(  0);
  PLCTAG_ERR_ABORT           = TLibPLCStatus( -1);
  PLCTAG_ERR_BAD_CONFIG      = TLibPLCStatus( -2);
  PLCTAG_ERR_BAD_CONNECTION  = TLibPLCStatus( -3);
  PLCTAG_ERR_BAD_DATA        = TLibPLCStatus( -4);
  PLCTAG_ERR_BAD_DEVICE      = TLibPLCStatus( -5);
  PLCTAG_ERR_BAD_GATEWAY     = TLibPLCStatus( -6);
  PLCTAG_ERR_BAD_PARAM       = TLibPLCStatus( -7);
  PLCTAG_ERR_BAD_REPLY       = TLibPLCStatus( -8);
  PLCTAG_ERR_BAD_STATUS      = TLibPLCStatus( -9);
  PLCTAG_ERR_CLOSE           = TLibPLCStatus(-10);
  PLCTAG_ERR_CREATE          = TLibPLCStatus(-11);
  PLCTAG_ERR_DUPLICATE       = TLibPLCStatus(-12);
  PLCTAG_ERR_ENCODE          = TLibPLCStatus(-13);
  PLCTAG_ERR_MUTEX_DESTROY   = TLibPLCStatus(-14);
  PLCTAG_ERR_MUTEX_INIT      = TLibPLCStatus(-15);
  PLCTAG_ERR_MUTEX_LOCK      = TLibPLCStatus(-16);
  PLCTAG_ERR_MUTEX_UNLOCK    = TLibPLCStatus(-17);
  PLCTAG_ERR_NOT_ALLOWED     = TLibPLCStatus(-18);
  PLCTAG_ERR_NOT_FOUND       = TLibPLCStatus(-19);
  PLCTAG_ERR_NOT_IMPLEMENTED = TLibPLCStatus(-20);
  PLCTAG_ERR_NO_DATA         = TLibPLCStatus(-21);
  PLCTAG_ERR_NO_MATCH        = TLibPLCStatus(-22);
  PLCTAG_ERR_NO_MEM          = TLibPLCStatus(-23);
  PLCTAG_ERR_NO_RESOURCES    = TLibPLCStatus(-24);
  PLCTAG_ERR_NULL_PTR        = TLibPLCStatus(-25);
  PLCTAG_ERR_OPEN            = TLibPLCStatus(-26);
  PLCTAG_ERR_OUT_OF_BOUNDS   = TLibPLCStatus(-27);
  PLCTAG_ERR_READ            = TLibPLCStatus(-28);
  PLCTAG_ERR_REMOTE_ERR      = TLibPLCStatus(-29);
  PLCTAG_ERR_THREAD_CREATE   = TLibPLCStatus(-30);
  PLCTAG_ERR_THREAD_JOIN     = TLibPLCStatus(-31);
  PLCTAG_ERR_TIMEOUT         = TLibPLCStatus(-32);
  PLCTAG_ERR_TOO_LARGE       = TLibPLCStatus(-33);
  PLCTAG_ERR_TOO_SMALL       = TLibPLCStatus(-34);
  PLCTAG_ERR_UNSUPPORTED     = TLibPLCStatus(-35);
  PLCTAG_ERR_WINSOCK         = TLibPLCStatus(-36);
  PLCTAG_ERR_WRITE           = TLibPLCStatus(-37);
  PLCTAG_ERR_PARTIAL         = TLibPLCStatus(-38);
  PLCTAG_ERR_BUSY            = TLibPLCStatus(-39);

  PLCTAG_EVENT_READ_STARTED    = TLibPLCEvents(1);
  PLCTAG_EVENT_READ_COMPLETED  = TLibPLCEvents(2);
  PLCTAG_EVENT_WRITE_STARTED   = TLibPLCEvents(3);
  PLCTAG_EVENT_WRITE_COMPLETED = TLibPLCEvents(4);
  PLCTAG_EVENT_ABORTED         = TLibPLCEvents(5);
  PLCTAG_EVENT_DESTROYED       = TLibPLCEvents(6);  

  PLCTAG_DEBUG_NONE   = TLibPLCDebug(0);
  PLCTAG_DEBUG_ERROR  = TLibPLCDebug(1);
  PLCTAG_DEBUG_WARN   = TLibPLCDebug(2);
  PLCTAG_DEBUG_INFO   = TLibPLCDebug(3);
  PLCTAG_DEBUG_DETAIL = TLibPLCDebug(4);
  PLCTAG_DEBUG_SPEW   = TLibPLCDebug(5);  




  {
   * helper function for errors.
   *
   * This takes one of the above errors and turns it into a const char * suitable
   * for printing.
  }
  
  //LIB_EXPORT const char *plc_tag_decode_error(int err);
  function plc_tag_decode_error(err:cint):PChar; cdecl; external LibPLCTagLibName;
  
  
  
  {
   * Set the debug level.
   *
   * This function takes values from the defined debug levels below.  It sets
   * the debug level to the passed value.  Higher numbers output increasing amounts
   * of information.   Input values not defined below will be ignored.
  }
  
  
  //LIB_EXPORT void plc_tag_set_debug_level(int debug_level);
  procedure plc_tag_set_debug_level(debug_level:TLibPLCDebug); cdecl; external LibPLCTagLibName;
  
  
  
  {
   * Check that the library supports the required API version.
   *
   * The version is passed as integers.   The three arguments are:
   *
   * ver_major - the major version of the library.  This must be an exact match.
   * ver_minor - the minor version of the library.   The library must have a minor
   *             version greater than or equal to the requested version.
   * ver_patch - the patch version of the library.   The library must have a patch
   *             version greater than or equal to the requested version if the minor
   *             version is the same as that requested.   If the library minor version
   *             is greater than that requested, any patch version will be accepted.
   *
   * PLCTAG_STATUS_OK is returned if the version is compatible.  If it does not,
   * PLCTAG_ERR_UNSUPPORTED is returned.
   *
   * Examples:
   *
   * To match version 2.1.4, call plc_tag_check_lib_version(2, 1, 4).
   *
  }
  
  //LIB_EXPORT int plc_tag_check_lib_version(int req_major, int req_minor, int req_patch);
  function plc_tag_check_lib_version(req_major, req_minor, req_patch:cint):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  {
   * tag functions
   *
   * The following is the public API for tag operations.
   *
   * These are implemented in a protocol-specific manner.
  }
  
  {
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
  }
  
  //LIB_EXPORT int32_t plc_tag_create(const char *attrib_str, int timeout);
  function plc_tag_create(const attrib_str:PChar; timeout:cint):cint32; cdecl; external LibPLCTagLibName;
  
  
  
  {
   * plc_tag_shutdown
   *
   * Some systems may not call the atexit() handlers.  In those cases, wrappers should
   * call this function before unloading the library or terminating.   Most OSes will cleanly
   * recover all system resources when a process is terminated and this will not be necessary.
   *
   * THIS IS NOT THREAD SAFE!   Do not call this if you have multiple threads running against
   * the library.  You have been warned.   Close all tags first with plc_tag_destroy() and make
   * sure that nothing can call any library functions until this function returns.
   *
   * Normally you do not need to call this function.   This is only for certain wrappers or
   * operating environments that use libraries in ways that prevent the normal exit handlers
   * from working.
  }
  
  //LIB_EXPORT void plc_tag_shutdown(void);
  procedure  plc_tag_shutdown(); cdecl; external LibPLCTagLibName;
  
  
  
  {
   * plc_tag_register_callback
   *
   * This function registers the passed callback function with the tag.  Only one callback function
   * may be registered on a tag at a time!
   *
   * Once registered, any of the following operations on or in the tag will result in the callback
   * being called:
   *
   *      * starting a tag read operation.
   *      * a tag read operation ending.
   *      * a tag read being aborted.
   *      * starting a tag write operation.
   *      * a tag write operation ending.
   *      * a tag write being aborted.
   *      * a tag being destroyed
   *
   * The callback is called outside of the internal tag mutex so it can call any tag functions safely.   However,
   * the callback is called in the context of the internal tag helper thread and not the client library thread(s).
   * This means that YOU are responsible for making sure that all client application data structures the callback
   * function touches are safe to access by the callback!
   *
   * Do not do any operations in the callback that block for any significant time.   This will cause library
   * performance to be poor or even to start failing!
   *
   * When the callback is called with the PLCTAG_EVENT_DESTROY_STARTED, do not call any tag functions.  It is
   * not guaranteed that they will work and they will possibly hang or fail.
   *
   * Return values:
   *
   * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
   * function may be registered at a time on each tag.
   *
   * If all is successful, the function will return PLCTAG_STATUS_OK.
  }
  
  //LIB_EXPORT int plc_tag_register_callback(int32_t tag_id, void ( *tag_callback_func)(int32_t tag_id, int event, int status));
  function plc_tag_register_callback(tag_id:cint32; callback:TTagCallbackFunc):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  
  
  {
   * plc_tag_unregister_callback
   *
   * This function removes the callback already registered on the tag.
   *
   * Return values:
   *
   * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
   * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
  }
  
  //LIB_EXPORT int plc_tag_unregister_callback(int32_t tag_id);
  function plc_tag_unregister_callback(tag_id:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  {
   * plc_tag_register_logger
   *
   * This function registers the passed callback function with the library.  Only one callback function
   * may be registered with the library at a time!
   *
   * Once registered, the function will be called with any logging message that is normally printed due 
   * to the current log level setting.
   *
   * WARNING: the callback will usually be called when the internal tag API mutex is held.   You cannot
   * call any tag functions within the callback!
   *
   * Return values:
   *
   * If there is already a callback registered, the function will return PLCTAG_ERR_DUPLICATE.   Only one callback
   * function may be registered at a time on each tag.
   *
   * If all is successful, the function will return PLCTAG_STATUS_OK.
  }
  
  //LIB_EXPORT int plc_tag_register_logger(void ( *log_callback_func)(int32_t tag_id, int debug_level, const char *message));
  function plc_tag_register_logger(log_callback_func:TLogCallbackFunc):cint; cdecl; external LibPLCTagLibName;
  
  
  
  {
   * plc_tag_unregister_logger
   *
   * This function removes the logger callback already registered for the library.
   *
   * Return values:
   *
   * The function returns PLCTAG_STATUS_OK if there was a registered callback and removing it went well.
   * An error of PLCTAG_ERR_NOT_FOUND is returned if there was no registered callback.
  }
  
  //LIB_EXPORT int plc_tag_unregister_logger(void);
  function plc_tag_unregister_logger():cint; cdecl; external LibPLCTagLibName;
  
  
  
  {
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
  }
  
  
  //LIB_EXPORT int plc_tag_lock(int32_t tag);
  function plc_tag_lock(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  {
   * plc_tag_unlock
   *
   * The opposite action of plc_tag_unlock.  This allows other threads to access the
   * tag.
  }
  
  //LIB_EXPORT int plc_tag_unlock(int32_t tag);
  function plc_tag_unlock(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  
  {
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
  }
  //LIB_EXPORT int plc_tag_abort(int32_t tag);
  function plc_tag_abort(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  {
   * plc_tag_destroy
   *
   * This frees all resources associated with the tag.  Internally, it may result in closed
   * connections etc.   This calls through to a protocol-specific function.
   *
   * This is a function provided by the underlying protocol implementation.
  }
  //LIB_EXPORT int plc_tag_destroy(int32_t tag);
  function plc_tag_destroy(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  
  
  {
   * plc_tag_read
   *
   * Start a read.  If the timeout value is zero, then wait until the read
   * returns or the timeout occurs, whichever is first.  Return the status.
   * If the timeout value is zero, then plc_tag_read will normally return
   * PLCTAG_STATUS_PENDING.
   *
   * This is a function provided by the underlying protocol implementation.
  }
  //LIB_EXPORT int plc_tag_read(int32_t tag, int timeout);
  function plc_tag_read(tag:cint32; timeout:cint):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  {
   * plc_tag_status
   *
   * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
   * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
   * errors will be returned as appropriate.
   *
   * This is a function provided by the underlying protocol implementation.
  }
  //LIB_EXPORT int plc_tag_status(int32_t tag);
  function plc_tag_status(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  
  
  {
   * plc_tag_write
   *
   * Start a write.  If the timeout value is zero, then wait until the write
   * returns or the timeout occurs, whichever is first.  Return the status.
   * If the timeout value is zero, then plc_tag_write will usually return
   * PLCTAG_STATUS_PENDING.  The write is considered done
   * when it has been written to the socket.
   *
   * This is a function provided by the underlying protocol implementation.
  }
  //LIB_EXPORT int plc_tag_write(int32_t tag, int timeout);
  function plc_tag_write(tag:cint32; timeout:cint):cint; cdecl; external LibPLCTagLibName;
  
  
  
  
  {
   * Tag data accessors.
  }
  
  
  //LIB_EXPORT int plc_tag_get_int_attribute(int32_t tag, const char *attrib_name, int default_value);
  function plc_tag_get_int_attribute(tag:cint32; const attrib_name:PChar; default_value:cint):cint; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_int_attribute(int32_t tag, const char *attrib_name, int new_value);
  function plc_tag_set_int_attribute(tag:cint32; const attrib_name:PChar; new_value:cint):cint; cdecl; external LibPLCTagLibName;
  
  //LIB_EXPORT int plc_tag_get_size(int32_t tag);
  function plc_tag_get_size(tag:cint32):cint; cdecl; external LibPLCTagLibName;
  
  //LIB_EXPORT int plc_tag_get_bit(int32_t tag, int offset_bit);
  function plc_tag_get_bit(tag:cint32; offset_bit:cint):cint; cdecl; external LibPLCTagLibName;
  //LIB_EXPORT int plc_tag_set_bit(int32_t tag, int offset_bit, int val);
  function plc_tag_set_bit(tag:cint32; offset_bit:cint; val:cint):cint; cdecl; external LibPLCTagLibName;
  
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
