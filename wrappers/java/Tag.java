package libplctag;


import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.NativeLibrary;
import com.sun.jna.Pointer;
import com.sun.jna.PointerType;

/*
 * Tag
 *
 * This class represents a tag in a PLC.
 *
 * The attribute string is something similar to:
 *
 * protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=secondCounter
 */

public class Tag implements Library {
    // static native library stuff
    public static final String JNA_LIBRARY_NAME = "plctag";
    public static final NativeLibrary JNA_NATIVE_LIB = NativeLibrary.getInstance(Tag.JNA_LIBRARY_NAME);
    static {
        //System.out.println("trying to register with JNA for native library");
        Native.register(Tag.JNA_LIBRARY_NAME);
    }

    // error codes
    public static final int PLCTAG_STATUS_PENDING = (int)(1);
    public static final int PLCTAG_STATUS_OK = (int)(0);

    public static final int PLCTAG_ERR_NULL_PTR = (int)(-1);
    public static final int PLCTAG_ERR_OUT_OF_BOUNDS = (int)(-2);
    public static final int PLCTAG_ERR_NO_MEM = (int)(-3);
    public static final int PLCTAG_ERR_LL_ADD = (int)(-4);
    public static final int PLCTAG_ERR_CREATE = (int)(-6);
    public static final int PLCTAG_ERR_BAD_PARAM = (int)(-5);
    public static final int PLCTAG_ERR_NOT_EMPTY = (int)(-7);
    public static final int PLCTAG_ERR_OPEN = (int)(-8);
    public static final int PLCTAG_ERR_SET = (int)(-9);
    public static final int PLCTAG_ERR_WRITE = (int)(-10);
    public static final int PLCTAG_ERR_TIMEOUT = (int)(-11);
    public static final int PLCTAG_ERR_TIMEOUT_ACK = (int)(-12);
    public static final int PLCTAG_ERR_RETRIES = (int)(-13);
    public static final int PLCTAG_ERR_READ = (int)(-14);
    public static final int PLCTAG_ERR_BAD_DATA = (int)(-15);
    public static final int PLCTAG_ERR_ENCODE = (int)(-16);
    public static final int PLCTAG_ERR_DECODE = (int)(-17);
    public static final int PLCTAG_ERR_UNSUPPORTED = (int)(-18);
    public static final int PLCTAG_ERR_TOO_LONG = (int)(-19);
    public static final int PLCTAG_ERR_CLOSE = (int)(-20);
    public static final int PLCTAG_ERR_NOT_ALLOWED = (int)(-21);
    public static final int PLCTAG_ERR_THREAD = (int)(-22);
    public static final int PLCTAG_ERR_NO_DATA = (int)(-23);
    public static final int PLCTAG_ERR_THREAD_JOIN = (int)(-24);
    public static final int PLCTAG_ERR_THREAD_CREATE = (int)(-25);
    public static final int PLCTAG_ERR_MUTEX_DESTROY = (int)(-26);
    public static final int PLCTAG_ERR_MUTEX_UNLOCK = (int)(-27);
    public static final int PLCTAG_ERR_MUTEX_INIT = (int)(-28);
    public static final int PLCTAG_ERR_MUTEX_LOCK = (int)(-29);
    public static final int PLCTAG_ERR_NOT_IMPLEMENTED = (int)(-30);
    public static final int PLCTAG_ERR_BAD_DEVICE = (int)(-31);
    public static final int PLCTAG_ERR_BAD_GATEWAY = (int)(-32);
    public static final int PLCTAG_ERR_REMOTE_ERR = (int)(-33);
    public static final int PLCTAG_ERR_NOT_FOUND = (int)(-34);
    public static final int PLCTAG_ERR_ABORT = (int)(-35);
    public static final int PLCTAG_ERR_WINSOCK = (int)(-36);


    public static final int PLCTAG_ERR_RECONNECTING = (int)(-100);

    private static Thread retryThread;
    private static ArrayList<WeakReference<Tag>> retryTags = new ArrayList<WeakReference<Tag>>();

    static {
        retryThread = new Thread() {
            @Override
            public void run() {
                ArrayList<WeakReference<Tag>> removeList = new ArrayList<WeakReference<Tag>>();

                while(true) {
                    synchronized(retryTags) {
                        for(WeakReference<Tag> weakTag : retryTags) {
                            Tag t = weakTag.get();

                            if(t == null) {
                                removeList.add(weakTag);
                            } else {
                                t.checkRetry();
                            }
                        }
                    }

                    if(removeList.size() > 0) {
                        synchronized(retryTags) {
                            retryTags.removeAll(removeList);
                        }

                        removeList.clear();
                    }


                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        e.printStackTrace();
                    }
                }
            }
        };

