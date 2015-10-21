import argparse
from plctag import libplctag
import time

PLC_TYPE_UINT8 = 'uint8'
PLC_TYPE_SINT8 = 'sint8'
PLC_TYPE_UINT16 = 'uint16'
PLC_TYPE_SINT16 = 'sint16'
PLC_TYPE_UINT32 = 'uint32'
PLC_TYPE_SINT32 = 'sint32'
PLC_TYPE_REAL32 = 'real32'

PLC_TYPES = [PLC_TYPE_UINT8,
             PLC_TYPE_SINT8,
             PLC_TYPE_UINT16,
             PLC_TYPE_SINT16,
             PLC_TYPE_UINT32,
             PLC_TYPE_SINT32,
             PLC_TYPE_REAL32]

DATA_TIMEOUT = 5000

def main():
    parser = argparse.ArgumentParser(description='python version of tag_rw example')
    parser.add_argument('-t','--type', required=True, help='Type is one of ' +
                                                           '\"' + PLC_TYPE_UINT8 + '\", ' +
                                                           '\"' + PLC_TYPE_SINT8 + '\", ' +
                                                           '\"' + PLC_TYPE_UINT16 + '\", ' +
                                                           '\"' + PLC_TYPE_SINT16 + '\", ' +
                                                           '\"' + PLC_TYPE_UINT32 + '\", ' +
                                                           '\"' + PLC_TYPE_SINT32 + '\", ' +
                                                           'or \"' + PLC_TYPE_REAL32 + '\".  The type is the type\n' +
                                                           'of the data to be read/written to the named tag.  The\n' +
                                                           'types starting with \"u\" are unsigned and with \"s\" are signed.\n' +
                                                           'For floating point, use \"' + PLC_TYPE_REAL32 + '\".')
    parser.add_argument('-p','--path', required=True, help='The path to the device containing the named data.')
    parser.add_argument('-w','--val', required=False, help='The value to write.  Must be formatted appropriately\n' +
                                                           'for the data type.',
                                                      type=str)
    args = parser.parse_args()

    is_write = False
    if args.val is not None:
        is_write = True

    plc_type = args.type.lower()
    if plc_type not in PLC_TYPES:
        print 'ERROR: Invalid value for type'
        parser.print_help()
        exit(-1)

    # In python, ints are ints regardless of the byte-width,
    # but a float is a float.  This check makes sure the incoming
    # value (if a write) matches the type, and converts it at the
    # same time.  An exception indicates mal intent or bad input.
    write_value = None
    if is_write:
        try:
            if plc_type == PLC_TYPE_REAL32:
                write_value = float(args.val)
            else:
                write_value = int(args.val)
        except:
            print 'ERROR: cannot convert incoming write value %s' % (args.val)
            exit(-1)

    plc_path = args.path.lower()
    tag = libplctag.plc_tag_create(plc_path)
    if not tag:
        print 'ERROR: error creating tag!'
        exit(-1)

    end = time.time() + 5.0
    rc = libplctag.plc_tag_status(tag)
    while time.time() < end and rc == libplctag.PLCTAG_STATUS_PENDING:
        time.sleep(1.0)
        rc = libplctag.plc_tag_status(tag)

    rc = libplctag.plc_tag_status(tag)
    if rc != libplctag.PLCTAG_STATUS_OK:
        print 'ERROR: tag creation error, tag status: %d' % (rc)
        libplctag.plc_tag_destroy(tag)
        exit(-1)

    if not is_write:
        rc = libplctag.plc_tag_read(tag, DATA_TIMEOUT)
        if rc != libplctag.PLCTAG_STATUS_OK:
            print 'ERROR: tag read error, tag status: %d' % (rc)
            libplctag.plc_tag_destroy(tag)
            exit(-1)

        # Display the data
        i = 0
        index = 0
        tag_size = libplctag.plc_tag_get_size(tag)
        while index < tag_size:
            if plc_type == PLC_TYPE_UINT8:
                data = libplctag.plc_tag_get_uint8(tag, index)
                print 'data[%d]=%u (%X)' % (i, data, data)
                index = index + 1
            elif plc_type == PLC_TYPE_UINT16:
                data = libplctag.plc_tag_get_uint16(tag, index)
                print 'data[%d]=%u (%X)' % (i, data, data)
                index = index + 2
            elif plc_type == PLC_TYPE_UINT32:
                data = libplctag.plc_tag_get_uint32(tag, index)
                print 'data[%d]=%u (%X)' % (i, data, data)
                index = index + 4
            elif plc_type == PLC_TYPE_SINT8:
                data = libplctag.plc_tag_get_int8(tag, index)
                print 'data[%d]=%d (%X)' % (i, data, data)
                index = index + 1
            elif plc_type == PLC_TYPE_SINT16:
                data = libplctag.plc_tag_get_int16(tag, index)
                print 'data[%d]=%d (%X)' % (i, data, data)
                index = index + 2
            elif plc_type == PLC_TYPE_SINT32:
                data = libplctag.plc_tag_get_int32(tag, index)
                print 'data[%d]=%d (%X)' % (i, data, data)
                index = index + 4
            elif plc_type == PLC_TYPE_REAL32:
                data = libplctag.plc_tag_get_float32(tag, index)
                print 'data[%d]=%f' % (i, data)
                index = index + 4
            
            i = i+1
    else:
        if plc_type == PLC_TYPE_UINT8:
            rc = libplctag.plc_tag_set_uint8(tag, 0, write_value)
        elif plc_type == PLC_TYPE_UINT16:
            rc = libplctag.plc_tag_set_uint16(tag, 0, write_value)
        elif plc_type == PLC_TYPE_UINT32:
            rc = libplctag.plc_tag_set_uint32(tag, 0, write_value)
        elif plc_type == PLC_TYPE_SINT8:
            rc = libplctag.plc_tag_set_int8(tag, 0, write_value)
        elif plc_type == PLC_TYPE_SINT16:
            rc = libplctag.plc_tag_set_int16(tag, 0, write_value)
        elif plc_type == PLC_TYPE_SINT32:
            rc = libplctag.plc_tag_set_int32(tag, 0, write_value)
        elif plc_type == PLC_TYPE_REAL32:
            rc = libplctag.plc_tag_set_float32(tag, 0, write_value)

        # Write the data
        rc = libplctag.plc_tag_write(tag, DATA_TIMEOUT)

        if rc != libplctag.PLCTAG_STATUS_OK:
            print 'ERROR: error writing the data: %d!' % (rc)
        else:
            print 'Wrote %s' % (str(write_value))

    # End if write or read


    if tag:
        libplctag.plc_tag_destroy(tag)

    print 'Done'
    exit(0)


if __name__ == '__main__':
    main()

