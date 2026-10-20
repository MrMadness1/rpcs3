#include "stdafx.h"
#include "np_handler.h"
#include "np_helpers.h"
#include "Emu/NP/rpcn_config.h"
#include "Emu/NP/np_event_data.h"
#include "Emu/system_config.h"
LOG_CHANNEL(rpcn_log, "rpcn");

namespace np
{
	namespace
	{
		constexpr const char* MGN_LOBBY_TAG = "[MGN lobby emu]";

		void local_user_to_SceNpUserInfo2(event_data& edata, const SceNpId& npid, const SceNpOnlineName& online_name, const SceNpAvatarUrl& avatar_url, SceNpUserInfo2* user_info, bool include_onlinename, bool include_avatarurl)
		{
			std::memcpy(user_info->npId.handle.data, npid.handle.data, sizeof(user_info->npId.handle.data));

			if (include_onlinename && online_name.data[0])
			{
				auto* ptr = edata.allocate<SceNpOnlineName>(sizeof(SceNpOnlineName), user_info->onlineName);
				std::memcpy(ptr->data, online_name.data, sizeof(ptr->data));
			}

			if (include_avatarurl && avatar_url.data[0])
			{
				auto* ptr = edata.allocate<SceNpAvatarUrl>(sizeof(SceNpAvatarUrl), user_info->avatarUrl);
				std::memcpy(ptr->data, avatar_url.data, sizeof(ptr->data));
			}
		}

		void store_bin_attrs_from_vm(vm::ptr<SceNpMatching2BinAttr> attrs, u32 num, std::vector<np_handler::lobby_emu_bin_attr>& out)
		{
			if (!attrs || num == 0)
			{
				return;
			}

			for (u32 i = 0; i < num; i++)
			{
				const auto& src = attrs[i];
				if (!src.ptr || src.size == 0)
				{
					continue;
				}

				np_handler::lobby_emu_bin_attr stored{};
				stored.attr.id   = src.id;
				stored.attr.size = src.size;
				stored.data.resize(src.size);
				std::memcpy(stored.data.data(), src.ptr.get_ptr(), src.size);
				out.push_back(std::move(stored));
			}
		}

		void merge_bin_attrs(std::vector<np_handler::lobby_emu_bin_attr>& existing, const std::vector<np_handler::lobby_emu_bin_attr>& incoming)
		{
			for (const auto& attr : incoming)
			{
				bool replaced = false;
				for (auto& cur : existing)
				{
					if (static_cast<u32>(cur.attr.id) == static_cast<u32>(attr.attr.id))
					{
						cur = attr;
						replaced = true;
						break;
					}
				}

				if (!replaced)
				{
					existing.push_back(attr);
				}
			}
		}

		void write_lobby_member_bin_attrs(event_data& edata, const std::vector<np_handler::lobby_emu_bin_attr>& attrs, SceNpMatching2LobbyMemberDataInternal* member)
		{
			if (attrs.empty())
			{
				return;
			}

			member->lobbyMemberBinAttrInternalNum = static_cast<u32>(attrs.size());
			auto* ptr_attrs                       = edata.allocate<SceNpMatching2LobbyMemberBinAttrInternal>(sizeof(SceNpMatching2LobbyMemberBinAttrInternal) * member->lobbyMemberBinAttrInternalNum, member->lobbyMemberBinAttrInternal);

			for (u32 i = 0; i < member->lobbyMemberBinAttrInternalNum; i++)
			{
				const auto& stored = attrs[i];
				ptr_attrs[i].data.id   = stored.attr.id;
				ptr_attrs[i].data.size = stored.attr.size;

				if (stored.attr.size != 0)
				{
					auto* data_ptr = edata.allocate<u8>(stored.attr.size, ptr_attrs[i].data.ptr);
					std::memcpy(data_ptr, stored.data.data(), stored.attr.size);
				}
			}
		}

