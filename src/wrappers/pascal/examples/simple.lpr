program simple;

uses libplctagv2, ctypes, sysutils;

const
  REQ_VERSION_MAJOR = 2;
  REQ_VERSION_MINOR = 1;
  REQ_VERSION_REV   = 4;

  TAG_PATH = 'protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray';
  DATA_TIMEOUT = 5000;

var
  tag:cint32 = 0;
  val:cint32;
  rc:cint;
  i :cint;
  elem_size :cint = 0;
  elem_count:cint = 0;

begin
      //* check the library version. */
      if(plc_tag_check_lib_version(REQ_VERSION_MAJOR, REQ_VERSION_MINOR, REQ_VERSION_REV) <> PLCTAG_STATUS_OK) then begin
          writeln(Format('Required compatible library version %d.%d.%d not available!', [REQ_VERSION_MAJOR, REQ_VERSION_MINOR, REQ_VERSION_REV]));
          ExitCode:=1;
          Exit;
      end;

      //* create the tag */
      tag := plc_tag_create(TAG_PATH, DATA_TIMEOUT);

      //* everything OK? */
      if(tag < 0) then begin
          writeln(Format('ERROR %s: Could not create tag!', [plc_tag_decode_error(tag)]));
          ExitCode:=1;
          Exit;
      end;

      //* get the data */
      rc := plc_tag_read(tag, DATA_TIMEOUT);
      if(rc <> PLCTAG_STATUS_OK) then begin
          writeln(Format('ERROR: Unable to read the data! Got error code %d: %s',[rc, plc_tag_decode_error(rc)]));
          plc_tag_destroy(tag);
          ExitCode:=1;
          Exit;
      end;

      //* get the tag size and element size. Do this _AFTER_ reading the tag otherwise we may not know how big the tag is! */
      elem_size := plc_tag_get_int_attribute(tag, 'elem_size', 0);
      elem_count := plc_tag_get_int_attribute(tag, 'elem_count', 0);

      writeln('Tag has ',elem_count,' elements each of ',elem_size,' bytes.');

      //* print out the data */
      for i:=0 to elem_count-1 do begin
          writeln(format('data[%d]=%d',[i,plc_tag_get_int32(tag,(i*elem_size))]));
      end;

      //* now test a write */
      for i:=0 to elem_count-1 do begin
          val := plc_tag_get_int32(tag,(i*elem_size));

          val := val+1;

          writeln(Format('Setting element %d to %d',[i,val]));

          plc_tag_set_int32(tag,(i*elem_size),val);
      end;

      rc := plc_tag_write(tag, DATA_TIMEOUT);
      if(rc <> PLCTAG_STATUS_OK) then begin
          writeln(Format('ERROR: Unable to write the data! Got error code %d: %s',[rc, plc_tag_decode_error(rc)]));
          plc_tag_destroy(tag);
          ExitCode:=1;
          Exit;
      end;

      //* get the data again*/
      rc := plc_tag_read(tag, DATA_TIMEOUT);

      if(rc <> PLCTAG_STATUS_OK) then begin
          writeln(Format('ERROR: Unable to read the data! Got error code %d: %s',[rc, plc_tag_decode_error(rc)]));
          plc_tag_destroy(tag);
          ExitCode:=1;
          Exit;
      end;

      //* print out the data */
      for i:=0 to elem_count-1 do begin
          writeln(Format('data[%d]=%d',[i,plc_tag_get_int32(tag,(i*elem_size))]));
      end;

      //* we are done */
      plc_tag_destroy(tag);
end.

