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

/*!
 * \file   os_interface.cpp
 */

#include "private/nas_os_if_priv.h"
#include "private/os_if_utils.h"
#include "private/os_interface_cache_utils.h"
#include "private/nas_os_if_conversion_utils.h"
#include "private/nas_os_l3_utils.h"

#include "netlink_tools.h"
#include "nas_nlmsg.h"
#include "nas_nlmsg_object_utils.h"
#include "nas_os_int_utils.h"
#include "nas_os_interface.h"
#include "vrf-mgmt.h"
#include "std_utils.h"

#include "cps_api_operation.h"
#include "cps_api_object_key.h"
#include "cps_class_map.h"

#include "std_time_tools.h"
#include "std_assert.h"
#include "std_mac_utils.h"
#include "event_log.h"

#include "dell-interface.h"
#include "dell-base-if.h"
#include "dell-base-if-linux.h"
#include "dell-base-if-vlan.h"
#include "dell-base-common.h"
#include "ds_api_linux_interface.h"

#include "iana-if-type.h"
#include "ietf-interfaces.h"
#include "ietf-ip.h"
#include "ietf-network-instance.h"

#include <sys/socket.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if.h>

#include <pthread.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <map>
#include <unordered_map>

#define NAS_OS_IF_OBJ_ID_RES_START 4
#define NAS_OS_IF_FLAGS_ID (NAS_OS_IF_OBJ_ID_RES_START) //reserve 4 inside the object for flags
#define NAS_OS_IF_ALIAS (NAS_OS_IF_OBJ_ID_RES_START+1)

#define NAS_LINK_MTU_HDR_SIZE 32
#define NL_MSG_INTF_BUFF_LEN 2048

/*
 * API to mask *any* interface publish event.
 * e.g admin-state in case of LAG member port add
 */
extern "C" bool os_interface_mask_event(hal_ifindex_t ifix, if_change_t mask_val)
{
    INTERFACE *fill = os_get_if_db_hdlr();

    if(fill) {
        return fill->if_info_setmask(ifix, mask_val);
    }
    return true;
}

static bool is_reserved_interface (if_details &details)
{
    if ((strncmp(details.if_name.c_str(), "eth", strlen("eth")) == 0)
            || (strncmp(details.if_name.c_str(), "mgmt", strlen("mgmt")) == 0)
            || (strncmp(details.if_name.c_str(), "veth-", strlen("veth-")) == 0)
            || (strncmp(details.if_name.c_str(), "vdef-", strlen("vdef-")) == 0)) {
        return true;
    }

    return false;

}


std::string nas_os_if_name_get(hal_ifindex_t ifix) {
    INTERFACE *fill = os_get_if_db_hdlr();

    if (!fill) return nullptr;

    EV_LOGGING(NAS_OS, INFO, "NAS-OS-CACHE", "get name  for ifindex %d", ifix);
    return (fill->if_info_get_name(ifix));
}

t_std_error os_intf_type_get(hal_ifindex_t ifix, BASE_CMN_INTERFACE_TYPE_t *if_type) {
    INTERFACE *fill = os_get_if_db_hdlr();

    if (!fill) return STD_ERR(INTERFACE,FAIL,0);

    EV_LOGGING(NAS_OS, INFO, "NAS-OS-CACHE", "get type  for ifindex %d", ifix);
    BASE_CMN_INTERFACE_TYPE_t  _type;
    _type = fill->if_info_get_type(ifix);
    if ( _type != BASE_CMN_INTERFACE_TYPE_NULL) {
        EV_LOGGING(NAS_OS, INFO, "NAS-OS-CACHE", "get type  %d for ifindex %d", _type, ifix );
        *if_type = _type;
        return STD_ERR_OK;
    }
    return STD_ERR(INTERFACE,FAIL,0);
}
t_std_error os_intf_master_get(hal_ifindex_t ifix, hal_ifindex_t *master_idx) {

    if (master_idx == NULL)  return STD_ERR(INTERFACE,FAIL,0);
    INTERFACE *fill = os_get_if_db_hdlr();
    if (!fill) return STD_ERR(INTERFACE,FAIL,0);

    *master_idx = fill->if_info_get_master(ifix);
    EV_LOGGING(NAS_OS, INFO, "NAS-OS-CACHE", "Master is  %d for ifindex %d", *master_idx, ifix );
    return STD_ERR_OK;
}

static auto info_kind_to_intf_type  = new std::unordered_map<std::string,BASE_CMN_INTERFACE_TYPE_t>
{
    {"tun", BASE_CMN_INTERFACE_TYPE_L3_PORT},
    {"bond", BASE_CMN_INTERFACE_TYPE_LAG},
    {"bridge", BASE_CMN_INTERFACE_TYPE_BRIDGE},
    {"vlan", BASE_CMN_INTERFACE_TYPE_VLAN_SUBINTF},
    {"macvlan", BASE_CMN_INTERFACE_TYPE_MACVLAN},
    {"vxlan", BASE_CMN_INTERFACE_TYPE_VXLAN},
    {"dummy", BASE_CMN_INTERFACE_TYPE_LOOPBACK},
};
static t_std_error nas_os_info_kind_to_intf_type(const char *info_kind, BASE_CMN_INTERFACE_TYPE_t *if_type)
{
    auto it = info_kind_to_intf_type->find(std::string(info_kind));
    if(it != info_kind_to_intf_type->end()){
        *if_type = it->second;
        return STD_ERR_OK;
    }
    return STD_ERR(INTERFACE,FAIL,0);
}