		void fill_local_lobby_member(event_data& edata, np_handler& nph, const np_handler::lobby_emu_state& state, SceNpMatching2LobbyMemberDataInternal* member, bool include_onlinename, bool include_avatarurl)
		{
			local_user_to_SceNpUserInfo2(edata, nph.get_npid(), nph.get_online_name(), nph.get_avatar_url(), &member->userInfo, include_onlinename, include_avatarurl);
			member->joinDate.tick = nph.get_network_time();
			member->memberId      = state.member_id;
			member->flagAttr      = 0;
			write_lobby_member_bin_attrs(edata, state.member_bin_attrs, member);
		}

		bool lobby_emu_active()
		{
			return g_cfg.net.psn_status == np_psn_status::psn_rpcn && g_cfg_rpcn.get_emulate_lobbies_locally();
		}
	}

	u32 np_handler::join_lobby(SceNpMatching2ContextId ctx_id, vm::cptr<SceNpMatching2RequestOptParam> optParam, const SceNpMatching2JoinLobbyRequest* req)
	{
		const u32 req_id = generate_callback_info(ctx_id, optParam, SCE_NP_MATCHING2_REQUEST_EVENT_JoinLobby, false);

		if (!lobby_emu_active())
		{
			rpcn_log.warning("%s JoinLobby skipped (local lobby emulation disabled)", MGN_LOBBY_TAG);
			return req_id;
		}

		auto cb_info_opt = take_pending_request(req_id);
		ensure(cb_info_opt);

		auto& state     = lobby_emu_states[ctx_id];
		state.joined    = true;
		state.lobby_id  = req->lobbyId;
		state.member_id = 1;
		state.server_id = 1;

		if (lobby_emu_last_world.contains(ctx_id))
		{
			state.world_id = lobby_emu_last_world[ctx_id];
		}

		std::vector<lobby_emu_bin_attr> join_attrs;
		store_bin_attrs_from_vm(req->lobbyMemberBinAttrInternal, req->lobbyMemberBinAttrInternalNum, join_attrs);
		merge_bin_attrs(state.member_bin_attrs, join_attrs);

		const u32 event_key = get_event_key();

		auto& edata = allocate_req_result(event_key, SCE_NP_MATCHING2_EVENT_DATA_MAX_SIZE_JoinLobby, sizeof(SceNpMatching2JoinLobbyResponse));
		auto* resp  = reinterpret_cast<SceNpMatching2JoinLobbyResponse*>(edata.data());
		auto* lobby = edata.allocate<SceNpMatching2LobbyDataInternal>(sizeof(SceNpMatching2LobbyDataInternal), resp->lobbyDataInternal);

		lobby->serverId = state.server_id;
		lobby->worldId  = state.world_id;
		lobby->lobbyId  = state.lobby_id;
		lobby->maxSlot  = 64;
		lobby->flagAttr = SCE_NP_MATCHING2_LOBBY_FLAG_ATTR_PERMANENT;

		auto* member_ids = edata.allocate<SceNpMatching2LobbyMemberId>(sizeof(SceNpMatching2LobbyMemberId), lobby->memberIdList.memberId);
		member_ids[0]              = state.member_id;
		lobby->memberIdList.memberIdNum = 1;
		lobby->memberIdList.me          = state.member_id;

		np_memory.shrink_allocation(edata.addr(), edata.size());

		rpcn_log.notice("%s JoinLobby success (ctx=%d, lobbyId=%d, memberId=%d)", MGN_LOBBY_TAG, ctx_id, state.lobby_id, state.member_id);
		cb_info_opt->queue_callback(req_id, event_key, CELL_OK, edata.size());

		return req_id;
	}

	u32 np_handler::leave_lobby(SceNpMatching2ContextId ctx_id, vm::cptr<SceNpMatching2RequestOptParam> optParam, const SceNpMatching2LeaveLobbyRequest* req)
	{
		const u32 req_id = generate_callback_info(ctx_id, optParam, SCE_NP_MATCHING2_REQUEST_EVENT_LeaveLobby, false);

		if (!lobby_emu_active())
		{
			return req_id;
		}

		auto cb_info_opt = take_pending_request(req_id);
		ensure(cb_info_opt);

		if (lobby_emu_states.contains(ctx_id))
		{
			lobby_emu_states.erase(ctx_id);
		}

		rpcn_log.notice("%s LeaveLobby success (ctx=%d, lobbyId=%d)", MGN_LOBBY_TAG, ctx_id, req->lobbyId);
		cb_info_opt->queue_callback(req_id, 0, CELL_OK, 0);

		return req_id;
	}

