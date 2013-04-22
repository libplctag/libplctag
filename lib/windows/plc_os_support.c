/***************************************************************************
 *   Copyright (C) 2012 by Process Control Engineers                       *
 *   Author Kyle Hayes  kylehayes@processcontrolengineers.com              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-07-05  KRH - Created file.                                        *
  *                                                                        *
  * 2012-07-25  KRH - Updates for new thread API.                          *
  *                                                                        *
  **************************************************************************/


/***************************************************************************
 ******************************* WINDOWS ***********************************
 **************************************************************************/


#define _WINSOCKAPI_
#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <io.h>
#include <Winsock2.h>
#include <Ws2tcpip.h>


#ifdef __cplusplus
extern "C"
{
#endif


/*#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>*/

#include <plc_os_support.h>
#include "libplctag_util.h"
#include "libplctag_lib.h"
#include "libplctag_tag.h"






/***************************************************************************
 ******************************* Threads ***********************************
 **************************************************************************/



struct plc_thread_t {
    HANDLE hThread;
    HANDLE hMutex;
};


/* 
 * plc_tickler_thread_func
 * 
 * This function is the primary function for tickling the protocols' tickle
 * functions.
 */
 
DWORD WINAPI plc_tickler_thread_func(LPVOID lpParam)
{
	plc_lib lib = (plc_lib)lpParam;
	
	/* WARNING: do not change the protocol list
	 * while this thread is running!  We are not
	 * using any mutex guards on the list!
	 */
	 
	/* flag that we are running */
	lib->tickler_running = 1;
	 
	while(lib->tickler_run) {
		/* lock the mutex to prevent other code from
		 * changing something we care about.
		 */
		plc_lib_lock_tickler(lib);
		
		/* call the stock library tickler function */
		plc_lib_tickle(lib);

		/* done, unlock the mutex. */
		plc_lib_unlock_tickler(lib);
		
		/* give back the CPU */
		sleep_ms(lib->tickler_ms);
	}
	
	/* flag that we are not running */
	lib->tickler_running = 0;
	
    return 0;
}
	

int plc_lib_start_tickler_thread(plc_lib lib)
{
	/* check our args */
	if(!lib)
		return 0;
		
	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");
		
	/* create a structure to hold our thread information */
	lib->tickler_thread = (plc_thread)calloc(1, sizeof(struct plc_thread_t));
	
	if(!lib->tickler_thread) {
		plc_err(lib, PLC_LOG_ERR, PLC_ERR_NO_MEM, "Unable to allocate plc_thread! errno=%d", errno);
		return 0;
	}
	
	/* FIXME - handle errors */
    lib->tickler_thread->hMutex = CreateMutex( 
                NULL,                   /* default security attributes  */
                FALSE,                  /* initially not owned          */
                NULL);                  /* unnamed mutex                */

    if(!lib->tickler_thread->hMutex) {
        lib->tickler_ms = 0;
		lib->tickler_running = 0;
		
		free(lib->tickler_thread);
		lib->tickler_thread = PLC_THREAD_NULL;
		
		/* FIXME - should really get pthread error. */
		plc_err(lib, PLC_LOG_ERR, PLC_ERR_THREAD, "Unable to create mutex for tickler thread!");
		
        return 0;
    }
	
	/* set the flag to allow the thread to run */
	lib->tickler_run = 1;
	
	/* create the thread. */
    lib->tickler_thread->hThread = CreateThread( 
                NULL,                   /* default security attributes */
                0,                      /* use default stack size      */
                plc_tickler_thread_func,/* thread function name        */
                lib,                    /* argument to thread function */
                0,                      /* use default creation flags  */
                NULL);                  /* do not need thread ID       */
	
    if(!lib->tickler_thread->hThread) {
        lib->tickler_ms = 0;
		lib->tickler_running = 0;
		
		/* close the mutex handle */
		CloseHandle(lib->tickler_thread->hMutex);

		free(lib->tickler_thread);
		lib->tickler_thread = PLC_THREAD_NULL;
		
		/* FIXME - should really get thread error. */
		plc_err(lib, PLC_LOG_ERR, PLC_ERR_THREAD, "Unable to create Windows thread for tickler!");
		
        return 0;
    }
	
	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Done.");
	
	return 1;
}



int plc_lib_stop_tickler_thread(plc_lib lib)
{
	void *unused;
	
	if(!lib)
		return 0;
		
	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");

	/* is it running? */
	if(!lib->tickler_running || !lib->tickler_thread) {
		plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Not running, Done.");
		
		return 1;
	}
		
	/* notify the tickler thread not to run any more. */
	lib->tickler_run = 0;
	
	/* wait for it to stop */
	while(lib->tickler_running) 
		sleep_ms(lib->tickler_ms);
	
	/* join to the thread to release its resources */
    WaitForSingleObject(lib->tickler_thread->hThread, 10*(lib->tickler_ms));

    /* close the thread handle */
    CloseHandle(lib->tickler_thread->hThread);

    /* close the mutex handle */
    CloseHandle(lib->tickler_thread->hMutex);

	/* the thread is done, now free memory. */
	free(lib->tickler_thread);
	
	lib->tickler_thread = PLC_THREAD_NULL;

	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Done.");
	
	return 1;
}



int plc_lib_lock_tickler(plc_lib lib)
{
    DWORD dwWaitResult;

	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");

    dwWaitResult = ~WAIT_OBJECT_0;

    /* FIXME - This will potentially hang forever! */
    while(dwWaitResult != WAIT_OBJECT_0)
        dwWaitResult = WaitForSingleObject(lib->tickler_thread->hMutex,INFINITE);

	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Done.");
	
	return 1;
}



int plc_lib_unlock_tickler(plc_lib lib)
{
	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");

    ReleaseMutex(lib->tickler_thread->hMutex);

    plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Done.");
	
	return 1;
}






int sleep_ms(int ms)
{
    Sleep(ms);
    return 1;
}



/* 
 * time_ms
 *
 * Return current system time in millisecond units.  This is NOT an
 * Unix epoch time.  Windows uses a different epoch starting 1/1/1601.
 */

uint64_t time_ms(void)
{
    FILETIME ft;
    uint64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (uint64_t)(ft.dwLowDateTime) + ((uint64_t)(ft.dwHighDateTime) << 32);

    /* get time in ms */

    res = res / 10000;

    return  res;
}



/***************************************************************************
 ****************************** Serial Port ********************************
 **************************************************************************/


struct serial_port_t {
	HANDLE hSerialPort;
    COMMCONFIG oldDCBSerialParams;
    COMMTIMEOUTS oldTimeouts;
};


serial_port_p plc_lib_open_serial_port(plc_lib lib, const char *path, int baud_rate, int data_bits, int stop_bits, int parity_type)
{
	serial_port_p serial_port;
    COMMCONFIG dcbSerialParams;
    COMMTIMEOUTS timeouts;
    HANDLE hSerialPort;
    int BAUD, PARITY, DATABITS, STOPBITS;

    plc_err(lib, PLC_LOG_DEBUG, PLC_ERR_NONE, "Starting.");


    /* create the configuration for the serial port. */

    /* code largely from Programmer's Heaven.
     */

    switch (baud_rate) {
        case 38400:
            BAUD = CBR_38400;
            break;
        case 19200:
            BAUD  = CBR_19200;
            break;
        case 9600:
            BAUD  = CBR_9600;
            break;
        case 4800:
            BAUD  = CBR_4800;
            break;
        case 2400:
            BAUD  = CBR_2400;
            break;
        case 1200:
            BAUD  = CBR_1200;
            break;
        case 600:
            BAUD  = CBR_600;
            break;
        case 300:
            BAUD  = CBR_300;
            break;
        case 110:
            BAUD  = CBR_110;
            break;
        default:
            /* unsupported baud rate */
            plc_err(lib, PLC_LOG_ERR, PLC_ERR_BAD_PARAM,"Unsupported baud rate: %d. Use standard baud rates (300,600,1200,2400...).",baud_rate);
            return PLC_SERIAL_PORT_NULL;
    }


	/* data bits */
	switch(data_bits) {
		case 5:
			DATABITS = 5;
			break;
			
		case 6:
			DATABITS = 6;
			break;
			
		case 7:
			DATABITS = 7;
			break;
		
		case 8:
			DATABITS = 8;
			break;
			
		default:
			/* unsupported number of data bits. */
            plc_err(lib, PLC_LOG_ERR, PLC_ERR_BAD_PARAM,"Unsupported number of data bits: %d. Use 5-8.",data_bits);
            return PLC_SERIAL_PORT_NULL;
	}
	

    switch(stop_bits) {
         case 1:
            STOPBITS = ONESTOPBIT;
            break;
         case 2:
            STOPBITS = TWOSTOPBITS;
            break;
        default:
            /* unsupported number of stop bits. */
            plc_err(lib, PLC_LOG_ERR, PLC_ERR_BAD_PARAM,"Unsupported number of stop bits, %d, must be 1 or 2.",stop_bits);
            return PLC_SERIAL_PORT_NULL;
    }

    switch(parity_type) {
        case 0:
            PARITY = NOPARITY;
            break;
        case 1: /* Odd parity */
            PARITY = ODDPARITY;
            break;
        case 2: /* Even parity */
            PARITY = EVENPARITY;
            break;
        default:
            /* unsupported number of stop bits. */
            plc_err(lib, PLC_LOG_ERR, PLC_ERR_BAD_PARAM,"Unsupported parity type, must be none (0), odd (1) or even (2).");
            return PLC_SERIAL_PORT_NULL;
    }

    /* allocate the structure */
	serial_port = (serial_port_p)calloc(1,sizeof(struct serial_port_t));
	
	if(!serial_port) {
		plc_err(lib, PLC_LOG_ERR, PLC_ERR_NO_MEM, "Unable to allocate serial port struct.");
		return PLC_SERIAL_PORT_NULL;
	}

    /* open the serial port device */
    hSerialPort = CreateFile(path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

    /* did the open succeed? */
    if(hSerialPort == INVALID_HANDLE_VALUE) {
        free(serial_port);
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Error opening serial device %s",path);
        return PLC_SERIAL_PORT_NULL;
    }

    /* get existing serial port configuration and save it. */
    if(!GetCommState(hSerialPort, &(serial_port->oldDCBSerialParams.dcb))) {
        free(serial_port);
        CloseHandle(hSerialPort);
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Error getting backup serial port configuration.",path);
        return PLC_SERIAL_PORT_NULL;
    }

    /* copy the params. */
    dcbSerialParams = serial_port->oldDCBSerialParams;

    dcbSerialParams.dcb.BaudRate = BAUD;
    dcbSerialParams.dcb.ByteSize = DATABITS;
    dcbSerialParams.dcb.StopBits = STOPBITS;
    dcbSerialParams.dcb.Parity = PARITY;

    dcbSerialParams.dcb.fBinary         = TRUE;
    dcbSerialParams.dcb.fDtrControl     = DTR_CONTROL_DISABLE;
    dcbSerialParams.dcb.fRtsControl     = RTS_CONTROL_DISABLE;
    dcbSerialParams.dcb.fOutxCtsFlow    = FALSE;
    dcbSerialParams.dcb.fOutxDsrFlow    = FALSE;
    dcbSerialParams.dcb.fDsrSensitivity = FALSE;
    dcbSerialParams.dcb.fAbortOnError   = TRUE; /* FIXME - should this be false? */

    /* attempt to set the serial params */
    if(!SetCommState(hSerialPort, &dcbSerialParams.dcb)) {
        free(serial_port);
        CloseHandle(hSerialPort);
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Error setting serial port configuration.",path);
        return PLC_SERIAL_PORT_NULL;
    }

    /* attempt to get the current serial port timeout set up */
    if(!GetCommTimeouts(hSerialPort, &(serial_port->oldTimeouts))) {
        SetCommState(hSerialPort, &(serial_port->oldDCBSerialParams.dcb));
        free(serial_port);
        CloseHandle(hSerialPort);
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Error getting backup serial port timeouts.",path);
        return PLC_SERIAL_PORT_NULL;
    }

    timeouts = serial_port->oldTimeouts;

    /* set the timeouts for asynch operation */
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;

    /* attempt to set the current serial port timeout set up */
    if(!SetCommTimeouts(hSerialPort, &timeouts)) {
        SetCommState(hSerialPort, &(serial_port->oldDCBSerialParams.dcb));
        free(serial_port);
        CloseHandle(hSerialPort);
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Error getting backup serial port timeouts.",path);
        return PLC_SERIAL_PORT_NULL;
    }
	
    return serial_port;
}







int plc_lib_close_serial_port(plc_lib lib, serial_port_p serial_port)
{
    /* try to prevent this from being called twice */
    if(!serial_port || !serial_port->hSerialPort)
        return 1;

    /* reset the old options */
    SetCommTimeouts(serial_port->hSerialPort, &(serial_port->oldTimeouts));
    SetCommState(serial_port->hSerialPort, &(serial_port->oldDCBSerialParams.dcb));
    CloseHandle(serial_port->hSerialPort);

    /* make sure that we do not call this twice. */
    serial_port->hSerialPort = 0;
	
	/* free the serial port */
	free(serial_port);

    return 1;
}




int plc_lib_serial_port_read(serial_port_p serial_port, uint8_t *data, int size)
{
    DWORD numBytesRead = 0;
    BOOL rc;

    rc = ReadFile(serial_port->hSerialPort,(LPVOID)data,(DWORD)size,&numBytesRead,NULL);

    if(rc != TRUE)
        return -1;

	return (int)numBytesRead;
}


int plc_lib_serial_port_write(serial_port_p serial_port, uint8_t *data, int size)
{
    DWORD numBytesWritten = 0;
    BOOL rc;

    rc = WriteFile(serial_port->hSerialPort,(LPVOID)data,(DWORD)size,&numBytesWritten,NULL);

	return (int)numBytesWritten;
}








/***************************************************************************
 ****************************** Serial Port ********************************
 **************************************************************************/


struct plc_socket_t {
	int fd;
	int port;
};


/* forward declarations used below */

int plc_lib_parse_host(plc_lib lib, const char *host_and_port, int default_port, int max_ips, struct in_addr *ips, int *num_ips, int *port);
int plc_lib_socket_lib_init(void);

/*
 * plc_lib_open_socket
 * 
 * Open a socket using POSIX functions.  This is not a generic function.
 * You get a non-blocking TCP socket only.  You pass it a string with
 * a host and optional port (separated by a colon) and a default port.
 */
 
#define MAX_IPS 8 
 
plc_socket plc_lib_open_socket(plc_lib lib, const char *host_and_port, int default_port)
{
	plc_socket sock;
    struct in_addr ips[MAX_IPS];
    int num_ips;
    int port = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    int i = 0;
    int done = 0;
	int fd;
	
	plc_err(lib, PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");

    /* attempt to open the socket library. */
    if(!plc_lib_socket_lib_init()) {
        plc_err(lib, PLC_LOG_ERR, PLC_ERR_OPEN, "Unable to initialize socket library.");
        return PLC_SOCKET_NULL;
    }
	
    /* parse the host and port string and look up the IPs. */
    if(!plc_lib_parse_host(lib, host_and_port, default_port, MAX_IPS, ips, &num_ips, &port)) {
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_BAD_PARAM, "Unable to parse gateway and port from path!");
        return PLC_SOCKET_NULL;
    }

    /* Open a socket for communication with the gateway. */
    fd = socket(AF_INET, SOCK_STREAM, 0/*IPPROTO_TCP*/);

    /* check for errors */
    if(fd < 0) {
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_OPEN, "Socket creation failed, errno: %d",errno);
        return PLC_SOCKET_NULL;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(char*)&sock_opt,sizeof(sock_opt))) {
		closesocket(fd);
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_OPEN, "Error setting socket option, errno: %d",errno);
        return 0;
    }

    /* now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    memset((void *)&gw_addr,0, sizeof(gw_addr));
    gw_addr.sin_family = AF_INET ;
    gw_addr.sin_port = htons(port);

    do {
        /* try each IP until we run out or get a connection. */
        gw_addr.sin_addr.s_addr = ips[i].S_un.S_addr;

        plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Attempting to connect to %s",inet_ntoa(*((struct in_addr *)&ips[i])));

        if(connect(fd,(struct sockaddr *)&gw_addr,sizeof(gw_addr)) == 0) {
            plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Attempt to connect to %s succeeded.",inet_ntoa(*((struct in_addr *)&ips[i])));
            done = 1;
        } else {
            plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Attempt to connect to %s failed, errno: %d",inet_ntoa(*((struct in_addr *)&ips[i])),errno);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
		closesocket(fd);
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_OPEN, "Unable to connect to any gateway host IP address!");
        return PLC_SOCKET_NULL;
    }

	sock = (plc_socket)calloc(1,sizeof(struct plc_socket_t));
	
	if(!sock) {
		closesocket(fd);
		plc_err(lib, PLC_LOG_ERR, PLC_ERR_NO_MEM, "Unable to allocat socket struct.");
		return PLC_SOCKET_NULL;
	}
	
	sock->fd = fd;
	sock->port = port;
	
	return sock;
}