static bool get_if_detail_from_netlink (int sock, int rt_msg_type, struct nlmsghdr *hdr, void *data,
                                        uint32_t vrf_id) {

    if (rt_msg_type > RTM_SETLINK) {
        EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "Wrong netlink msg msgType %d", rt_msg_type);
        return false;
    }
    if_details *details = (if_details *)data;

    struct ifinfomsg *ifmsg = (struct ifinfomsg *)NLMSG_DATA(hdr);

    if(hdr->nlmsg_len < NLMSG_LENGTH(sizeof(*ifmsg)))
        return false;

    details->_ifindex = ifmsg->ifi_index;
    details->_op = cps_api_oper_NULL;
    details->_family = ifmsg->ifi_family;
    details->_flags = ifmsg->ifi_flags;
    details->_type = BASE_CMN_INTERFACE_TYPE_L3_PORT;


    EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "## msgType %d, ifindex %d change %x\n",
           rt_msg_type, ifmsg->ifi_index, ifmsg->ifi_change);

    int nla_len = nlmsg_attrlen(hdr,sizeof(*ifmsg));
    struct nlattr *head = nlmsg_attrdata(hdr, sizeof(struct ifinfomsg));

    memset(details->_attrs,0,sizeof(details->_attrs));

    if (nla_parse(details->_attrs,__IFLA_MAX,head,nla_len)!=0) {
        EV_LOGGING(NAS_OS, ERR,"NL-PARSE","Failed to parse attributes");
        return false;
    }
    return true;
}

bool nas_os_if_index_get(std::string &if_name , hal_ifindex_t &index) {

    INTERFACE *fill = os_get_if_db_hdlr();
    if (!fill) return false;

    if_info_t ifinfo;
    if(!fill->get_ifindex_from_name(if_name, index)) {
        /*  if not present then check in the kernel */
        index = cps_api_interface_name_to_if_index(if_name.c_str());
    }
    return true;
}

 bool check_bridge_membership_in_os(hal_ifindex_t bridge_idx, hal_ifindex_t mem_idx)
{
    int if_sock = 0;
    if((if_sock = nas_nl_sock_create(NL_DEFAULT_VRF_NAME, nas_nl_sock_T_INT,false)) < 0) {
        EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "Socket create failure");
       return false;
    }

    const int BUFF_LEN=4196;
    char buff[BUFF_LEN];
    if_details details;
    memset(buff,0,sizeof(buff));
    memset(static_cast<void *>(&details),0,sizeof(details));


    struct ifinfomsg ifmsg;
    memset(&ifmsg,0,sizeof(ifmsg));

    nas_os_pack_if_hdr(&ifmsg, AF_NETLINK, 0, mem_idx );

    int seq = (int)std_get_uptime(NULL);
    if (nl_send_request(if_sock, RTM_GETLINK, (NLM_F_REQUEST | NLM_F_ACK ), seq,&ifmsg, sizeof(ifmsg))) {
        netlink_tools_process_socket(if_sock,get_if_detail_from_netlink,
                &details,buff,sizeof(buff),&seq,NULL, NL_DEFAULT_VRF_ID);
    }

    if (details._ifindex != mem_idx) {
        // returned msg is not for the same member
        EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "member index %d and received index %d mismatch", mem_idx, details._ifindex);
        close(if_sock);
        return false;
    }
    if(details._attrs[IFLA_MASTER]!=NULL){
        hal_ifindex_t master_idx = *(int *)nla_data(details._attrs[IFLA_MASTER]);
        EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "member name %d and received bridge index %d", details._ifindex, master_idx);
        if (master_idx == bridge_idx) {
            // interface is the member of the bridge in OS
            close(if_sock);
            return true;
        }
    }
    EV_LOGGING(NAS_OS, INFO, "NET-MAIN", " no bridge info or wrong bridge found with the interface %d bridge idx %d ",
                 details._ifindex, bridge_idx);
    close(if_sock);
    return false;
}

