// Copyright (c) 2012-2013 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "include_base_utils.h"
using namespace epee;

#include "wallet_rpc_server.h"
#include "common/command_line.h"
#include "currency_core/currency_format_utils.h"
#include "currency_core/account.h"
#include "misc_language.h"
#include "crypto/hash.h"

namespace tools
{
  //-----------------------------------------------------------------------------------
  const command_line::arg_descriptor<std::string> wallet_rpc_server::arg_rpc_bind_port = {"rpc-bind-port", "Starts wallet as rpc server for wallet operations, sets bind port for server", "", true};
  const command_line::arg_descriptor<std::string> wallet_rpc_server::arg_rpc_bind_ip = {"rpc-bind-ip", "Specify ip to bind rpc server", "127.0.0.1"};

  void wallet_rpc_server::init_options(boost::program_options::options_description& desc)
  {
    command_line::add_arg(desc, arg_rpc_bind_ip);
    command_line::add_arg(desc, arg_rpc_bind_port);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  wallet_rpc_server::wallet_rpc_server(wallet2& w):m_wallet(w)
  {}
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::run(bool offline_mode)
  {
    if (!offline_mode)
    {
      m_net_server.add_idle_handler([this](){
        size_t blocks_fetched = 0;
        bool received_money = false;
        bool ok;
        m_wallet.refresh(blocks_fetched, received_money, ok);
        return true;
      }, 20000);
    }

    //DO NOT START THIS SERVER IN MORE THEN 1 THREADS WITHOUT REFACTORING
    return epee::http_server_impl_base<wallet_rpc_server, connection_context>::run(1, true);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::handle_command_line(const boost::program_options::variables_map& vm)
  {
    m_bind_ip = command_line::get_arg(vm, arg_rpc_bind_ip);
    m_port = command_line::get_arg(vm, arg_rpc_bind_port);
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::init(const boost::program_options::variables_map& vm)
  {
    m_net_server.set_threads_prefix("RPC");
    bool r = handle_command_line(vm);
    CHECK_AND_ASSERT_MES(r, false, "Failed to process command line in core_rpc_server");
    return epee::http_server_impl_base<wallet_rpc_server, connection_context>::init(m_port, m_bind_ip);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_getbalance(const wallet_rpc::COMMAND_RPC_GET_BALANCE::request& req, wallet_rpc::COMMAND_RPC_GET_BALANCE::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    try
    {
      res.balance = m_wallet.balance();
      res.unlocked_balance = m_wallet.unlocked_balance();
    }
    catch (std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = e.what();
      return false;
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_getaddress(const wallet_rpc::COMMAND_RPC_GET_ADDRESS::request& req, wallet_rpc::COMMAND_RPC_GET_ADDRESS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    try
    {
      res.address = m_wallet.get_account().get_public_address_str();
    }
    catch (std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = e.what();
      return false;
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_transfer(const wallet_rpc::COMMAND_RPC_TRANSFER::request& req, wallet_rpc::COMMAND_RPC_TRANSFER::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::payment_id_t payment_id;
    if (!req.payment_id_hex.empty() && !currency::parse_payment_id_from_hex_str(req.payment_id_hex, payment_id))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
      er.message = std::string("Invalid payment id: ") + req.payment_id_hex;
      return false;
    }

    std::vector<currency::tx_destination_entry> dsts;
    for (auto it = req.destinations.begin(); it != req.destinations.end(); it++) 
    {
      currency::tx_destination_entry de;
      currency::payment_id_t integrated_payment_id;
      if (!m_wallet.get_transfer_address(it->address, de.addr, integrated_payment_id))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_ADDRESS;
        er.message = std::string("WALLET_RPC_ERROR_CODE_WRONG_ADDRESS: ") + it->address;
        return false;
      }
      
      if (!integrated_payment_id.empty())
      {
        if (!payment_id.empty())
        {
          er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
          er.message = std::string("address ") + it->address + " has integrated payment id " + epee::string_tools::buff_to_hex_nodelimer(integrated_payment_id) +
            " which is incompatible with payment id " + epee::string_tools::buff_to_hex_nodelimer(payment_id) + " that was already assigned to this transfer";
          return false;
        }
        payment_id = integrated_payment_id;
      }
      
      de.amount = it->amount;
      dsts.push_back(de);
    }

    try
    {
      std::vector<uint8_t> extra;
      if (!payment_id.empty())
        currency::set_payment_id_to_tx_extra(extra, payment_id);

      currency::transaction tx;
      currency::blobdata tx_blob;
      m_wallet.transfer(dsts, req.mixin, req.unlock_time, req.fee, extra, tx, tx_blob, req.do_not_relay);
      if (m_wallet.is_view_only())
      {
        res.tx_unsigned_hex = epee::string_tools::buff_to_hex_nodelimer(tx_blob); // view-only wallets can't sign and relay transactions, so extract unsigned blob from tx_blob
        // leave res.tx_hash empty, because tx has will change after signing
      }
      else
      {
        res.tx_blob = epee::string_tools::buff_to_hex_nodelimer(tx_blob);
        res.tx_hash = epee::string_tools::pod_to_hex(currency::get_transaction_hash(tx));
      }
      return true;
    }
    catch (const tools::error::daemon_busy& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_DAEMON_IS_BUSY;
      er.message = e.what();
      return false; 
    }
    catch (const std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
      er.message = e.what();
      return false; 
    }
    catch (...)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
      return false; 
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_store(const wallet_rpc::COMMAND_RPC_STORE::request& req, wallet_rpc::COMMAND_RPC_STORE::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    try
    {
      m_wallet.store();
    }
    catch (std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = e.what();
      return false;
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_get_payments(const wallet_rpc::COMMAND_RPC_GET_PAYMENTS::request& req, wallet_rpc::COMMAND_RPC_GET_PAYMENTS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::payment_id_t payment_id;
    if (!currency::parse_payment_id_from_hex_str(req.payment_id, payment_id))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
      er.message = "Payment ID has invald format";
      return false;
    }

    res.payments.clear();
    std::list<wallet2::payment_details> payment_list;
    m_wallet.get_payments(payment_id, payment_list);
    for (auto & payment : payment_list)
    {
      if (payment.m_unlock_time && !req.allow_locked_transactions)
      {
        //check that transaction don't have locking for time longer then 10 blocks ahead
        //TODO: add code for "unlock_time" set as timestamp, now it's all being filtered
        if(payment.m_unlock_time > payment.m_block_height + DEFAULT_TX_SPENDABLE_AGE)
          continue;
      }
      wallet_rpc::payment_details rpc_payment;
      rpc_payment.payment_id   = req.payment_id;
      rpc_payment.tx_hash      = epee::string_tools::pod_to_hex(payment.m_tx_hash);
      rpc_payment.amount       = payment.m_amount;
      rpc_payment.block_height = payment.m_block_height;
      rpc_payment.unlock_time  = payment.m_unlock_time;
      res.payments.push_back(rpc_payment);
    }

    return true;
  }
  bool wallet_rpc_server::on_get_bulk_payments(const wallet_rpc::COMMAND_RPC_GET_BULK_PAYMENTS::request& req, wallet_rpc::COMMAND_RPC_GET_BULK_PAYMENTS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    res.payments.clear();

    for (auto & payment_id_str : req.payment_ids)
    {
      currency::payment_id_t payment_id;

      // TODO - should the whole thing fail because of one bad id?

      if(!currency::parse_payment_id_from_hex_str(payment_id_str, payment_id))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
        er.message = "Payment ID has invalid format: " + payment_id_str;
        return false;
      }

      std::list<wallet2::payment_details> payment_list;
      m_wallet.get_payments(payment_id, payment_list, req.min_block_height);

      for (auto & payment : payment_list)
      {
        wallet_rpc::payment_details rpc_payment;
        rpc_payment.payment_id   = payment_id_str;
        rpc_payment.tx_hash      = epee::string_tools::pod_to_hex(payment.m_tx_hash);
        rpc_payment.amount       = payment.m_amount;
        rpc_payment.block_height = payment.m_block_height;
        rpc_payment.unlock_time  = payment.m_unlock_time;
        res.payments.push_back(std::move(rpc_payment));
      }
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_get_transfers(const wallet_rpc::COMMAND_RPC_GET_TRANSFERS::request& req, wallet_rpc::COMMAND_RPC_GET_TRANSFERS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    return m_wallet.get_transfers(req, res);
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_convert_address(const wallet_rpc::COMMAND_RPC_CONVERT_ADDRESS::request& req, wallet_rpc::COMMAND_RPC_CONVERT_ADDRESS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    //
    // 1/2 standard address + payment id => integrated address
    if (!req.address_str.empty() && req.integrated_address_str.empty())
    {
      currency::account_public_address addr;
      if (!currency::get_account_address_from_str(addr, req.address_str))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_ADDRESS;
        er.message = "invalid address: " + req.address_str;
        return false;
      }

      currency::payment_id_t payment_id;
      if (!req.payment_id_hex.empty() && !currency::parse_payment_id_from_hex_str(req.payment_id_hex, payment_id))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
        er.message = "invalid payment id: " + req.payment_id_hex;
        return false;
      }

      res.integrated_address_str = currency::get_account_address_as_str(addr, payment_id);
      return true;
    }

    //
    // 2/2 integrated address => payment id + standard address
    if (req.address_str.empty() && !req.integrated_address_str.empty())
    {
      currency::account_public_address addr;
      currency::payment_id_t payment_id;
      if (!currency::get_account_address_and_payment_id_from_str(addr, payment_id, req.integrated_address_str))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_ADDRESS;
        er.message = "invalid address: " + req.integrated_address_str;
        return false;
      }

      res.address_str = currency::get_account_address_as_str(addr);
      res.payment_id_hex = epee::string_tools::buff_to_hex_nodelimer(payment_id);
      return true;
    }

    er.code = WALLET_RPC_ERROR_CODE_WRONG_ADDRESS;
    er.message = "wrong arguments, request was not understood";
    return false;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_maketelepod(const wallet_rpc::COMMAND_RPC_MAKETELEPOD::request& req, wallet_rpc::COMMAND_RPC_MAKETELEPOD::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    //check available balance
    if (m_wallet.unlocked_balance() <= req.amount)
    { 
      res.status = "INSUFFICIENT_COINS";
      return true;
    }

    currency::account_base acc;
    acc.generate();
    std::vector<currency::tx_destination_entry> dsts(1);
    dsts.back().amount = req.amount;
    dsts.back().addr = acc.get_keys().m_account_address;
    currency::transaction tx = AUTO_VAL_INIT(tx);
    try
    {
      std::vector<uint8_t> extra;
      m_wallet.transfer(dsts, 0, 0, DEFAULT_FEE, extra, tx);
    }
    catch (const std::runtime_error& er)
    {
      LOG_ERROR("Failed to send transaction: " << er.what());
      res.status = "INTERNAL_ERROR";
      return true;
    }

    res.tpd.basement_tx_id_hex = string_tools::pod_to_hex(currency::get_transaction_hash(tx));    
    std::string buff = epee::serialization::store_t_to_binary(acc);    
    res.tpd.account_keys_hex = string_tools::buff_to_hex_nodelimer(buff);

    res.status = "OK";
    LOG_PRINT_GREEN("TELEPOD ISSUED [" << currency::print_money(req.amount) << "BBR, base_tx_id: ]" << currency::get_transaction_hash(tx), LOG_LEVEL_0);

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::build_transaction_from_telepod(const wallet_rpc::telepod& tlp, const currency::account_public_address& acc2, currency::transaction& tx2, std::string& status)
  {
    //check if base transaction confirmed
    currency::COMMAND_RPC_GET_TRANSACTIONS::request get_tx_req = AUTO_VAL_INIT(get_tx_req);
    currency::COMMAND_RPC_GET_TRANSACTIONS::response get_tx_rsp = AUTO_VAL_INIT(get_tx_rsp);
    get_tx_req.txs_hashes.push_back(tlp.basement_tx_id_hex);
    if (!m_wallet.get_core_proxy()->call_COMMAND_RPC_GET_TRANSACTIONS(get_tx_req, get_tx_rsp)
      || get_tx_rsp.status != CORE_RPC_STATUS_OK
      || !get_tx_rsp.txs_as_hex.size())
    {
      status = "UNCONFIRMED";
      return false;
    }

    //extract account keys
    std::string acc_buff;
    currency::account_base acc = AUTO_VAL_INIT(acc);
    if (!string_tools::parse_hexstr_to_binbuff(tlp.account_keys_hex, acc_buff))
    {
      LOG_ERROR("Failed to parse_hexstr_to_binbuff(tlp.account_keys_hex, acc_buff)");
      status = "BAD";
      return false;
    }
    if (!epee::serialization::load_t_from_binary(acc, acc_buff))
    {
      LOG_ERROR("Failed to load_t_from_binary(acc, acc_buff)");
      status = "BAD";
      return false;
    }

    //extract transaction
    currency::transaction tx = AUTO_VAL_INIT(tx);
    std::string buff;
    if (!string_tools::parse_hexstr_to_binbuff(get_tx_rsp.txs_as_hex.back(), buff))
    {
      LOG_ERROR("Failed to parse_hexstr_to_binbuff(get_tx_rsp.txs_as_hex.back(), buff)");
      status = "INTERNAL_ERROR";
      return false;
    }
    if (!currency::parse_and_validate_tx_from_blob(buff, tx))
    {
      LOG_ERROR("Failed to currency::parse_and_validate_tx_from_blob(buff, tx)");
      status = "INTERNAL_ERROR";
      return false;
    }

    crypto::public_key tx_pub_key = currency::get_tx_pub_key_from_extra(tx);
    if (tx_pub_key == currency::null_pkey)
    {
      LOG_ERROR("Failed to currency::get_tx_pub_key_from_extra(tx)");
      status = "BAD";
      return false;

    }

    //get transaction global output indices 
    currency::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::request get_ind_req = AUTO_VAL_INIT(get_ind_req);
    currency::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::response get_ind_rsp = AUTO_VAL_INIT(get_ind_rsp);
    get_ind_req.txid = currency::get_transaction_hash(tx);
    if (!m_wallet.get_core_proxy()->call_COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES(get_ind_req, get_ind_rsp)
      || get_ind_rsp.status != CORE_RPC_STATUS_OK
      || get_ind_rsp.o_indexes.size() != tx.vout.size())
    {
      LOG_ERROR("Problem with call_COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES(....) ");
      status = "INTERNAL_ERROR";
      return false;
    }

    //prepare inputs
    std::vector<currency::tx_source_entry> sources;
    size_t i = 0;
    uint64_t amount = 0;
    for (auto& o : get_ind_rsp.o_indexes)
    {
      //check if input is for telepod's address
      if (currency::is_out_to_acc(acc.get_keys(), boost::get<currency::txout_to_key>(tx.vout[i].target), tx_pub_key, i))
      {
        //income output 
        amount += tx.vout[i].amount;
        sources.resize(sources.size() + 1);
        currency::tx_source_entry& tse = sources.back();
        tse.amount = tx.vout[i].amount;
        tse.outputs.push_back(currency::tx_source_entry::output_entry(o, boost::get<currency::txout_to_key>(tx.vout[i].target).key));
        tse.real_out_tx_key = tx_pub_key;
        tse.real_output = 0;
        tse.real_output_in_tx_index = i;
      }
      ++i;
    }


    //prepare outputs
    std::vector<currency::tx_destination_entry> dsts(1);
    currency::tx_destination_entry& dst = dsts.back();
    dst.addr = acc2;
    dst.amount = amount - DEFAULT_FEE;

    //generate transaction
    const std::vector<uint8_t> extra;
    currency::keypair txkey;
    bool r = currency::construct_tx(acc.get_keys(), sources, dsts, extra, tx2, txkey, 0);
    if (!r)
    {
      LOG_ERROR("Problem with construct_tx(....) ");
      status = "INTERNAL_ERROR";
      return false;
    }
    if (CURRENCY_MAX_TRANSACTION_BLOB_SIZE <= get_object_blobsize(tx2))
    {
      LOG_ERROR("Problem with construct_tx(....), blobl size os too big: " << get_object_blobsize(tx2));
      status = "INTERNAL_ERROR";
      return false;
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_clonetelepod(const wallet_rpc::COMMAND_RPC_CLONETELEPOD::request& req, wallet_rpc::COMMAND_RPC_CLONETELEPOD::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::transaction tx2 = AUTO_VAL_INIT(tx2);
    //new destination account 
    currency::account_base acc2 = AUTO_VAL_INIT(acc2);
    acc2.generate();

    if (!build_transaction_from_telepod(req.tpd, acc2.get_keys().m_account_address, tx2, res.status))
    {
      LOG_ERROR("Failed to build_transaction_from_telepod(...)");
      return true;
    }

    //send transaction to daemon
    currency::COMMAND_RPC_SEND_RAW_TX::request req_send_raw;
    req_send_raw.tx_as_hex = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(tx2));
    currency::COMMAND_RPC_SEND_RAW_TX::response rsp_send_raw;
    bool r = m_wallet.get_core_proxy()->call_COMMAND_RPC_SEND_RAW_TX(req_send_raw, rsp_send_raw);
    if (!r || rsp_send_raw.status != CORE_RPC_STATUS_OK)
    {
      LOG_ERROR("Problem with construct_tx(....), blobl size os too big: " << get_object_blobsize(tx2));
      res.status = "INTERNAL_ERROR";
      return true;
    }

    res.tpd.basement_tx_id_hex = string_tools::pod_to_hex(currency::get_transaction_hash(tx2));
    std::string acc2_buff = epee::serialization::store_t_to_binary(acc2);
    res.tpd.account_keys_hex = string_tools::buff_to_hex_nodelimer(acc2_buff);

    res.status = "OK";
    LOG_PRINT_GREEN("TELEPOD ISSUED [" << currency::print_money(currency::get_outs_money_amount(tx2)) << "BBR, base_tx_id: ]" << currency::get_transaction_hash(tx2), LOG_LEVEL_0);

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_telepodstatus(const wallet_rpc::COMMAND_RPC_TELEPODSTATUS::request& req, wallet_rpc::COMMAND_RPC_TELEPODSTATUS::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::transaction tx2 = AUTO_VAL_INIT(tx2);
    //new destination account 
    currency::account_base acc2 = AUTO_VAL_INIT(acc2);
    acc2.generate();

    if (!build_transaction_from_telepod(req.tpd, acc2.get_keys().m_account_address, tx2, res.status))
    {
      return true;
    }
    //check if transaction is spent
    currency::COMMAND_RPC_CHECK_KEYIMAGES::request req_ki = AUTO_VAL_INIT(req_ki);
    currency::COMMAND_RPC_CHECK_KEYIMAGES::response rsp_ki = AUTO_VAL_INIT(rsp_ki);
    for (auto& i : tx2.vin)
      req_ki.images.push_back(boost::get<currency::txin_to_key>(i).k_image);

    if (!m_wallet.get_core_proxy()->call_COMMAND_RPC_COMMAND_RPC_CHECK_KEYIMAGES(req_ki, rsp_ki) 
      || rsp_ki.status != CORE_RPC_STATUS_OK
      || rsp_ki.images_stat.size() != req_ki.images.size())
    {
      LOG_ERROR("Problem with call_COMMAND_RPC_COMMAND_RPC_CHECK_KEYIMAGES(....)");
      res.status = "INTERNAL_ERROR";
      return true;
    }

    for (auto s : rsp_ki.images_stat)
    {
      if (!s)
      {
        res.status = "SPENT";
        return true;
      }
    }

    res.status = "OK";
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_withdrawtelepod(const wallet_rpc::COMMAND_RPC_WITHDRAWTELEPOD::request& req, wallet_rpc::COMMAND_RPC_WITHDRAWTELEPOD::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::transaction tx2 = AUTO_VAL_INIT(tx2);
    //parse destination add 
    currency::account_public_address acc_addr = AUTO_VAL_INIT(acc_addr);
    if (!currency::get_account_address_from_str(acc_addr, req.addr))
    {
      LOG_ERROR("Failed to build_transaction_from_telepod(...)");
      res.status = "BAD_ADDRESS";
      return true;
    }


    if (!build_transaction_from_telepod(req.tpd, acc_addr, tx2, res.status))
    {
      LOG_ERROR("Failed to build_transaction_from_telepod(...)");
      return true;
    }

    //send transaction to daemon
    currency::COMMAND_RPC_SEND_RAW_TX::request req_send_raw;
    req_send_raw.tx_as_hex = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(tx2));
    currency::COMMAND_RPC_SEND_RAW_TX::response rsp_send_raw;
    bool r = m_wallet.get_core_proxy()->call_COMMAND_RPC_SEND_RAW_TX(req_send_raw, rsp_send_raw);
    if (!r || rsp_send_raw.status != CORE_RPC_STATUS_OK)
    {
      LOG_ERROR("Problem with construct_tx(....), blobl size os too big: " << get_object_blobsize(tx2));
      res.status = "INTERNAL_ERROR";
      return true;
    }

    res.status = "OK";
    LOG_PRINT_GREEN("TELEPOD WITHDRAWN [" << currency::print_money(currency::get_outs_money_amount(tx2)) << "BBR, tx_id: ]" << currency::get_transaction_hash(tx2), LOG_LEVEL_0);

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_sweep_below(const wallet_rpc::COMMAND_SWEEP_BELOW::request& req, wallet_rpc::COMMAND_SWEEP_BELOW::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    currency::payment_id_t payment_id;
    if (!req.payment_id_hex.empty() && !currency::parse_payment_id_from_hex_str(req.payment_id_hex, payment_id))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
      er.message = std::string("Invalid payment id: ") + req.payment_id_hex;
      return false;
    }

    currency::account_public_address addr;
    currency::payment_id_t integrated_payment_id;
    if (!m_wallet.get_transfer_address(req.address, addr, integrated_payment_id))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_ADDRESS;
      er.message = std::string("Invalid address: ") + req.address;
      return false;
    }

    if (!integrated_payment_id.empty())
    {
      if (!payment_id.empty())
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_PAYMENT_ID;
        er.message = std::string("address ") + req.address + " has integrated payment id " + epee::string_tools::buff_to_hex_nodelimer(integrated_payment_id) +
          " which is incompatible with payment id " + epee::string_tools::buff_to_hex_nodelimer(payment_id) + " that was already assigned to this transfer";
        return false;
      }
      payment_id = integrated_payment_id;
    }

    if (req.fee < TX_POOL_MINIMUM_FEE)
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_ARGUMENT;
      er.message = std::string("Given fee is too low: ") + epee::string_tools::num_to_string_fast(req.fee) + ", minimum is: " + epee::string_tools::num_to_string_fast(TX_POOL_MINIMUM_FEE);
      return false;
    }

    try
    {
      currency::transaction tx = AUTO_VAL_INIT(tx);
      size_t outs_total = 0, outs_swept = 0;
      uint64_t amount_total = 0, amount_swept = 0;

      m_wallet.sweep_below(req.mixin, addr, req.amount, payment_id, req.fee, outs_total, amount_total, outs_swept, &tx);

      get_inputs_money_amount(tx, amount_swept);
      res.amount_swept = amount_swept;
      res.amount_total = amount_total;
      res.outs_swept = outs_swept;
      res.outs_total = outs_total;
      res.tx_hash = string_tools::pod_to_hex(currency::get_transaction_hash(tx));
    }
    catch (const tools::error::daemon_busy& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_DAEMON_IS_BUSY;
      er.message = e.what();
      return false;
    }
    catch (const std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
      er.message = e.what();
      return false;
    }
    catch (...)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
      return false;
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_sign_transfer(const wallet_rpc::COMMAND_SIGN_TRANSFER::request& req, wallet_rpc::COMMAND_SIGN_TRANSFER::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    try
    {
      currency::transaction tx = AUTO_VAL_INIT(tx);
      std::string tx_unsigned_blob;
      if (!string_tools::parse_hexstr_to_binbuff(req.tx_unsigned_hex, tx_unsigned_blob))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_ARGUMENT;
        er.message = "tx_unsigned_hex is invalid";
        return false;
      }
      std::string tx_signed_blob;
      m_wallet.sign_transfer(tx_unsigned_blob, tx_signed_blob, tx);
      
      res.tx_signed_hex = epee::string_tools::buff_to_hex_nodelimer(tx_signed_blob);
      res.tx_hash = epee::string_tools::pod_to_hex(currency::get_transaction_hash(tx));
    }
    catch (const std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
      er.message = e.what();
      return false;
    }
    catch (...)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
      return false;
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_submit_transfer(const wallet_rpc::COMMAND_SUBMIT_TRANSFER::request& req, wallet_rpc::COMMAND_SUBMIT_TRANSFER::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    std::string tx_unsigned_blob;
    if (!string_tools::parse_hexstr_to_binbuff(req.tx_unsigned_hex, tx_unsigned_blob))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_ARGUMENT;
      er.message = "tx_unsigned_hex is invalid";
      return false;
    }

    std::string tx_signed_blob;
    if (!string_tools::parse_hexstr_to_binbuff(req.tx_signed_hex, tx_signed_blob))
    {
      er.code = WALLET_RPC_ERROR_CODE_WRONG_ARGUMENT;
      er.message = "tx_signed_hex is invalid";
      return false;
    }

    try
    {
      currency::transaction tx = AUTO_VAL_INIT(tx);
      m_wallet.submit_transfer(tx_unsigned_blob, tx_signed_blob, tx);
      res.tx_hash = epee::string_tools::pod_to_hex(currency::get_transaction_hash(tx));
    }
    catch (const std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
      er.message = e.what();
      return false;
    }
    catch (...)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
      return false;
    }
    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------
  bool wallet_rpc_server::on_cancel_transfer(const wallet_rpc::COMMAND_CANCEL_TRANSFER::request& req, wallet_rpc::COMMAND_CANCEL_TRANSFER::response& res, epee::json_rpc::error& er, connection_context& cntx)
  {
    try
    {
      std::string tx_unsigned_blob;
      if (!string_tools::parse_hexstr_to_binbuff(req.tx_unsigned_hex, tx_unsigned_blob))
      {
        er.code = WALLET_RPC_ERROR_CODE_WRONG_ARGUMENT;
        er.message = "tx_unsigned_hex is invalid";
        return false;
      }
      m_wallet.cancel_transfer(tx_unsigned_blob);
    }
    catch (const std::exception& e)
    {
      er.code = WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR;
      er.message = e.what();
      return false;
    }
    catch (...)
    {
      er.code = WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR;
      er.message = "WALLET_RPC_ERROR_CODE_UNKNOWN_ERROR";
      return false;
    }

    return true;
  }
  //------------------------------------------------------------------------------------------------------------------------------

}

