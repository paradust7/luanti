// Minetest
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "networkprotocol.h" // session_t
#include "peerhandler.h"
#include "socket.h" // Address, UDPSocket
#include "types.h" // rtt_stat_type, rate_stat_type
#include <memory>
#include <optional>
#include <string>

class NetworkPacket;

namespace con
{

class ITransport;

class IConnection
{
public:
	virtual ~IConnection() = default;

	virtual void Connect(Address address) = 0;
	virtual bool Connected() const = 0;
	virtual void Disconnect() = 0;
	virtual void DisconnectPeer(session_t peer_id) = 0;

	virtual bool ReceiveTimeoutMs(NetworkPacket *pkt, u32 timeout_ms) = 0;
	bool TryReceive(NetworkPacket *pkt) {
		return ReceiveTimeoutMs(pkt, 0);
	}

	virtual void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) = 0;

	virtual Address GetPeerAddress(session_t peer_id) const = 0;
	virtual float getPeerStat(session_t peer_id, rtt_stat_type type) const = 0;
	virtual float getLocalStat(rate_stat_type type) const = 0;

	// The address we are bound to. For a server, where clients can reach us.
	virtual Address getBindAddress() const = 0;

	// Client only: the address of the server
	virtual Address getRemoteAddress() const = 0;
};

// Overrides g_settings / defaults
struct NetworkOverrides
{
	std::optional<std::string> bind_address{};
	std::optional<u16> bind_port{};
	std::optional<u32> max_packet_size{};
	std::optional<float> timeout{};
	std::optional<std::string> transport{};
};

// Create a client connection.
// May throw SocketException or IPv6DisabledException.
std::unique_ptr<IConnection> createClient(PeerHandler* handler,
		bool is_simple_singleplayer, bool ipv6,
		const NetworkOverrides &overrides = {});

// Create a server connection.
// May throw SocketException.
std::unique_ptr<IConnection> createServer(PeerHandler* handler,
		bool is_simple_singleplayer,
		const NetworkOverrides &overrides = {});

} // namespace
