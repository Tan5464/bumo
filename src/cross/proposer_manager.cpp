/*
bumo is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

bumo is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with bumo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "proposer_manager.h"
#include<cross/cross_utils.h>
namespace bumo {
	ProposerManager::ProposerManager() :
		enabled_(false),
		last_uptate_validate_address_time_(0),
		last_uptate_handle_child_chain_time_(0),
		thread_ptr_(NULL){

	}

	ProposerManager::~ProposerManager(){
		if (thread_ptr_){
			delete thread_ptr_;
			thread_ptr_ = NULL;
		}
	}

	bool ProposerManager::Initialize(){
		if (General::GetSelfChainId() != General::MAIN_CHAIN_ID){
			return true;
		}

		enabled_ = true;
		thread_ptr_ = new utils::Thread(this);
		if (!thread_ptr_->Start("ProposerManager")) {
			return false;
		}
		bumo::MessageChannel::GetInstance()->RegisterMessageChannelConsumer(this, protocol::MESSAGE_CHANNEL_SUBMIT_HEAD);
		return true;
	}

	bool ProposerManager::Exit(){
		if (General::GetSelfChainId() != General::MAIN_CHAIN_ID){
			return true;
		}

		enabled_ = false;
		if (thread_ptr_) {
			thread_ptr_->JoinWithStop();
		}
		bumo::MessageChannel::GetInstance()->UnregisterMessageChannelConsumer(this, protocol::MESSAGE_CHANNEL_SUBMIT_HEAD);
		return true;
	}

	void ProposerManager::Run(utils::Thread *thread) {
		while (enabled_){
			int64_t current_time = utils::Timestamp::HighResolution();
			if (current_time > 2 * utils::MICRO_UNITS_PER_SEC + last_uptate_handle_child_chain_time_){
				//Handel block list//
				HandleChildChainBlock();
				last_uptate_handle_child_chain_time_ = current_time;
			}
		}
	}

	void ProposerManager::HandleChildChainBlock(){
		//handel child chain block, and call MessageChannel to send main chain proc 

		utils::MutexGuard guard(handle_child_chain_list_lock_);
		std::list<protocol::LedgerHeader>::const_iterator itor = handle_child_chain_block_list_.begin();
		while (itor != handle_child_chain_block_list_.end()){
			if (HandleSingleChildChainBlock(*itor)){
				handle_child_chain_block_list_.erase(itor++); // delete node£,find next node
			}
			else{
				++itor;
			}
		}
	}

	void ProposerManager::HandleMessageChannelConsumer(const protocol::MessageChannel &message_channel){
		if (message_channel.msg_type() != protocol::MESSAGE_CHANNEL_SUBMIT_HEAD){
			LOG_ERROR("Failed to message_channel type is not MESSAGE_CHANNEL_SUBMIT_HEAD, error msg type is %d", message_channel.msg_type());
			return;
		}

		protocol::LedgerHeader ledger_header;
		ledger_header.ParseFromString(message_channel.msg_data());
		utils::MutexGuard guard(handle_child_chain_list_lock_);
		handle_child_chain_block_list_.push_back(ledger_header);

	}


	void ProposerManager::UpdateValidateAddressList(utils::StringList& validate_address, int64_t chain_id){

		Json::FastWriter json_input;
		Json::Value input_value;
		Json::Value params;
		params["chain_id"] = chain_id;
		input_value["method"] = "queryChildChainValidators";
		input_value["params"] = params;
		std::string input = json_input.write(input_value);


		Json::Value object;
		Json::Value result_list;

		int32_t error_code = bumo::CrossUtils::QueryContract(General::CONTRACT_CMC_ADDRESS, input.c_str(), result_list);

		std::string result = result_list[Json::UInt(0)]["result"]["value"].asString();
		object.fromString(result.c_str());

		if (error_code != protocol::ERRCODE_SUCCESS){
			LOG_ERROR("Failed to query validators .%d", error_code);
			return;
		}

		if (!object["validators"].isArray()){
			LOG_ERROR("Failed to validators list is not array");
			return;
		}

		validate_address.clear();
		int32_t size = object["validators"].size();
		for (int32_t i = 0; i < size; i++){
			std::string address = object["validators"][i].asString().c_str();
			validate_address.push_back(address.c_str());
		}

	}

	bool ProposerManager::CheckNodeIsValidate(const std::string &address, int64_t chain_id){
		utils::StringList::const_iterator itor;
		bool flag = false;
		utils::StringList validate_address;
		UpdateValidateAddressList(validate_address, chain_id);
		itor = std::find(validate_address.begin(), validate_address.end(), address.c_str());
		if (itor != validate_address.end()){
			flag = true;
		}
		else{
			flag = false;
		}
		return flag;
	}

	bool ProposerManager::CheckChildPeviousBlockExsit(const protocol::LedgerHeader& ledger_header){
		Json::Value block_header = bumo::Proto2Json(ledger_header);
		if (block_header["seq"].asInt64() == 1){
			LOG_INFO("child block is genesis block! hash is  %s,seq is %d", block_header["hash"].asString(), block_header["seq"].asInt64());
			return true;
		}

		if (!CheckChildBlockExsit(block_header["previous_hash"].asString(), block_header["chain_id"].asInt64())){
			LOG_INFO("child previous block is not exsit! hash is  %s", block_header["previous_hash"].asString());
			return false;
		}
		return true;
	}

	bool ProposerManager::CheckChildBlockExsit(const std::string& hash, int64_t chain_id){
		// Check for child chain block in CMC
		bool flag = false;
		Json::FastWriter json_input;
		Json::Value input_value;
		Json::Value params;
		params["chain_id"] = chain_id;
		params["header_hash"] = hash.c_str();
		input_value["method"] = "queryChildBlockHeader";
		input_value["params"] = params;
		std::string input = json_input.write(input_value);

		Json::Value object;
		Json::Value result_list;

		int32_t error_code = bumo::CrossUtils::QueryContract(General::CONTRACT_CMC_ADDRESS, input.c_str(),result_list);

		std::string result = result_list[Json::UInt(0)]["result"]["value"].asString();
		object.fromString(result.c_str());

		if (error_code != protocol::ERRCODE_SUCCESS){
			LOG_ERROR("Failed to query child block .%d", error_code);
			flag = false;
		}

		if (object["hash"].asString().compare(hash) == 0){
			LOG_INFO("child block is not exsit!");
			flag = true;
		}

		return flag;
	}


	bool ProposerManager::CommitTransaction(const protocol::LedgerHeader& ledger_header){
		//create a mainchain transaction with private key to CMC
		Json::Value block_header = bumo::Proto2Json(ledger_header);
		Json::FastWriter json_input;
		Json::Value input_value;
		Json::Value params;

		params["chain_id"] = block_header["chain_id"].asInt64();
		params["block_header"] = block_header;
		input_value["method"] = "submitChildBlockHeader";
		input_value["params"] = params;
		std::string input = json_input.write(input_value);

		int32_t error_code = bumo::CrossUtils::PayCoin(Configure::Instance().ledger_configure_.validation_privatekey_, General::CONTRACT_CMC_ADDRESS, input.c_str(), 0);

		if (error_code != protocol::ERRCODE_SUCCESS){
			LOG_ERROR("Failed to query child block .%d", error_code);
			return false;
		}
		return true;
	}

	void ProposerManager::ProcessPeviousBlockNotExsit(const protocol::LedgerHeader& ledger_header){
		Json::Value block_header = bumo::Proto2Json(ledger_header);
		protocol::MessageChannel message_channel;
		protocol::MessageChannelQueryHead query_head;
		query_head.set_ledger_seq(block_header["seq"].asInt64());
		message_channel.set_target_chain_id(block_header["chain_id"].asInt64());
		message_channel.set_msg_type(protocol::MESSAGE_CHANNEL_QUERY_HEAD);
		message_channel.set_msg_data(query_head.SerializeAsString());
		bumo::MessageChannel::GetInstance()->MessageChannelProducer(message_channel);
	}

	bool ProposerManager::HandleSingleChildChainBlock(const protocol::LedgerHeader& ledger_header){

		PrivateKey private_key(Configure::Instance().ledger_configure_.validation_privatekey_);
		std::string node_address = private_key.GetEncAddress();
		Json::Value block_header = bumo::Proto2Json(ledger_header);


		if (!CheckNodeIsValidate(node_address.c_str(), block_header["chain_id"].asInt64())){
			LOG_INFO("this node is not validators,address is %s", node_address.c_str());
			return true;
		}

		if (!CheckChildPeviousBlockExsit(ledger_header)){
			ProcessPeviousBlockNotExsit(ledger_header);
			return false;
		}

		if (CheckChildBlockExsit(block_header["hash"].asString(), block_header["chain_id"].asInt64())){
			LOG_INFO("child block is not exsit! hash is  %s", block_header["hash"].asString().c_str());
			return true;
		}

		if (!CommitTransaction(ledger_header)){
			LOG_INFO("CommitTransaction child block is not success !");
			return false;
		}

		return true;

	}
}
