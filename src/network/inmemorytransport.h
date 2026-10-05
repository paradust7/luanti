// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "transport.h"
#include "constants.h" // PEER_ID_INEXISTENT
#include "util/basic_macros.h"
#include "util/pointer.h" // Buffer
#include <memory>
#include <mutex>

namespace con
{

/*
	InMemoryTransport: passes packets between client and server
	in the the same process.
*/
class InMemoryTransport final : public ITransport
{
public:
	InMemoryTransport(bool is_server);
	~InMemoryTransport();

	/* ITransport */
	void attach(ITransportManager *manager) override;
	void stop() override;
	Address getBindAddress() const override;
	void Connect(Address address) override;
	void Disconnect() override;
	void DisconnectPeer(session_t peer_id) override;
	void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) override;

private:
	// Ends the current session (if any) on both sides.
	// link_mutex must be held.
	void endSession(bool timeout);

	bool m_is_server;
	ITransportManager *m_manager = nullptr;

	// The other side of the current session, or nullptr if there is none.
	// A server has at most one session (with a client) at a time.
	InMemoryTransport *m_other = nullptr;

	// This is the id of the peer this transport created in the peer table.
	// So it is actually the peer id of the other side.
	// Only set while in a session.
	session_t m_peer_id = PEER_ID_INEXISTENT;
};

} // namespace con
