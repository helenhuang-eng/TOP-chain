#include "xblockmaker/xfulltable_statistics.h"
#include "xcertauth/xcertauth_face.h"
#include "xcertauth/src/xsigndata.h"
#include "xvm/manager/xcontract_manager.h"

#include "xvledger/xvblock.h"
// TODO(jimmy) #include "xbase/xvledger.h"
#include "xbase/xobject_ptr.h"

namespace top {
namespace blockmaker {

using top::auth::xmutisigdata_t;
using top::auth::xnodebitset;

static void tableblock_statistics_handle(const xvip2_t leader_xip, const uint32_t txs_count, uint32_t vote_num, xstatistics_data_t & data){
    // height
    uint64_t block_height = get_network_height_from_xip2(leader_xip);
    auto it_height = data.detail.find(block_height);
    if (it_height == data.detail.end()) {
        xelection_related_statistics_data_t election_related_data;
        std::pair<std::map<uint64_t, xelection_related_statistics_data_t>::iterator, bool> ret = data.detail.insert(std::make_pair(block_height, election_related_data));
        it_height = ret.first;
    }
    // gid
    auto group_addr = common::xgroup_address_t{ common::xip_t{leader_xip.low_addr} };
    // common::xgroup_id_t group_id = common::xgroup_id_t{group_idx};
    auto it_group = it_height->second.group_statistics_data.find(group_addr);
    if (it_group == it_height->second.group_statistics_data.end()) {
        xgroup_related_statistics_data_t group_related_data;
        auto ret = it_height->second.group_statistics_data.insert(std::make_pair(group_addr, group_related_data));
        it_group = ret.first;
    }
    // nid
    uint16_t slot_idx = uint16_t(get_node_id_from_xip2(leader_xip));
    common::xslot_id_t slot_id = common::xslot_id_t{slot_idx};
    if(it_group->second.account_statistics_data.size() < size_t(slot_idx+1)){
        it_group->second.account_statistics_data.resize(slot_idx+1);
    }
    // workload
    it_group->second.account_statistics_data[slot_idx].block_data.block_count++;
    it_group->second.account_statistics_data[slot_idx].block_data.transaction_count += txs_count;
    // vote
    it_group->second.account_statistics_data[slot_idx].vote_data.vote_count += vote_num;
    xdbg("[tableblock_statistics] xip: [%lu, %lu], block_height: %lu, group_addr: %s, slot_id: %u, "
        "work add block_count: %u, block_count: %u, add txs_count %u, transaction_count: %u, add vote count: %u, vote_count: %u",
            leader_xip.high_addr,
            leader_xip.low_addr,
            block_height,
            group_addr.to_string().c_str(),
            slot_idx,
            1,
            it_group->second.account_statistics_data[slot_idx].block_data.block_count,
            txs_count,
            it_group->second.account_statistics_data[slot_idx].block_data.transaction_count,
            vote_num,
            it_group->second.account_statistics_data[slot_idx].vote_data.vote_count);
}

static uint32_t cal_vote_num(xobject_ptr_t<data::xblock_t> const & block, bool is_auditor) {
    std::string aggregated_signatures_bin;
    if (is_auditor) {
        aggregated_signatures_bin = block->get_cert()->get_audit_signature();
    } else {
        aggregated_signatures_bin = block->get_cert()->get_verify_signature();
    }
    xassert(!aggregated_signatures_bin.empty());

    xmutisigdata_t aggregated_sig_obj;
    if (aggregated_sig_obj.serialize_from_string(aggregated_signatures_bin) <= 0) {
        xassert(false);
        return 0;
    }
    xnodebitset & nodebits = aggregated_sig_obj.get_nodebitset();
    uint32_t vote_num = 0;
    for (int i = 0; i < nodebits.get_alloc_bits(); ++i) {
        if (nodebits.is_set(i)) {
            vote_num++;
        }
    }
    xassert(vote_num > 0);
    return vote_num;
}

static void set_vote_info(xvip2_t const& vote_xip, xobject_ptr_t<data::xblock_t> const & block, xstatistics_data_t& data, bool is_auditor) {
    // height
    uint64_t block_height = get_network_height_from_xip2(vote_xip);
    auto it_height = data.detail.find(block_height);
    if (it_height == data.detail.end()) {
        xelection_related_statistics_data_t election_related_data;
        std::pair<std::map<uint64_t, xelection_related_statistics_data_t>::iterator, bool> ret = data.detail.insert(std::make_pair(block_height, election_related_data));
        it_height = ret.first;
    }

    // gid
    auto group_addr = common::xgroup_address_t{ common::xip_t{vote_xip.low_addr} };
    // common::xgroup_id_t group_id = common::xgroup_id_t{group_idx};
    auto it_group = it_height->second.group_statistics_data.find(group_addr);
    if (it_group == it_height->second.group_statistics_data.end()) {
        xgroup_related_statistics_data_t group_related_data;
        auto ret = it_height->second.group_statistics_data.insert(std::make_pair(group_addr, group_related_data));
        it_group = ret.first;
    }


    std::string aggregated_signatures_bin;
    if (is_auditor) {
      aggregated_signatures_bin = block->get_cert()->get_audit_signature();
    } else {
      aggregated_signatures_bin = block->get_cert()->get_verify_signature();
    }
    xassert(!aggregated_signatures_bin.empty());

    xmutisigdata_t aggregated_sig_obj;
    xassert(aggregated_sig_obj.serialize_from_string(aggregated_signatures_bin) > 0);


    xnodebitset & nodebits = aggregated_sig_obj.get_nodebitset();
    if(it_group->second.account_statistics_data.size() < (uint32_t)nodebits.get_alloc_bits()){
        it_group->second.account_statistics_data.resize(nodebits.get_alloc_bits());
    }
    for (int i = 0; i < nodebits.get_alloc_bits(); ++i) {
        it_group->second.account_statistics_data[i].vote_data.block_count++;
        if (nodebits.is_set(i)) {
            it_group->second.account_statistics_data[i].vote_data.vote_count++;
        }
    }
}

static void process_vote_info(xobject_ptr_t<data::xblock_t> const & block, xstatistics_data_t& data) {
    auto auditor_xip = block->get_cert()->get_auditor();
    auto validator_xip = block->get_cert()->get_validator();

    if (!is_xip2_empty(auditor_xip)) {//block has auditor info
        set_vote_info(auditor_xip, block, data, true);
    }

    if (!is_xip2_empty(validator_xip)) {
        set_vote_info(validator_xip, block, data, false);
    }

}

xstatistics_data_t tableblock_statistics(std::vector<xobject_ptr_t<data::xblock_t>> const & blks) {
    xstatistics_data_t data;
    xdbg("[tableblock_statistics] blks size: %u", blks.size());
    for (size_t i = 0; i < blks.size(); i++) {
        if (nullptr == blks[i]) {
            xerror("[tableblock_statistics] blks[%u] null", i);
            continue;
        }

        uint32_t txs_count = blks[i]->get_txs_count();

        bool is_auditor = false;
        auto leader_xip = blks[i]->get_cert()->get_validator();
        if (get_node_id_from_xip2(leader_xip) == 0x3FF) {
            leader_xip = blks[i]->get_cert()->get_auditor();
            is_auditor = true;
            xassert(!blks[i]->get_cert()->get_audit_signature().empty());
        }

        uint32_t vote_num = cal_vote_num(blks[i], is_auditor);
        tableblock_statistics_handle(leader_xip, txs_count, 0, data);
        process_vote_info(blks[i], data);

    }

    return data;
}
}
}
