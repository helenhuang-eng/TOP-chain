// Copyright (c) 2018-2020 Telos Foundation & contributors
// taylor.wei@topnetwork.org
// Licensed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <inttypes.h>

#include "xconsdriver.h"
#include "xbase/xutl.h"

namespace top
{
    namespace xconsensus
    {
        //////////////////////////////////////////xcsdriver_t////////////////////////////////////////////////
        xcsdriver_t *  xcsdriver_t::create_driver_object(xcscontext_t & parent_context)
        {
            xcsdriver_t * new_driver = new xBFTdriver_t(parent_context);

            xvip2_t alloc_address;
            alloc_address.high_addr = 0;
            alloc_address.low_addr = 0;
            std::string _extra_data;
            parent_context.attach_child_node(new_driver,alloc_address,_extra_data);
            return new_driver;
        }

        //////////////////////////////////////////xBFTdriver_t////////////////////////////////////////////////
        xBFTdriver_t::xBFTdriver_t(xcscoreobj_t & parent_object)
            :xBFTSyncdrv(parent_object)
        {
            xinfo("xBFTdriver_t::xBFTdriver_t,create,this=%p,parent=%p,account=%s",this,&parent_object,parent_object.get_account().c_str());
        }

        xBFTdriver_t::~xBFTdriver_t()
        {
            xinfo("xBFTdriver_t::~xBFTdriver_t,destroy,this=%p",this);
        }

        //call from lower layer to higher layer(parent) ,or parent->childs
        bool  xBFTdriver_t::on_consensus_update(const base::xvevent_t & event,xcsobject_t* from_child,const int32_t cur_thread_id,const uint64_t timenow_ms)
        {
            xconsensus_update * _evt_obj = (xconsensus_update*)&event;
            set_lock_block(_evt_obj->get_latest_lock());
            return true;
        }

        //call from higher layer to lower layer(child)
        bool  xBFTdriver_t::on_proposal_start(const base::xvevent_t & event,xcsobject_t* from_parent,const int32_t cur_thread_id,const uint64_t timenow_ms)
        {
            xproposal_start * _evt_obj = (xproposal_start*)&event;
            base::xvblock_t * proposal = _evt_obj->get_proposal();

            base::xvblock_t * lastest_lock_block = _evt_obj->get_latest_lock();
            if(NULL == lastest_lock_block) //using commit if not have lock
                lastest_lock_block = _evt_obj->get_latest_commit();

            //step#1: do sanity check for lock ,then  now update lock block
            if( (lastest_lock_block != NULL) && (set_lock_block(lastest_lock_block) == false) )
            {
                if(lastest_lock_block->get_viewid() < get_lock_block()->get_viewid())
                {
                    xerror("xBFTdriver_t::start_consensus,fail-lock block is out of date,need to be update,lock->dump(%s) at node=0x%llx",lastest_lock_block->dump().c_str(),get_xip2_addr().low_addr);
                }
            }
            if(_evt_obj->get_latest_cert() != NULL)   //update latest cert block from upper layer
                add_cert_block(_evt_obj->get_latest_cert());

            if(NULL != proposal)//leader ' behavior
            {
                //step#2: verify proposal block basicly
                if( (proposal->get_prev_block() == nullptr)
                   || (proposal->get_height() != (proposal->get_prev_block()->get_height() + 1))
                   || (proposal->get_last_block_hash() != proposal->get_prev_block()->get_block_hash())
                   || (false == proposal->get_prev_block()->is_deliver(false)) )
                {
                    if(proposal->get_prev_block() == nullptr)
                        xerror("xBFTdriver_t::start_consensus,fail-empty parent block for proposal->dump(%s) at node=0x%llx",proposal->dump().c_str(),get_xip2_addr().low_addr);
                    else
                        xerror("xBFTdriver_t::start_consensus,fail-invalid parent block=%s vs proposal->dump(%s) at node=0x%llx",proposal->get_prev_block()->dump().c_str(),proposal->dump().c_str(),get_xip2_addr().low_addr);

                    async_fire_proposal_finish_event(enum_xconsensus_error_bad_proposal,proposal);
                    return true;
                }
                if(proposal->get_height() > (get_lock_block()->get_height() + 2))
                {
                    xwarn("xBFTdriver_t::start_consensus,warn-proposal out of control by locked block=%s vs proposal=%s at node=0x%llx",get_lock_block()->dump().c_str(),proposal->dump().c_str(),get_xip2_addr().low_addr);

                    async_fire_proposal_finish_event(enum_xconsensus_error_bad_proposal,proposal);
                    return true;
                }

                //justify -> current locked block,so once this proposal is certified ->proof that locked-block is at commit status.
                if(proposal->get_header()->get_block_level() == base::enum_xvblock_level_unit)
                    proposal->get_cert()->set_justify_cert_hash(get_lock_block()->get_block_hash());
                else //for non-unit block justify_cert_hash always point to locked block' output_root_hash
                    proposal->get_cert()->set_justify_cert_hash(get_lock_block()->get_cert()->get_output_root_hash());

                if(   (false == proposal->is_input_ready(true))  //leader should has full block
                   || (false == proposal->is_output_ready(true))
                   || (false == proposal->is_valid(true))
                   || (false == proposal->get_cert_hash().empty()) //proposal should not have build cert hash before verify muti-sign
                   || (proposal->check_block_flag(base::enum_xvblock_flag_authenticated)) )//proposal should not add authenticated flag
                {
                    xerror("xBFTdriver_t::start_consensus,fail-bad proposal_block, proposal->dump(%s) at node=0x%llx",proposal->dump().c_str(),get_xip2_addr().low_addr);
                    async_fire_proposal_finish_event(enum_xconsensus_error_bad_proposal,proposal);
                    return true;
                }

                //step#3: safe check proposal that must be dervied from current lock-block
                if(safe_precheck_for_voting(proposal) == false)
                {
                    xwarn("xBFTdriver_t::start_consensus,fail-safe_check for proposal_block=%s vs driver=%s,at node=0x%llx",proposal->dump().c_str(),dump().c_str(),get_xip2_addr().low_addr);

                    async_fire_proposal_finish_event(enum_xconsensus_error_outofdate,proposal);
                    return true;
                }

                //step#4: do signature here
                if(proposal->get_cert()->is_validator(get_xip2_addr().low_addr))
                {
                    proposal->set_verify_signature(get_vcertauth()->do_sign(get_xip2_addr(), proposal->get_cert(),base::xtime_utl::get_fast_random64()));//bring leader 'signature
                }
                else
                {
                    proposal->set_audit_signature(get_vcertauth()->do_sign(get_xip2_addr(), proposal->get_cert(),base::xtime_utl::get_fast_random64()));//bring leader 'signature
                }

                //ready to insert cache now
                base::xauto_ptr<xproposal_t> _proposal_obj(add_proposal(proposal,proposal->get_prev_block(),_evt_obj->get_expired_ms()));//insert to local cache
                _proposal_obj->add_voted_cert(get_xip2_addr(),_proposal_obj->get_cert(),get_vcertauth()); //count leader 'vote
                _proposal_obj->mark_leader(); //mark original proposal at leader side
                _proposal_obj->mark_voted();  //mark voted,for leader it is always true
                _proposal_obj->set_result_of_verify_proposal(enum_xconsensus_code_successful);//mark verified

                //step#5: now ready to start consensus for this proposal
                std::string msg_stream;
                xproposal_msg_t msg(*proposal,NULL);
                msg.set_expired_ms(_evt_obj->get_expired_ms() * 2);//add addtional seconds for replica
                msg.serialize_to_string(msg_stream);

                //addres of -1 means broadcast to all consensus node,0 means not specified address that upper layer need fillin based on message type
                xvip2_t broadcast_addr = {(xvip_t)-1,(uint64_t)-1};
                xvip2_t self_addr = get_xip2_addr();
                // get leader xip
                auto leader_xip = _evt_obj->get_proposal()->get_cert()->get_validator();
                if (get_node_id_from_xip2(leader_xip) == 0x3FF) {
                    leader_xip = _evt_obj->get_proposal()->get_cert()->get_auditor();
                }
                xassert(is_xip2_equal(self_addr, leader_xip));

                xinfo("xBFTdriver_t::start_consensus --> successful at node=0x%llx,for proposal=%s with prev=%s",self_addr.low_addr,proposal->dump().c_str(),proposal->get_prev_block()->dump().c_str());

                std::string last_block_cert;
                proposal->get_prev_block()->get_cert()->serialize_to_string(last_block_cert);
                fire_pdu_event_up(xproposal_msg_t::get_msg_type(), msg_stream, 0, self_addr, broadcast_addr, proposal,last_block_cert);
            }
            return true;
        }

