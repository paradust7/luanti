// Minetest
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "network/connection.h"
#include "network/inmemorytransport.h"
#include "network/mtp/impl.h"
#include "network/networkexceptions.h"
#include "network/networkpacket.h"
#include "network/peertable.h"
#include "network/transport.h"
#include "log.h"
#include "settings.h"
#include "util/container.h"
#include "util/string.h"
#include <mutex>
#include <vector>

namespace con
{

/*
	ConnectionEvent
*/

struct ConnectionEvent;
typedef std::shared_ptr<ConnectionEvent> ConnectionEventPtr;

enum ConnectionEventType {
	CONNEVENT_NONE,
	CONNEVENT_DATA_RECEIVED,
	CONNEVENT_PEER_ADDED,
	CONNEVENT_PEER_REMOVED
};

struct ConnectionEvent
{
	const ConnectionEventType type;
	session_t peer_id = 0;
	Buffer<u8> data;
	bool timeout = false;
	Address address;

	// We don't want to copy "data"
	DISABLE_CLASS_COPY(ConnectionEvent);

	static ConnectionEventPtr create(ConnectionEventType type);
	static ConnectionEventPtr dataReceived(session_t peer_id, const Buffer<u8> &data);
	static ConnectionEventPtr peerAdded(session_t peer_id, Address address);
	static ConnectionEventPtr peerRemoved(session_t peer_id, bool is_timeout, Address address);

	const char *describe() const;

private:
	ConnectionEvent(ConnectionEventType type_) :
		type(type_) {}
};

const char *ConnectionEvent::describe() const
{
	switch(type) {
	case CONNEVENT_NONE:
		return "CONNEVENT_NONE";
	case CONNEVENT_DATA_RECEIVED:
		return "CONNEVENT_DATA_RECEIVED";
	case CONNEVENT_PEER_ADDED:
		return "CONNEVENT_PEER_ADDED";
	case CONNEVENT_PEER_REMOVED:
		return "CONNEVENT_PEER_REMOVED";
	}
	return "Invalid ConnectionEvent";
}


ConnectionEventPtr ConnectionEvent::create(ConnectionEventType type)
{
	return std::shared_ptr<ConnectionEvent>(new ConnectionEvent(type));
}

ConnectionEventPtr ConnectionEvent::dataReceived(session_t peer_id, const Buffer<u8> &data)
{
	auto e = create(CONNEVENT_DATA_RECEIVED);
	e->peer_id = peer_id;
	data.copyTo(e->data);
	return e;
}

ConnectionEventPtr ConnectionEvent::peerAdded(session_t peer_id, Address address)
{
	auto e = create(CONNEVENT_PEER_ADDED);
	e->peer_id = peer_id;
	e->address = address;
	return e;
}

ConnectionEventPtr ConnectionEvent::peerRemoved(session_t peer_id, bool is_timeout, Address address)
{
	auto e = create(CONNEVENT_PEER_REMOVED);
	e->peer_id = peer_id;
	e->timeout = is_timeout;
	e->address = address;
	return e;
}

/*
	Connection
*/

/*
	Connects to Luanti peers using one or more transport methods.
	The PeerTable is shared by all transports.
	Each Peer is owned by the transport that added it.
*/
class Connection final : public IConnection, private PeerHandler, private ITransportManager
{
public:
	Connection(bool is_server, PeerHandler *peerhandler);
	~Connection();

	// Takes ownership of the transport. A client can only have one.
	// Must be called before the connection is used.
	void addTransport(std::unique_ptr<ITransport> transport);

	void Connect(Address address) override;
	void Disconnect() override;
	void DisconnectPeer(session_t peer_id) override;
	bool ReceiveTimeoutMs(NetworkPacket *pkt, u32 timeout_ms) override;
	void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) override;

	// Returns the address of the first (primary) transport
	Address getBindAddress() const override;

	bool Connected() const override;
	Address getRemoteAddress() const override;
	Address GetPeerAddress(session_t peer_id) const override;
	float getPeerStat(session_t peer_id, rtt_stat_type type) const override;
	float getLocalStat(rate_stat_type type) const override;

private:
	bool isServer() const;
	std::string getDesc() const;

	// The transport that owns the peer, or nullptr if there is no such peer
	ITransport *getPeerOwner(session_t peer_id) const;

	ConnectionEventPtr waitEvent(u32 timeout_ms);
	void putEvent(ConnectionEventPtr e);