int plc_lib_close_socket(plc_lib lib, plc_socket sock)
{
	if(!sock)
		return 1;
		
	if(!sock->fd)
		return 1;
		
	closesocket(sock->fd);
	
	sock->fd = 0;
	
	free(sock);
	
	return 1;
}




int plc_lib_socket_get_port(plc_socket sock)
{
	if(!sock)
		return -1;
		
	return sock->port;
}


/*
 * plc_lib_parse_host
 * 
 * This function takes a string of "host:port" and parses it out.  It returns a list of IP addresses.  If
 * there is no port part to the string, it will use the default port passed in.  You can hand it a host
 * name or a string with a numeric IP address.  IPv4 only.
 */

int plc_lib_parse_host(plc_lib lib, const char *host_and_port, int default_port, int max_ips, struct in_addr *ips, int *num_ips, int *port)
{
    struct hostent *h=NULL;
    int num_entries;
    const char *start = NULL;
    const char *end = NULL;
    int path_len;
    char *host = NULL;
    uint32_t tmp_ip;

    plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Starting.");

    path_len = strlen(host_and_port);

    host = (char *)calloc(path_len+1,1);

    if(!host) {
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_NO_MEM, "Calloc failed, errno: %d!",errno);
        return 0;
    }

    start=&host_and_port[0];
    end=&host_and_port[0];

    /* scan to the first ':' or end of the string. */
    while(end && *end && end < (host_and_port+path_len) && *end != ':') end++;

    if(*end == ':') {
        /* we have a port number. */
        *port = atoi(end+1);
    } else {
        *port = default_port;
    }

    strncpy(host,start,end-start);

    host[end-start] = 0;
    
    tmp_ip = inet_addr(host);

    if(tmp_ip != INADDR_NONE) {
        ips[0].S_un.S_addr = tmp_ip;

        plc_err(lib,PLC_LOG_DEBUG, PLC_ERR_NONE, "Found numeric IP address: %s",host);

        *num_ips = 1;

        free(host);

        plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Done.");

        return 1;
    }

    plc_err(lib,PLC_LOG_DEBUG, PLC_ERR_NONE, "No numeric IP string, trying DNS on hostname.");

    h = (struct hostent *)gethostbyname(host);

    if(!h) {
        free(host);
        plc_err(lib,PLC_LOG_ERR, PLC_ERR_OPEN, "Call to gethostbyname() failed, errno: %d!", errno);
        return 0;
    }

    for(num_entries = 0; h->h_addr_list[num_entries] && num_entries < max_ips; num_entries++) {
        ips[num_entries] = *((struct in_addr *)h->h_addr_list[num_entries]);
    }

    *num_ips = num_entries;

    free(host);
    free(h);

    plc_err(lib,PLC_LOG_INFO, PLC_ERR_NONE, "Done.");

    return 1;
}