        retryThread.start();
    }

    private long retryMS;
    private String attributeString;
    private int timeout;
    private long nextRetry;
    private long lastAccess;
    private int autoCloseDelay;
    private boolean isClosed;
    private Lock lock = new ReentrantLock();

    // the wrapped tag
    private int tag;





    /*
     * Public API
     */


     public Tag(String attributes, int timeout) {
        this.retryMS = 0;
        this.attributeString = attributes;
        this.timeout = timeout;
        this.nextRetry = 0L;
        this.lastAccess = System.currentTimeMillis();
        this.autoCloseDelay = 0;

        /* try to open the tag */
        this.tag = Tag.plc_tag_create(attributes, timeout);

        /* good result? */
        if(this.tag > Tag.PLCTAG_STATUS_OK) {
            isClosed = false;

            /* put the tag into the retry list. */
            synchronized(retryTags) {
                retryTags.add(new WeakReference<Tag>(this));
            }
        } else {
            isClosed = true;
        }
    }

    public String getAttributes() {
        return attributeString;
    }

    public void setRetryTime(int newRetryMS) {
        retryMS = newRetryMS;
    }

    public long getLastAccess() {
        return this.lastAccess;
    }

    public int close() {
        int rc = Tag.PLCTAG_ERR_NULL_PTR;

        lock.lock();
        if(tag > Tag.PLCTAG_STATUS_OK) {
            rc = Tag.plc_tag_destroy(tag);
        } /* otherwise the tag is already destroyed */

        // make sure no one uses this again.
        tag = 0;
        isClosed = true;
        lock.unlock();

        return rc;
    }


    public int size() {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_get_size(tag);
            lock.unlock();
            return checkResponse(rc);
        } else {
            /* bad tag, failed in creation */
            return checkResponse(tag);
        }
    }

    public int read(int timeout) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            this.lastAccess = System.currentTimeMillis();
            lock.lock();
            int rc = Tag.plc_tag_read(tag, timeout);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }

    public int status() {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_status(tag);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }

    public int write(int timeout) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            this.lastAccess = System.currentTimeMillis();
            lock.lock();
            int rc = Tag.plc_tag_write(tag, timeout);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }




    /* data routines */
    public int getUInt32(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_uint32(tag, offset);
        lock.unlock();
        return val;
    }

    public int setUInt32(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_uint32(tag, offset, val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public int getInt32(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_int32(tag, offset);
        lock.unlock();
        return val;
    }

    public int setInt32(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_int32(tag, offset, val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public int getUInt16(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_uint16(tag, offset);
        lock.unlock();
        return val;
    }

    public int setUInt16(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_uint16(tag, offset, (short)val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public int getInt16(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_int16(tag, offset);
        lock.unlock();
        return val;
    }

    public int setInt16(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_int16(tag, offset, (short)val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public int getUInt8(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_uint8(tag, offset);
        lock.unlock();
        return val;
    }

    public int setUInt8(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_uint8(tag, offset, (byte)val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public int getInt8(int offset) {
        lock.lock();
        int val = Tag.plc_tag_get_int8(tag, offset);
        lock.unlock();
        return val;
    }

    public int setInt8(int offset, int val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_int8(tag, offset, (byte)val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }



    public float getFloat32(int offset) {
        lock.lock();
        float val = Tag.plc_tag_get_float32(tag, offset);
        lock.unlock();
        return val;
    }

    public int setFloat32(int offset, float val) {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            lock.lock();
            int rc = Tag.plc_tag_set_float32(tag, offset, val);
            lock.unlock();
            return checkResponse(rc);
        } else {
            return checkResponse(tag);
        }
    }

    /**
     * The following are the wrapped native functions of the C library.
     */


    /**
     * Original signature : <code>plc_tag plc_tag_create(const char*)</code><br>
     * <i>native declaration : line 75</i>
     */
    private static native Tag.plc_tag plc_tag_create(String attrib_str);

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
     * Original signature : <code>int plc_tag_abort(plc_tag)</code>
     * <i>native declaration : line 98</i>
     */
    private static native int plc_tag_abort(Tag.plc_tag tag);

    /**
     * plc_tag_destroy
     *
     * This frees all resources associated with the tag.  Internally, it may result in closed
     * connections etc.   This calls through to a protocol-specific function.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_destroy(plc_tag)</code>
     * <i>native declaration : line 111</i>
     */
    private static native int plc_tag_destroy(Tag.plc_tag tag);

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
     * Original signature : <code>int plc_tag_read(plc_tag, int)</code>
     * <i>native declaration : line 128</i>
     */
    private static native int plc_tag_read(Tag.plc_tag tag, int timeout);

    /**
     * plc_tag_status
     *
     * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
     * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
     * errors will be returned as appropriate.
     *
     * This is a function provided by the underlying protocol implementation.
     *
     * Original signature : <code>int plc_tag_status(plc_tag)</code>
     * <i>native declaration : line 142</i>
     */
    private static native int plc_tag_status(Tag.plc_tag tag);

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
     * Original signature : <code>int plc_tag_write(plc_tag, int)</code>
     * <i>native declaration : line 160</i>
     */
    private static native int plc_tag_write(Tag.plc_tag tag, int timeout);

    /**
     * plc_tag_get_size
     *
     * This function returns the size, in bytes, of the data that the tag
     * can read or write.
     *
     * Original signature : <code>int plc_tag_get_size(plc_tag)</code>
     * <i>native declaration : line 169</i>
     */
    private static native int plc_tag_get_size(Tag.plc_tag tag);

    /**
     * plc_tag_get_uint32
     *
     * Return an unsigned 32-bit integer value from the passed offset in the
     * tag's data.  UINT_MAX is returned if the offset is out of bounds.
     *
     * Original signature : <code>uint32_t plc_tag_get_uint32(plc_tag, int)</code>
     * <i>native declaration : line 171</i>
     */
    private static native int plc_tag_get_uint32(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint32(plc_tag, int, uint32_t)</code>
     * <i>native declaration : line 172</i>
     */
    private static native int plc_tag_set_uint32(Tag.plc_tag tag, int offset, int val);

    /**
     * Original signature : <code>int32_t plc_tag_get_int32(plc_tag, int)</code>
     * <i>native declaration : line 174</i>
     */
    private static native int plc_tag_get_int32(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int32(plc_tag, int, int32_t)</code>
     * <i>native declaration : line 175</i>
     */
    private static native int plc_tag_set_int32(Tag.plc_tag plc_tag1, int offset, int val);

    /**
     * Original signature : <code>uint16_t plc_tag_get_uint16(plc_tag, int)</code>
     * <i>native declaration : line 178</i>
     */
    private static native short plc_tag_get_uint16(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint16(plc_tag, int, uint16_t)</code>
     * <i>native declaration : line 179</i>
     */
    private static native int plc_tag_set_uint16(Tag.plc_tag tag, int offset, short val);

    /**
     * Original signature : <code>int16_t plc_tag_get_int16(plc_tag, int)</code>
     * <i>native declaration : line 181</i>
     */
    private static native short plc_tag_get_int16(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int16(plc_tag, int, int16_t)</code>
     * <i>native declaration : line 182</i>
     */
    private static native int plc_tag_set_int16(Tag.plc_tag plc_tag1, int offset, short val);

    /**
     * Original signature : <code>uint8_t plc_tag_get_uint8(plc_tag, int)</code>
     * <i>native declaration : line 185</i>
     */
    private static native byte plc_tag_get_uint8(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_uint8(plc_tag, int, uint8_t)</code>
     * <i>native declaration : line 186</i>
     */
    private static native int plc_tag_set_uint8(Tag.plc_tag tag, int offset, byte val);

    /**
     * Original signature : <code>int8_t plc_tag_get_int8(plc_tag, int)</code>
     * <i>native declaration : line 188</i>
     */
    private static native byte plc_tag_get_int8(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_int8(plc_tag, int, int8_t)</code>
     * <i>native declaration : line 189</i>
     */
    private static native int plc_tag_set_int8(Tag.plc_tag plc_tag1, int offset, byte val);

    /**
     * Original signature : <code>float plc_tag_get_float32(plc_tag, int)</code>
     * <i>native declaration : line 192</i>
     */
    private static native float plc_tag_get_float32(Tag.plc_tag tag, int offset);

    /**
     * Original signature : <code>int plc_tag_set_float32(plc_tag, int, float)</code>
     * <i>native declaration : line 193</i>
     */
    private static native int plc_tag_set_float32(Tag.plc_tag tag, int offset, float val);

    /*
     * wrapper for the plc_tag opaque pointer.
     */
    public static class plc_tag extends PointerType {
        public plc_tag(Pointer address) {
            super(address);
        }
        public plc_tag() {
            super();
        }
    }


    /*
     * Private implementation methods etc.
     */

    /*
     * Constructor is private, use the static create() method.
     *
     * This is so that we can return null when the underlying library returns null.
     */

    private Tag(String attrs) {
        attributeString = attrs;
        retryMS = 0;
        nextRetry = 0;
        createTag();
    }


    private void createTag() {
        lock.lock();
        tag = Tag.plc_tag_create(attributeString);

        /* how to check for NULL? */
        if(tag == null || tag.getPointer().equals(Pointer.NULL)) {
            tag = null;
        }
        lock.unlock();
    }

    private int checkResponse(int rc) {
        if(retryMS > 0 && rc < 0) {
            if(nextRetry == 0) {
                // close the tag if we have one.
                close();

                nextRetry = System.currentTimeMillis() + retryMS;
            }

            return Tag.PLCTAG_ERR_RECONNECTING;
        } else {
            nextRetry = 0;
            return rc;
        }
    }

    /*
     * Called when the retry timer fires.  Not called otherwise.
     */
    public void checkRetry() {
        if(nextRetry == 0 || nextRetry > System.currentTimeMillis()) {
            return;
        }

        // try to recreate the tag.
        createTag();

        nextRetry = 0;
    }


    /**
     * finalize
     *
     * This should never do anything.  But, just in case close() is not
     * called first, this will catch the problem and free up the
     * tag's memory, eventually.
     */
    protected void finalize() {
        if(tag > Tag.PLCTAG_STATUS_OK) {
            close();
        }
    }
}