	// These are called by PeerTable to queue the events
	void peerAdded(session_t peer_id, const Address &address) override;
	void peerRemoved(session_t peer_id, bool timeout, const Address &address) override;

	// For ITransportManager
	PeerTable *getPeerTable() override
	{
		return m_peer_table.get();
	}
	void dataReceived(session_t peer_id, const Buffer<u8> &data) override;

	PeerHandler *m_peerhandler;

	std::unique_ptr<PeerTable> m_peer_table;

	// Event queue: transports -> user
	MutexedQueue<ConnectionEventPtr> m_event_queue;

	std::vector<std::unique_ptr<ITransport>> m_transports;
};

Connection::Connection(bool is_server, PeerHandler *peerhandler) :
	m_peerhandler(peerhandler),
	m_peer_table(new PeerTable(this))
{
	if (is_server)
		m_peer_table->setOurPeerID(PEER_ID_SERVER);
}

Connection::~Connection()
{
	// Stop the transports first, so nothing else is using the peer table
	for (auto &transport : m_transports)
		transport->stop();

	// Delete peers while the transports that own their data still exist
	m_peer_table->shutdown();

	m_transports.clear();
}

void Connection::addTransport(std::unique_ptr<ITransport> transport)
{
	FATAL_ERROR_IF(!isServer() && !m_transports.empty(),
			"A client connection can only have one transport");
	transport->attach(this);
	m_transports.push_back(std::move(transport));
}

bool Connection::isServer() const
{
	// Set by the constructor and never changed for a server
	return m_peer_table->getOurPeerID() == PEER_ID_SERVER;
}

std::string Connection::getDesc() const
{
	return "con(" + itos(m_peer_table->getOurPeerID()) + ")";
}

Address Connection::getBindAddress() const
{
	if (m_transports.empty())
		return Address();
	return m_transports.front()->getBindAddress();
}

ITransport *Connection::getPeerOwner(session_t peer_id) const
{
	auto peer = m_peer_table->getPeerNoEx(peer_id);
	return peer ? peer->getOwner() : nullptr;
}

/* Internal stuff */

void Connection::putEvent(ConnectionEventPtr e)
{
	assert(e->type != CONNEVENT_NONE); // Pre-condition
	m_event_queue.push_back(e);
}

ConnectionEventPtr Connection::waitEvent(u32 timeout_ms)
{
	try {
		return m_event_queue.pop_front(timeout_ms);
	} catch(ItemNotFoundException &ex) {
		return ConnectionEvent::create(CONNEVENT_NONE);
	}
}

void Connection::peerAdded(session_t peer_id, const Address &address)
{
	putEvent(ConnectionEvent::peerAdded(peer_id, address));
}

void Connection::peerRemoved(session_t peer_id, bool timeout, const Address &address)
{
	putEvent(ConnectionEvent::peerRemoved(peer_id, timeout, address));
}

void Connection::dataReceived(session_t peer_id, const Buffer<u8> &data)
{
	putEvent(ConnectionEvent::dataReceived(peer_id, data));
}

/* Interface */

void Connection::Connect(Address address)
{
	FATAL_ERROR_IF(isServer(), "Connect() called on a server connection");
	FATAL_ERROR_IF(m_transports.empty(), "Connection has no transport");
	m_transports.front()->Connect(address);
}

void Connection::Disconnect()
{
	for (auto &transport : m_transports)
		transport->Disconnect();
}

void Connection::DisconnectPeer(session_t peer_id)
{
	ITransport *transport = getPeerOwner(peer_id);
	if (!transport) {
		dout_con << getDesc() << " DisconnectPeer(): peer_id="
			<< peer_id << " not found" << std::endl;
		return;
	}
	transport->DisconnectPeer(peer_id);
}

bool Connection::ReceiveTimeoutMs(NetworkPacket *pkt, u32 timeout_ms)
{
	/*
		Note that this function can potentially wait infinitely if non-data
		events keep happening before the timeout expires.
		This is not considered to be a problem (is it?)
	*/
	for(;;) {
		ConnectionEventPtr e_ptr = waitEvent(timeout_ms);
		const ConnectionEvent &e = *e_ptr;

		if (e.type != CONNEVENT_NONE) {
			dout_con << getDesc() << ": Receive: got event: "
					<< e.describe() << std::endl;
		}

		switch (e.type) {
		case CONNEVENT_NONE:
			return false;
		case CONNEVENT_DATA_RECEIVED:
			// Data size is lesser than command size, ignoring packet
			if (e.data.getSize() < 2) {
				continue;
			}

			pkt->putRawPacket(*e.data, e.data.getSize(), e.peer_id);
			return true;
		case CONNEVENT_PEER_ADDED: {
			if (m_peerhandler)
				m_peerhandler->peerAdded(e.peer_id, e.address);
			continue;
		}
		case CONNEVENT_PEER_REMOVED: {
			if (m_peerhandler)
				m_peerhandler->peerRemoved(e.peer_id, e.timeout, e.address);
			continue;
		}
		}
	}
	return false;
}

