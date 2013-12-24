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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-06-20  KRH - Created file.                                        *
  *                                                                        *
  **************************************************************************/


#ifndef __AB_AB_COMMON_H__
#define __AB_AB_COMMON_H__

#include <ab/ab.h>
#include <ab/ab_defs.h>
#include <util/attr.h>


#ifdef __cplusplus
extern "C"
{
#endif


extern mutex_p tag_mutex;
extern thread_p request_handler_thread;

/* generic */
int ab_tag_abort(ab_tag_p tag);
int ab_tag_destroy(ab_tag_p p_tag);

int check_cpu(ab_tag_p tag, attr attribs);
int check_tag_name(ab_tag_p tag, const char *name);
int send_eip_request(ab_request_p req);
int recv_eip_response(ab_session_p session);
int check_mutex(int debug);

uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

int request_create(ab_request_p *req);
int request_add_unsafe(ab_session_p sess, ab_request_p req);
int request_add(ab_session_p sess, ab_request_p req);
int request_remove_unsafe(ab_session_p sess, ab_request_p req);
int request_remove(ab_session_p sess, ab_request_p req);
int request_destroy(ab_request_p *req);

int find_or_create_session(ab_tag_p tag, attr attribs);
int add_session_unsafe(ab_tag_p tag,  ab_session_p n);
int add_session(ab_tag_p tag,  ab_session_p s);
int remove_session_unsafe(ab_tag_p tag, ab_session_p n);
int remove_session(ab_tag_p tag,  ab_session_p s);
ab_session_p find_session_by_host_unsafe(ab_tag_p tag, const char  *t);
int session_add_tag_unsafe(ab_tag_p tag, ab_session_p session);
int session_remove_tag_unsafe(ab_tag_p tag, ab_session_p session);
ab_session_p ab_session_create(ab_tag_p tag, const char *host, int gw_port);
int ab_session_connect(ab_tag_p tag, ab_session_p session, const char *host);
int ab_session_destroy_unsafe(ab_tag_p tag, ab_session_p session);
int ab_session_destroy(ab_tag_p tag, ab_session_p session);
int ab_session_empty(ab_session_p session);
int ab_session_register(ab_tag_p tag, ab_session_p session);
int ab_session_unregister(ab_tag_p tag, ab_session_p session);


int session_check_incoming_data(ab_session_p session);
int request_check_outgoing_data(ab_session_p session, ab_request_p req);

#ifdef WIN32
DWORD __stdcall request_handler_func(LPVOID not_used);
#else
void *request_handler_func(void *not_used);
#endif

#ifdef __cplusplus
}
#endif


#endif
