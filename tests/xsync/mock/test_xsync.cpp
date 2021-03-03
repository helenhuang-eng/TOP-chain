// Copyright (c) 2017-2018 Telos Foundation & contributors
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <inttypes.h>
#include <cinttypes>
#include <gtest/gtest.h>
#include "xdata/xchain_param.h"
#include "xconfig/xconfig_register.h"
#include "xdata/xblocktool.h"
#include "xmock_system.h"
#include "../../xblockstore_test/test_blockmock.hpp"
#include "../../mock/xtableblock_util.hpp"
#include "xsync/xsync_util.h"
#include "../../mock/xmock_network_config.hpp"
#include "../../mock/xmock_network.hpp"

using namespace top;

using namespace top::base;
//using namespace top::vnetwork;
//using namespace top::store;
using namespace top::data;
using namespace top::mock;
using namespace top::sync;
using namespace top::syncbase;

static void do_multi_sign(std::vector<std::shared_ptr<xmock_node_t>> &shard_nodes, const xvip2_t &leader_xip, base::xvblock_t* block) {

    //printf("do multi sign : {%" PRIx64 ", %" PRIx64 "}\n", leader_xip.high_addr, leader_xip.low_addr);

    block->get_cert()->set_validator(leader_xip);

    std::map<xvip2_t,std::string,xvip2_compare> validators;

    for (auto &it: shard_nodes) {
        std::shared_ptr<xmock_node_t> &node = it;
        xvip2_t xip = node->m_addr.xip2();

        auto sign = node->m_certauth->do_sign(xip, block, base::xtime_utl::get_fast_random64());
        //block->set_verify_signature(sign);
        //xassert(get_vcertauth()->verify_sign(xip_addr, vote) == base::enum_vcert_auth_result::enum_successful);
        validators[xip] = sign;
    }

    std::string sign = shard_nodes[0]->m_certauth->merge_muti_sign(validators, block->get_cert());
    block->set_verify_signature(sign);
    block->reset_block_flags();
    block->set_block_flag(base::enum_xvblock_flag_authenticated);

    assert(shard_nodes[0]->m_certauth->verify_muti_sign(block) == base::enum_vcert_auth_result::enum_successful);
}

static void create_tableblock(xobject_ptr_t<store::xstore_face_t> &store, xobject_ptr_t<base::xvblockstore_t> &blockstore, std::vector<std::shared_ptr<xmock_node_t>> &shard_nodes, std::string &account_address, uint32_t count) {
    // create data
    test_blockmock_t blockmock(store.get());
    std::string table_address = account_address_to_block_address(top::common::xaccount_address_t{account_address});
    std::string property("election_list");
    base::xvblock_t *prev_unit_block = blockmock.create_property_block(nullptr, account_address, property);
    base::xvblock_t* prev_table_block = xblocktool_t::create_genesis_empty_table(table_address);

    for (uint32_t i = 0; i < count; i++) {
        std::string value(std::to_string(i));
        base::xvblock_t *curr_unit_block = blockmock.create_property_block(prev_unit_block, account_address, property, value);

        std::vector<base::xvblock_t*> units;
        units.push_back(curr_unit_block);

        xvip2_t leader_xip = shard_nodes[0]->m_xip;

        base::xvblock_t* curr_table_block = xtableblock_util::create_tableblock_no_sign(units, prev_table_block, leader_xip);
        do_multi_sign(shard_nodes, leader_xip, curr_table_block);
        assert(blockstore->store_block(curr_table_block));

        base::xauto_ptr<base::xvblock_t> lock_tableblock = xblocktool_t::create_next_emptyblock(curr_table_block);
        do_multi_sign(shard_nodes, leader_xip, lock_tableblock.get());
        assert(blockstore->store_block(lock_tableblock.get()));

        base::xauto_ptr<base::xvblock_t> cert_tableblock = xblocktool_t::create_next_emptyblock(lock_tableblock.get());
        do_multi_sign(shard_nodes, leader_xip, cert_tableblock.get());
        assert(blockstore->store_block(cert_tableblock.get()));

        base::xauto_ptr<base::xvblock_t> commit_unitblock = blockstore->get_latest_committed_block(account_address);

        prev_unit_block->release_ref();
        prev_unit_block = commit_unitblock.get();
        prev_unit_block->add_ref();

        prev_table_block->release_ref();
        prev_table_block = cert_tableblock.get();;
        prev_table_block->add_ref();
    }

    prev_unit_block->release_ref();
    prev_table_block->release_ref();
}