/* windows needs to have the Winsock library initialized 
 * before use. Does it need to be static?
 */

static BOOL bWinsockInitialized = FALSE;
static WSADATA wsaData;

int plc_lib_socket_lib_init(void)
{
    if(bWinsockInitialized)
        return 1;

    if(WSAStartup(MAKEWORD(2,2), &wsaData) != NO_ERROR) {
        return 0;
    }

    bWinsockInitialized = TRUE;

    return 1;
}







/*
 * plc_lib_send_buf
 * 
 * Just a wrapper around POSIX send.
 */

int plc_lib_send_buf(plc_socket sock,uint8_t *buf, int size)
{
    return send(sock->fd,(char*)buf,size,MSG_NOSIGNAL);
}




/*
 * plc_lib_recv_buf
 *
 * Receive data from a socket into a buffer.  Mostly a wrapper.
 *
 * Returns:
 * size - amount of data read.
 * 0    - when timeout or invalid dbuf
 * -1   - when select/recv error.
 */

int plc_lib_recv_buf(plc_socket sock, uint8_t *buf, int size, int timeout)
{
    fd_set read_fds;
    struct timeval tv;
    int rc;
    int max_fd = sock->fd + 1;

    /* set up the read_fds for a call to select */
    FD_ZERO(&read_fds);
    FD_SET(sock->fd,&read_fds);

    /* set up the timeout. */
    tv.tv_sec = timeout/1000;
    tv.tv_usec = (timeout % 1000)*1000;

    rc = select(max_fd,&read_fds,NULL,NULL,&tv);

    /* timeout==0? error==-1? */
    if(rc <= 0) {
        return rc;
    }

    /* The socket is non-blocking, so what does MSG_WAITALL do? */
    rc = recv(sock->fd,(char *)buf,size, 0);

    return rc;
}



#ifdef __cplusplus
}
#endif