	u32 np_handler::set_lobby_member_data_internal(SceNpMatching2ContextId ctx_id, vm::cptr<SceNpMatching2RequestOptParam> optParam, const SceNpMatching2SetLobbyMemberDataInternalRequest* req)
	{
		const u32 req_id = generate_callback_info(ctx_id, optParam, SCE_NP_MATCHING2_REQUEST_EVENT_SetLobbyMemberDataInternal, false);

		if (!lobby_emu_active())
		{
			return req_id;
		}

		auto cb_info_opt = take_pending_request(req_id);
		ensure(cb_info_opt);

		if (!lobby_emu_states.contains(ctx_id))
		{
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_LOBBY, 0);
			return req_id;
		}

		auto& state = lobby_emu_states[ctx_id];
		if (!state.joined || state.lobby_id != req->lobbyId)
		{
			rpcn_log.warning("%s SetLobbyMemberDataInternal: not in lobby %d", MGN_LOBBY_TAG, req->lobbyId);
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_LOBBY, 0);
			return req_id;
		}

		std::vector<lobby_emu_bin_attr> new_attrs;
		store_bin_attrs_from_vm(req->lobbyMemberBinAttrInternal, req->lobbyMemberBinAttrInternalNum, new_attrs);
		merge_bin_attrs(state.member_bin_attrs, new_attrs);

		rpcn_log.notice("%s SetLobbyMemberDataInternal success (ctx=%d, lobbyId=%d)", MGN_LOBBY_TAG, ctx_id, req->lobbyId);
		cb_info_opt->queue_callback(req_id, 0, CELL_OK, 0);

		return req_id;
	}

	u32 np_handler::get_lobby_member_data_internal(SceNpMatching2ContextId ctx_id, vm::cptr<SceNpMatching2RequestOptParam> optParam, const SceNpMatching2GetLobbyMemberDataInternalRequest* req)
	{
		const u32 req_id = generate_callback_info(ctx_id, optParam, SCE_NP_MATCHING2_REQUEST_EVENT_GetLobbyMemberDataInternal, true);

		if (!lobby_emu_active())
		{
			return req_id;
		}

		auto cb_info_opt = take_pending_request(req_id);
		ensure(cb_info_opt);

		if (!lobby_emu_states.contains(ctx_id))
		{
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_USER, 0);
			return req_id;
		}

		const auto& state = ::at32(lobby_emu_states, ctx_id);
		if (!state.joined || state.lobby_id != req->lobbyId || req->memberId != state.member_id)
		{
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_USER, 0);
			return req_id;
		}

		const u32 event_key = get_event_key();
		auto [include_onlinename, include_avatarurl] = get_match2_context_options(ctx_id);

		auto& edata = allocate_req_result(event_key, SCE_NP_MATCHING2_EVENT_DATA_MAX_SIZE_GetLobbyMemberDataInternal, sizeof(SceNpMatching2GetLobbyMemberDataInternalResponse));
		auto* resp  = reinterpret_cast<SceNpMatching2GetLobbyMemberDataInternalResponse*>(edata.data());
		auto* member = edata.allocate<SceNpMatching2LobbyMemberDataInternal>(sizeof(SceNpMatching2LobbyMemberDataInternal), resp->lobbyMemberDataInternal);
		fill_local_lobby_member(edata, *this, state, member, include_onlinename, include_avatarurl);
		np_memory.shrink_allocation(edata.addr(), edata.size());

		cb_info_opt->queue_callback(req_id, event_key, CELL_OK, edata.size());
		return req_id;
	}

	u32 np_handler::get_lobby_member_data_internal_list(SceNpMatching2ContextId ctx_id, vm::cptr<SceNpMatching2RequestOptParam> optParam, const SceNpMatching2GetLobbyMemberDataInternalListRequest* req)
	{
		const u32 req_id = generate_callback_info(ctx_id, optParam, SCE_NP_MATCHING2_REQUEST_EVENT_GetLobbyMemberDataInternalList, true);

		if (!lobby_emu_active())
		{
			return req_id;
		}

		auto cb_info_opt = take_pending_request(req_id);
		ensure(cb_info_opt);

		if (!lobby_emu_states.contains(ctx_id))
		{
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_LOBBY, 0);
			return req_id;
		}

		const auto& state = ::at32(lobby_emu_states, ctx_id);
		if (!state.joined || state.lobby_id != req->lobbyId)
		{
			cb_info_opt->queue_callback(req_id, 0, SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_LOBBY, 0);
			return req_id;
		}

		bool include_member = true;
		if (req->memberId && req->memberIdNum != 0)
		{
			include_member = false;
			for (u32 i = 0; i < req->memberIdNum; i++)
			{
				if (req->memberId[i] == state.member_id)
				{
					include_member = true;
					break;
				}
			}
		}

		if (!include_member)
		{
			const u32 event_key = get_event_key();
			auto& edata = allocate_req_result(event_key, SCE_NP_MATCHING2_EVENT_DATA_MAX_SIZE_GetLobbyMemberDataInternalList, sizeof(SceNpMatching2GetLobbyMemberDataInternalListResponse));
			auto* resp  = reinterpret_cast<SceNpMatching2GetLobbyMemberDataInternalListResponse*>(edata.data());
			resp->lobbyMemberDataInternalNum = 0;
			np_memory.shrink_allocation(edata.addr(), edata.size());
			cb_info_opt->queue_callback(req_id, event_key, CELL_OK, edata.size());
			return req_id;
		}

		const u32 event_key = get_event_key();
		auto [include_onlinename, include_avatarurl] = get_match2_context_options(ctx_id);

		auto& edata = allocate_req_result(event_key, SCE_NP_MATCHING2_EVENT_DATA_MAX_SIZE_GetLobbyMemberDataInternalList, sizeof(SceNpMatching2GetLobbyMemberDataInternalListResponse));
		auto* resp  = reinterpret_cast<SceNpMatching2GetLobbyMemberDataInternalListResponse*>(edata.data());
		auto* member = edata.allocate<SceNpMatching2LobbyMemberDataInternal>(sizeof(SceNpMatching2LobbyMemberDataInternal), resp->lobbyMemberDataInternal);
		fill_local_lobby_member(edata, *this, state, member, include_onlinename, include_avatarurl);
		resp->lobbyMemberDataInternalNum = 1;
		np_memory.shrink_allocation(edata.addr(), edata.size());

		rpcn_log.notice("%s GetLobbyMemberDataInternalList success (ctx=%d, lobbyId=%d)", MGN_LOBBY_TAG, ctx_id, req->lobbyId);
		cb_info_opt->queue_callback(req_id, event_key, CELL_OK, edata.size());

		return req_id;
	}

	error_code np_handler::get_lobby_member_id_list_local(SceNpMatching2ContextId ctx_id, SceNpMatching2LobbyId lobby_id, vm::ptr<SceNpMatching2LobbyMemberId> member_id, u32 member_id_num, vm::ptr<SceNpMatching2LobbyMemberId> me)
	{
		if (!lobby_emu_active())
		{
			return CELL_OK;
		}

		if (!lobby_emu_states.contains(ctx_id))
		{
			return CELL_OK;
		}

		const auto& state = ::at32(lobby_emu_states, ctx_id);
		if (!state.joined || state.lobby_id != lobby_id)
		{
			return CELL_OK;
		}

		if (member_id_num >= 1 && member_id)
		{
			member_id[0] = state.member_id;
		}

		if (me)
		{
			*me = state.member_id;
		}

		return CELL_OK;
	}
}