static int duplicate_block(xobject_ptr_t<base::xvblockstore_t> &from, xobject_ptr_t<base::xvblockstore_t> &to, const std::string & address, uint64_t height) {
    xauto_ptr<xvblock_t> block = from->load_block_object(address, height);
    if (block == nullptr)
        return -1;

    base::xstream_t stream(base::xcontext_t::instance());
    {
        dynamic_cast<xblock_t*>(block.get())->full_block_serialize_to(stream);
    }

    xblock_ptr_t block_ptr = nullptr;
    {
        xblock_t* _data_obj = dynamic_cast<xblock_t*>(xblock_t::full_block_read_from(stream));
        block_ptr.attach(_data_obj);
    }

    block_ptr->reset_block_flags();
    block_ptr->set_block_flag(base::enum_xvblock_flag_authenticated);

    bool ret = to->store_block(block_ptr.get());

    if (ret)
        return 0;

    return -2;
}

TEST(test_xsync, behind) {

    xmock_network_config_t cfg_network("xsync_validator_behind");
    xmock_network_t network(cfg_network);
    xmock_system_t sys(network);

    std::vector<std::shared_ptr<xmock_node_t>> shard_nodes = sys.get_group_node("shard0");

    xobject_ptr_t<store::xstore_face_t> store = store::xstore_factory::create_store_with_memdb(nullptr);
    xobject_ptr_t<base::xvblockstore_t> blockstore = nullptr;
    blockstore.attach(store::xblockstorehub_t::instance().create_block_store(*store, ""));

    std::string account_address = xblocktool_t::make_address_user_account("11111111111111111112");
    std::string table_address = account_address_to_block_address(top::common::xaccount_address_t{account_address});

    create_tableblock(store, blockstore, shard_nodes, account_address, 1000);

    sys.start();

    sleep(1);

    for (uint32_t i=0; i<5; i++) {

        // set block to node2
        for (uint64_t h=i*100+1; h<=(i+1)*100; h++) {
            duplicate_block(blockstore, shard_nodes[1]->m_blockstore, table_address, h);
        }

        // tell node1 table chain is behind and sync from node2
        mbus::xevent_behind_ptr_t ev = std::make_shared<mbus::xevent_behind_origin_t>(
                    table_address, mbus::enum_behind_type_common, "test");
        shard_nodes[0]->m_mbus->push_event(ev);

        sleep(1);
    }

    base::xauto_ptr<base::xvblock_t> blk = shard_nodes[0]->m_blockstore->get_latest_current_block(table_address);
    assert(blk->get_height() == 500);

    while (1)
        sleep(1);
}

TEST(test_xsync, broadcast) {

    xmock_network_config_t cfg_network("xsync_broadcast");
    xmock_network_t network(cfg_network);
    xmock_system_t sys(network);

    std::vector<std::shared_ptr<xmock_node_t>> shard_nodes = sys.get_group_node("shard0");

    xobject_ptr_t<store::xstore_face_t> store = store::xstore_factory::create_store_with_memdb(nullptr);
    xobject_ptr_t<base::xvblockstore_t> blockstore = nullptr;
    blockstore.attach(store::xblockstorehub_t::instance().create_block_store(*store, ""));

    std::string account_address = xblocktool_t::make_address_user_account("11111111111111111112");
    std::string table_address = account_address_to_block_address(top::common::xaccount_address_t{account_address});

    create_tableblock(store, blockstore, shard_nodes, account_address, 1000);

    sys.start();

    for (uint64_t h=1; h<=999; h++) {
        duplicate_block(blockstore, shard_nodes[0]->m_blockstore, table_address, h);
        duplicate_block(blockstore, shard_nodes[1]->m_blockstore, table_address, h);

        base::xauto_ptr<base::xvblock_t> vblock = blockstore->load_block_object(table_address, h);

        vblock->add_ref();
        mbus::xevent_ptr_t ev = std::make_shared<mbus::xevent_consensus_data_t>(vblock.get(), false);
        shard_nodes[0]->m_mbus->push_event(ev);
        shard_nodes[1]->m_mbus->push_event(ev);

        usleep(10);
    }

    sleep(1);

    std::vector<std::shared_ptr<xmock_node_t>> arc_nodes = sys.get_group_node("arc0");
    for (auto &it: arc_nodes) {
        printf("%s height=%lu\n", it->m_vnode_id.c_str(), it->m_blockstore->get_latest_current_block(table_address)->get_height());
    }

    while (1)
        sleep(1);
}

class test_filter_t {
public:
    test_filter_t(const std::string &table_address, const xobject_ptr_t<base::xvblockstore_t> &blockstore, const std::vector<std::shared_ptr<xmock_node_t>> &shard_nodes):
    m_table_address(table_address),
    m_blockstore(blockstore),
    m_shard_nodes(shard_nodes) {
    }

