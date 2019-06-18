# Copyright (c) 2019 Dell Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may
# not use this file except in compliance with the License. You may obtain
# a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
#
# THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
# LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
# FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
#
# See the Apache Version 2.0 License for specific language governing
# permissions and limitations under the License.

"""This module provides a OPX base python API constructed
   around the brctl utility"""

import subprocess
import re

BRCTL_CMD = '/sbin/brctl'
BRIDGE_CMD = '/sbin/bridge'

def run_command(cmd, respose):

    """Method to run a command in shell"""

    prt = subprocess.Popen(
        cmd,
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    for line in prt.stdout.readlines():
        respose.append(line.rstrip())
    retval = prt.wait()
    return retval


def _find_field(pattern, key, line):

    """Method to parse regex matching return value"""

    match = re.search(pattern, line)
    if match is None:
        return None
    match = match.groupdict()
    if key in match:
        return match[key]
    return None


def create_br(bname):

    """Method to create a bridge.
    Args:
        bname (str): Name of the Bridge
    Returns:
        bool: The return value. True for success, False otherwise
    """
    res = []
    if run_command([BRCTL_CMD, 'addbr', bname], res) == 0:
        return True
    return False


def del_br(bname):

    """Method to delete a bridge
    Args:
        bname (str): Name of the Bridge
    Returns:
        bool: The return value. True for success, False otherwise
    """

    res = []
    if run_command([BRCTL_CMD, 'delbr', bname], res) == 0:
        return True
    return False


def add_if(bname, iname):

    """Method to add interface to the bridge
    Args:
        bname (str): Name of the Bridge
    Returns:
        bool: The return value. True for success, False otherwise
    """

    res = []
    if run_command([BRCTL_CMD, 'addif', bname, iname], res) == 0:
        return True
    return False


def del_if(bname, iname):

    """Method to delete interface from the bridge
    Args:
        bname (str): Name of the Bridge
        iname (str): Name of the Interface
    Returns:
        bool: The return value. True for success, False otherwise
    """

    res = []
    if run_command([BRCTL_CMD, 'delif', bname, iname], res) == 0:
        return True
    return False


def get_br_list():

    """Method to return a list of all the bridges created on the system
    Args:
        None
    Returns:
        list: The return value. List of bridges for success, None otherwise
    """

    res = []
    ret_list = []
    if run_command([BRCTL_CMD, 'show'], res) == 0:

        for idx in xrange(0, len(res)):

            if idx == 0:
                continue

            else:
                _bname = _find_field(r'(?P<bname>\S+)\s+', 'bname', res[idx])
                if _bname is not None:
                    ret_list.append(_bname)

        return ret_list

    return None


def get_if_list(bname):

    """Method to return a a list of the interfaces that are a part of the bridge
    Args:
        bname (str): Name of the Bridge
    Returns:
        list: The return value. List of member interfaces for success,
              None otherwise
    """

    res = []
    ret_list = []
    if run_command([BRCTL_CMD, 'show', bname], res) == 0:

        for idx in xrange(0, len(res)):

            if idx == 0:
                continue

            elif idx == 1:
                _iname = _find_field(r'\S+\s+\S+\s+\S+\s+(?P<iname>\S+)',
                                     'iname', res[idx])

            else:
                _iname = _find_field(r'\s+(?P<iname>\S+)', 'iname', res[idx])

            ret_list.append(_iname)
        return ret_list

    return None


def get_id(bname):

    """Method to return the bridge ID
    Args:
        bname (str): Name of the Bridge
    Returns:
        str: The return value. Bridge ID for success, None otherwise
    """

    res = []
    if run_command([BRCTL_CMD, 'show', bname], res) == 0:
        _br_id = _find_field(r'\S+\s+(?P<br_id>\S+)\s+', 'br_id', res[1])
        return _br_id
    return None


#bridge fdb { add | append | del } LLADDR dev DEV { local | temp } { self } { embedded } { router } [ dst IPADDR ] [ vni VNI ] [ port PORT ] [ via DEVICE ]
def del_learnt_mac_from_vtep_fdb(iname, dst_ip, addr_family, learnt_mac, vni=None, port=None, via=None):

    """Remove MAC entry learnt on a destination IP in the FDB
    Args:
        iname (str): Name of the Interface
        dst_ip (bin): Destination IP Address
        addr_family (int): Address family of the IP address
        learnt_mac (str): Learnt MAC Address
        vni (int): VxLAN VNI Identifier
        port (int): destination UDP port number to connect to Remote VTEP
        via (str): Interface to reach Remote VTEP
    Returns:
        bool: The return value. True for success, False otherwise
    """

    res = []

    cmd = [BRIDGE_CMD, 'fdb',
           'del', str(learnt_mac),
           'dev', str(iname),
           'dst', dst_ip]

    if vni is not None:
        cmd.append('vni')
        cmd.append(vni)

    if port is not None:
        cmd.append('port')
        cmd.append(port)

    if via is not None:
        cmd.append('via')
        cmd.append(via)

    if run_command(cmd, res) == 0:
        return True
    return False

def add_learnt_mac_to_vtep_fdb(iname, dst_ip, addr_family, learnt_mac, vni=None, port=None, via=None):

    """Add MAC entry learnt on a destination IP in the FDB
    Args:
        iname (str): Name of the Interface
        dst_ip (bin): Destination IP Address
        addr_family (int): Address family of the IP address
        learnt_mac (str): Learnt MAC Address
        vni (int): VxLAN VNI Identifier
        port (int): destination UDP port number to connect to Remote VTEP
        via (str): Interface to reach Remote VTEP
    Returns:
        bool: The return value. True for success, False otherwise
    """

    res = []

    cmd = [BRIDGE_CMD, 'fdb',
           'append', str(learnt_mac),
           'dev', str(iname),
           'dst', dst_ip]

    if vni is not None:
        cmd.append('vni')
        cmd.append(vni)

    if port is not None:
        cmd.append('port')
        cmd.append(port)

    if via is not None:
        cmd.append('via')
        cmd.append(via)

    if run_command(cmd, res) == 0:
        return True
    return False
