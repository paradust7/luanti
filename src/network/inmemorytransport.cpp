// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "network/inmemorytransport.h"
#include "network/networkexceptions.h"
#include "network/networkpacket.h"
#include "network/peertable.h"
#include "log.h"

namespace con
{

// This mutex protects shared state of all InMemoryTransports:
//   link_server, m_other, m_peer_id
static std::mutex link_mutex;

// The server transport that clients connect to, while it exists
static InMemoryTransport *link_server = nullptr;

class InMemoryPeer final : public IPeerData
{
public:
	InMemoryPeer(Peer *parent, InMemoryTransport *owner) :
		IPeerData(parent, owner)
	{
		parent->rttClear();
	}

	bool isTimedOut(float timeout, std::string &reason) override
	{
		return false;
	}

	float getRateStat(rate_stat_type type) override
	{
		return 0.0f;
	}
};

/*
	InMemoryTransport
*/

InMemoryTransport::InMemoryTransport(bool is_server) :
	m_is_server(is_server)
{
}

InMemoryTransport::~InMemoryTransport()
{
	stop();
}

void InMemoryTransport::attach(ITransportManager *manager)
{
	assert(manager && !m_manager);
	m_manager = manager;

	const bool is_server = m_manager->getPeerTable()->getOurPeerID() == PEER_ID_SERVER;
	FATAL_ERROR_IF(is_server != m_is_server, "InMemoryTransport server mismatch");

	if (!m_is_server)
		return;

	MutexAutoLock lock(link_mutex);
	if (link_server)
		throw ConnectionException("An in-memory server is already running");
	link_server = this;
}

void InMemoryTransport::stop()
{
	MutexAutoLock lock(link_mutex);
	// The other side sees this as a timeout.
	endSession(true);
	if (link_server == this)
		link_server = nullptr;
	// Our reference to the manager is no longer valid after stop().
	m_manager = nullptr;
}

Address InMemoryTransport::getBindAddress() const
{
	return Address(127, 0, 0, 1, 0);
}

void InMemoryTransport::Connect(Address address)
{
	sanity_check(!m_is_server);

	MutexAutoLock lock(link_mutex);
	PeerTable *peer_table = m_manager->getPeerTable();

	// In this client's peer table, add the server peer entry.
	auto server_peer = peer_table->addServerPeer<InMemoryPeer>(address, this);
	if (!server_peer)
		throw ConnectionException("Already connected to a server");

	// If there's no server, fail immediately like a timeout.
	InMemoryTransport *server = link_server;
	const char *error = nullptr;
	if (!server)
		error = "no server is running";
	else if (server->m_other)
		error = "the server already has a client";
	if (error) {
		errorstream << "InMemoryTransport: Can't connect (" << error << ")" << std::endl;
		peer_table->deletePeer(PEER_ID_SERVER, true);
		return;
	}

	// In the server's peer table, add a peer entry for this client.
	auto fake_client_addr = Address(127, 0, 0, 1, 0);
	auto peer = server->m_manager->getPeerTable()->addPeer<InMemoryPeer>(fake_client_addr, server);
	if (!peer) {
		peer_table->deletePeer(PEER_ID_SERVER, true);
		return;
	}
	peer->setFullyOpen();

	m_other = server;
	m_peer_id = PEER_ID_SERVER;
	server->m_other = this;
	server->m_peer_id = peer->getPeerID();

	// Set the client's peer id to what the server's peer table chose
	peer_table->setOurPeerID(peer->getPeerID());
}

void InMemoryTransport::Disconnect()
{
	MutexAutoLock lock(link_mutex);
	endSession(false);
}

void InMemoryTransport::DisconnectPeer(session_t peer_id)
{
	MutexAutoLock lock(link_mutex);
	if (peer_id == m_peer_id)
		endSession(false);
}

void InMemoryTransport::Send(session_t peer_id, u8 channelnum,
		NetworkPacket *pkt, bool reliable)
{
	const Buffer<u8> data = pkt->oldForgePacket();

	MutexAutoLock lock(link_mutex);

	if (m_other && peer_id == m_peer_id)
		m_other->m_manager->dataReceived(m_other->m_peer_id, data);
}

void InMemoryTransport::endSession(bool timeout)
{
	if (!m_other)
		return;

	InMemoryTransport* other = m_other;
	m_manager->getPeerTable()->deletePeer(m_peer_id, timeout);
	other->m_manager->getPeerTable()->deletePeer(other->m_peer_id, timeout);

	m_other = nullptr;
	m_peer_id = PEER_ID_INEXISTENT;
	other->m_other = nullptr;
	other->m_peer_id = PEER_ID_INEXISTENT;
}

} // namespace con
