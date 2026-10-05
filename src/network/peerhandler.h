// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "address.h"
#include "networkprotocol.h"

namespace con
{

class PeerHandler
{
public:
	PeerHandler() = default;
	virtual ~PeerHandler() = default;

	// Note: for the handler passed to createMTP(), all functions are called
	// from within a Receive() call on the same thread. But a PeerTable
	// calls its handler from whichever thread added or removed the peer.

	/*
		This is called after the Peer has been inserted into the
		Connection's peer container.
	*/
	virtual void peerAdded(session_t peer_id, const Address &address) = 0;

	/*
		This is called after the Peer has been removed from the
		Connection's peer container.
	*/
	virtual void peerRemoved(session_t peer_id, bool timeout, const Address &address) = 0;
};

}
