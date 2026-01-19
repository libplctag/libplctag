#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include "err.h"

const char* util_err_str(util_err_t e) {
    switch (e) {
        case UTIL_OK:           return "OK";
        case UTIL_EABORT:       return "Operation aborted";
        case UTIL_EAGAIN:       return "Would block";
        case UTIL_EBIND:        return "Bind failed";
        case UTIL_EBOUNDS:      return "Out of bounds";
        case UTIL_EBUSY:        return "Resource busy";
        case UTIL_ECANCELLED:   return "Operation cancelled";
        case UTIL_ECLOSED:      return "Peer closed";
        case UTIL_ECONNECT:     return "Connect failed";
        case UTIL_ECONNREFUSED: return "Connection refused";
        case UTIL_ECONNRESET:   return "Connection reset by peer";
        case UTIL_EDESTROYED:   return "Object destroyed";
        case UTIL_EHOSTUNREACH: return "Host unreachable";
        case UTIL_EINTERNAL:    return "Internal error";
        case UTIL_EINTR:        return "Interrupted system call";
        case UTIL_EINVAL:       return "Invalid argument";
        case UTIL_EIO:          return "I/O error";
        case UTIL_ELISTEN:      return "Listen failed";
        case UTIL_ENETUNREACH:  return "Network unreachable";
        case UTIL_ENOTCONNECT:  return "Socket not connected";
        case UTIL_ENOTFOUND:    return "Not found";
        case UTIL_ENOTSUPPORTED: return "Operation not supported by platform";
        case UTIL_ENULL:        return "Null pointer";
        case UTIL_EREAD:        return "Read failed";
        case UTIL_ERESOLVE:     return "Name resolution failed";
        case UTIL_ERESOURCE:    return "Out of resources (memory, etc.)";
        case UTIL_ETIMEOUT:     return "Operation timed out";
        case UTIL_EWRITE:       return "Write failed";
        case UTIL_EARGS_MISSING_REQUIRED: return "Required flag not provided";
        case UTIL_EARGS_UNKNOWN_FLAG:     return "Unknown flag provided";
        case UTIL_EARGS_INVALID_VALUE:    return "Value failed to parse";
        case UTIL_EARGS_INVALID_FORMAT:   return "Argument format invalid";
        case UTIL_EARGS_DUPLICATE:        return "Flag appears multiple times";
        case UTIL_EARGS_PARSE_ERROR:      return "Generic parse error";
        default:                return "Unknown error";
    }
}

util_err_t util_err_from_errno(int e) {
    switch (e) {
        case 0:
            return UTIL_OK;

        case EINVAL:
            return UTIL_EINVAL;

#ifdef ENOMEM
        case ENOMEM:
            return UTIL_ERESOURCE;
#endif

#ifdef EAGAIN
        case EAGAIN:
            return UTIL_EAGAIN;
#endif

#if defined(EWOULDBLOCK) && (EAGAIN != EWOULDBLOCK)
        case EWOULDBLOCK:
            return UTIL_EAGAIN;
#endif

#ifdef ETIMEDOUT
        case ETIMEDOUT:
            return UTIL_ETIMEOUT;
#endif

#ifdef EPIPE
        case EPIPE:
            return UTIL_ECLOSED;
#endif

#ifdef EBADF
        case EBADF:
            return UTIL_ECLOSED;
#endif

#ifdef ECONNRESET
        case ECONNRESET:
            return UTIL_ECONNRESET;
#endif

#ifdef ENOTCONN
        case ENOTCONN:
            return UTIL_ECLOSED;
#endif

#ifdef ECONNREFUSED
        case ECONNREFUSED:
            return UTIL_ECONNREFUSED;
#endif

#ifdef EHOSTUNREACH
        case EHOSTUNREACH:
            return UTIL_EHOSTUNREACH;
#endif

#ifdef ENETUNREACH
        case ENETUNREACH:
            return UTIL_ENETUNREACH;
#endif

#ifdef EINTR
        case EINTR:
            return UTIL_EINTR;
#endif

#ifdef ENETDOWN
        case ENETDOWN:
            return UTIL_ECONNECT;
#endif

#ifdef EADDRINUSE
        case EADDRINUSE:
            return UTIL_EBIND;
#endif

#ifdef EADDRNOTAVAIL
        case EADDRNOTAVAIL:
            return UTIL_EBIND;
#endif

#ifdef EAFNOSUPPORT
        case EAFNOSUPPORT:
            return UTIL_ENOTSUPPORTED;
#endif

#ifdef EPROTONOSUPPORT
        case EPROTONOSUPPORT:
            return UTIL_ENOTSUPPORTED;
#endif

#ifdef ENOSYS
        case ENOSYS:
            return UTIL_ENOTSUPPORTED;
#endif

        default:
            return UTIL_EINTERNAL;
    }
}

#ifdef _WIN32
#include <winsock2.h>

util_err_t util_err_from_wsa(int w) {
    switch (w) {
        case 0:
            return UTIL_OK;

        case WSAEINVAL:
            return UTIL_EINVAL;

        case WSAENOBUFS:
        case WSA_NOT_ENOUGH_MEMORY:
            return UTIL_ERESOURCE;

        case WSAEWOULDBLOCK:
            return UTIL_EAGAIN;

        case WSAETIMEDOUT:
            return UTIL_ETIMEOUT;

        case WSAENOTSOCK:
        case WSAESHUTDOWN:
        case WSAENETRESET:
            return UTIL_ECLOSED;

        case WSAECONNRESET:
            return UTIL_ECONNRESET;

        case WSAECONNREFUSED:
            return UTIL_ECONNREFUSED;

        case WSAEHOSTUNREACH:
            return UTIL_EHOSTUNREACH;

        case WSAENETUNREACH:
        case WSAENETDOWN:
            return UTIL_ENETUNREACH;

        case WSAEADDRINUSE:
        case WSAEADDRNOTAVAIL:
            return UTIL_EBIND;

        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAEOPNOTSUPP:
            return UTIL_ENOTSUPPORTED;

        default:
            return UTIL_EINTERNAL;
    }
}
#endif
