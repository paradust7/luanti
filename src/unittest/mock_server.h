// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 Minetest core developers & community

#pragma once

#include "server.h"
#include "server/mods.h"
#include "network/connection.h"
#include "scripting_server.h"

class MockServer : public Server
{
public:
	/* Set `path_world` to a real existing folder if you plan to initialize scripting! */
	MockServer(const std::string &path_world = "fakepath") :
		Server(path_world, SubgameSpec("fakespec", "fakespec"),
			   true, getNetworkOverrides(), true, nullptr
		)
	{}

	/*
	 * Use this in unit tests to create scripting.
	 * Note that you still need to call script->loadBuiltin() and don't forget
	 * a try-catch for `ModError`.
	 */
	void createScripting() {
		m_script = std::make_unique<ServerScripting>(this);
		m_modmgr = std::make_unique<ServerModManager>(nullptr);
	}

	void start() = delete;
	void stop() = delete;

private:
	// Any port, and always UDP so that the config file doesn't matter
	static con::NetworkOverrides getNetworkOverrides()
	{
		con::NetworkOverrides overrides;
		overrides.bind_address = "0.0.0.0";
		overrides.bind_port = 0;
		overrides.transport = "legacy";
		return overrides;
	}

	void SendChatMessage(session_t peer_id, const ChatMessage &message) override {}
};