    int32_t filter(const vnetwork::xmessage_t &in, vnetwork::xmessage_t &out) {

        auto const message_id = in.id();

        // discard this request and send lower newblock message
        if (message_id == syncbase::xmessage_id_sync_get_blocks) {

            xbyte_buffer_t message;
            xmessage_pack_t::unpack_message(in.payload(), message);
            base::xstream_t stream(base::xcontext_t::instance(), (uint8_t*)message.data(), message.size());

            xsync_message_header_ptr_t header = make_object_ptr<xsync_message_header_t>();
            header->serialize_from(stream);

            xsync_message_get_blocks_ptr_t ptr = make_object_ptr<xsync_message_get_blocks_t>();
            ptr->serialize_from(stream);

            const std::string &owner = ptr->owner;
            uint64_t start_height = ptr->start_height;
            uint32_t count = ptr->count;

            bool filter = false;
            if (start_height > 70 && start_height < 100 && !filter) {
                filter = true;
                send_lower_newblock_message();
                printf("send lower newblock\n");
                return -1;
            }
        }

        return 1;
    }

private:
    void send_lower_newblock_message() {

        uint64_t height = 95;
        base::xauto_ptr<base::xvblock_t> block = m_blockstore->load_block_object(m_table_address, height);

        base::xstream_t stream(base::xcontext_t::instance());
        auto header = make_object_ptr<xsync_message_header_t>(RandomUint64());
        header->serialize_to(stream);

        xblock_ptr_t block_ptr = autoptr_to_blockptr(block);
        auto body = make_object_ptr<sync::xsync_message_newblock_t>(block_ptr);
        body->serialize_to(stream);
        vnetwork::xmessage_t _msg = vnetwork::xmessage_t({stream.data(), stream.data() + stream.size()}, syncbase::xmessage_id_sync_newblock);

        xmessage_t msg;
        xmessage_pack_t::pack_message(_msg, ((int) _msg.payload().size()) >= DEFAULT_MIN_COMPRESS_THRESHOLD, msg);

        m_shard_nodes[0]->m_vnet->on_message(m_shard_nodes[1]->m_addr, msg);
    }

private:
    std::string m_table_address;
    xobject_ptr_t<base::xvblockstore_t> m_blockstore{};
    std::vector<std::shared_ptr<xmock_node_t>> m_shard_nodes;
};

TEST(test_xsync, exception) {

    xmock_network_config_t cfg_network("xsync_validator_behind");
    xmock_network_t network(cfg_network);
    xmock_system_t sys(network);

    std::vector<std::shared_ptr<xmock_node_t>> shard_nodes = sys.get_group_node("shard0");

    xobject_ptr_t<store::xstore_face_t> store = store::xstore_factory::create_store_with_memdb(nullptr);
    xobject_ptr_t<base::xvblockstore_t> blockstore = nullptr;
    blockstore.attach(store::xblockstorehub_t::instance().create_block_store(*store, ""));

    std::string account_address = xblocktool_t::make_address_user_account("11111111111111111112");
    std::string table_address = account_address_to_block_address(top::common::xaccount_address_t{account_address});

    create_tableblock(store, blockstore, shard_nodes, account_address, 1000);

    sys.start();

    sleep(1);

    // set filter
    test_filter_t filter(table_address, blockstore, shard_nodes);
    shard_nodes[1]->m_vhost->set_recv_filter(std::bind(&test_filter_t::filter, &filter, std::placeholders::_1, std::placeholders::_2));

    {
        // set block to node2
        for (uint64_t h=1; h<=100; h++) {
            duplicate_block(blockstore, shard_nodes[1]->m_blockstore, table_address, h);
        }

        // tell node1 table chain is behind and sync from node2
        mbus::xevent_behind_ptr_t ev = std::make_shared<mbus::xevent_behind_origin_t>(
                    table_address, mbus::enum_behind_type_common, "test");
        shard_nodes[0]->m_mbus->push_event(ev);
    }

    sleep(1);

    {
        // set block to node2
        for (uint64_t h=101; h<=200; h++) {
            duplicate_block(blockstore, shard_nodes[1]->m_blockstore, table_address, h);
        }

        // tell node1 table chain is behind and sync from node2
        mbus::xevent_behind_ptr_t ev = std::make_shared<mbus::xevent_behind_origin_t>(
                    table_address, mbus::enum_behind_type_common, "test");
        shard_nodes[0]->m_mbus->push_event(ev);
    }

    sleep(2);
    //base::xauto_ptr<base::xvblock_t> blk = shard_nodes[0]->m_blockstore->get_latest_current_block(table_address);
    //assert(blk->get_height() == 200);

    while (1)
        sleep(1);
}