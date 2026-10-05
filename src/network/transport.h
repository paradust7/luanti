// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "address.h"
#include "networkprotocol.h" // session_t
#include "util/pointer.h" // Buffer

class NetworkPacket;

namespace con
{

class PeerTable;

// The part of a Connection that its transports can see.
class ITransportManager
{
public:
	virtual ~ITransportManager() = default;

	// The peer table shared by all transports of the Connection.
	// Each peer is owned by the transport that added it (see IPeerData).
	virtual PeerTable *getPeerTable() = 0;

	// Called by a transport, from any thread, when it has a complete
	// packet from one of its peers to hand to the user.
	virtual void dataReceived(session_t peer_id, const Buffer<u8> &data) = 0;
};

// A way of exchanging packets with peers (e.g. UDP).
class ITransport
{
public:
	virtual ~ITransport() = default;

	// Called once by the Connection that owns this transport, before any
	// other method. The manager outlives the transport.
	virtual void attach(ITransportManager *manager) = 0;

	// Stops all activity (e.g. joins threads). The Connection calls this
	// before it deletes the peers, and the transport must not use the
	// manager afterwards.
	virtual void stop() = 0;

	// The local address this transport is bound to (if any)
	virtual Address getBindAddress() const = 0;

	// Client only: start connecting to a server
	virtual void Connect(Address address) = 0;
	// Disconnect from all peers owned by this transport
	virtual void Disconnect() = 0;

	// These are only called for peers owned by this transport. The peer
	// could still be removed concurrently, so the transport must handle
	// a peer id that no longer exists.
	virtual void DisconnectPeer(session_t peer_id) = 0;
	virtual void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) = 0;
};

} // namespace con
