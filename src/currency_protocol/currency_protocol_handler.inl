// Copyright (c) 2012-2013 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/interprocess/detail/atomic.hpp>
#include "currency_core/currency_format_utils.h"
#include "profile_tools.h"
namespace currency
{
  namespace
  {
    const command_line::arg_descriptor<bool>               arg_currency_protocol_explicit_set_online = { "explicit-set-online", "Explicitly set node to online mode (needed for launch first node in network)", false, true};
  }
  //-----------------------------------------------------------------------------------------------------------------------  
  template<class t_core>
    t_currency_protocol_handler<t_core>::t_currency_protocol_handler(t_core& rcore, nodetool::i_p2p_endpoint<connection_context>* p_net_layout):m_core(rcore), 
                                                                                                              m_p2p(p_net_layout),
                                                                                                              m_syncronized_connections_count(0),
                                                                                                              m_synchronized(false),
                                                                                                              m_been_synchronized(false),
                                                                                                              m_max_height_seen(0),
                                                                                                              m_core_inital_height(0),
                                                                                                              m_core_current_height(0),
                                                                                                              m_want_stop(false)

  {
    if(!m_p2p)
      m_p2p = &m_p2p_stub;
  }
    //-----------------------------------------------------------------------------------
    template<class t_core>
    void t_currency_protocol_handler<t_core>::init_options(boost::program_options::options_description& desc)
    {
      command_line::add_arg(desc, arg_currency_protocol_explicit_set_online);
    }
    //-----------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::init(const boost::program_options::variables_map& vm)
  {
    if (command_line::has_arg(vm, arg_currency_protocol_explicit_set_online))
      m_been_synchronized = true;
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::deinit()
  {
    m_want_stop = true;

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core> 
  void t_currency_protocol_handler<t_core>::set_p2p_endpoint(nodetool::i_p2p_endpoint<connection_context>* p2p)
  {
    if(p2p)
      m_p2p = p2p;
    else
      m_p2p = &m_p2p_stub;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::on_callback(currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_L2("callback fired");
    CHECK_AND_ASSERT_MES_CC( context.m_callback_request_count > 0, false, "false callback fired, but context.m_callback_request_count=" << context.m_callback_request_count);
    --context.m_callback_request_count;

    if(context.m_state == currency_connection_context::state_synchronizing)
    {
      NOTIFY_REQUEST_CHAIN::request r = boost::value_initialized<NOTIFY_REQUEST_CHAIN::request>();

      bool have_called = false;
      m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
      {
        m_core.get_short_chain_history(r.block_ids);
        return true;
      });
      if (have_called)
      {
        LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_REQUEST_CHAIN: m_block_ids.size()=" << r.block_ids.size());
        post_notify<NOTIFY_REQUEST_CHAIN>(r, context);
      }
      else
      {
        LOG_PRINT_CCONTEXT_MAGENTA("[On_callback] Changed connection state to state_idle while core is in batch update", LOG_LEVEL_0);
        context.m_state = currency_connection_context::state_idle;
      }
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::get_stat_info(core_stat_info& stat_inf)
  {
    bool have_called = false;
    bool res = m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
    {
      return m_core.get_stat_info(stat_inf);
    });
    return have_called && res;
  }
  //------------------------------------------------------------------------------------------------------------------------   
  template<class t_core> 
  void t_currency_protocol_handler<t_core>::log_connections()
  {
    std::stringstream ss;

    ss << std::setw(29) << std::left << "Remote Host" 
      << std::setw(20) << "Peer id"
      << std::setw(25) << "Recv/Sent (idle,sec)"
      << std::setw(25) << "State"
      << std::setw(20) << "Height"
      << std::setw(20) << "Livetime (sec)"
      << std::setw(20) << "Version" << ENDL;

    m_p2p->for_each_connection([&](const connection_context& cntxt, nodetool::peerid_type peer_id)
    {
      ss << std::setw(29) << std::left << std::string(cntxt.m_is_income ? "[INC]":"[OUT]") + 
        string_tools::get_ip_string_from_int32(cntxt.m_remote_ip) + ":" + std::to_string(cntxt.m_remote_port) 
        << std::setw(20) << std::hex << peer_id
        << std::setw(25) << std::to_string(cntxt.m_recv_cnt)+ "(" + std::to_string(time(NULL) - cntxt.m_last_recv) + ")" + "/" + std::to_string(cntxt.m_send_cnt) + "(" + std::to_string(time(NULL) - cntxt.m_last_send) + ")"
        << std::setw(25) << get_protocol_state_string(cntxt.m_state) 
        << std::setw(20) << std::dec << cntxt.m_remote_blockchain_height
        << std::setw(20) << std::to_string(time(NULL) - cntxt.m_started)
        << std::setw(20) << cntxt.m_remote_version
        << ENDL;
      return true;
    });
    LOG_PRINT_L0("Connections: " << ENDL << ss.str());
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::process_payload_sync_data(const CORE_SYNC_DATA& hshd, currency_connection_context& context, bool is_inital)
  {
    context.m_remote_version = hshd.client_version;
    context.m_remote_blockchain_height = hshd.current_height;
    LOG_PRINT_MAGENTA("[PROCESS_PAYLOAD_SYNC_DATA][m_been_synchronized=" << m_been_synchronized << "]: hshd.current_height = " << hshd.current_height << "(" << hshd.top_id << ")", LOG_LEVEL_3);
    if (context.m_state == currency_connection_context::state_befor_handshake && !is_inital)
    {
      LOG_PRINT_L3("[PROCESS_PAYLOAD_SYNC_DATA]: state_befor_handshake, ignored");
      return true;
    }

    if (context.m_state == currency_connection_context::state_synchronizing)
    {
      LOG_PRINT_L3("[PROCESS_PAYLOAD_SYNC_DATA]:m_state = state_synchronizing, ignored");
      return true;
    }

    //while we not synchronized, we have to keep major amount of connections with nodes servicing our blockchain download
    if (!m_been_synchronized && !context.m_is_income && hshd.current_height == 1 && is_inital)
    {
      LOG_PRINT_CCONTEXT_MAGENTA("[PROCESS_PAYLOAD_SYNC_DATA()]: rejected busy node.", LOG_LEVEL_0);
      m_p2p->drop_connection(context);
      return true;
    }

    bool have_called = false;
    bool res = m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
    {
      if (m_core.have_block(hshd.top_id))
      {
        LOG_PRINT_MAGENTA("[PROCESS_PAYLOAD_SYNC_DATA]: remote top " << hshd.top_id << " is already in core, m_state set to state_normal", LOG_LEVEL_3);
        context.m_state = currency_connection_context::state_normal;
        return true;
      }
      int64_t diff = static_cast<int64_t>(hshd.current_height) - static_cast<int64_t>(m_core.get_current_blockchain_height());
      LOG_PRINT_CCONTEXT_YELLOW("Sync data returned unknown top block: " << m_core.get_current_blockchain_height() << " -> " << hshd.current_height
        << " [" << std::abs(diff) << " blocks (" << diff / (24 * 60 * 60 / DIFFICULTY_TARGET) << " days) "
        << (0 <= diff ? std::string("behind") : std::string("ahead"))
        << "] " << ENDL << "SYNCHRONIZATION started", (is_inital ? LOG_LEVEL_0 : LOG_LEVEL_1));
      LOG_PRINT_L1("Remote top block height: " << hshd.current_height << ", id: " << hshd.top_id);

      /*check if current height is in remote checkpoints zone*/
      if (hshd.last_checkpoint_height
        && m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height() < hshd.last_checkpoint_height
        && m_core.get_current_blockchain_height() < hshd.last_checkpoint_height)
      {
        LOG_PRINT_CCONTEXT_RED("Remote node have longer checkpoints zone( " << hshd.last_checkpoint_height << ") " <<
          "that local (" << m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height() << ")" <<
          "That means that current software is outdated, please updated it." <<
          "Current heigh lay under checkpoints on remote host, so it is not possible validate this transactions on local host, disconnecting.", LOG_LEVEL_0);
        return false;
      }
      else if (m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height() < hshd.last_checkpoint_height)
      {
        LOG_PRINT_CCONTEXT_MAGENTA("Remote node have longer checkpoints zone( " << hshd.last_checkpoint_height << ") " <<
          "that local (" << m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height() << ")" <<
          "That means that current software is outdated, please updated it.", LOG_LEVEL_0);
      }

      LOG_PRINT_MAGENTA("[PROCESS_PAYLOAD_SYNC_DATA]: m_state set to state_synchronizing", LOG_LEVEL_3);
      context.m_state = currency_connection_context::state_synchronizing;
      context.m_remote_blockchain_height = hshd.current_height;
      //let the socket to send response to handshake, but request callback, to let send request data after response
      LOG_PRINT_CCONTEXT_L2("requesting callback");
      ++context.m_callback_request_count;
      m_p2p->request_callback(context);
      //update progress vars 
      if (m_max_height_seen < hshd.current_height)
        m_max_height_seen = hshd.current_height;
      if (!m_core_inital_height)
      {
        m_core_current_height = m_core_inital_height = m_core.get_current_blockchain_height();        
      }
      return true;
    });

    if (!have_called)
    {
      LOG_PRINT_CCONTEXT_MAGENTA("[PROCESS_PAYLOAD_SYNC_DATA(is_inital=" << is_inital<<")]: call blocked.set to state_idle", LOG_LEVEL_0);
      context.m_state = currency_connection_context::state_idle;
      return true;
    }



    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core>
  uint64_t t_currency_protocol_handler<t_core>::get_core_inital_height()
  {
    return m_core_inital_height;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core>
  uint64_t t_currency_protocol_handler<t_core>::get_core_current_height()
  {
    return m_core_current_height;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core>
  uint64_t t_currency_protocol_handler<t_core>::get_max_seen_height()
  {
    return m_max_height_seen;
  }
  //------------------------------------------------------------------------------------------------------------------------  
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::get_payload_sync_data(CORE_SYNC_DATA& hshd)
  {
    hshd.client_version = PROJECT_VERSION_LONG;
    bool have_called = false;
    
    m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
    {
      m_core.get_blockchain_top(hshd.current_height, hshd.top_id);
      hshd.current_height += 1;
      hshd.last_checkpoint_height = m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height();
      LOG_PRINT_MAGENTA("[GET_PAYLOAD_SYNC_DATA][m_been_synchronized=" << m_been_synchronized << "]: hshd.current_height " << hshd.current_height, LOG_LEVEL_2);
      return true;
    });
    if (!have_called)
    {
      //wile we sync say to others that we have only genesis to avoid sync requests
      hshd.current_height = 1;
      hshd.top_id = get_genesis_id();
      hshd.last_checkpoint_height = m_core.get_blockchain_storage().get_checkpoints().get_top_checkpoint_height();
      LOG_PRINT_MAGENTA("[GET_PAYLOAD_SYNC_DATA][m_been_synchronized=" << m_been_synchronized << "]: call blocked.", LOG_LEVEL_2);
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------  
    template<class t_core> 
    bool t_currency_protocol_handler<t_core>::get_payload_sync_data(blobdata& data)
  {
    CORE_SYNC_DATA hsd = boost::value_initialized<CORE_SYNC_DATA>();
    get_payload_sync_data(hsd);
    epee::serialization::store_t_to_binary(hsd, data);
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------  
    template<class t_core> 
    int t_currency_protocol_handler<t_core>::handle_notify_new_block(int command, NOTIFY_NEW_BLOCK::request& arg, currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_L2("NOTIFY_NEW_BLOCK (hop " << arg.hop << ")");
    if (!m_synchronized || context.m_state != currency_connection_context::state_normal || context.m_remote_blockchain_height <=1)
      return 1;

    for(auto tx_blob_it = arg.b.txs.begin(); tx_blob_it!=arg.b.txs.end();tx_blob_it++)
    {
      currency::tx_verification_context tvc = AUTO_VAL_INIT(tvc);
      m_core.handle_incoming_tx(*tx_blob_it, tvc, true);
      if(tvc.m_verifivation_failed)
      {
        LOG_PRINT_CCONTEXT_L0("Block verification failed: transaction verification failed, dropping connection");
        m_p2p->drop_connection(context);
        return 1;
      }
    }
    

    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    m_core.pause_mine();
    m_core.handle_incoming_block(arg.b.block, bvc);
    m_core.resume_mine();
    if(bvc.m_verifivation_failed)
    {
      LOG_PRINT_CCONTEXT_L0("Block verification failed, dropping connection");
      m_p2p->drop_connection(context);
      return 1;
    }
    if(bvc.m_added_to_main_chain)
    {
      m_core_current_height = bvc.height;
      ++arg.hop;
      //TODO: Add here announce protocol usage
      relay_block(arg, context);
    }else if(bvc.m_marked_as_orphaned)
    {
      LOG_PRINT_MAGENTA("[NOTIFY_NEW_BLOCK]: m_state set state_synchronizing", LOG_LEVEL_3);
      context.m_state = currency_connection_context::state_synchronizing;
      NOTIFY_REQUEST_CHAIN::request r = boost::value_initialized<NOTIFY_REQUEST_CHAIN::request>();
      m_core.get_short_chain_history(r.block_ids);
      LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_REQUEST_CHAIN: m_block_ids.size()=" << r.block_ids.size() );
      post_notify<NOTIFY_REQUEST_CHAIN>(r, context);
    }
      
    return 1;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  int t_currency_protocol_handler<t_core>::handle_notify_new_transactions(int command, NOTIFY_NEW_TRANSACTIONS::request& arg, currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_L2("NOTIFY_NEW_TRANSACTIONS");

    if (!m_synchronized || context.m_state != currency_connection_context::state_normal || context.m_remote_blockchain_height <= 1)
      return 1;


    for(auto tx_blob_it = arg.txs.begin(); tx_blob_it!=arg.txs.end();)
    {
      currency::tx_verification_context tvc = AUTO_VAL_INIT(tvc);
      m_core.handle_incoming_tx(*tx_blob_it, tvc, false);
      if(tvc.m_verifivation_failed)
      {
        LOG_PRINT_CCONTEXT_L0("Tx verification failed, dropping connection");
        m_p2p->drop_connection(context);

        return 1;
      }
      if(tvc.m_should_be_relayed)
        ++tx_blob_it;
      else
        arg.txs.erase(tx_blob_it++);
    }

    if(arg.txs.size())
    {
      //TODO: add announce usage here
      relay_transactions(arg, context);
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  int t_currency_protocol_handler<t_core>::handle_request_get_objects(int command, NOTIFY_REQUEST_GET_OBJECTS::request& arg, currency_connection_context& context)
  {
    if (arg.blocks.size() > CURRENCY_PROTOCOL_MAX_BLOCKS_REQUEST_COUNT || 
      arg.txs.size() > CURRENCY_PROTOCOL_MAX_TXS_REQUEST_COUNT)
    {
      LOG_ERROR_CCONTEXT("Requested objects count is to big (" << arg.blocks.size() << ")expected not more then " << CURRENCY_PROTOCOL_MAX_BLOCKS_REQUEST_COUNT);
      m_p2p->drop_connection(context);
    }

    if (!m_been_synchronized)
    {
      LOG_ERROR_CCONTEXT("Internal error: got NOTIFY_REQUEST_GET_OBJECTS while m_been_synchronized = false, dropping connection");
      return LEVIN_ERROR_INTERNAL;
    }


    LOG_PRINT_CCONTEXT_L2("NOTIFY_REQUEST_GET_OBJECTS");
    NOTIFY_RESPONSE_GET_OBJECTS::request rsp;
    if(!m_core.handle_get_objects(arg, rsp, context))
    {
      LOG_ERROR_CCONTEXT("failed to handle request NOTIFY_REQUEST_GET_OBJECTS, dropping connection");
      m_p2p->drop_connection(context);
    }
    LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_RESPONSE_GET_OBJECTS: blocks.size()=" << rsp.blocks.size() << ", txs.size()=" << rsp.txs.size() 
                            << ", rsp.m_current_blockchain_height=" << rsp.current_blockchain_height << ", missed_ids.size()=" << rsp.missed_ids.size());
    post_notify<NOTIFY_RESPONSE_GET_OBJECTS>(rsp, context);
    return 1;
  }

  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core>
  bool t_currency_protocol_handler<t_core>::check_stop_flag_and_exit(currency_connection_context& context)
  {
    if (m_p2p->is_stop_signal_sent() || m_want_stop)
    {
      m_p2p->drop_connection(context);
      return true;
    }
    return false;
  }
  //------------------------------------------------------------------------------------------------------------------------
#define CHECK_STOP_FLAG_EXIT_IF_SET(ret_v, msg) if (check_stop_flag_and_exit(context)) { LOG_PRINT_YELLOW("Stop flag detected within NOTIFY_RESPONSE_GET_OBJECTS. " << msg, LOG_LEVEL_0); return ret_v; }
  template<class t_core>
  int t_currency_protocol_handler<t_core>::handle_response_get_objects(int command, NOTIFY_RESPONSE_GET_OBJECTS::request& arg, currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_YELLOW("NOTIFY_RESPONSE_GET_OBJECTS: " << arg.blocks.size() << " blocks, " << arg.txs.size() << " txs, remote height: " << arg.current_blockchain_height, LOG_LEVEL_2);

    if (context.m_last_response_height > arg.current_blockchain_height)
    {
      LOG_ERROR_CCONTEXT("sent wrong NOTIFY_HAVE_OBJECTS: arg.m_current_blockchain_height=" << arg.current_blockchain_height
        << " < m_last_response_height=" << context.m_last_response_height << ", dropping connection");
      m_p2p->drop_connection(context);
      return 1;
    }

    context.m_remote_blockchain_height = arg.current_blockchain_height;

    PROF_L2_DO(uint64_t syncing_conn_count_sum = get_synchronizing_connections_count(); uint64_t syncing_conn_count_count = 1);

    bool have_called = false;
    int res = m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<int>(have_called, [&]()
    {
      PROF_L1_START(block_complete_entries_prevalidation_time);
      size_t count = 0;
      for (const block_complete_entry& block_entry : arg.blocks)
      {
        CHECK_STOP_FLAG_EXIT_IF_SET(1, "Blocks processing interrupted, connection dropped");

        ++count;
        block b;
        if (!parse_and_validate_block_from_blob(block_entry.block, b))
        {
          LOG_ERROR_CCONTEXT("sent wrong block: failed to parse and validate block: \r\n"
            << string_tools::buff_to_hex_nodelimer(block_entry.block) << "\r\n dropping connection");
          m_p2p->drop_connection(context);
          m_p2p->add_ip_fail(context.m_remote_ip);
          return 1;
        }
        //to avoid concurrency in core between connections, suspend connections which delivered block later then first one
        if (count == 2)
        {
          if (m_core.have_block(get_block_hash(b)))
          {
            LOG_PRINT_MAGENTA("[RESPONSE_GET_OBJECTS]: m_state set state_idle", LOG_LEVEL_3);
            context.m_state = currency_connection_context::state_idle;
            context.m_needed_objects.clear();
            context.m_requested_objects.clear();
            LOG_PRINT_CCONTEXT_L1("Connection set to idle state.");
            return 1;
          }
        }

        auto req_it = context.m_requested_objects.find(get_block_hash(b));
        if (req_it == context.m_requested_objects.end())
        {
          LOG_ERROR_CCONTEXT("sent wrong NOTIFY_RESPONSE_GET_OBJECTS: block with id=" << string_tools::pod_to_hex(get_blob_hash(block_entry.block))
            << " wasn't requested, dropping connection");
          m_p2p->drop_connection(context);
          return 1;
        }
        if (b.tx_hashes.size() != block_entry.txs.size())
        {
          LOG_ERROR_CCONTEXT("sent wrong NOTIFY_RESPONSE_GET_OBJECTS: block with id=" << string_tools::pod_to_hex(get_blob_hash(block_entry.block))
            << ", tx_hashes.size()=" << b.tx_hashes.size() << " mismatch with block_complete_entry.m_txs.size()=" << block_entry.txs.size() << ", dropping connection");
          m_p2p->drop_connection(context);
          return 1;
        }

        context.m_requested_objects.erase(req_it);
      }
      PROF_L1_FINISH(block_complete_entries_prevalidation_time);

      PROF_L2_DO(syncing_conn_count_sum += get_synchronizing_connections_count(); ++syncing_conn_count_count);

      if (context.m_requested_objects.size())
      {
        LOG_PRINT_CCONTEXT_RED("returned not all requested objects (context.m_requested_objects.size()="
          << context.m_requested_objects.size() << "), dropping connection", LOG_LEVEL_0);
        m_p2p->drop_connection(context);
        return 1;
      }

      PROF_L1_START(blocks_handle_time);
      {
        m_core.pause_mine();
        m_core.get_tx_pool().lock();
        m_core.get_blockchain_storage().start_batch_exclusive_operation();
        bool success = false;
        misc_utils::auto_scope_leave_caller scope_exit_handler = misc_utils::create_scope_leave_handler([&, this](){
          boost::bind(&t_core::resume_mine, &m_core);
          m_core.get_blockchain_storage().finish_batch_exclusive_operation(success);
          m_core.get_tx_pool().unlock();
        });

        BOOST_FOREACH(const block_complete_entry& block_entry, arg.blocks)
        {
          CHECK_STOP_FLAG_EXIT_IF_SET(1, "Blocks processing interrupted, connection dropped");
          //process transactions
          PROF_L1_START(transactions_process_time);
          BOOST_FOREACH(auto& tx_blob, block_entry.txs)
          {
            //CHECK_STOP_FLAG_EXIT_IF_SET(1, "Blocks processing interrupted, connection dropped");
            if (check_stop_flag_and_exit(context)) 
            {             
              LOG_PRINT_YELLOW("Stop flag detected within NOTIFY_RESPONSE_GET_OBJECTS. ", LOG_LEVEL_0);
              //commit transaction
              success = true;
              return 1; 
            }
            tx_verification_context tvc = AUTO_VAL_INIT(tvc);
            m_core.handle_incoming_tx(tx_blob, tvc, true);
            if (tvc.m_verifivation_failed)
            {
              LOG_ERROR_CCONTEXT("transaction verification failed on NOTIFY_RESPONSE_GET_OBJECTS, \r\ntx_id = "
                << string_tools::pod_to_hex(get_blob_hash(tx_blob)) << ", dropping connection");
              m_p2p->drop_connection(context);
              return 1;
            }
          }
          PROF_L1_FINISH(transactions_process_time);

          //process block
          PROF_L1_START(block_process_time);
          block_verification_context bvc = boost::value_initialized<block_verification_context>();

          m_core.handle_incoming_block(block_entry.block, bvc, false);

          if (bvc.m_verifivation_failed)
          {
            LOG_PRINT_CCONTEXT_L0("Block verification failed, dropping connection");
            m_p2p->drop_connection(context);
            m_p2p->add_ip_fail(context.m_remote_ip);
            return 1;
          }
          if (bvc.m_marked_as_orphaned)
          {
            LOG_PRINT_CCONTEXT_L0("Block received at sync phase was marked as orphaned, dropping connection");
            m_p2p->drop_connection(context);
            m_p2p->add_ip_fail(context.m_remote_ip);
            return 1;
          }
          m_core_current_height = bvc.height;
          PROF_L1_FINISH(block_process_time);
          PROF_L1_DO(LOG_PRINT_CCONTEXT_L2("Block process time: " << print_mcsec_as_ms(block_process_time + transactions_process_time) << "(" << print_mcsec_as_ms(transactions_process_time) << "/" << print_mcsec_as_ms(block_process_time) << ") ms"));

          PROF_L2_DO(syncing_conn_count_sum += get_synchronizing_connections_count(); ++syncing_conn_count_count);
        }
        success = true;
      }
      PROF_L1_FINISH(blocks_handle_time);

      uint64_t current_height = m_core.get_current_blockchain_height();
      LOG_PRINT_CCONTEXT_YELLOW(">>>>>>>>> sync progress: " << arg.blocks.size() << " blocks added"
        "(" << print_mcsec_as_ms(blocks_handle_time) << "+" << print_mcsec_as_ms(block_complete_entries_prevalidation_time) << "), now have "
        << current_height << " of " << context.m_remote_blockchain_height
        << " ( " << std::fixed << std::setprecision(2) << current_height * 100.0 / context.m_remote_blockchain_height << "% ) and "
        << context.m_remote_blockchain_height - current_height << " blocks left"
        , LOG_LEVEL_0);

        PROF_L2_DO(syncing_conn_count_sum += get_synchronizing_connections_count(); ++syncing_conn_count_count);
     


#if PROFILING_LEVEL >= 2
      double syncing_conn_count_av = syncing_conn_count_sum / static_cast<double>(syncing_conn_count_count);
      size_t blocks_count = arg.blocks.size();
      LOG_PRINT_CCONTEXT_YELLOW("NOTIFY_RESPONSE_GET_OBJECTS: " << blocks_count << " blocks were prevalidated in " << block_complete_entries_prevalidation_time / 1000
        << " ms (" << std::fixed << std::setprecision(2) << block_complete_entries_prevalidation_time / 1000.0f / blocks_count << " ms per block av) and handled in " << blocks_handle_time / 1000
        << " ms (" << std::fixed << std::setprecision(2) << blocks_handle_time / 1000.0f / blocks_count << " ms per block av)"
        << " syncing conns av: " << std::fixed << std::setprecision(2) << syncing_conn_count_av, LOG_LEVEL_1);
#endif

      request_missing_objects(context, true);
      return 1;
    });

    if (!have_called)
    {
      context.m_state = currency_connection_context::state_idle;
      LOG_PRINT_CCONTEXT_MAGENTA("[HANDLE_RESPONSE_GET_OBJECTS]: Core blocked response, m_state set state_idle", LOG_LEVEL_0);
    }
      
    return res;
  }
#undef CHECK_STOP_FLAG__DROP_AND_RETURN_IF_SET
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::on_idle()
  {

    size_t count_synced = 0;
    size_t count_total = 0;
    m_p2p->for_each_connection([&](currency_connection_context& context, nodetool::peerid_type peer_id)->bool{
      if (context.m_state == currency_connection_context::state_normal && context.m_remote_blockchain_height > 1)
      {
        ++count_synced;
      }
      ++count_total;
      return true;
    });

    if (count_total && count_synced  && count_synced > count_total / 2 && !m_synchronized)
    {
      on_connection_synchronized();
      m_synchronized = true;
      m_been_synchronized = true;
      LOG_PRINT_MAGENTA("Synchronized set to TRUE (idle)", LOG_LEVEL_0);
    }
    else if ((!count_total || count_synced < count_total / 3) && m_synchronized)
    {
      LOG_PRINT_MAGENTA("Synchronized set to FALSE (idle)", LOG_LEVEL_0);
      m_synchronized = false;
    }

    return m_core.on_idle();
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core>
  int t_currency_protocol_handler<t_core>::handle_request_chain(int command, NOTIFY_REQUEST_CHAIN::request& arg, currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_L2("NOTIFY_REQUEST_CHAIN: m_been_synchronized = " << m_been_synchronized  << "m_block_ids.size()=" << arg.block_ids.size());
    NOTIFY_RESPONSE_CHAIN_ENTRY::request r;
    //workaround to less load of the core storage of requests from other seeds while it being synchronized
    if (!m_been_synchronized)
    {
      r.m_block_ids.push_back(get_genesis_id());
      r.start_height = 0;
      r.total_height = 1;
      LOG_PRINT_CCONTEXT_MAGENTA("[HANDLE_REQUEST_CHAIN][Unsyncronized]Income chain request responded with empty chain.", LOG_LEVEL_0);
      LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_RESPONSE_CHAIN_ENTRY: m_start_height=" << r.start_height << ", m_total_height=" << r.total_height << ", m_block_ids.size()=" << r.m_block_ids.size());
      post_notify<NOTIFY_RESPONSE_CHAIN_ENTRY>(r, context);
      return 1;
    }

    bool have_called = false;
    int res = m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
    {
      if (!m_core.find_blockchain_supplement(arg.block_ids, r))
      {
        LOG_ERROR_CCONTEXT("Internal error: failed to handle NOTIFY_REQUEST_CHAIN.");
        return 1;
      }
      LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_RESPONSE_CHAIN_ENTRY: m_start_height=" << r.start_height << ", m_total_height=" << r.total_height << ", m_block_ids.size()=" << r.m_block_ids.size());
      post_notify<NOTIFY_RESPONSE_CHAIN_ENTRY>(r, context);
      return 1;
    });

    if (!have_called)
    {
      r.m_block_ids.push_back(get_genesis_id());
      r.start_height = 0;
      r.total_height = 1;
      LOG_PRINT_CCONTEXT_MAGENTA("[HANDLE_REQUEST_CHAIN]Core blocked, respond empty chain.", LOG_LEVEL_0);
      LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_RESPONSE_CHAIN_ENTRY: m_start_height=" << r.start_height << ", m_total_height=" << r.total_height << ", m_block_ids.size()=" << r.m_block_ids.size());
      post_notify<NOTIFY_RESPONSE_CHAIN_ENTRY>(r, context);
    }
    return res;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::request_missing_objects(currency_connection_context& context, bool check_having_blocks)
  {
    if(context.m_needed_objects.size())
    {
      //we know objects that we need, request this objects
      NOTIFY_REQUEST_GET_OBJECTS::request req;
      size_t count = 0;
      auto it = context.m_needed_objects.begin();

      while(it != context.m_needed_objects.end() && count < BLOCKS_SYNCHRONIZING_DEFAULT_COUNT)
      {
        if( !(check_having_blocks && m_core.have_block(*it)))
        {
          req.blocks.push_back(*it);
          ++count;
          context.m_requested_objects.insert(*it);
        }
        context.m_needed_objects.erase(it++);
      }
      LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_REQUEST_GET_OBJECTS: blocks.size()=" << req.blocks.size() << ", txs.size()=" << req.txs.size());
      post_notify<NOTIFY_REQUEST_GET_OBJECTS>(req, context);    
    }else if(context.m_last_response_height < context.m_remote_blockchain_height-1)
    {//we have to fetch more objects ids, request blockchain entry
     
      NOTIFY_REQUEST_CHAIN::request r = boost::value_initialized<NOTIFY_REQUEST_CHAIN::request>();
      bool have_called = false;
      m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
      {
        return m_core.get_short_chain_history(r.block_ids);
      });
      if (!have_called)
      {
        LOG_PRINT_CCONTEXT_MAGENTA("[REQUEST_MISSING_OBJECTS] core request blocked, m_state set state_idle", LOG_LEVEL_0);
        context.m_state = currency_connection_context::state_idle;
      }
      else
      {
        LOG_PRINT_CCONTEXT_L2("-->>NOTIFY_REQUEST_CHAIN: m_block_ids.size()=" << r.block_ids.size());
        post_notify<NOTIFY_REQUEST_CHAIN>(r, context);
      }
    }else
    { 
      CHECK_AND_ASSERT_MES(context.m_last_response_height == context.m_remote_blockchain_height-1 
                           && !context.m_needed_objects.size() 
                           && !context.m_requested_objects.size(), false, "request_missing_blocks final condition failed!" 
                           << "\r\nm_last_response_height=" << context.m_last_response_height
                           << "\r\nm_remote_blockchain_height=" << context.m_remote_blockchain_height
                           << "\r\nm_needed_objects.size()=" << context.m_needed_objects.size()
                           << "\r\nm_requested_objects.size()=" << context.m_requested_objects.size()
                           << "\r\non connection [" << net_utils::print_connection_context_short(context)<< "]");
      
      LOG_PRINT_CCONTEXT_MAGENTA("[REQUEST_MISSING_OBJECTS] m_state set state_normal", LOG_LEVEL_0);
      context.m_state = currency_connection_context::state_normal;
      LOG_PRINT_CCONTEXT_GREEN(" SYNCHRONIZED OK", LOG_LEVEL_0);
      do_force_handshake_idle_connections();
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core>
  bool t_currency_protocol_handler<t_core>::do_force_handshake_idle_connections()
  {
    nodetool::connections_list_type peer_list;
    m_p2p->for_each_connection([&](currency_connection_context& context, nodetool::peerid_type peer_id)->bool{
      if (context.m_state == currency_connection_context::state_idle)
      {
        peer_list.push_back(std::make_pair(static_cast<net_utils::connection_context_base>(context), peer_id));
      }
      return true;
    });
    if (peer_list.size())
    {
      LOG_PRINT_CCONTEXT_MAGENTA("[ON_SYNCHRONIZED] Explicit resync idle connections (" << peer_list.size() << ")", LOG_LEVEL_0);
      return m_p2p->do_idle_sync_with_peers(peer_list);
    }
    else 
      return true;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::on_connection_synchronized()
  {
    bool val_expected = false;
    if(m_synchronized.compare_exchange_strong(val_expected, true))
    {
      LOG_PRINT_L0(ENDL << "**********************************************************************" << ENDL 
        << "You are now synchronized with the network. You may now start simplewallet." << ENDL 
        << ENDL
        << "Please note, that the blockchain will be saved only after you quit the daemon with \"exit\" command or if you use \"save\" command." << ENDL 
        << "Otherwise, you will possibly need to synchronize the blockchain again." << ENDL
        << ENDL
        << "Use \"help\" command to see the list of available commands." << ENDL
        << "**********************************************************************");
      m_core.on_synchronized();
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  size_t t_currency_protocol_handler<t_core>::get_synchronizing_connections_count()
  {
    size_t count = 0;
    m_p2p->for_each_connection([&](currency_connection_context& context, nodetool::peerid_type peer_id)->bool{
      if(context.m_state == currency_connection_context::state_synchronizing)
        ++count;
      return true;
    });
    return count;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  int t_currency_protocol_handler<t_core>::handle_response_chain_entry(int command, NOTIFY_RESPONSE_CHAIN_ENTRY::request& arg, currency_connection_context& context)
  {
    LOG_PRINT_CCONTEXT_L2("NOTIFY_RESPONSE_CHAIN_ENTRY: m_block_ids.size()=" << arg.m_block_ids.size() 
      << ", m_start_height=" << arg.start_height << ", m_total_height=" << arg.total_height);
    
    if(!arg.m_block_ids.size())
    {
      LOG_ERROR_CCONTEXT("sent empty m_block_ids, dropping connection");
      m_p2p->drop_connection(context);
      m_p2p->add_ip_fail(context.m_remote_ip);
      return 1;
    }

    bool have_called = false;
    m_core.get_blockchain_storage().template call_if_no_batch_exclusive_operation<bool>(have_called, [&]()
    {
      if (!m_core.have_block(arg.m_block_ids.front()))
      {
        LOG_ERROR_CCONTEXT("sent m_block_ids starting from unknown id: "
          << string_tools::pod_to_hex(arg.m_block_ids.front()) << " , dropping connection");
        m_p2p->drop_connection(context);
        m_p2p->add_ip_fail(context.m_remote_ip);
        return 1;
      }

      context.m_remote_blockchain_height = arg.total_height;
      context.m_last_response_height = arg.start_height + arg.m_block_ids.size() - 1;
      if (context.m_last_response_height > context.m_remote_blockchain_height)
      {
        LOG_ERROR_CCONTEXT("sent wrong NOTIFY_RESPONSE_CHAIN_ENTRY, with \r\nm_total_height=" << arg.total_height
          << "\r\nm_start_height=" << arg.start_height
          << "\r\nm_block_ids.size()=" << arg.m_block_ids.size());
        m_p2p->drop_connection(context);
      }

      for(auto& bl_id: arg.m_block_ids)
      {
        if (check_stop_flag_and_exit(context))
          return 1;
        if (!m_core.have_block(bl_id))
          context.m_needed_objects.push_back(bl_id);
      }

      request_missing_objects(context, false);
      return 1;
    });



    return 1;
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::relay_block(NOTIFY_NEW_BLOCK::request& arg, currency_connection_context& exclude_context)
  {
    return relay_post_notify<NOTIFY_NEW_BLOCK>(arg, exclude_context);
  }
  //------------------------------------------------------------------------------------------------------------------------
  template<class t_core> 
  bool t_currency_protocol_handler<t_core>::relay_transactions(NOTIFY_NEW_TRANSACTIONS::request& arg, currency_connection_context& exclude_context)
  {
    return relay_post_notify<NOTIFY_NEW_TRANSACTIONS>(arg, exclude_context);
  }
}