        //xBFTdriver_t guanrentee three factors for local block:
            //#1: guanrentee certificate,block'check has verified,and block'input ready as well, when block->is_deliver(false)
            //#2: any local block has been pass the test for xvblock_t::is_valid(true) before insert.but that not means finish cerficate verification
            //#3: one view#id only allow one block existing
        int  xBFTdriver_t::handle_proposal_msg(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            //step#0: verified that replica and leader are valid by from_addr and to_addr at top layer like xconsnetwork or xconsnode_t. here just consider pass.
            base::xcspdu_t & packet = event_obj->_packet;
            //step#1: do sanity check packet first
            xproposal_msg_t _proposal_msg;
            if(safe_check_for_proposal_packet(packet,_proposal_msg) == false)
            {
                xwarn("xBFTdriver_t::handle_proposal_msg,fail-safe_check_for_proposal_packet=%s vs driver=%s,at node=0x%llx",packet.dump().c_str(),dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_packet;
            }
            //checking local first
            xproposal_t * _local_block = find_proposal(packet.get_block_viewid());
            if(NULL != _local_block)
            {
                if(_local_block->is_valid_packet(packet))
                {
                    xwarn("xBFTdriver_t::handle_proposal_msg,fail-unmatched packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_error_bad_packet;
                }
                if( (_local_block->is_input_ready(false)) && (_local_block->is_valid(false)) )  // local proposal may be non-full block
                {
                    xinfo("xBFTdriver_t::handle_proposal_msg,a duplicated packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_error_duplicated;
                }
                if(_local_block->is_deliver(false))//target has been  finish one round of consensus
                {
                    xdbg("xBFTdriver_t::handle_proposal_msg,target proposal has finished voted as _local_block=%s, at node=0x%llx",_local_block->dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_code_successful;
                }
            }
            else
            {
                base::xvblock_t* _local_cert_block = find_cert_block(packet.get_block_viewid());
                if(_local_cert_block)
                {
                    if(   (_local_cert_block->get_height()     != packet.get_block_height())
                       || (_local_cert_block->get_chainid()    != packet.get_block_chainid())
                       || (_local_cert_block->get_viewid()     != packet.get_block_viewid())
                       || (_local_cert_block->get_account()    != packet.get_block_account())
                       )
                    {
                        xwarn("xBFTdriver_t::handle_proposal_msg,fail-unmatched packet=%s vs local certified block=%s,at node=0x%llx",packet.dump().c_str(),_local_cert_block->dump().c_str(),get_xip2_low_addr());
                        return enum_xconsensus_error_bad_packet;
                    }
                    xdbg("xBFTdriver_t::handle_proposal_msg,target proposal has finished and changed to certified _local_cert_block=%s, at node=0x%llx",_local_cert_block->dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_code_successful;//local proposal block has verified and ready,so it is duplicated commit msg
                }
            }
            //then check carried cert of prev block
            base::xvqcert_t * _peer_prev_block_cert = NULL;
            if(event_obj->get_vblock_cert() != NULL) //has build and verification at pacemaker layer
            {
                _peer_prev_block_cert = event_obj->get_vblock_cert();
                _peer_prev_block_cert->add_ref(); //paired with _dummy_to_release
            }
            else
            {
                _peer_prev_block_cert = base::xvblock_t::create_qcert_object(packet.get_vblock_cert());
                if(NULL == _peer_prev_block_cert)
                {
                    xerror("xBFTdriver_t::handle_proposal_msg,fail-a empty cert of prev-block from packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_error_bad_proposal;
                }
                //note:#1 safe rule, always cleans up flags carried by peer
                _peer_prev_block_cert->reset_block_flags();  //now force to clean all flags
            }
            base::xauto_ptr<base::xvqcert_t> _dummy_to_release(_peer_prev_block_cert);


            //step#2: load proposal block and do safe check
            base::xauto_ptr<base::xvblock_t> _peer_block(base::xvblock_t::create_block_object(_proposal_msg.get_block_object()));
            if( (!_peer_block) || (false == _peer_block->is_valid(false)) )
            {
                xerror("xBFTdriver_t::handle_proposal_msg,fail-invalid proposal from packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_proposal;
            }
            //note:#1 safe rule, always cleans up flags carried by peer
            _peer_block->reset_block_flags();  //now force to clean all flags for block and cert
            auto leader_xvip2 = get_leader_address(_peer_block.get());
            if(is_xip2_equal(leader_xvip2,to_addr)) //leader should not handle proposal packet from self
            {
                xwarn("xBFTdriver_t::handle_proposal_msg,fail-leader receve it's proposal,duplicated packet=%s and peer_block=%s,at node=0x%llx",packet.dump().c_str(),_peer_block->dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_proposal;
            }
            const xvip2_t replica_xip = get_xip2_addr();
            if(   (false == _peer_block->get_cert()->is_validator(replica_xip.low_addr))
               && (false == _peer_block->get_cert()->is_auditor(replica_xip.low_addr)) )
            {
                xwarn("xBFTdriver_t::fire_verify_proposal_job,fail-vote for validator(%llx) and auditor(%llx) of _proposal=%s,probabbly network changed,at node=0x%llx",_peer_block->get_cert()->get_validator().low_addr,_peer_block->get_cert()->get_auditor().low_addr,_peer_block->dump().c_str(),replica_xip.low_addr);
                return enum_xconsensus_error_bad_proposal; //that is not a qualified node for vote
            }
            //proposal ==> input ==> output
            _peer_block->get_input()->set_proposal(_proposal_msg.get_input_proposal());  //copy proposal
//            _peer_block->set_input_resources(_proposal_msg.get_input_resource());//copy proposal
//            _peer_block->set_output_resources(_proposal_msg.get_ouput_resource());//copy proposal
            
            //sanity check the proposal block
            if(   (_peer_block->get_height()  != packet.get_block_height())
               || (_peer_block->get_chainid() != packet.get_block_chainid())
               || (_peer_block->get_account() != packet.get_block_account())
               || (_peer_block->get_viewid()  != packet.get_block_viewid())
               || (_peer_block->get_viewtoken() != packet.get_block_viewtoken())
               || (_peer_block->is_input_ready() == false)  //input must be present right now
               || (_peer_block->is_output_ready() == false) //output must be present right now
               )
            {
                xerror("xBFTdriver_t::handle_proposal_msg,fail-invalid proposal=%s <!=> packet=%s,at node=0x%llx",_peer_block->dump().c_str(),packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_proposal;
            }
             
            //step#3: load/sync the missed commit,lock and cert blocks
            //first check whether need sync lock and cert blocks as well
            if(_peer_block->get_height() > (get_lock_block()->get_height() + 1) )
            {
                base::xvblock_t * prev_block = NULL;
                //base::xvblock_t * prev_prev_block = NULL;
                auto  cert_blocks = get_cert_blocks();
                for(auto it = cert_blocks.rbegin(); it != cert_blocks.rend(); ++it)
                {
                    if(   (_peer_block->get_height() == (it->second->get_height() + 1)) //found parent qc cert
                       && (_peer_block->get_last_block_hash() == it->second->get_block_hash()) )
                    {
                        //sync the pre-prev block now
                        prev_block = it->second;
                        //prev_prev_block = prev_block->get_prev_block();
                        break;
                    }
                }
                if(NULL == prev_block) //not voting but trigger sync prev-cert
                {
                    send_sync_request(to_addr,from_addr, (_peer_block->get_height() - 1),_peer_block->get_last_block_hash(),_peer_prev_block_cert,(_peer_block->get_height() - 1),event_obj->get_clock() + 2,_peer_block->get_chainid());
                    return enum_xconsensus_code_need_data;
                }
                else if(_peer_block->get_height() != (get_lock_block()->get_height() + 2))//if prev_prev NOT point to current locked block
                {
                    //xassert(NULL == prev_prev_block); //should be empty
                    send_sync_request(to_addr,from_addr, (prev_block->get_height() - 1),prev_block->get_last_block_hash(),_peer_prev_block_cert,(_peer_block->get_height() - 1),event_obj->get_clock() + 2,_peer_block->get_chainid());
                    return enum_xconsensus_code_need_data;//not voting but trigger sync prev-prev-cert
                }
            }
            //then sync latest commit if need
            if(get_lock_block()->get_height() > 1)
            {
                base::xvblock_t* last_commit_block = get_lock_block()->get_prev_block();
                if(last_commit_block == nullptr)//connection might be closed possible,so not rely on it
                {
                    //check at blockstore
                    base::xauto_ptr<base::xvblock_t> _block = get_vblockstore()->load_block_object(*this, get_lock_block()->get_height() - 1,0,false);
                    if(_block == nullptr)
                    {
                        send_sync_request(to_addr,from_addr, (get_lock_block()->get_height() - 1),get_lock_block()->get_last_block_hash(),_peer_prev_block_cert,(_peer_block->get_height() - 1),event_obj->get_clock() + 2,get_lock_block()->get_chainid());
                        return enum_xconsensus_code_need_data;//not voting but trigger sync missed commited block
                    }
                }
                
                #ifdef __xbft_enable_sync_unconneted_commit_block__
                while(last_commit_block != NULL)
                {
                    if(last_commit_block->check_block_flag(base::enum_xvblock_flag_connected))//connect must be commit as well
                        break; //all are fine
                    
                    if(NULL == last_commit_block->get_prev_block())
                    {
                        send_sync_request(to_addr,from_addr, (last_commit_block->get_height() - 1),last_commit_block->get_last_block_hash(),_peer_prev_block_cert,(_peer_block->get_height() - 1),event_obj->get_clock() + 2,last_commit_block->get_chainid());
                        break; //only allow trigger one block ,and continue go let application do completely sync
                    }
                    last_commit_block = last_commit_block->get_prev_block();
                }
                #endif
            }
            
            //apply safe rule for view-alignment,after sync check.note: event_obj->get_cookie() carry latest viewid at this node
            if(event_obj->get_cookie() != packet.get_block_viewid()) //a proposal not alignment with current view
            {
                xwarn("xBFTdriver_t::handle_proposal_msg,fail-unalignment viewid=%llu vs packet=%s.dump=%s at node=0x%llx",event_obj->get_cookie(),packet.dump().c_str(),dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_wrong_view; //notify peer sync data between nodes if need
            }
            if(safe_precheck_for_voting(_peer_block.get()) == false) //apply the basic safe-rule for block
            {
                xwarn("xBFTdriver_t::handle_proposal_msg,fail-an outofdate proposal=%s from packet=%s,at node=0x%llx",_peer_block->dump().c_str(),packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_outofdate;
            }

            //step#4: //pre-create proposal wrap
            base::xauto_ptr<xproposal_t> _final_proposal_block(new xproposal_t(*_peer_block,_peer_prev_block_cert));
            _final_proposal_block->set_expired_ms(get_time_now() + _proposal_msg.get_expired_ms());
            _final_proposal_block->set_bind_clock_cert(event_obj->get_xclock_cert());

            //step#5: verify proposal completely.it is a realy heavy job that run at worker thread,then callback
            const uint16_t  new_msg_nounce = packet.get_msg_nonce() + 1;
            std::function<void(void*)> _after_verify_proposal_job = [this,new_msg_nounce,from_addr,to_addr](void* _block_ptr)->void{
                xproposal_t* _proposal = ((xproposal_t*)_block_ptr); //callback running at thread of xBFTdriver_t
                if(_proposal->get_result_of_verify_proposal() == enum_xconsensus_code_successful)
                {
                    if(safe_finalcheck_for_voting(_proposal->get_block())) //apply safe rule again,in case mutiple-thread' race
                    {
                        xinfo("xBFTdriver_t::_after_verify_proposal_job, suffix process for proposal=%s,at node=0x%llx",_proposal->dump().c_str(),get_xip2_low_addr());

                        _proposal->mark_voted();  //mark voted at replica side

                        //update hqc certification if need
                        if(_proposal->get_last_block_cert()->check_unit_flag(base::enum_xvblock_flag_authenticated))
                            fire_certificate_finish_event(_proposal->get_last_block_cert());

                        std::string msg_stream;
                        xvote_msg_t _vote_msg(*_proposal->get_cert());
                        _vote_msg.serialize_to_string(msg_stream);
                        fire_pdu_event_up(xvote_msg_t::get_msg_type(),msg_stream,new_msg_nounce,to_addr,from_addr,_proposal->get_block());
                    }
                }
                else //fail as verify_proposal
                {
                    remove_proposal(_proposal->get_viewid());//note:remove verify fail proposal  note:remove_proposal must paired with fire_proposal_finish_event
                    const std::string errdetail;
                    fire_proposal_finish_event(_proposal->get_result_of_verify_proposal(),errdetail,_proposal->get_block(),NULL,NULL,NULL,NULL);
                }
                _proposal->release_ref(); //release reference added by fire_verify_proposal_job
            };

            add_proposal(*_final_proposal_block); //add proposal block and increase voted-height/voted-view first
            xdbg("xBFTdriver_t::handle_proposal_msg,finally start verify proposal=%s of packet=%s at node=0x%llx",_peer_block->dump().c_str(), packet.dump().c_str(),get_xip2_low_addr());
            //routing to worker thread per account_address
            fire_verify_proposal_job(from_addr,replica_xip,_final_proposal_block(),_after_verify_proposal_job);
            return enum_xconsensus_code_successful;
        }

        //leader get vote msg from replica
        int  xBFTdriver_t::handle_vote_msg(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            //step#0: verified that replica and leader are valid by from_addr and to_addr at top layer like xconsnetwork or xconsnode_t. here just consider pass.
            base::xcspdu_t & packet = event_obj->_packet;
            //step#1: do sanity check,verify proposal packet first
            xvote_msg_t _vote_msg;
            if(safe_check_for_vote_packet(packet,_vote_msg) == false)
            {
                xwarn("xBFTdriver_t::handle_vote_msg,fail-safe_check_for_vote_packet=%s vs driver=%s,at node=0x%llx",packet.dump().c_str(),dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_packet;
            }

            //step#2: verify has matched proposal at leader side
            xproposal_t * _local_proposal_block = find_proposal(packet.get_block_viewid());
            if(NULL == _local_proposal_block)//must has proposal first to receive vote
            {
                xwarn("xBFTdriver_t::handle_vote_msg,fail-local proposal has been removed for packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_vote;
            }
            //step#3: verify view_id & viewtoken to protect from DDOS attack
            if( false == _local_proposal_block->is_valid_packet(packet) )
            {
                xwarn("xBFTdriver_t::handle_vote_msg,fail-unmatched proposal=%s vs packet=%s,at node=0x%llx",_local_proposal_block->dump().c_str(), packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_vote;
            }
            
            auto leader_xvip2 = get_leader_address(_local_proposal_block->get_block());
            if(false == is_xip2_equal(leader_xvip2,to_addr))
            {
                xwarn("xBFTdriver_t::handle_vote_msg,fail-replica should not received vote-packet=%s vs local=%s at node=0x%llx",packet.dump().c_str(),_local_proposal_block->dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_packet;
            }
            //step#4: check if has enough voted
            if(_local_proposal_block->is_vote_finish())
            {
                xinfo("xBFTdriver_t::handle_vote_msg,proposal has finished vote as packet=%s,at node=0x%llx", packet.dump().c_str(),get_xip2_low_addr());//enough voted,just drop it
                return enum_xconsensus_code_successful;
            }

            //step#5: load qcert from bin data and do check
            base::xauto_ptr<base::xvqcert_t> _voted_cert(base::xvblock_t::create_qcert_object(_vote_msg.get_justify_source()));
            if(!_voted_cert) //carry invalid certificaiton of block
            {
                xerror("xBFTdriver_t::handle_vote_msg,fail-invalid justify source for packet=%s,at node=0x%llx", packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_vote;
            }
            //note:#1 safe rule, always cleans up flags carried by peer
            _voted_cert->reset_block_flags();  //now force to clean all flags

            if(_voted_cert->is_equal(*_local_proposal_block->get_cert()) == false)//test target_hash,viewid,viewtoken ...
            {
                xerror("xBFTdriver_t::handle_vote_msg,fail-unmatched justify source=%s vs packet=%s,at node=0x%llx",_voted_cert->dump().c_str(), packet.dump().c_str(),get_xip2_low_addr());//possible attack
                return enum_xconsensus_error_bad_vote;
            }

           //verification is a heavy job that run at worker thread,then callback to current thread
            std::function<void(void*)> _after_verify_vote_callback = [this](void* block_ptr)->void{
                xproposal_t * _local_proposal = (xproposal_t*)block_ptr;
                if(_local_proposal->check_block_flag(base::enum_xvblock_flag_authenticated))
                {
                    xinfo("xBFTdriver_t::handle_vote_msg,finish voted for proposal block:%s at node=0x%llx",_local_proposal->dump().c_str(),get_xip2_addr().low_addr);

                    remove_proposal(_local_proposal->get_viewid());//note:remove_proposal must paired with fire_proposal_finish_event

                    //step#8: call on_consensus_finish() to let upper layer know it
                    if(add_cert_block(_local_proposal->get_block()))//set certified block(QC block)
                    {
                        xdbgassert(_local_proposal->get_block()->is_input_ready(true));
                        xdbgassert(_local_proposal->get_block()->is_output_ready(true));

                        //collect data from propoal first
                        std::string msg_stream;
                        std::string   _commit_block_cert; //ship by packet' header instead of xcommit_msg_t for optimization
                        xcommit_msg_t _commit_msg(enum_xconsensus_code_successful);
                        _commit_msg.set_proof_certificate(std::string(), _local_proposal->get_height());//just carry empty cert
                        _commit_msg.serialize_to_string(msg_stream);

                        _local_proposal->get_cert()->serialize_to_string(_commit_block_cert);//here generated full data of cert

                        //then fire proposal event now
                        fire_proposal_finish_event(_local_proposal->get_block(), NULL, NULL, NULL, NULL);

                        //at last send out commit message
                        //addres of -1 means broadcast to all consensus node,0 means not specified address that upper layer need fillin based on message type
                        xvip2_t broadcast_addr = {(xvip_t)-1,(uint64_t)-1};
                        fire_pdu_event_up(xcommit_msg_t::get_msg_type(),msg_stream,1,get_xip2_addr(),broadcast_addr,_local_proposal->get_block(),_commit_block_cert);//ship block cert by packet
                    }
                }
                _local_proposal->release_ref();//release reference added by fire_verify_vote_job
            };
            xdbg("xBFTdriver_t::handle_vote_msg,finally start verify vote=%s of packet=%s at node=0x%llx",_voted_cert->dump().c_str(), packet.dump().c_str(),get_xip2_low_addr());

            //routing to worer thread per account_address
            fire_verify_vote_job(from_addr,_voted_cert.get(),_local_proposal_block,_after_verify_vote_callback);
            return enum_xconsensus_code_successful;
        }

        int   xBFTdriver_t::handle_commit_msg(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            //step#0: verified that replica and leader are valid by from_addr and to_addr at top layer like xconsnetwork or xconsaccount. here just consider pass.
            base::xcspdu_t & packet = event_obj->_packet;
            //step#1: do sanity check and verify proposal packet first, also do check whether behind too much
            xcommit_msg_t _commit_msg;
            if(false == safe_check_for_commit_packet(packet,_commit_msg))
            {
                xwarn("xBFTdriver_t::handle_commit_msg,fail-safe_check_for_commit_packet for packet=%s vs local=%s,errcode=%d,at node=0x%llx",packet.dump().c_str(),dump().c_str(),_commit_msg.get_commit_error_code(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_packet;
            }
            //note:packet carry the information of proposal instead of commit qc
            //safe rule#2: one view# only allow on cert
            {
                base::xvblock_t* _local_cert_block = find_cert_block(packet.get_block_viewid());
                if(_local_cert_block)
                {
                    if(   (_local_cert_block->get_height()     != packet.get_block_height())
                       || (_local_cert_block->get_chainid()    != packet.get_block_chainid())
                       || (_local_cert_block->get_viewid()     != packet.get_block_viewid())
                       || (_local_cert_block->get_account()    != packet.get_block_account())
                       )
                    {
                        xwarn("xBFTdriver_t::handle_commit_msg,fail-unmatched packet=%s vs local certified block=%s,errcode=%d,at node=0x%llx",packet.dump().c_str(),_local_cert_block->dump().c_str(),_commit_msg.get_commit_error_code(),get_xip2_low_addr());
                        return enum_xconsensus_error_bad_packet;
                    }
                    xdbg("xBFTdriver_t::handle_commit_msg,target commit has finished and submit to certified _local_cert_block=%s,errcode=%d,at node=0x%llx",_local_cert_block->dump().c_str(),_commit_msg.get_commit_error_code(),get_xip2_low_addr());
                    return enum_xconsensus_code_successful;//local proposal block has verified and ready,so it is duplicated commit msg
                }
            }

            bool is_match_local_proposal = false;
            //step#2: verify view_id & viewtoken etc to protect from DDOS attack by local blocks
            base::auto_reference<xproposal_t> _local_proposal_block(find_proposal(packet.get_block_viewid()));
            if(_local_proposal_block != nullptr)
            {
                is_match_local_proposal = true;
                if(_local_proposal_block->is_deliver(false))
                {
                    xdbg("xBFTdriver_t::handle_commit_msg,found delivered proposal for packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_code_successful;//local proposal block has verified and ready,so it is duplicated commit msg
                }
                if(_local_proposal_block->is_valid_packet(packet) == false)//different proposal at same view,it is possible
                {
                    xwarn("xBFTdriver_t::handle_commit_msg,warn-unmatched packet=%s against the existing proposal=%s,at node=0x%llx",packet.dump().c_str(),_local_proposal_block->dump().c_str(),get_xip2_low_addr());
                    is_match_local_proposal = false;
                }

                if (!_local_proposal_block->get_block()->is_output_ready(true)) //proposal have no output, that means verify proposal fail
                {
                    xwarn("xBFTdriver_t::handle_commit_msg, empty output for this proposal=%s,at node=0x%llx", _local_proposal_block->dump().c_str(), get_xip2_low_addr());
                    is_match_local_proposal = false;
                }

                auto leader_xvip2 = get_leader_address(_local_proposal_block->get_block());
                if(false == is_xip2_equal(leader_xvip2,from_addr)) //check leader
                {
                    xwarn("xBFTdriver_t::handle_commit_msg,fail-non-leader issue commit for this proposal=%s by packet=%s,at node=0x%llx",_local_proposal_block->dump().c_str(),packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_error_bad_packet; //now we can tell that is very likely attack
                }
            }

            //step#3: load cert object from bin data and do check
            base::xauto_ptr<base::xvqcert_t> _peer_commit_cert(base::xvblock_t::create_qcert_object(packet.get_vblock_cert()));
            if(!_peer_commit_cert) //carry invalid cert for commit
            {
                xwarn_err("xBFTdriver_t::handle_commit_msg,fail-create_qcert_object for packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_commit;//every commit must carry one cert(could be last block'cert) even fail
            }
            const std::string _peer_commit_cert_hash = _peer_commit_cert->hash(packet.get_vblock_cert());//calculate hash before reset flags
            //note:#1 safe rule, always cleans up flags carried by peer
            _peer_commit_cert->reset_block_flags();//force clean

            if(false == _peer_commit_cert->is_deliver())
            {
                xwarn_err("xBFTdriver_t::handle_commit_msg,fail-undelivered commit cert(%s) from packet=%s,and errcode=%d,at node=0x%llx",_peer_commit_cert->dump().c_str(), packet.dump().c_str(),_commit_msg.get_commit_error_code(),get_xip2_low_addr());
                return enum_xconsensus_error_bad_commit;//every commit must carry one cert(could be last block'cert) even fail
            }

            if(_commit_msg.get_commit_error_code() == enum_xconsensus_code_successful)//_peer_commit_cert carry the certificaiton for target proposal
            {
                //for successful case, commit_cert must matched as packet
                if(   (_peer_commit_cert->get_viewid()    != packet.get_block_viewid())
                   || (_peer_commit_cert->get_viewtoken() != packet.get_block_viewtoken())
                   )
                {
                    xwarn_err("xBFTdriver_t::handle_commit_msg,fail-wrong cert=%s vs packet=%s,at node=0x%llx",_peer_commit_cert->dump().c_str(),packet.dump().c_str(),get_xip2_low_addr());
                    return enum_xconsensus_error_bad_commit;
                }
                if(is_match_local_proposal)
                {
                    //step#4: verify whether has matched block at local side
                    if(_peer_commit_cert->is_equal(*_local_proposal_block->get_cert()) == false)//test cert except signature etc ...
                    {
                        xwarn_err("xBFTdriver_t::handle_commit_msg,fail-wrong commit_cert=%s vs proposal=%s,at node=0x%llx",_peer_commit_cert->dump().c_str(),_local_proposal_block->dump().c_str(),get_xip2_low_addr());
                        return enum_xconsensus_error_bad_commit; //possible attack
                    }

                    xdbgassert(_local_proposal_block->get_block()->is_input_ready(true));
                    xdbgassert(_local_proposal_block->get_block()->is_output_ready(true));

                    xdbg("xBFTdriver_t::handle_commit_msg,a matched commit ,goto fire_verify_commit_job for commit=%s,at node=0x%llx",_local_proposal_block->dump().c_str(),get_xip2_low_addr(),_peer_commit_cert.get());
                    //step#6: verify certificdation completely. it is a realy heavy job that run at worker thread
                    //routing to worer thread per account_address,so verify_qc and verify_block may running at same thread as order
                    fire_verify_commit_job(_local_proposal_block->get_block(),_peer_commit_cert.get());
                }
                else //_peer_commit_cert carry the certificaiton of this proposal,so try to pull it back from peer
                {
                    xinfo("xBFTdriver_t::handle_commit_msg,successful result but local proposal not found,goto sync for packet=%s,at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());

                    const uint64_t    sync_target_block_height = _commit_msg.get_proof_cert_height();
                    const std::string sync_target_block_hash   = _peer_commit_cert_hash;
                    send_sync_request(to_addr,from_addr,sync_target_block_height,sync_target_block_hash,_peer_commit_cert.get(),sync_target_block_height,(event_obj->get_clock() + 2),packet.get_block_chainid());//download it
                }
            }
            else//fail case: _peer_commit_cert carry some certificaiton that might be parent block of proposal,or any other cert
            {
                if(is_match_local_proposal)//do clean at last step
                {
                    //now safe to clean existing proposal
                    xwarn("xBFTdriver_t::handle_commit_msg,fail-leader notify err-code(%d) for this proposal=%s,from packet,at node=0x%llx",_commit_msg.get_commit_error_code(),_local_proposal_block->dump().c_str(),packet.dump().c_str(),get_xip2_low_addr());
                    
                    remove_proposal(_local_proposal_block->get_viewid());//note:remove_proposal must paired with fire_proposal_finish_event

                    fire_proposal_finish_event(_commit_msg.get_commit_error_code(),_commit_msg.get_commit_error_reason(),_local_proposal_block->get_block(), NULL, NULL, get_latest_cert_block(), NULL);//then fire event
                }
                else
                {
                    if(_peer_commit_cert->get_viewid() > get_lock_block()->get_viewid())
                    {
                        base::xvblock_t* _local_commit_cert = find_cert_block(_peer_commit_cert->get_viewid());
                        if(NULL == _local_commit_cert) //carried commit cert is newer than local
                        {
                            const uint64_t    sync_target_block_height = _commit_msg.get_proof_cert_height();
                            const std::string sync_target_block_hash   = _peer_commit_cert_hash;
                            send_sync_request(to_addr,from_addr,sync_target_block_height,sync_target_block_hash,_peer_commit_cert.get(),_commit_msg.get_proof_cert_height(),(event_obj->get_clock() + 2),packet.get_block_chainid());//download it
                        }
                    }
                    xwarn("xBFTdriver_t::handle_commit_msg,leader notify err-code(%d) from packet=%s,at node=0x%llx",_commit_msg.get_commit_error_code(),packet.dump().c_str(),get_xip2_low_addr());
                }
            }
            return enum_xconsensus_code_successful;
        }

        int   xBFTdriver_t::handle_votereport_msg(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xvote_report_t _report_msg;
            if(_report_msg.serialize_from_string(packet.get_msg_body()) > 0)
            {
                xwarn("xBFTdriver_t::handle_votereport_msg,fail-peer node report err-code(%d) with peer=%s vs local=%s from peer:0x%llx,at node=0x%llx",_report_msg.get_error_code(),_report_msg.get_error_detail().c_str(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                
                if( get_lock_block()->get_height() < (_report_msg.get_latest_commit_height() + 1) )
                {
                    const uint64_t    sync_target_block_height = _report_msg.get_latest_commit_height();
                    const std::string sync_target_block_hash   = _report_msg.get_latest_commit_hash();
                    send_sync_request(to_addr,from_addr,sync_target_block_height,sync_target_block_hash,get_lock_block()->get_cert(),get_lock_block()->get_height(),(event_obj->get_clock() + 2),packet.get_block_chainid());//download peer 'commit block
                }
                if(get_lock_block()->get_height() < _report_msg.get_latest_lock_height())
                {
                    const uint64_t    sync_target_block_height = _report_msg.get_latest_lock_height();
                    const std::string sync_target_block_hash   = _report_msg.get_latest_lock_hash();
                    send_sync_request(to_addr,from_addr,sync_target_block_height,sync_target_block_hash,get_lock_block()->get_cert(),get_lock_block()->get_height(),(event_obj->get_clock() + 2),packet.get_block_chainid());//download peer 'locked block
                }
                base::xvblock_t * local_latest_cert = get_latest_cert_block();
                if(_report_msg.get_latest_cert_viewid() > 0)
                {
                    if( (NULL == local_latest_cert) || (local_latest_cert->get_viewid() < _report_msg.get_latest_cert_viewid()) )
                    {
                        const uint64_t    sync_target_block_height = _report_msg.get_latest_cert_height();
                        const std::string sync_target_block_hash   = _report_msg.get_latest_cert_hash();
                        send_sync_request(to_addr,from_addr,sync_target_block_height,sync_target_block_hash,get_lock_block()->get_cert(),get_lock_block()->get_height(),(event_obj->get_clock() + 2),packet.get_block_chainid());//download peer 'latest cert block
                    }
                }
            }
            else
            {
                xerror("xcsdriver_t::handle_votereport_msg,fail-decode packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            }
            return enum_xconsensus_code_successful;
        }

        //clock block always pass by higher layer to lower layer
        bool  xBFTdriver_t::on_clock_fire(const base::xvevent_t & event,xcsobject_t* from_parent,const int32_t cur_thread_id,const uint64_t timenow_ms)
        {
            xBFTSyncdrv::on_clock_fire(event,from_parent,cur_thread_id,timenow_ms);//let sync clean first

            std::vector<xproposal_t*> timeout_list;
            std::vector<xproposal_t*> outofdate_list;
            std::map<uint64_t,xproposal_t*> & proposal_blocks = get_proposals();
            for(auto it = proposal_blocks.begin(); it != proposal_blocks.end();)
            {
                auto old_it = it; //copy it first
                ++it; //move forward

                xproposal_t * _proposal = old_it->second;
                if(safe_check_for_block(_proposal->get_block()) == false)
                {
                    outofdate_list.push_back(_proposal);
                    proposal_blocks.erase(old_it);//erase old one
                    continue;
                }
            }
            notify_proposal_fail(timeout_list,outofdate_list);
            return true;
        }

        //fire view-change event
        bool  xBFTdriver_t::on_view_fire(const base::xvevent_t & event,xcsobject_t* from_parent,const int32_t cur_thread_id,const uint64_t timenow_ms)
        {
            xBFTSyncdrv::on_view_fire(event,from_parent,cur_thread_id,timenow_ms);//let sync clean first

            xcsview_fire * _ev_obj = (xcsview_fire*)&event;

            //filter too old proposal and put into removed_list
            std::vector<xproposal_t*> timeout_list;
            std::vector<xproposal_t*> outofdate_list;

            std::map<uint64_t,xproposal_t*> & proposal_blocks = get_proposals();
            for(auto it = proposal_blocks.begin(); it != proposal_blocks.end();)
            {
                auto old_it = it; //copy it first
                ++it; //move forward

                xproposal_t * _proposal = old_it->second;
                if(_ev_obj->get_viewid() > _proposal->get_viewid()) //clean all unfinished proposoal as viewid upgraded
                {
                    timeout_list.push_back(_proposal);
                    proposal_blocks.erase(old_it);//erase old one
                    continue;
                }
                else if(safe_check_for_block(_proposal->get_block()) == false)
                {
                    outofdate_list.push_back(_proposal);
                    proposal_blocks.erase(old_it);//erase old one
                    continue;
                }
            }
            notify_proposal_fail(timeout_list,outofdate_list);
            return true;
        }

        bool    xBFTdriver_t::notify_proposal_fail(std::vector<xproposal_t*> & timeout_list,std::vector<xproposal_t*> &outofdate_list)
        {
            std::string   _commit_result_cert;//ship by packet'header
            xcommit_msg_t _commit_msg(enum_xconsensus_error_timeout);//notify error for replica
            if( (timeout_list.empty() == false) || (outofdate_list.empty() == false) )
            {
                base::xvblock_t* _highest_cert_block = get_latest_cert_block();
                if(NULL == _highest_cert_block)
                    _highest_cert_block = get_lock_block();

                _commit_msg.set_proof_certificate(std::string(), _highest_cert_block->get_height());//carry empty cert data
                _highest_cert_block->get_cert()->serialize_to_string(_commit_result_cert);
            }
            //notify each one to upper layer as enum_xconsensus_error_outofdate
            if(timeout_list.empty() == false)
            {
                std::string msg_stream;
                _commit_msg.serialize_to_string(msg_stream);
                xvip2_t broadcast_addr = {(xvip_t)-1,(uint64_t)-1};

                std::sort(timeout_list.begin(), timeout_list.end(), sort_proposal());
                for(auto it = timeout_list.begin(); it != timeout_list.end(); ++it)
                {
                    xproposal_t * _to_remove = *it;
                    if(_to_remove->is_leader()) //at leader'node for this proposal
                    {
                        xwarn("xBFTdriver_t::notify_proposal_fail,leader timeout for a proposal=%s,at node=0x%llx",_to_remove->dump().c_str(),get_xip2_low_addr());

                        fire_pdu_event_up(xcommit_msg_t::get_msg_type(),msg_stream,0,get_xip2_addr(),broadcast_addr,_to_remove->get_block(),_commit_result_cert);//_commit_result_cert embbed into packet'header
                    }
                    else
                    {
                        xwarn("xBFTdriver_t::notify_proposal_fail,replica timeout for a proposal=%s,at node=0x%llx",_to_remove->dump().c_str(),get_xip2_low_addr());
                    }

                    const std::string errdetail;
                    fire_proposal_finish_event(enum_xconsensus_error_timeout,errdetail,_to_remove->get_block(), NULL, NULL, NULL,NULL);

                    _to_remove->release_ref();
                }
                timeout_list.clear();
            }
            if(outofdate_list.empty() == false)
            {
                std::sort(outofdate_list.begin(), outofdate_list.end(), sort_proposal());
                for(auto it = outofdate_list.begin(); it != outofdate_list.end(); ++it)
                {
                    xproposal_t * _to_remove = *it;

                    xwarn("xBFTdriver_t::notify_proposal_fail,out-of-date for a proposal=%s,at node=0x%llx of replica",_to_remove->dump().c_str(),get_xip2_low_addr());

                    const std::string errdetail;
                    fire_proposal_finish_event(enum_xconsensus_error_cancel,errdetail,_to_remove->get_block(), NULL, NULL, NULL, NULL);

                    _to_remove->release_ref();
                }
                timeout_list.clear();
            }
            return true;
        }

        bool  xBFTdriver_t::async_fire_consensus_update_event()
        {
            //send_call may automatically hold this reference whiling executing,so here is safe to using "this"
            std::function<void(void*)> _aysn_job = [this](void*)->void{
                fire_consensus_update_event_up(NULL,NULL,get_latest_cert_block());
            };
            return send_call(_aysn_job,(void*)NULL);
        }

        bool  xBFTdriver_t::async_fire_proposal_finish_event(const int err_code,base::xvblock_t* proposal)
        {
            if(NULL == proposal)
                return false;

            proposal->add_ref();//hold proposal reference first
            std::function<void(void*)> _aysn_job = [this,err_code](void* _block)->void{
                base::xvblock_t* _target_proposal_ = (base::xvblock_t*)_block;
                const std::string errdetail;
                fire_proposal_finish_event(err_code,errdetail,_target_proposal_,NULL,NULL,get_latest_cert_block(),NULL);

                _target_proposal_->release_ref(); //release it,paired with above reference
            };
            return send_call(_aysn_job,(void*)proposal);//send_call may automatically hold this reference whiling executing,so here is safe to using "this"
        }

        bool  xBFTdriver_t::async_fire_proposal_finish_event(const int err_code,const std::string & err_detail,base::xvblock_t* proposal)
        {
            if(NULL == proposal)
                return false;

            proposal->add_ref();//hold proposal reference first
            std::function<void(void*)> _aysn_job = [this,err_code,err_detail](void* _block)->void{
                base::xvblock_t* _target_proposal_ = (base::xvblock_t*)_block;
                fire_proposal_finish_event(err_code,err_detail,_target_proposal_,NULL,NULL,get_latest_cert_block(),NULL);
                _target_proposal_->release_ref();
            };
            return send_call(_aysn_job,(void*)proposal);//send_call may automatically hold this reference whiling executing,so here is safe to using "this"
        }

        bool xBFTdriver_t::fire_verify_cert_job(base::xvqcert_t * test_cert)
        {
            if(NULL == test_cert)
                return false;

            if(false == test_cert->is_deliver())
                return false;

            //now safe to do heavy job to verify quorum_ceritification completely
            std::function<void(void*)> _after_verify_cert_job = [this](void* _cert)->void{

                base::xvqcert_t* _target_cert_ = (base::xvqcert_t*)_cert;
                xinfo("xBFTdriver_t::_after_verify_cert_job,Valid Certification:%s at node=0x%llx",_target_cert_->dump().c_str(),get_xip2_addr().low_addr);

                if(_target_cert_->check_unit_flag(base::enum_xvblock_flag_authenticated))
                    fire_certificate_finish_event(_target_cert_);

                _target_cert_->release_ref(); //release refernce added by _verify_function;
            };

            auto _verify_function = [this](base::xcall_t & call, const int32_t cur_thread_id,const uint64_t timenow_ms)->bool{
                if(is_close() == false)
                {
                    base::xvqcert_t* _to_verify_cert_ = (base::xvqcert_t *)call.get_param1().get_object();
                    if(get_vcertauth()->verify_muti_sign(_to_verify_cert_,get_account()) == base::enum_vcert_auth_result::enum_successful)
                    {
                        _to_verify_cert_->set_unit_flag(base::enum_xvblock_flag_authenticated);
                        base::xfunction_t* _callback_ = (base::xfunction_t *)call.get_param2().get_function();
                        if(_callback_ != NULL)
                        {
                            _to_verify_cert_->add_ref(); //hold reference for async call
                            dispatch_call(*_callback_,(void*)_to_verify_cert_);
                        }
                    }
                }
                return true;
            };
            base::xcall_t asyn_verify_call(_verify_function,(base::xobject_t*)test_cert,&_after_verify_cert_job,(base::xobject_t*)this);
            asyn_verify_call.bind_taskid(get_account_index());
            base::xworkerpool_t * _workers_pool = get_workerpool();
            if(_workers_pool != NULL)
                return (_workers_pool->send_call(asyn_verify_call) == enum_xcode_successful);
            else
                return (dispatch_call(asyn_verify_call) == enum_xcode_successful);
        }

        //note:for commit msg we need merger local proposal and received certifcate from leader
        //but  for sync msg "target_block" already carry full certifcate ,it no-need merge again
        bool xBFTdriver_t::fire_verify_commit_job(base::xvblock_t * target_block,base::xvqcert_t * paired_cert)
        {
            if(NULL == target_block)
                return false;

            if(false == target_block->is_valid(true))//add addtional simple check,optional
                return false;

            //now safe to do heavy job to verify quorum_ceritification completely
            std::function<void(void*)> _after_verify_commit_job = [this](void* _block)->void{
                base::xvblock_t* _full_block_ = (base::xvblock_t*)_block;

                if(add_cert_block(_full_block_))//set certified block(QC block)
                {
                    if(remove_proposal(_full_block_->get_viewid()))//note:remove_proposal must paired with fire_proposal_finish_event
                        xinfo("xBFTdriver_t::_after_verify_commit_job,deliver an proposal-and-authed block:%s at node=0x%llx",_full_block_->dump().c_str(),get_xip2_addr().low_addr);
                    else
                        xinfo("xBFTdriver_t::_after_verify_commit_job,deliver an replicated-and-authed block:%s at node=0x%llx",_full_block_->dump().c_str(),get_xip2_addr().low_addr);

                    fire_proposal_finish_event(_full_block_, NULL, NULL, NULL, NULL);//call on_consensus_finish(block) to driver context layer
                }
                _full_block_->release_ref(); //release reference hold by _verify_function
            };

            if(paired_cert != nullptr) //manually add reference for _verify_function call
                paired_cert->add_ref();
            auto _verify_function = [this,paired_cert](base::xcall_t & call, const int32_t cur_thread_id,const uint64_t timenow_ms)->bool{
                base::xauto_ptr<base::xvqcert_t> _merge_cert(paired_cert);//auto release the added addtional once quit
                if(is_close() == false)
                {
                    base::xvblock_t* _for_check_block_ = (base::xvblock_t *)call.get_param1().get_object();
                    if( (_merge_cert != nullptr) && (false == _for_check_block_->merge_cert(*_merge_cert)) ) //here is thread-safe to merge cert into block
                    {
                        xwarn_err("xBFTdriver_t::fire_verify_commit_job,fail-unmatched commit_cert=%s vs proposal=%s,at node=0x%llx",_merge_cert->dump().c_str(),_for_check_block_->dump().c_str(),get_xip2_low_addr());
                        return true;
                    }
                    if(get_vcertauth()->verify_muti_sign(_for_check_block_) == base::enum_vcert_auth_result::enum_successful)
                    {
                        _for_check_block_->get_cert()->set_unit_flag(base::enum_xvblock_flag_authenticated);
                        _for_check_block_->set_block_flag(base::enum_xvblock_flag_authenticated);

                        xinfo("xBFTdriver_t::fire_verify_commit_job,successful finish verify for commit block:%s at node=0x%llx",_for_check_block_->dump().c_str(),get_xip2_addr().low_addr);

                        base::xfunction_t* _callback_ = (base::xfunction_t *)call.get_param2().get_function();
                        if(_callback_ != NULL)
                        {
                            _for_check_block_->add_ref(); //hold reference for async
                            dispatch_call(*_callback_,(void*)_for_check_block_);
                        }
                    }
                    else
                        xerror("xBFTdriver_t::fire_verify_commit_job,fail-verify_muti_sign for block=%s,at node=0x%llx",_for_check_block_->dump().c_str(),get_xip2_low_addr());
                }
                return true;
            };
            base::xcall_t asyn_verify_call(_verify_function,(base::xobject_t*)target_block,&_after_verify_commit_job,(base::xobject_t*)this);
            asyn_verify_call.bind_taskid(get_account_index());
            base::xworkerpool_t * _workers_pool = get_workerpool();
            if(_workers_pool != NULL)
                return (_workers_pool->send_call(asyn_verify_call) == enum_xcode_successful);
            else
                return (dispatch_call(asyn_verify_call) == enum_xcode_successful);
        }

        bool xBFTdriver_t::fire_verify_vote_job(const xvip2_t replica_xip,base::xvqcert_t*replica_cert,xproposal_t * local_proposal,base::xfunction_t &callback)
        {
            if( (NULL == replica_cert) || (NULL == local_proposal) )
                return false;

            replica_cert->add_ref();
            auto _verify_function = [this,replica_xip,replica_cert](base::xcall_t & call, const int32_t cur_thread_id,const uint64_t timenow_ms)->bool{
                if(is_close() == false)//running at a specific worker thread of pool
                {
                    xproposal_t * _proposal = (xproposal_t *)call.get_param1().get_object();
                    if(false == _proposal->is_vote_finish()) //check first as async case,it might be finished already
                    {
                        if(get_vcertauth()->verify_sign(replica_xip, replica_cert,_proposal->get_account()) == base::enum_vcert_auth_result::enum_successful) //verify partial-certication of msg
                        {
                            if(_proposal->add_voted_cert(replica_xip,replica_cert,get_vcertauth())) //add to local list
                            {
                                if(_proposal->is_vote_finish()) //check again
                                {
                                    if(false == _proposal->get_voted_validators().empty())
                                    {
                                        const std::string merged_sign_for_validators = get_vcertauth()->merge_muti_sign(_proposal->get_voted_validators(), _proposal->get_block());
                                        _proposal->get_block()->set_verify_signature(merged_sign_for_validators);
                                    }
                                    if(false == _proposal->get_voted_auditors().empty())
                                    {
                                        const std::string merged_sign_for_auditors = get_vcertauth()->merge_muti_sign(_proposal->get_voted_auditors(), _proposal->get_block());
                                        _proposal->get_block()->set_audit_signature(merged_sign_for_auditors);
                                    }
                                    if(get_vcertauth()->verify_muti_sign(_proposal->get_block()) == base::enum_vcert_auth_result::enum_successful) //quorum certification and  check if majority voted
                                    {
                                        _proposal->get_cert()->set_unit_flag(base::enum_xvblock_flag_authenticated);
                                        _proposal->get_block()->set_block_flag(base::enum_xvblock_flag_authenticated);
                                        //--------------after below line, block not allow do any change  anymore------------
                                        xinfo("xBFTdriver_t::fire_verify_vote_job,successful collect enough vote and verified for _proposal=%s,at node=0x%llx",_proposal->dump().c_str(),get_xip2_low_addr());

                                        base::xfunction_t* _callback_ = (base::xfunction_t *)call.get_param2().get_function();
                                        if(_callback_ != NULL)
                                        {
                                            _proposal->add_ref(); //hold for async call
                                            dispatch_call(*_callback_,(void*)_proposal);//send callback to engine'own thread
                                        }
                                    }
                                    else
                                        xerror("xBFTdriver_t::fire_verify_vote_job,fail-verify_muti_sign for _proposal=%s,at node=0x%llx",_proposal->dump().c_str(),get_xip2_low_addr());
                                }
                            }
                        }
                        else
                            xerror("xBFTdriver_t::fire_verify_vote_job,fail-verify_sign for replica_cert=%s,at node=0x%llx",replica_cert->dump().c_str(),get_xip2_low_addr());
                    }
                }
                replica_cert->release_ref();
                return true;
            };

            base::xcall_t asyn_verify_call(_verify_function,(base::xobject_t*)local_proposal,&callback,(base::xobject_t*)this);
            asyn_verify_call.bind_taskid(get_account_index());
            base::xworkerpool_t * _workers_pool = get_workerpool();
            if(_workers_pool != NULL)
                return (_workers_pool->send_call(asyn_verify_call) == enum_xcode_successful);
            else
                return (dispatch_call(asyn_verify_call) == enum_xcode_successful);
        }

        bool xBFTdriver_t::fire_verify_proposal_job(const xvip2_t leader_xip,const xvip2_t replica_xip,xproposal_t * target_proposal,base::xfunction_t &callback)
        {
            if(NULL == target_proposal)
                return false;

            //now safe to do heavy job to verify quorum_ceritification completely
            auto _verify_function = [this,leader_xip,replica_xip](base::xcall_t & call, const int32_t cur_thread_id,const uint64_t timenow_ms)->bool{
                if(is_close() == false)//running at a specific worker thread of pool
                {
                    xproposal_t * _proposal = (xproposal_t *)call.get_param1().get_object();
                    if(_proposal->get_block()->check_unit_flag(base::enum_xvblock_flag_authenticated))
                    {
                        xwarn("xBFTdriver_t::fire_verify_proposal_job,proposal had been changed to authenticated by commit msg while async-verifing for proposal=%s,at node=0x%llx",_proposal->dump().c_str(),replica_xip.low_addr);
                        return true;
                    }
                    
                    base::xvqcert_t*   _bind_xclock_cert = _proposal->get_bind_clock_cert();
                    if( (_bind_xclock_cert != NULL) && (false ==_bind_xclock_cert->check_unit_flag(base::enum_xvblock_flag_authenticated)) ) //recheck it in case for pacemaker not verified yet
                    {
                        const std::string xclock_account_addrs = xcsobject_t::get_xclock_account_address();
                        _bind_xclock_cert->reset_unit_flag(base::enum_xvblock_flag_authenticated);//remove first
                        if(get_vcertauth()->verify_muti_sign(_bind_xclock_cert,xclock_account_addrs) == base::enum_vcert_auth_result::enum_successful)
                        {
                            _bind_xclock_cert->set_unit_flag(base::enum_xvblock_flag_authenticated);
                        }
                        else
                        {
                            xwarn("xBFTdriver_t::fire_verify_proposal_job,bad xclock cert=%s against proposal=%s,at node=0x%llx",_bind_xclock_cert->dump().c_str(),_proposal->dump().c_str(),replica_xip.low_addr);
                        }
                    }

                    if(get_vcertauth()->verify_sign(leader_xip,_proposal->get_block()) == base::enum_vcert_auth_result::enum_successful)//first verify leader'signature as well
                    {
                        const int result_of_verify_proposal = verify_proposal(_proposal->get_block(),_bind_xclock_cert,this);
                        _proposal->set_result_of_verify_proposal(result_of_verify_proposal);
                        if(result_of_verify_proposal == enum_xconsensus_code_successful)//verify proposal then
                        {
                            std::string empty;
                            _proposal->add_voted_cert(leader_xip, _proposal->get_cert(),get_vcertauth());//add leader'cert to list
                            _proposal->get_block()->set_verify_signature(empty);//reset cert
                            _proposal->get_block()->set_audit_signature(empty); //reset cert

                            const std::string signature = get_vcertauth()->do_sign(replica_xip, _proposal->get_cert(),base::xtime_utl::get_fast_random64());//sign for this proposal at replica side

                            if(_proposal->get_cert()->is_validator(replica_xip.low_addr))
                                _proposal->get_block()->set_verify_signature(signature); //verification node
                            else  if(_proposal->get_cert()->is_auditor(replica_xip.low_addr))
                                _proposal->get_block()->set_audit_signature(signature);  //auditor node
                            else //should not happen since has been tested before call
                            {
                                xwarn("xBFTdriver_t::fire_verify_proposal_job,fail-vote for validator(%llx) and auditor(%llx) as network change for _proposal=%s,at node=0x%llx",_proposal->get_cert()->get_validator().low_addr,_proposal->get_cert()->get_auditor().low_addr,_proposal->dump().c_str(),replica_xip.low_addr);
                            }
                            xinfo("xBFTdriver_t::fire_verify_proposal_job,successful finish verification for proposal=%s,at node=0x%llx",_proposal->dump().c_str(),replica_xip.low_addr);
                        }
                        else
                        {
                            xwarn("xBFTdriver_t::fire_verify_proposal_job,fail-verify_proposal for _proposal:%s,at node=0x%llx",_proposal->dump().c_str(),replica_xip.low_addr);
                        }
                        base::xfunction_t* _callback_ = (base::xfunction_t *)call.get_param2().get_function();
                        if(_callback_ != NULL)
                        {
                            _proposal->add_ref(); //hold for asyn call
                            dispatch_call(*_callback_,(void*)_proposal);//send callback to engine'own thread
                        }
                    }
                    else
                    {
                        xwarn("xBFTdriver_t::fire_verify_proposal_job,fail-bad signature tested by verify_sign for cert:%s,at node=0x%llx",_proposal->get_cert()->dump().c_str(),replica_xip.low_addr);
                    }
                }
                return true;
            };

            base::xcall_t asyn_verify_call(_verify_function,(base::xobject_t*)target_proposal,&callback,(base::xobject_t*)this);
            asyn_verify_call.bind_taskid(get_account_index());
            base::xworkerpool_t * _workers_pool = get_workerpool();
            if(_workers_pool != NULL)
                return (_workers_pool->send_call(asyn_verify_call) == enum_xcode_successful);
            else
                return (dispatch_call(asyn_verify_call) == enum_xcode_successful);
        }
        
        bool  xBFTdriver_t::on_proposal_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xdbg_info("xcsdriver_t::on_proposal_msg_recv start at node=0x%llx for packet=%s",get_xip2_low_addr(),packet.dump().c_str());

            const int result = handle_proposal_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);
            if(result != enum_xconsensus_code_successful)
            {
                xwarn("handle_proposal_msg err-code(%d) --> proposal={height=%llu,viewid=%llu,viewtoken=%u} vs local=%s from 0x%llx to 0x%llx",result,packet.get_block_height(),packet.get_block_viewid(),packet.get_block_viewtoken(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                
                std::string msg_stream;
                xvote_report_t _vote_msg(result,dump());
                _vote_msg.set_latest_cert_block(get_latest_cert_block());
                _vote_msg.set_latest_lock_block(get_lock_block());
                _vote_msg.set_latest_commit_block(get_lock_block()->get_prev_block());
                _vote_msg.serialize_to_string(msg_stream);

                base::xcspdu_t & packet = event_obj->_packet;
                base::xauto_ptr<xcspdu_fire>_report_event(new xcspdu_fire());
                _report_event->set_from_xip(to_addr);
                _report_event->set_to_xip(from_addr);
                _report_event->_packet.set_block_chainid(packet.get_block_chainid());
                _report_event->_packet.set_block_account(packet.get_block_account());
                _report_event->_packet.set_block_height(packet.get_block_height());
                _report_event->_packet.set_block_clock(packet.get_block_clock());

                _report_event->_packet.set_block_viewid(packet.get_session_id());
                _report_event->_packet.set_block_viewtoken(packet.get_session_key());

                _report_event->_packet.reset_message(xvote_report_t::get_msg_type(),12,msg_stream,packet.get_msg_nonce() + 1,to_addr.low_addr,from_addr.low_addr);

                _report_event->set_route_path(base::enum_xevent_route_path_up);
                get_parent_node()->push_event_up(*_report_event, this, get_thread_id(), get_time_now());

                return false;
            }
            //xdbg("xcsdriver_t::on_proposal_msg_recv finish --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            return true;
        }

        bool  xBFTdriver_t::on_vote_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xdbg_info("xcsdriver_t::on_vote_msg_recv start --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());

            const int result = handle_vote_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);
            if(result != enum_xconsensus_code_successful)
            {
                xwarn("handle_vote_msg err-code(%d) --> vote={height=%llu,viewid=%llu,viewtoken=%u} vs local=%s from 0x%llx to 0x%llx",result,packet.get_block_height(),packet.get_block_viewid(),packet.get_block_viewtoken(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                
                return false;
            }

            //xdbg("xcsdriver_t::on_vote_msg_recv finish --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            return true;
        }

        bool  xBFTdriver_t::on_commit_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xdbg_info("xcsdriver_t::on_commit_msg_recv start --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());

            const int result = handle_commit_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);
            if(result != enum_xconsensus_code_successful)
            {
                xwarn("handle_commit_msg err-code(%d) --> commit={height=%llu,viewid=%llu,viewtoken=%u} vs local=%s from 0x%llx to 0x%llx",result,packet.get_block_height(),packet.get_block_viewid(),packet.get_block_viewtoken(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                
                return false;
            }

            //xdbg("xcsdriver_t::on_commit_msg_recv finish --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            return true;
        }

        bool  xBFTdriver_t::on_sync_request_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xdbg_info("xcsdriver_t::on_sync_request_msg_recv start --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());

            const int result = handle_sync_request_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);
            if(result != enum_xconsensus_code_successful)
            {
                xwarn("handle_sync_request_msg err-code(%d) --> request proof={height=%llu,viewid=%llu,viewtoken=%u} vs local=%s from 0x%llx to 0x%llx",result,packet.get_block_height(),packet.get_block_viewid(),packet.get_block_viewtoken(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                return false;
            }

            //xdbg("xcsdriver_t::on_sync_request_msg_recv finish --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            return true;
        }

        bool  xBFTdriver_t::on_sync_respond_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            base::xcspdu_t & packet = event_obj->_packet;
            xdbg_info("xcsdriver_t::on_sync_respond_msg_recv start --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());

            const int result = handle_sync_respond_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);
            if(result != enum_xconsensus_code_successful)
            {
                xwarn("handle_sync_respond_msg err-code(%d) --> respond={height=%llu,viewid=%llu,viewtoken=%u} vs local=%s from 0x%llx to 0x%llx",result,packet.get_block_height(),packet.get_block_viewid(),packet.get_block_viewtoken(),dump().c_str(),from_addr.low_addr,to_addr.low_addr);
                return false;
            }

            //xdbg("xcsdriver_t::on_sync_respond_msg_recv finish --> packet=%s at node=0x%llx",packet.dump().c_str(),get_xip2_low_addr());
            return true;
        }
        
        bool  xBFTdriver_t::send_report(const int result,const xvip2_t & from_addr,const xvip2_t & to_addr)
        {
            std::string msg_stream;
            xvote_report_t _vote_msg(result,dump());
            _vote_msg.set_latest_cert_block(get_latest_cert_block());
            _vote_msg.set_latest_lock_block(get_lock_block());
            _vote_msg.set_latest_commit_block(get_lock_block()->get_prev_block());
            _vote_msg.serialize_to_string(msg_stream);
            
            base::xauto_ptr<xcspdu_fire>_report_event(new xcspdu_fire());
            _report_event->set_from_xip(from_addr);
            _report_event->set_to_xip(to_addr);
            _report_event->_packet.set_block_chainid(get_lock_block()->get_chainid());  // use block chainid
            _report_event->_packet.set_block_account(get_account());
            _report_event->_packet.set_block_height(0);
            _report_event->_packet.set_block_clock(0);
            _report_event->_packet.set_block_viewid(0);
            _report_event->_packet.set_block_viewtoken(0);
            
            _report_event->_packet.reset_message(xvote_report_t::get_msg_type(),12,msg_stream,1,from_addr.low_addr,to_addr.low_addr);
            
            _report_event->set_route_path(base::enum_xevent_route_path_up);
            get_parent_node()->push_event_up(*_report_event, this, get_thread_id(), get_time_now());
            
            return true;
        }

        bool  xBFTdriver_t::on_votereport_msg_recv(const xvip2_t & from_addr,const xvip2_t & to_addr,xcspdu_fire * event_obj,int32_t cur_thread_id,uint64_t timenow_ms,xcsobject_t * _parent)
        {
            handle_votereport_msg(from_addr,to_addr,event_obj,cur_thread_id,timenow_ms,_parent);

            return true;
        }

        //note: to return false may call child'push_event_down,or stop further routing when return true
        bool  xBFTdriver_t::on_pdu_event_down(const base::xvevent_t & event,xcsobject_t* from_parent,const int32_t cur_thread_id,const uint64_t timenow_ms)
        {
            const xvip2_t & from_addr = event.get_from_xip();
            const xvip2_t & to_addr   = event.get_to_xip();
            xcspdu_fire * _evt_obj = (xcspdu_fire*)&event;
            if(is_xip2_equal(from_addr, to_addr))
            {
                xwarn("xcsdriver_t::on_pdu_event_down,a loopback' packet=%s at node=0x%llx",_evt_obj->_packet.dump().c_str(),get_xip2_low_addr());
                return true;
            }
            if(_evt_obj->_packet.get_pdu_class() != get_target_pdu_class())//pdu class
            {
                return false;
            }
            switch (_evt_obj->_packet.get_msg_type()) //msg'type is under specific pdu class
            {
                case enum_consensus_msg_type_proposal:
                    return on_proposal_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);

                case enum_consensus_msg_type_vote:
                    return on_vote_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);

                case enum_consensus_msg_type_commit:
                    return on_commit_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);

                case enum_consensus_msg_type_sync_reqt:
                    return on_sync_request_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);

                case enum_consensus_msg_type_sync_resp:
                    return on_sync_respond_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);

                case enum_consensus_msg_type_vote_report:
                    return on_votereport_msg_recv(from_addr,to_addr,_evt_obj,cur_thread_id,timenow_ms,(xcsobject_t*)from_parent);
            }
            return false;
        }
    };//end of namespace of xconsensus

};//end of namespace of top