void Connection::Send(session_t peer_id, u8 channelnum,
		NetworkPacket *pkt, bool reliable)
{
	ITransport *transport = getPeerOwner(peer_id);
	if (!transport) {
		dout_con << getDesc() << " dropped " << (reliable ? "reliable " : "")
			<< "packet for nonexistent peer_id=" << peer_id << std::endl;
		return;
	}
	transport->Send(peer_id, channelnum, pkt, reliable);
}

bool Connection::Connected() const
{
	return m_peer_table->isConnected();
}

Address Connection::getRemoteAddress() const
{
	return m_peer_table->getPeerAddress(PEER_ID_SERVER);
}

Address Connection::GetPeerAddress(session_t peer_id) const
{
	return m_peer_table->getPeerAddress(peer_id);
}

float Connection::getPeerStat(session_t peer_id, rtt_stat_type type) const
{
	return m_peer_table->getPeerStat(peer_id, type);
}

float Connection::getLocalStat(rate_stat_type type) const
{
	return m_peer_table->getLocalStat(type);
}

// Decide which address a server listens on
static Address getServerBindAddress(const NetworkOverrides &overrides)
{
	const std::string bind_str = overrides.bind_address.value_or(
			g_settings->get("bind_address"));
	u16 bind_port = overrides.bind_port.value_or(g_settings->getU16("port"));
	Address bind_addr(0, 0, 0, 0, bind_port);

	if (g_settings->getBool("ipv6_server"))
		bind_addr.setAddress(static_cast<IPv6AddressBytes*>(nullptr));
	try {
		bind_addr.Resolve(bind_str.c_str());
	} catch (const ResolveError &e) {
		warningstream << "Resolving bind address \"" << bind_str
			<< "\" failed: " << e.what()
			<< " -- Listening on all addresses." << std::endl;
	}
	if (bind_addr.isIPv6() && !g_settings->getBool("enable_ipv6"))
		throw IPv6DisabledException(bind_addr.serializeString());

	return bind_addr;
}

static std::unique_ptr<ITransport> getTransport(bool is_server,
		bool is_simple_singleplayer, bool ipv6,
		const NetworkOverrides &overrides)
{
	const std::string transport = overrides.transport ?
		overrides.transport.value() :
		(is_simple_singleplayer ? g_settings->get("singleplayer_transport") :
		"legacy");

	if (transport == "memory" && is_simple_singleplayer)
		return std::make_unique<InMemoryTransport>(is_server);

	if (transport != "legacy")
		warningstream << "Unknown transport \"" << transport
			<< "\", using \"legacy\"" << std::endl;

	// safe minimum across internet networks for ipv4 and ipv6
	u32 max_packet_size = overrides.max_packet_size.value_or(MAX_PACKET_SIZE);
	float connection_timeout = overrides.timeout.value_or(CONNECTION_TIMEOUT);
	UDPSocket socket = is_server ?
			UDPSocket::Create(getServerBindAddress(overrides)) :
			UDPSocket::CreateEphemeral(ipv6);

	return std::make_unique<LegacyTransport>(max_packet_size,
		connection_timeout, std::move(socket));
}

std::unique_ptr<IConnection> createConnection(PeerHandler *handler,
		bool is_server, bool is_simple_singleplayer, bool ipv6,
		const NetworkOverrides &overrides)
{
	auto con = std::make_unique<Connection>(is_server, handler);
	auto transport = getTransport(is_server, is_simple_singleplayer, ipv6,
			overrides);
	con->addTransport(std::move(transport));
	return con;
}

std::unique_ptr<IConnection> createClient(PeerHandler* handler,
		bool is_simple_singleplayer, bool ipv6,
		const NetworkOverrides &overrides)
{
	return createConnection(handler, false, is_simple_singleplayer, ipv6,
			overrides);
}

std::unique_ptr<IConnection> createServer(PeerHandler* handler,
		bool is_simple_singleplayer,
		const NetworkOverrides &overrides)
{
	return createConnection(handler, true, is_simple_singleplayer, false,
			overrides);
}

}