t_std_error os_interface_to_object (int rt_msg_type, struct nlmsghdr *hdr, cps_api_object_t obj, bool* p_pub_evt,
                                    uint32_t vrf_id)
{
    struct ifinfomsg *ifmsg = (struct ifinfomsg *)NLMSG_DATA(hdr);

    if(hdr->nlmsg_len < NLMSG_LENGTH(sizeof(*ifmsg))) {
        EV_LOGGING(NAS_OS, ERR, "NL-PARSE", "Invalid msg length in netlink header: %d", hdr->nlmsg_len);
        return STD_ERR(INTERFACE, PARAM, 0);
    }

    int track_change = OS_IF_CHANGE_NONE;
    if_details details;
    if_info_t ifinfo;

    details._op = cps_api_oper_NULL;
    details._family = ifmsg->ifi_family;

    static const std::unordered_map<int,cps_api_operation_types_t> _to_op_type = {
            { RTM_NEWLINK, cps_api_oper_CREATE },
            { RTM_DELLINK, cps_api_oper_DELETE }
    };

    EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "VRF-id:%d msgType %d, ifindex %d change 0x%x flags 0x%x\n",
           vrf_id, rt_msg_type, ifmsg->ifi_index, ifmsg->ifi_change, ifmsg->ifi_flags);

    auto it = _to_op_type.find(rt_msg_type);
    if (it==_to_op_type.end()) {
        details._op = cps_api_oper_SET;
    } else {
        details._op = it->second;
    }

    details._flags = ifmsg->ifi_flags;
    details._type = BASE_CMN_INTERFACE_TYPE_L3_PORT;
    details._ifindex = ifmsg->ifi_index;
    details.master_idx = 0;
    details.parent_idx = 0;

    // Check if the interface already exists in the local database.
    // If present then mark it as SET operation otherwise CREATE operation.
    if ((vrf_id == NAS_DEFAULT_VRF_ID) && (details._op == cps_api_oper_CREATE)) {
        bool present = false;
        if (nas_os_check_intf_exists(details._ifindex, &present) != STD_ERR_OK) {
            EV_LOGGING(NAS_OS, ERR, "NET-MAIN", " Failed to get cached info");
            return false;
        }
        if (present) { details._op = cps_api_oper_SET; }
    }
    cps_api_operation_types_t _if_op = details._op;
    cps_api_object_attr_add_u32(obj,BASE_IF_LINUX_IF_INTERFACES_INTERFACE_IF_FLAGS, details._flags);
    cps_api_object_attr_add_u32(obj, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX,ifmsg->ifi_index);

    cps_api_object_attr_add_u32(obj,IF_INTERFACES_INTERFACE_ENABLED,
        (ifmsg->ifi_flags & IFF_UP) ? true :false);

    ifinfo.parent_idx = 0;
    ifinfo.ev_mask = OS_IF_CHANGE_NONE;
    ifinfo.admin = (ifmsg->ifi_flags & IFF_UP) ? true :false;
    ifinfo.oper = (ifmsg->ifi_flags & IFF_RUNNING) ? true :false;

    int nla_len = nlmsg_attrlen(hdr,sizeof(*ifmsg));
    struct nlattr *head = nlmsg_attrdata(hdr, sizeof(struct ifinfomsg));

    memset(details._attrs,0,sizeof(details._attrs));
    memset(details._linkinfo,0,sizeof(details._linkinfo));
    details._info_kind = nullptr;

    if (nla_parse(details._attrs,__IFLA_MAX,head,nla_len)!=0) {
        EV_LOGGING(NAS_OS,ERR,"NL-PARSE","Failed to parse attributes");
        return STD_ERR(INTERFACE, FAIL, 0);
    }

    if (details._attrs[IFLA_LINKINFO]) {
        nla_parse_nested(details._linkinfo,IFLA_INFO_MAX,details._attrs[IFLA_LINKINFO]);
    }

    if (details._attrs[IFLA_LINKINFO] != nullptr && details._linkinfo[IFLA_INFO_KIND]!=nullptr) {
        details._info_kind = (const char *)nla_data(details._linkinfo[IFLA_INFO_KIND]);
        ifinfo.os_link_type.assign(details._info_kind,strlen(details._info_kind));
        EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Intf type %s ifindex %d", ifinfo.os_link_type.c_str(), ifmsg->ifi_index);
    }

    if (details._attrs[IFLA_ADDRESS]!=NULL) {
        char buff[40];
        const char *_p = std_mac_to_string((const hal_mac_addr_t *)(nla_data(details._attrs[IFLA_ADDRESS])), buff, sizeof(buff));
        cps_api_object_attr_add(obj,DELL_IF_IF_INTERFACES_INTERFACE_PHYS_ADDRESS,_p,strlen(_p)+1);
        memcpy(ifinfo.phy_addr, (nla_data(details._attrs[IFLA_ADDRESS])), sizeof(hal_mac_addr_t));
    }

    if (details._attrs[IFLA_IFNAME]!=NULL) {
        rta_add_name(details._attrs[IFLA_IFNAME],obj,IF_INTERFACES_INTERFACE_NAME);
        details.if_name = static_cast <char *> (nla_data(details._attrs[IFLA_IFNAME]));
    }

    if(details._attrs[IFLA_MTU]!=NULL) {
        int *mtu = (int *) nla_data(details._attrs[IFLA_MTU]);
        cps_api_object_attr_add_u32(obj, DELL_IF_IF_INTERFACES_INTERFACE_MTU,(*mtu + NAS_LINK_MTU_HDR_SIZE));
        ifinfo.mtu = *mtu;
    }

    if (details._attrs[IFLA_LINK] != NULL) {
        EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Rcvd Link index index %d",
                *(int *)nla_data(details._attrs[IFLA_LINK]));
    }

    if(details._attrs[IFLA_MASTER]!=NULL) {
            /* This gives us the bridge index, which should be sent to
             * NAS for correlation  */
        EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Rcvd master index %d",
                *(int *)nla_data(details._attrs[IFLA_MASTER]));
        ifinfo.master_idx = *(int *)nla_data(details._attrs[IFLA_MASTER]);
        cps_api_object_attr_add_u32(obj,BASE_IF_LINUX_IF_INTERFACES_INTERFACE_IF_MASTER,
                                   *(int *)nla_data(details._attrs[IFLA_MASTER]));

    }
    if (details._info_kind != nullptr) {
        if ( nas_os_info_kind_to_intf_type(details._info_kind, &details._type) != STD_ERR_OK) {
            EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "info kind not known %s",details._info_kind);
        }
    } else {
        // In case if info_kind not present in the netlink event then look into
        // the local cache.
        EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "info kind not present in netlink %d",details._ifindex);
        if (os_intf_type_get(details._ifindex, &details._type) != STD_ERR_OK) {
                EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "unknown interface %d", details._ifindex);
        }
    }

    INTERFACE *fill = os_get_if_db_hdlr();
    if_change_t mask = OS_IF_CHANGE_NONE;
    if(fill && !(fill->if_hdlr(&details, obj))) {
        EV_LOGGING(NAS_OS, INFO, "NL-PARSE", "Failure on sub-interface handling");
        return STD_ERR(INTERFACE, FAIL, 0); // Return in case of sub-interfaces etc (Handler will return false)
    }

    ifinfo.if_type = details._type;
    ifinfo.if_name = details.if_name;
    ifinfo.parent_idx = details.parent_idx;

    bool evt_publish = true;
    /* Dont update the intf cache for non-default VRF since if-index can be same in multiple VRFs */
    if (vrf_id == NAS_DEFAULT_VRF_ID) {

        if (!fill) {
            track_change = OS_IF_CHANGE_ALL;
        } else {
            track_change = fill->if_info_update(ifmsg->ifi_index, ifinfo);
        }
        /*
         * Delete the interface from cache if interface type is not vlan or lag
         * If lag, check for lag member delete vs actual bond interface delete.
         */
        EV_LOGGING(NAS_OS,INFO,"NET-MAIN","ifidx %d, if-type %d track %d",
                   ifmsg->ifi_index,details._type, track_change);

        if(_if_op == cps_api_oper_DELETE) {
            if((details._type != BASE_CMN_INTERFACE_TYPE_L2_PORT)&&
               (details._type != BASE_CMN_INTERFACE_TYPE_LAG)&&
               (details._type != BASE_CMN_INTERFACE_TYPE_MACVLAN)) {
                if(fill) fill->if_info_delete(ifmsg->ifi_index, details.if_name);
            } else if(details._type == BASE_CMN_INTERFACE_TYPE_LAG &&
                      (!strncmp(details._info_kind, "bond", 4))) {
                if(fill) fill->if_info_delete(ifmsg->ifi_index, details.if_name);
            }
            if(details._type == BASE_CMN_INTERFACE_TYPE_L2_PORT) {
                ifinfo.master_idx = 0; // in case of L2 PORT member delete.
                fill->if_info_update(ifmsg->ifi_index, ifinfo);
            }
        }

        if ((details._type == BASE_CMN_INTERFACE_TYPE_L2_PORT) ||
            ((details._type == BASE_CMN_INTERFACE_TYPE_LAG) && (details._attrs[IFLA_MASTER] != NULL))) {
            /*
             * If member addition/deletion in the LAG or bridge
             */
            int ifix = *(int *)nla_data(details._attrs[IFLA_MASTER]);
            std::string if_name = nas_os_if_name_get(ifix);
            if (if_name.empty()) {
                EV_LOGGING(NAS_OS,ERR,"NET-MAIN"," Interface not present, index %d ", ifix);
                return STD_ERR(INTERFACE, FAIL, 0);
            }
            EV_LOGGING(NAS_OS,INFO,"NET-MAIN"," ifidx %d Remove attrs in case of member add/del to master %s",
                       ifmsg->ifi_index, if_name.c_str());
            if (details._type == BASE_CMN_INTERFACE_TYPE_L2_PORT) {
                // Delete the previously filled attributes in case of Vlan/Lag member add/del
                cps_api_object_attr_delete(obj, DELL_IF_IF_INTERFACES_INTERFACE_MTU);
                cps_api_object_attr_delete(obj, DELL_IF_IF_INTERFACES_INTERFACE_PHYS_ADDRESS);
                cps_api_object_attr_delete(obj, IF_INTERFACES_INTERFACE_NAME);
                cps_api_object_attr_delete(obj, IF_INTERFACES_INTERFACE_ENABLED);
                cps_api_object_attr_delete(obj, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
                cps_api_object_attr_add(obj, IF_INTERFACES_INTERFACE_NAME, if_name.c_str(), (strlen(if_name.c_str())+1));
                cps_api_object_attr_add_u32(obj, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX,ifix);
                cps_api_object_attr_add_u32(obj, BASE_IF_LINUX_IF_INTERFACES_INTERFACE_MBR_IFINDEX, ifmsg->ifi_index);
            }
        } else if (!track_change) {
            /*
             * If track change is false, check for interface type - Return false ONLY in the individual update case
             * If the handler has identified it as VLAN or Bond member addition, then continue with publishing
             */
            if (details._op != cps_api_oper_DELETE && details._type != BASE_CMN_INTERFACE_TYPE_L2_PORT) {

                /*
                 * Avoid filtering netlink events for reserved interface (eth0/mgmtxxx-xx).
                 * check if its not a reserved interface return false otherwise contiue publishing
                 * the CPS interface object.
                 */
                if (!is_reserved_interface(details)) {
                    evt_publish = false;
                }
            }

            // If mask is set to disable admin state publish event, remove the attribute
        } else if(fill && (mask = fill->if_info_getmask(ifmsg->ifi_index))) {
            EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Masking set for %d, mask %d, track_chg %d",
                       ifmsg->ifi_index, mask, track_change);
            if(track_change != OS_IF_ADM_CHANGE && mask == OS_IF_ADM_CHANGE)
                cps_api_object_attr_delete(obj, IF_INTERFACES_INTERFACE_ENABLED);
            else if (mask == OS_IF_ADM_CHANGE)
                evt_publish = false;
        }
    }
    if (p_pub_evt != NULL) {
        *p_pub_evt = evt_publish;
    }

    const char *vrf_name = nas_os_get_vrf_name(vrf_id);
    if (vrf_name == NULL) {
        EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "VRF id:%d to name mapping not present, index %d type %d!",
                   vrf_id, details._ifindex, details._type);
        return STD_ERR(INTERFACE, PARAM, 0);
    }
    cps_api_object_attr_add(obj, NI_IF_INTERFACES_INTERFACE_BIND_NI_NAME, vrf_name, strlen(vrf_name)+1);
    cps_api_object_attr_add_u32(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID, vrf_id);
    EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "VRF:%s(%d) Publishing index %d type %d",
               vrf_name, vrf_id, details._ifindex, details._type);

    cps_api_key_from_attr_with_qual(cps_api_object_key(obj),BASE_IF_LINUX_IF_INTERFACES_INTERFACE_OBJ,
            cps_api_qualifier_OBSERVED);
    cps_api_object_set_type_operation(cps_api_object_key(obj),details._op);
    cps_api_object_attr_add_u32(obj, BASE_IF_LINUX_IF_INTERFACES_INTERFACE_DELL_TYPE, details._type);
    EV_LOGGING(NAS_OS, INFO, "NET-MAIN"," NAS  OS interface event type \n %s",
            (track_change ==OS_IF_CHANGE_ALL) ? "Change all" : "Interface Update");
    EV_LOGGING(NAS_OS, INFO, "NET-MAIN"," NAS  OS interface event publish\n %s", cps_api_object_to_c_string(obj).c_str());

    return STD_ERR_OK;
}

