/*
 * Copyright (c) 2019 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*
 * filename: nl_api.h
 */

#ifndef __NL_API_H
#define __NL_API_H


#include "ds_common_types.h"
#include "cps_api_interface_types.h"
#include "cps_api_operation.h"
#include "std_type_defs.h"
#include "std_error_codes.h"
#include "std_socket_tools.h"
#include "nas_vrf_utils.h"

#include <stddef.h>
#include <linux/rtnetlink.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This define was selected to give about the buffer space on each netlink socket
 * created for kernel communication.  If the socket buffer is low for any reason
 * the kernel will discard events.  Missing discarded events is something
 * that we don't expect and would have undesirable consequences
 */

/* Netlink socket buffer size for interface events - 60MB */
#define NL_INTF_SOCKET_BUFFER_LEN (60*1024*1024)

/* Netlink socket buffer size for neighbor events - 150MB */
#define NL_NEIGH_SOCKET_BUFFER_LEN (150*1024*1024)

/* Netlink socket buffer size for route events - 250MB */
#define NL_ROUTE_SOCKET_BUFFER_LEN (250*1024*1024)

#define NL_SCRATCH_BUFFER_LEN (1*1024*1024) /* Scratch buffer size - 1MB */

/* Netlink socket buffer size for netconf events - 1MB */
#define NL_NETCONF_SOCKET_BUFFER_LEN (1*1024*1024)
/* @@TODO looks like the macro SOL_NETLINK is not present in linux/socket.h,
 * revisit when the kernel version is upgarded.
 * and hence defined the below macro */
#define NL_SOL_NETLINK 270
/* Default VRF name */
#define NL_DEFAULT_VRF_NAME NAS_DEFAULT_VRF_NAME
#define NL_DEFAULT_VRF_ID NAS_DEFAULT_VRF_ID
typedef enum  {
    nas_nl_sock_T_ROUTE=0,
    nas_nl_sock_T_INT=1,
    nas_nl_sock_T_NEI=2,
    nas_nl_sock_T_NETCONF=3,
    nas_nl_sock_T_MCAST_SNOOP=4,
    nas_nl_sock_T_MAX
}nas_nl_sock_TYPES;


int nas_nl_sock_create(const char* vrf_name, nas_nl_sock_TYPES type, bool include_bind) ;


void os_send_refresh(nas_nl_sock_TYPES type, char *vrf_name, uint32_t vrf_id);

typedef bool (*fun_process_nl_message) (int sock, int rt_msg_type, struct nlmsghdr *hdr, void * context, uint32_t);

/**
 * Handle get and set using netlink messages. This function is called to receive resposes after a get, set is issued
 */
bool netlink_tools_process_socket(int sock, fun_process_nl_message handlers,
        void * context, char * scratch_buff, size_t scratch_buff_len,
        const int * seq, int *error_code, uint32_t vrf_id);

/**
 * Handle the netlink events only - only one netlink event per funcion call
 */
void netlink_tools_receive_event(int sock,fun_process_nl_message handlers,
        void * context, char * scratch_buff, size_t scratch_buff_len,int *error_code, uint32_t vrf_id);

bool nl_send_request(int sock, int type, int flags, int seq, void * req, size_t len );

bool nl_send_nlmsg(int sock, struct nlmsghdr *m);

t_std_error nl_do_set_request(const char *vrf_name, nas_nl_sock_TYPES type,struct nlmsghdr *m,
                              void *buff, size_t bufflen);

void nas_os_pack_nl_hdr(struct nlmsghdr *nlh, __u16 msg_type, __u16 nl_flags);

void nas_os_pack_if_hdr(struct ifinfomsg *ifmsg, unsigned char ifi_family,
                        unsigned int flags, int if_index);

int nas_os_bind_nf_sub(int fd, int family, int type, int queue_num);
bool os_nflog_enable ();

t_std_error os_create_netlink_sock(const char *vrf_name, uint32_t vrf_id);
t_std_error os_del_netlink_sock(const char *vrf_name);
t_std_error os_sock_create(const char *vrf_name, e_std_socket_domain_t domain,
                           e_std_sock_type_t type, int protocol, int *sock);

#ifdef __cplusplus
}
#endif

#endif
