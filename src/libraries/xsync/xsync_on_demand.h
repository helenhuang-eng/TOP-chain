// Copyright (c) 2017-2018 Telos Foundation & contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include "xdata/xdata_common.h"
#include "xbasic/xmemory.hpp"
#include "xbase/xutl.h"

#include "xmbus/xmessage_bus.h"
#include "xvledger/xvblock.h"
#include "xsync/xsync_store.h"
#include "xsync/xrole_chains_mgr.h"
#include "xsync/xrole_xips_manager.h"
#include "xsync/xsync_sender.h"
#include "xsync/xsync_download_tracer_mgr.h"

NS_BEG2(top, sync)

const uint32_t max_request_block_count = 100;

class xsync_on_demand_t {
public:
    xsync_on_demand_t(std::string vnode_id, const observer_ptr<mbus::xmessage_bus_face_t> &mbus,
        const observer_ptr<base::xvcertauth_t> &certauth,
        xsync_store_face_t *sync_store,
        xrole_chains_mgr_t *role_chains_mgr,
        xrole_xips_manager_t *role_xips_mgr,
        xsync_sender_t *sync_sender);
    void on_behind_event(const mbus::xevent_ptr_t &e);
    void on_response_event(const std::string account);
    void handle_blocks_request(const xsync_message_get_on_demand_blocks_t &block, 
        const vnetwork::xvnode_address_t &to_address, const vnetwork::xvnode_address_t &network_self);
    void handle_blocks_response(const std::vector<data::xblock_ptr_t> &blocks, 
        const vnetwork::xvnode_address_t &to_address, const vnetwork::xvnode_address_t &network_self);
    void handle_chain_snapshot_meta(xsync_message_chain_snapshot_meta_t &chain_meta, 
        const vnetwork::xvnode_address_t &to_address, const vnetwork::xvnode_address_t &network_self);
    void handle_chain_snapshot(xsync_message_chain_snapshot_t &chain_snapshot, 
        const vnetwork::xvnode_address_t &to_address, const vnetwork::xvnode_address_t &network_self);
    xsync_download_tracer_mgr* download_tracer_mgr();

private:
    int32_t check(const std::string &account_address);
    int32_t check(const std::string &account_address, 
        const vnetwork::xvnode_address_t &to_address, const vnetwork::xvnode_address_t &network_self);

private:
    std::string m_vnode_id;
    observer_ptr<mbus::xmessage_bus_face_t> m_mbus;
    observer_ptr<base::xvcertauth_t> m_certauth;
    xsync_store_face_t *m_sync_store;
    xrole_chains_mgr_t *m_role_chains_mgr;
    xrole_xips_manager_t *m_role_xips_mgr;
    xsync_sender_t *m_sync_sender;
    xsync_download_tracer_mgr m_download_tracer{};
};

NS_END2