static bool get_netlink_data(int sock, int rt_msg_type, struct nlmsghdr *hdr, void *data, uint32_t vrf_id) {
    if (rt_msg_type <= RTM_SETLINK) {
        cps_api_object_list_t * list = (cps_api_object_list_t*)data;
        cps_api_object_guard og(cps_api_object_create());
        if (!og.valid()) return false;

        if (os_interface_to_object(rt_msg_type,hdr,og.get(), NULL, vrf_id) == STD_ERR_OK) {
            if (cps_api_object_list_append(*list,og.get())) {
                og.release();
                return true;
            }
        }
        return false;
    }
    return true;
}

static bool os_interface_info_to_object(hal_ifindex_t ifix, if_info_t& ifinfo, cps_api_object_t obj)
{
    char if_name[HAL_IF_NAME_SZ+1];
    if(cps_api_interface_if_index_to_name(ifix, if_name, sizeof(if_name)) == NULL) {
        EV_LOGGING(NAS_OS, ERR, "NAS-OS", "Failure getting interface name for %d", ifix);
        return false;
    } else
        cps_api_object_attr_add(obj, IF_INTERFACES_INTERFACE_NAME, if_name, (strlen(if_name)+1));
    cps_api_key_from_attr_with_qual(cps_api_object_key(obj), BASE_IF_LINUX_IF_INTERFACES_INTERFACE_OBJ,
            cps_api_qualifier_OBSERVED);

    cps_api_object_attr_add_u32(obj, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX, ifix);

    cps_api_object_attr_add_u32(obj,IF_INTERFACES_INTERFACE_ENABLED, ifinfo.admin);

    char buff[HAL_INET6_TEXT_LEN];
    const char *_p = std_mac_to_string((const hal_mac_addr_t *)ifinfo.phy_addr, buff, sizeof(buff));
    cps_api_object_attr_add(obj,DELL_IF_IF_INTERFACES_INTERFACE_PHYS_ADDRESS,_p,strlen(_p)+1);
    cps_api_object_attr_add_u32(obj, DELL_IF_IF_INTERFACES_INTERFACE_MTU,
                                (ifinfo.mtu  + NAS_LINK_MTU_HDR_SIZE));
    cps_api_object_attr_add_u32(obj, BASE_IF_LINUX_IF_INTERFACES_INTERFACE_DELL_TYPE, ifinfo.if_type);
    return true;
}

