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
 * filename : nas_os_l3_utils.h
 */

#ifndef NAS_OS_L3_UTILS_H_
#define NAS_OS_L3_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _nas_rt_msg_type {
    NAS_RT_ADD,
    NAS_RT_DEL,
    NAS_RT_SET,
    NAS_RT_REFRESH,
    NAS_RT_RESOLVE,
    NAS_RT_SET_STATE
}nas_rt_msg_type;

t_std_error nas_os_update_vrf(cps_api_object_t obj, nas_rt_msg_type m_type);
t_std_error nas_os_handle_intf_to_mgmt_vrf(cps_api_object_t obj, nas_rt_msg_type m_type);
t_std_error nas_os_handle_intf_to_vrf(cps_api_object_t obj, nas_rt_msg_type m_type);
bool nas_os_get_vrf_id(const char *vrf_name, uint32_t *p_vrf_id);
const char* nas_os_get_vrf_name(uint32_t vrf_id);
t_std_error nas_remove_intf_to_vrf_binding(uint32_t if_index);
bool nas_rt_is_reserved_intf_idx (unsigned int if_idx, bool sub_intf_check_required);

typedef struct nas_os_ip_info_s {
   uint32_t ip_family;
   uint32_t if_index;
   uint32_t vrf_id;
   bool     filter_if_index;
   cps_api_get_params_t *param;
} nas_os_ip_info_t;

t_std_error os_ip_addr_object_reg(cps_api_operation_handle_t handle);
#ifdef __cplusplus
}
#endif

#endif /* NAS_OS_L3_UTILS_H_ */
