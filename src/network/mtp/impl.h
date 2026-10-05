// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "network/peertable.h"
#include "network/socket.h"
#include "network/transport.h"
#include "constants.h"
#include "util/pointer.h"
#include "util/container.h"
#include "porting.h"
#include "network/address.h"
#include "network/networkprotocol.h"
#include <atomic>
#include <cfloat>
#include <vector>
#include <memory>
#include <map>

namespace con
{

class ConnectionReceiveThread;
class ConnectionSendThread;

struct ConnectionCommand;
typedef std::shared_ptr<ConnectionCommand> ConnectionCommandPtr;

struct BufferedPacket;
typedef std::shared_ptr<BufferedPacket> BufferedPacketPtr;

/*
	LegacyTransport: the original UDP-based Minetest protocol
*/

class LegacyTransport final : public ITransport
{
public:
	friend class ConnectionSendThread;
	friend class ConnectionReceiveThread;

	LegacyTransport(u32 max_packet_size, float timeout, UDPSocket &&socket);
	~LegacyTransport();

	/* ITransport */
	void attach(ITransportManager *manager) override;
	void stop() override;
	Address getBindAddress() const override;
	void Connect(Address address) override;
	void Disconnect() override;
	void DisconnectPeer(session_t peer_id) override;
	void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) override;

	void putCommand(ConnectionCommandPtr c);

	u32 GetProtocolID() const { return m_protocol_id; };
	const std::string getDesc();

	PeerTable* getPeerTable()
	{
		return m_manager->getPeerTable();
	}

protected:
	session_t createPeer(const Address& sender, int fd);
	void createServerPeer(const Address& sender);

	void doResendOne(session_t peer_id);

	void sendAck(session_t peer_id, u8 channelnum, u16 seqnum);

	UDPSocket m_udpSocket;
	// Command queue: user -> SendThread
	MutexedQueue<ConnectionCommandPtr> m_command_queue;

	void dataReceived(session_t peer_id, const Buffer<u8> &data)
	{
		m_manager->dataReceived(peer_id, data);
	}

	void TriggerSend();

	bool ConnectedToServer()
	{
		return getPeerTable()->getPeerNoEx(PEER_ID_SERVER) != nullptr;
	}

private:
	u32 m_protocol_id;

	std::unique_ptr<ConnectionSendThread> m_sendThread;
	std::unique_ptr<ConnectionReceiveThread> m_receiveThread;

	ITransportManager *m_manager = nullptr;

	std::atomic<bool> m_shutting_down = false;
};

} // namespace