static cps_api_return_code_t _get_db_interface( cps_api_object_list_t *list, hal_ifindex_t ifix,
                                                bool get_all, uint_t if_type )
{
    INTERFACE *fill = os_get_if_db_hdlr();

    if (!fill) return cps_api_ret_code_ERR;

    if_info_t ifinfo;
    if(!get_all && fill->if_info_get(ifix, ifinfo)) {
        EV_LOGGING(NAS_OS, INFO,"NET-MAIN", "Get ifinfo for %d", ifix);

        cps_api_object_t obj = cps_api_object_create();
        if(obj == nullptr) return cps_api_ret_code_ERR;
        if(!os_interface_info_to_object(ifix, ifinfo, obj)) {
            cps_api_object_delete(obj);
            return cps_api_ret_code_ERR;
        }

        cps_api_object_set_type_operation(cps_api_object_key(obj),cps_api_oper_NULL);
        if (cps_api_object_list_append(*list,obj)) {
            return cps_api_ret_code_OK;
        }
        else {
            cps_api_object_delete(obj);
            return cps_api_ret_code_ERR;
        }
    } else if (get_all) {
        fill->for_each_mbr([if_type, &list](int idx, if_info_t& ifinfo) {
            if(if_type != 0 && ifinfo.if_type != static_cast<BASE_CMN_INTERFACE_TYPE_t>(if_type)) {
                return;
            }

            EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Get all ifinfo for %d", idx);

            cps_api_object_t obj = cps_api_object_create();
            if(obj == nullptr) return;
            if(!os_interface_info_to_object(idx, ifinfo, obj)) {
                cps_api_object_delete(obj);
                return;
            }
            cps_api_object_attr_t attr_id = cps_api_object_attr_get(obj,
                    BASE_IF_LINUX_IF_INTERFACES_INTERFACE_DELL_TYPE);
            if (attr_id != NULL) {
                BASE_CMN_INTERFACE_TYPE_t type = (BASE_CMN_INTERFACE_TYPE_t)
                                                  cps_api_object_attr_data_uint(attr_id);
                if (type == BASE_CMN_INTERFACE_TYPE_MANAGEMENT) {
                    attr_id = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_NAME);
                    if (attr_id != NULL) {
                        char if_name[HAL_IF_NAME_SZ+1];
                        safestrncpy(if_name, (const char*)cps_api_object_attr_data_bin(attr_id),
                                HAL_IF_NAME_SZ);
                        os_get_interface_ethtool_cmd_data(if_name, obj);
                        os_get_interface_oper_status(if_name, obj);
                    }
                }
            }
            cps_api_object_set_type_operation(cps_api_object_key(obj),cps_api_oper_NULL);
            if (cps_api_object_list_append(*list,obj)) {
                return;
            } else {
                cps_api_object_delete(obj);
                return;
            }

        });
        return cps_api_ret_code_OK;
    }

    return cps_api_ret_code_ERR;
}
cps_api_return_code_t _get_interfaces( cps_api_object_list_t list, hal_ifindex_t ifix,
                                       bool get_all, uint_t if_type )
{

    if(_get_db_interface(&list, ifix, get_all, if_type) == cps_api_ret_code_OK)
        return cps_api_ret_code_OK;

    EV_LOGGING(NAS_OS, INFO, "NET-MAIN", "Get interface info for ifindex %d", ifix);
    int if_sock = 0;
    if((if_sock = nas_nl_sock_create(NL_DEFAULT_VRF_NAME, nas_nl_sock_T_INT,false)) < 0) {
        EV_LOGGING(NAS_OS, ERR, "NET-MAIN", "soc create failure for get ifindex %d", ifix);
        return cps_api_ret_code_ERR;
    }

    const int BUFF_LEN=4196;
    char buff[BUFF_LEN];
    memset(buff,0,sizeof(buff));

    struct ifinfomsg ifmsg;
    memset(&ifmsg,0,sizeof(ifmsg));

    nas_os_pack_if_hdr(&ifmsg, AF_NETLINK, 0, ifix );

    int seq = (int)pthread_self();
    int dump_flags = NLM_F_ROOT| NLM_F_DUMP;

    if (nl_send_request(if_sock, RTM_GETLINK,
            (NLM_F_REQUEST | NLM_F_ACK | (get_all ? dump_flags : 0)),
            seq,&ifmsg, sizeof(ifmsg))) {
        netlink_tools_process_socket(if_sock,get_netlink_data,
                &list,buff,sizeof(buff),&seq,NULL, NL_DEFAULT_VRF_ID);
    }

    size_t mx = cps_api_object_list_size(list);
    for (size_t ix = 0 ; ix < mx ; ++ix ) {
        cps_api_object_t ret = cps_api_object_list_get(list,ix);
        STD_ASSERT(ret!=NULL);
        cps_api_object_set_type_operation(cps_api_object_key(ret),cps_api_oper_NULL);
        cps_api_object_attr_t attr_id = cps_api_object_attr_get(ret,
                BASE_IF_LINUX_IF_INTERFACES_INTERFACE_DELL_TYPE);
        if (attr_id != NULL) {
           BASE_CMN_INTERFACE_TYPE_t type = (BASE_CMN_INTERFACE_TYPE_t)
                                              cps_api_object_attr_data_uint(attr_id);
           if (type == BASE_CMN_INTERFACE_TYPE_MANAGEMENT) {
               attr_id = cps_api_object_attr_get(ret, IF_INTERFACES_INTERFACE_NAME);
               if (attr_id != NULL) {
                   char if_name[HAL_IF_NAME_SZ+1];
                   safestrncpy(if_name, (const char*)cps_api_object_attr_data_bin(attr_id),
                           HAL_IF_NAME_SZ);
                   os_get_interface_ethtool_cmd_data(if_name, ret);
                   os_get_interface_oper_status(if_name, ret);
               }
           }
        }
    }

    close(if_sock);
    return cps_api_ret_code_OK;
}

