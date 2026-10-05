// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "irrlichttypes.h"
#include "address.h"
#include "constants.h"
#include "debug.h"
#include "log.h"
#include "networkexceptions.h"
#include "peerhandler.h"
#include "peer.h"
#include "threading/mutex_auto_lock.h"
#include "util/basic_macros.h"
#include "util/numeric.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace con
{

constexpr u32 MAX_PEERS = 65535;

class PeerTable
{
public:
	// PeerTable calls handler methods in whatever thread performed
	// the peer operation. For this reason, only a Connection should
	// be used as a handler, since it translates the calls into
	// thread-safe events.
	PeerTable(PeerHandler* handler) : m_handler(handler) { }

	DISABLE_CLASS_COPY(PeerTable);

	session_t getOurPeerID() const {
		return m_our_peer_id;
	}

	Address getPeerAddress(session_t peer_id) const {
		auto peer = getPeerNoEx(peer_id);
		if (!peer)
			throw PeerNotFoundException("No address for peer found!");
		return peer->getAddress();
	}

	float getPeerStat(session_t peer_id, rtt_stat_type type) const {
		auto peer = getPeerNoEx(peer_id);
		if (!peer)
			return -1;
		return peer->getStat(type);
	}

	float getLocalStat(rate_stat_type type) const {
		auto peer = getPeerNoEx(PEER_ID_SERVER);
		FATAL_ERROR_IF(!peer,
				"PeerTable::getLocalStat we couldn't get our own peer? are you serious???");
		return peer->getRateStat(type);
	}

	std::vector<session_t> getPeerIDs() const {
		MutexAutoLock lock(m_mutex);
		return m_peer_ids;
	}

	std::shared_ptr<const Peer> getPeerNoEx(session_t peer_id) const {
		return findPeer(peer_id);
	}

	std::shared_ptr<Peer> getPeerNoEx(session_t peer_id) {
		return findPeer(peer_id);
	}

	// Find the peer of the given transport which has this address
	session_t lookupPeer(const Address& sender, const ITransport* owner) const {
		MutexAutoLock lock(m_mutex);
		for (const auto& [peer_id, peer] : m_peers) {
			if (peer->getOwner() == owner && peer->getAddress() == sender)
				return peer_id;
		}
		return PEER_ID_INEXISTENT;
	}

	// T is constructed with: T(Peer*, args...)
	// The peer id has not been set when T's constructor runs.
	template<typename T, typename... Args>
	std::shared_ptr<Peer> addPeer(const Address& address, Args&&... args) {
		auto new_peer = makePeer<T>(address, std::forward<Args>(args)...);
		const session_t minimum = 2;
		const session_t overflow = MAX_PEERS;
		session_t new_peer_id = PEER_ID_INEXISTENT;
		{
			MutexAutoLock lock(m_mutex);

			// Find an unused unique peer id
			for (int tries = 0; tries < 100; tries++) {
				session_t candidate = myrand_range(minimum, overflow - 1);
				if (m_peers.find(candidate) == m_peers.end()) {
					new_peer_id = candidate;
					break;
				}
			}
			if (new_peer_id == PEER_ID_INEXISTENT) {
				errorstream << "PeerTable: ran out of peer ids" << std::endl;
				return nullptr;
			}
			new_peer->init(new_peer_id);
			m_peers[new_peer_id] = new_peer;
			m_peer_ids.push_back(new_peer_id);
		}
		if (m_handler)
			m_handler->peerAdded(new_peer_id, address);
		return new_peer;
	}

	// See addPeer() comment for how T's constructor is called.
	template<typename T, typename... Args>
	std::shared_ptr<Peer> addServerPeer(const Address& address, Args&&... args) {
		auto new_peer = makePeer<T>(address, std::forward<Args>(args)...);
		new_peer->setFullyOpen();
		{
			MutexAutoLock lock(m_mutex);
			if (m_peers.find(PEER_ID_SERVER) != m_peers.end())
				return nullptr;
			new_peer->init(PEER_ID_SERVER);
			m_peers[PEER_ID_SERVER] = new_peer;
			m_peer_ids.push_back(PEER_ID_SERVER);
		}
		if (m_handler)
			m_handler->peerAdded(PEER_ID_SERVER, address);
		return new_peer;
	}

	bool deletePeer(session_t peer_id, bool timeout) {
		decltype(m_peers)::node_type node;
		// Hold lock as little as possible
		{
			MutexAutoLock lock(m_mutex);
			node = m_peers.extract(peer_id);
			if (node.empty())
				return false;
			auto it = std::find(m_peer_ids.begin(), m_peer_ids.end(), peer_id);
			m_peer_ids.erase(it);
		}
		const auto& peer = node.mapped();
		if (m_handler)
			m_handler->peerRemoved(peer_id, timeout, peer->getAddress());
		return true;
	}

	void setOurPeerID(session_t id) {
		m_our_peer_id = id;
	}

	// True if we are connected to a server and it has given us a peer id.
	bool isConnected() const {
		if (m_our_peer_id == PEER_ID_INEXISTENT || m_our_peer_id == PEER_ID_SERVER)
			return false;

		MutexAutoLock lock(m_mutex);
		if (m_peer_ids.size() != 1)
			return false;
		if (m_peer_ids[0] != PEER_ID_SERVER)
			return false;
		return true;
	}

	// Number of fully open peers owned by the given transport
	u32 getActiveCount(const ITransport* owner) const {
		MutexAutoLock lock(m_mutex);
		u32 count = 0;
		for (const auto& [peer_id, peer] : m_peers) {
			if (peer->getOwner() != owner || peer->isHalfOpen())
				continue;
			count++;
		}
		return count;
	}

	void shutdown() {
		decltype(m_peers) peers;
		{
			MutexAutoLock lock(m_mutex);
			peers.swap(m_peers);
			m_peer_ids.clear();
		}
		// We skip sending peerRemoved.
		m_handler = nullptr;
	}

private:
	// Shared implementation of the getPeerNoEx() overloads
	std::shared_ptr<Peer> findPeer(session_t peer_id) const {
		MutexAutoLock lock(m_mutex);
		auto node = m_peers.find(peer_id);
		if (node == m_peers.end()) {
			return nullptr;
		}
		// Error checking
		FATAL_ERROR_IF(node->second->getPeerID() != peer_id, "Invalid peer id");
		return node->second;
	}

	template<typename T, typename... Args>
	static std::shared_ptr<Peer> makePeer(const Address& address, Args&&... args) {
		static_assert(std::is_base_of_v<IPeerData, T>, "T must extend IPeerData");
		std::shared_ptr<Peer> new_peer(new Peer(address));
		new_peer->setPeerData(std::make_unique<T>(new_peer.get(),
				std::forward<Args>(args)...));
		return new_peer;
	}

	PeerHandler* m_handler;

	std::atomic<session_t> m_our_peer_id = PEER_ID_INEXISTENT;

	mutable std::mutex m_mutex;

	// These are protected by m_mutex
	std::map<session_t, std::shared_ptr<Peer>> m_peers;
	std::vector<session_t> m_peer_ids;
};

} // namespace con
