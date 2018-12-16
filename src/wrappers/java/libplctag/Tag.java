/***************************************************************************
 *   Copyright (C) 2018 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


package libplctag;

import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.NativeLibrary;

public class Tag implements Library {
    // static native library stuff
    public static final String JNA_LIBRARY_NAME = "plctag";
    public static final NativeLibrary JNA_NATIVE_LIB = NativeLibrary.getInstance(Tag.JNA_LIBRARY_NAME);
    static {
        Native.register(Tag.JNA_LIBRARY_NAME);
    }

    // error codes
    public static final int PLCTAG_STATUS_PENDING = (int)(1);
    public static final int PLCTAG_STATUS_OK = (int)(0);

    public static final int PLCTAG_ERR_ABORT           = (-1);
    public static final int PLCTAG_ERR_BAD_CONFIG      = (-2);
    public static final int PLCTAG_ERR_BAD_CONNECTION  = (-3);
    public static final int PLCTAG_ERR_BAD_DATA        = (-4);
    public static final int PLCTAG_ERR_BAD_DEVICE      = (-5);
    public static final int PLCTAG_ERR_BAD_GATEWAY     = (-6);
    public static final int PLCTAG_ERR_BAD_PARAM       = (-7);
    public static final int PLCTAG_ERR_BAD_REPLY       = (-8);
    public static final int PLCTAG_ERR_BAD_STATUS      = (-9);
    public static final int PLCTAG_ERR_CLOSE           = (-10);
    public static final int PLCTAG_ERR_CREATE          = (-11);
    public static final int PLCTAG_ERR_DUPLICATE       = (-12);
    public static final int PLCTAG_ERR_ENCODE          = (-13);
    public static final int PLCTAG_ERR_MUTEX_DESTROY   = (-14);
    public static final int PLCTAG_ERR_MUTEX_INIT      = (-15);
    public static final int PLCTAG_ERR_MUTEX_LOCK      = (-16);
    public static final int PLCTAG_ERR_MUTEX_UNLOCK    = (-17);
    public static final int PLCTAG_ERR_NOT_ALLOWED     = (-18);
    public static final int PLCTAG_ERR_NOT_FOUND       = (-19);
    public static final int PLCTAG_ERR_NOT_IMPLEMENTED = (-20);
    public static final int PLCTAG_ERR_NO_DATA         = (-21);
    public static final int PLCTAG_ERR_NO_MATCH        = (-22);
    public static final int PLCTAG_ERR_NO_MEM          = (-23);
    public static final int PLCTAG_ERR_NO_RESOURCES    = (-24);
    public static final int PLCTAG_ERR_NULL_PTR        = (-25);
    public static final int PLCTAG_ERR_OPEN            = (-26);
    public static final int PLCTAG_ERR_OUT_OF_BOUNDS   = (-27);
    public static final int PLCTAG_ERR_READ            = (-28);
    public static final int PLCTAG_ERR_REMOTE_ERR      = (-29);
    public static final int PLCTAG_ERR_THREAD_CREATE   = (-30);
    public static final int PLCTAG_ERR_THREAD_JOIN     = (-31);
    public static final int PLCTAG_ERR_TIMEOUT         = (-32);
    public static final int PLCTAG_ERR_TOO_LARGE       = (-33);
    public static final int PLCTAG_ERR_TOO_SMALL       = (-34);
    public static final int PLCTAG_ERR_UNSUPPORTED     = (-35);
    public static final int PLCTAG_ERR_WINSOCK         = (-36);
    public static final int PLCTAG_ERR_WRITE           = (-37);


    private int tag_id;
    private int status;
    private String attributes;


    /*
     * Public API
     */

    public Tag(String attributes, int timeout) {
    	this.attributes = attributes;
        this.tag_id = Tag.plc_tag_create(this.attributes, timeout);

        if(tag_id < 0) {
            // error creating tag!
            this.status = tag_id;
        } else {
            this.status = Tag.PLCTAG_STATUS_OK;
        }
    }


    public int close() {
        int rc = Tag.PLCTAG_ERR_NULL_PTR;

        if(this.tag_id > 0) {
            rc = Tag.plc_tag_destroy(this.tag_id);
        }

        // make sure no one uses this again.
        this.tag_id = 0;
        this.status = Tag.PLCTAG_ERR_NULL_PTR;

        return rc;
    }
    
    public String decodeError(int rc) {
    	return Tag.plc_tag_decode_error(rc);
    }


    public int abort() {
        return Tag.plc_tag_abort(this.tag_id);
    }


    public int read(int timeout) {
        return Tag.plc_tag_read(this.tag_id, timeout);
    }

    public int status() {
        // check if we had a create problem.
        if(this.status < 0) {
            return this.status;
        } else {
            return Tag.plc_tag_status(this.tag_id);
        }
    }

    public int write(int timeout) {
        return Tag.plc_tag_write(this.tag_id, timeout);
    }



    public int size() {
        return Tag.plc_tag_get_size(this.tag_id);
    }


    /* data routines */
    // Java does not have a 64-bit unsigned type. */

    public long getInt64(int offset) {
        return Tag.plc_tag_get_int64(this.tag_id, offset);
    }

    public int setInt64(int offset, long val) {
        return Tag.plc_tag_set_int64(this.tag_id, offset, val);
    }



    public long getUInt32(int offset) {
        return Tag.plc_tag_get_uint32(this.tag_id, offset);
    }

    public int setUInt32(int offset, long val) {
        return Tag.plc_tag_set_uint32(this.tag_id, offset, (int)val);
    }



    public int getInt32(int offset) {
        return Tag.plc_tag_get_int32(this.tag_id, offset);
    }

    public int setInt32(int offset, int val) {
        return Tag.plc_tag_set_int32(this.tag_id, offset, val);
    }



    public int getUInt16(int offset) {
        return Tag.plc_tag_get_uint16(this.tag_id, offset);
    }

    public int setUInt16(int offset, int val) {
        return Tag.plc_tag_set_uint16(this.tag_id, offset, (short)val);
    }



    public int getInt16(int offset) {
        return Tag.plc_tag_get_int16(this.tag_id, offset);
    }

    public int setInt16(int offset, int val) {
        return Tag.plc_tag_set_int16(this.tag_id, offset, (short)val);
    }



    public int getUInt8(int offset) {
        return Tag.plc_tag_get_uint8(this.tag_id, offset);
    }

    public int setUInt8(int offset, int val) {
        return Tag.plc_tag_set_uint8(this.tag_id, offset, (byte)val);
    }



    public int getInt8(int offset) {
        return Tag.plc_tag_get_int8(this.tag_id, offset);
    }

    public int setInt8(int offset, int val) {
        return Tag.plc_tag_set_int8(this.tag_id, offset, (byte)val);
    }



    public double getFloat64(int offset) {
        return Tag.plc_tag_get_float64(this.tag_id, offset);
    }

    public int setFloat64(int offset, double val) {
        return Tag.plc_tag_set_float64(this.tag_id, offset, val);
    }

    public float getFloat32(int offset) {
        return Tag.plc_tag_get_float32(this.tag_id, offset);
    }

    public int setFloat32(int offset, float fVal) {
        return Tag.plc_tag_set_float32(this.tag_id, offset, fVal);
    }








    /**
     * The following are the wrapped native functions of the C library.
     */
    
    private static native String plc_tag_decode_error(int rc);


    /**
     * Original signature : <code>int32_t plc_tag_create(const char*, int)</code><br>
     * <i>native declaration : line 75</i>
     */
    private static native int plc_tag_create(String attrib_str, int timeout);

    /**
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
     *
     * Original signature : <code>int plc_tag_abort(int32_t)</code>
     * <i>native declaration : line 98</i>
     */
    private static native int plc_tag_abort(int tag_id);

    /**
     * plc_tag_destroy
     *
     * This frees all resources associated with the tag.  Internally, it may result in closed
     * connections etc.   This calls through to a protocol-specific function.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_destroy(int32_t)</code>
     * <i>native declaration : line 111</i>
     */
    private static native int plc_tag_destroy(int tag_id);

    /**
     * plc_tag_read
     *
     * Start a read.  If the timeout value is not zero, then wait until the read
     * completes or the timeout occurs, whichever is first.  Return the status.
     *
     * If the timeout value is zero, then plc_tag_read will normally return
     * PLCTAG_STATUS_PENDING.  Unless there is an error.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_read(int32_t, int)</code>
     * <i>native declaration : line 128</i>
     */
    private static native int plc_tag_read(int tag_id, int timeout);

    /**
     * plc_tag_status
     *
     * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
     * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
     * errors will be returned as appropriate.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_status(int32_t)</code>
     * <i>native declaration : line 142</i>
     */
    private static native int plc_tag_status(int tag_id);

    /**
     * plc_tag_write
     *
     * Start a write.  If the timeout value is zero, then wait until the write
     * returns or the timeout occurs, whichever is first.  Return the status.
     *
     * If the timeout value is zero, then plc_tag_write will usually return
     * PLCTAG_STATUS_PENDING.  The write is considered done when it has been
     * written to the socket.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_write(int32_t, int)</code>
     * <i>native declaration : line 160</i>
     */
    private static native int plc_tag_write(int tag_id, int timeout);

    /**
     * plc_tag_get_size
     *
     * This function returns the size, in bytes, of the data that the tag
     * can read or write.
     *
     * Original signature : <code>int plc_tag_get_size(int32_t)</code>
     * <i>native declaration : line 169</i>
     */
    private static native int plc_tag_get_size(int tag_id);


    /**
     * Original signature : <code>int64_t plc_tag_get_int64(int32_t, int)</code>
     * <i>native declaration : line 174</i>
     */
    private static native long plc_tag_get_int64(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int64(int32_t, int, int32_t)</code>
     * <i>native declaration : line 175</i>
     */
    private static native int plc_tag_set_int64(int tag_id, int offset, long val);

    /**
     * plc_tag_get_uint32
     *
     * Return an unsigned 32-bit integer value from the passed offset in the
     * tag's data.  UINT_MAX is returned if the offset is out of bounds.
     *
     * Original signature : <code>uint32_t plc_tag_get_uint32(int32_t, int)</code>
     * <i>native declaration : line 171</i>
     */
    private static native long plc_tag_get_uint32(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint32(int32_t, int, uint32_t)</code>
     * <i>native declaration : line 172</i>
     */
    private static native int plc_tag_set_uint32(int tag_id, int offset, int val);

    /**
     * Original signature : <code>int32_t plc_tag_get_int32(int32_t, int)</code>
     * <i>native declaration : line 174</i>
     */
    private static native int plc_tag_get_int32(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int32(int32_t, int, int32_t)</code>
     * <i>native declaration : line 175</i>
     */
    private static native int plc_tag_set_int32(int plc_tag1, int offset, int val);

    /**
     * Original signature : <code>uint16_t plc_tag_get_uint16(int32_t, int)</code>
     * <i>native declaration : line 178</i>
     */
    private static native short plc_tag_get_uint16(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint16(int32_t, int, uint16_t)</code>
     * <i>native declaration : line 179</i>
     */
    private static native int plc_tag_set_uint16(int tag_id, int offset, short val);

    /**
     * Original signature : <code>int16_t plc_tag_get_int16(int32_t, int)</code>
     * <i>native declaration : line 181</i>
     */
    private static native short plc_tag_get_int16(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int16(int32_t, int, int16_t)</code>
     * <i>native declaration : line 182</i>
     */
    private static native int plc_tag_set_int16(int plc_tag1, int offset, short val);

    /**
     * Original signature : <code>uint8_t plc_tag_get_uint8(int32_t, int)</code>
     * <i>native declaration : line 185</i>
     */
    private static native byte plc_tag_get_uint8(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint8(int32_t, int, uint8_t)</code>
     * <i>native declaration : line 186</i>
     */
    private static native int plc_tag_set_uint8(int tag_id, int offset, byte val);

    /**
     * Original signature : <code>int8_t plc_tag_get_int8(int32_t, int)</code>
     * <i>native declaration : line 188</i>
     */
    private static native byte plc_tag_get_int8(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int8(int32_t, int, int8_t)</code>
     * <i>native declaration : line 189</i>
     */
    private static native int plc_tag_set_int8(int plc_tag1, int offset, byte val);

    /**
     * Original signature : <code>float plc_tag_get_float32(int32_t, int)</code>
     * <i>native declaration : line 192</i>
     */
    private static native double plc_tag_get_float64(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_float32(int32_t, int, float)</code>
     * <i>native declaration : line 193</i>
     */
    private static native int plc_tag_set_float64(int tag_id, int offset, double val);

    /**
     * Original signature : <code>float plc_tag_get_float32(int32_t, int)</code>
     * <i>native declaration : line 192</i>
     */
    private static native float plc_tag_get_float32(int tag_id, int offset);

    /**
     * Original signature : <code>int plc_tag_set_float32(int32_t, int, float)</code>
     * <i>native declaration : line 193</i>
     */
    private static native int plc_tag_set_float32(int tag_id, int offset, float val);


    /**
     * finalize
     *
     * This should never do anything.  But, just in case close() is not
     * called first, this will catch the problem and free up the
     * tag's memory, eventually.
     */
    protected void finalize() {
        if(tag_id > 0) {
            close();
        }
    }
}