cps_api_return_code_t __rd(void * context, cps_api_get_params_t * param,  size_t key_ix) {

    cps_api_object_t obj = cps_api_object_list_get(param->filters,key_ix);
    STD_ASSERT(obj!=nullptr);

    if (nas_os_get_interface(obj,param->list)==STD_ERR_OK) {
        return cps_api_ret_code_OK;
    }

    return cps_api_ret_code_ERR;
}

cps_api_return_code_t __wr(void * context, cps_api_transaction_params_t * param,size_t ix) {

    return cps_api_ret_code_ERR;
}

t_std_error os_interface_object_reg(cps_api_operation_handle_t handle) {
    cps_api_registration_functions_t f;
    memset(&f,0,sizeof(f));

    char buff[CPS_API_KEY_STR_MAX];
    if (!cps_api_key_from_attr_with_qual(&f.key, BASE_IF_LINUX_IF_INTERFACES_INTERFACE,cps_api_qualifier_TARGET)) {
        EV_LOGGING(NAS_OS, ERR,"NAS-IF-REG","Could not translate %d to key %s",
            (int)(BASE_IF_LINUX_IF_INTERFACES_INTERFACE),cps_api_key_print(&f.key,buff,sizeof(buff)-1));
        return STD_ERR(INTERFACE,FAIL,0);
    }

    f.handle = handle;
    f._read_function = __rd;
    f._write_function = __wr;

    if (cps_api_register(&f)!=cps_api_ret_code_OK) {
        return STD_ERR(INTERFACE,FAIL,0);
    }

    return STD_ERR_OK;

}

extern "C" t_std_error os_intf_admin_state_get(hal_ifindex_t ifix, bool *p_admin_status) {
    INTERFACE *fill = os_get_if_db_hdlr();
    bool admin = false;

    if (!fill) return STD_ERR(INTERFACE,FAIL,0);

    if(fill->if_info_get_admin(ifix, admin)) {
        *p_admin_status = admin;
        return STD_ERR_OK;
    }
    return STD_ERR(INTERFACE,FAIL,0);
}

extern "C" t_std_error os_intf_mac_addr_get(hal_ifindex_t ifix, hal_mac_addr_t mac) {
    INTERFACE *fill = os_get_if_db_hdlr();
    if_info_t if_info;

    if (!fill) return STD_ERR(INTERFACE,FAIL,0);

    if(fill->if_info_get(ifix, if_info)) {
        memcpy(mac, if_info.phy_addr, HAL_MAC_ADDR_LEN);
        return STD_ERR_OK;
    }
    return STD_ERR(INTERFACE,FAIL,0);
}
extern "C" t_std_error os_get_interface_oper_status(const char *ifname, cps_api_object_t obj)
{
    IF_INTERFACES_STATE_INTERFACE_OPER_STATUS_t oper_status;
    cps_api_object_attr_t   attr = cps_api_object_attr_get(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID);
    uint32_t                vrf_id = NAS_DEFAULT_VRF_ID;
    const char              *vrf_name = NULL;

    if (attr != NULL) {
        vrf_id = cps_api_object_attr_data_uint(attr);
        vrf_name = nas_os_get_vrf_name(vrf_id);
    }

    t_std_error ret = nas_os_util_int_oper_status_get(vrf_name, ifname, &oper_status);
    if (ret == STD_ERR_OK) {
        cps_api_object_attr_add_u32(obj, IF_INTERFACES_STATE_INTERFACE_OPER_STATUS, oper_status);
    }
    return ret;
}

extern "C"  t_std_error os_get_interface_ethtool_cmd_data(const char *ifname, cps_api_object_t obj)
{
    ethtool_cmd_data_t eth_cmd;
    uint32_t           idx = 0;
    cps_api_object_attr_t   attr = cps_api_object_attr_get(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID);
    uint32_t           vrf_id = NAS_DEFAULT_VRF_ID;
    const char  *vrf_name = NULL;

    if (attr != NULL) {
        vrf_id = cps_api_object_attr_data_uint(attr);
        vrf_name = nas_os_get_vrf_name(vrf_id);
    }
    memset(&eth_cmd, 0, sizeof(eth_cmd));

    t_std_error ret = nas_os_util_int_ethtool_cmd_data_get(vrf_name, ifname, &eth_cmd);
    if (ret == STD_ERR_OK) {
        cps_api_object_attr_add_u32(obj, DELL_IF_IF_INTERFACES_INTERFACE_SPEED,
                eth_cmd.speed);
        cps_api_object_attr_add_u32(obj, DELL_IF_IF_INTERFACES_STATE_INTERFACE_DUPLEX,
                eth_cmd.duplex);
        cps_api_object_attr_add(obj, DELL_IF_IF_INTERFACES_STATE_INTERFACE_AUTO_NEGOTIATION,
                &eth_cmd.autoneg, sizeof(eth_cmd.autoneg));
        for (idx = 0; idx < BASE_IF_SPEED_MAX; idx++) {
            if (eth_cmd.supported_speed[idx] == true) {
                cps_api_object_attr_add_u32(obj,
                        DELL_IF_IF_INTERFACES_STATE_INTERFACE_SUPPORTED_SPEED, idx);
            }
        }
    }
    return ret;
}

extern "C"  t_std_error os_set_interface_ethtool_cmd_data (const char *ifname, cps_api_object_t obj)
{
    ethtool_cmd_data_t  eth_cmd;
    t_std_error         ret = STD_ERR_OK;
    cps_api_object_attr_t sp_attr, dup_attr, an_attr;

    cps_api_object_attr_t   attr = cps_api_object_attr_get(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID);
    uint32_t           vrf_id = NAS_DEFAULT_VRF_ID;
    const char         *vrf_name = NULL;

    if (attr != NULL) {
        vrf_id = cps_api_object_attr_data_uint(attr);
        vrf_name = nas_os_get_vrf_name(vrf_id);
    }
    memset(&eth_cmd, 0, sizeof(eth_cmd));

    sp_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_SPEED);
    dup_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_DUPLEX);
    an_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_AUTO_NEGOTIATION);

    eth_cmd.speed = BASE_IF_SPEED_AUTO;
    eth_cmd.duplex = BASE_CMN_DUPLEX_TYPE_AUTO;
    eth_cmd.autoneg = true;
    if (sp_attr != NULL) {
        eth_cmd.speed = (BASE_IF_SPEED_t)cps_api_object_attr_data_uint(sp_attr);
    }
    if (dup_attr != NULL) {
        eth_cmd.duplex = (BASE_CMN_DUPLEX_TYPE_t)cps_api_object_attr_data_uint(dup_attr);
    }
    if (an_attr != NULL) {
        eth_cmd.autoneg = cps_api_object_attr_data_uint(an_attr);
    }

    ret = nas_os_util_int_ethtool_cmd_data_set(vrf_name, ifname, &eth_cmd);

    return ret;
}

extern "C"  t_std_error os_get_interface_stats (const char *ifname, cps_api_object_t obj)
{
    os_int_stats_t data;
    uint32_t       vrf_id = NAS_DEFAULT_VRF_ID;
    const char     *vrf_name = NULL;

    cps_api_object_attr_t   attr = cps_api_object_attr_get(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID);
    if (attr != NULL) {
        vrf_id = cps_api_object_attr_data_uint(attr);
        vrf_name = nas_os_get_vrf_name(vrf_id);
    }
    memset(&data, 0, sizeof(data));

    t_std_error ret = nas_os_util_int_stats_get(vrf_name, ifname, &data);
    if (ret == STD_ERR_OK) {
        cps_api_object_attr_add_u64(obj, DELL_IF_IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_PKTS,
                data.input_packets);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_OCTETS,
                data.input_bytes);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_MULTICAST_PKTS,
                data.input_multicast);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_ERRORS,
                data.input_errors);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_DISCARDS,
                data.input_discards);
        cps_api_object_attr_add_u64(obj, DELL_IF_IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_PKTS,
                data.output_packets);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_OCTETS,
                data.output_bytes);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_MULTICAST_PKTS,
                data.output_multicast);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_ERRORS,
                data.output_errors);
        cps_api_object_attr_add_u64(obj, IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_DISCARDS,
                data.output_invalid_protocol);
    }
    return ret;
}
