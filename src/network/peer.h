// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "irrlichttypes.h"
#include "address.h"
#include "constants.h"
#include "debug.h"
#include "porting.h"
#include "networkprotocol.h"
#include "types.h"

#include <atomic>
#include <cfloat>
#include <memory>
#include <string>

namespace con
{

class PeerTable;

class Peer;
class ITransport;

// Transport-specific data attached to a Peer. The owner identifies the
// transport that created it.
class IPeerData {
public:
	IPeerData(Peer *parent, ITransport *owner) : m_parent(parent), m_owner(owner) {}
	virtual bool isTimedOut(float timeout, std::string &reason) = 0;
	virtual float getRateStat(rate_stat_type type) = 0;
	virtual ~IPeerData() = default;

	ITransport* getOwner() const {
		return m_owner;
	}
protected:
	Peer* m_parent;
private:
	ITransport* m_owner;
};

class Peer {
public:
	~Peer() {
		// Clear m_peer_data first, because it holds a pointer to this Peer
		// and its destructor might want to access our state.
		m_peer_data = nullptr;
	}

	session_t getPeerID() const {
		return m_peer_id;
	}

	const Address &getAddress() const {
		return m_address;
	}

	void resetTimeout() {
		m_receive_time = porting::getTimeMs();
	}

	bool isHalfOpen() const {
		return m_half_open;
	}

	void setFullyOpen() {
		m_half_open = false;
	}

	bool isTimedOut(float timeout, std::string &reason) const {
		// Because m_receive_time could be set concurrently by the receive thread,
		// there's a tiny chance that it is larger than current_time. Check
		// for this to prevent u64 underflow and erroneous timeout.
		u64 receive_time = m_receive_time.load();
		u64 current_time = porting::getTimeMs();
		float elapsed = current_time > receive_time ?
			(current_time - receive_time) / 1000.0f : 0.0f;
		if (elapsed > timeout) {
			reason = "timeout counter";
			return true;
		}
		if (m_peer_data)
			return m_peer_data->isTimedOut(timeout, reason);
		return false;
	}

	float getStat(rtt_stat_type type) const {
		switch (type) {
			case MIN_RTT:
				return m_rtt.min_rtt;
			case MAX_RTT:
				return m_rtt.max_rtt;
			case AVG_RTT:
				return m_rtt.avg_rtt;
			case MIN_JITTER:
				return m_rtt.jitter_min;
			case MAX_JITTER:
				return m_rtt.jitter_max;
			case AVG_JITTER:
				return m_rtt.jitter_avg;
		}
		return -1;
	}

	float getRateStat(rate_stat_type rtype) const {
		if (m_peer_data)
			return m_peer_data->getRateStat(rtype);
		return 0;
	}

	// The transport that owns this peer
	ITransport* getOwner() const {
		return m_peer_data ? m_peer_data->getOwner() : nullptr;
	}

	// Peer data should only be seen and modified by the transport that owns the peer.
	template<typename T>
	T* getPeerDataAs(const ITransport* owner) {
		if (!m_peer_data || m_peer_data->getOwner() != owner)
			return nullptr;
		return static_cast<T*>(m_peer_data.get());
	}

	void rttStatistics(float rtt,
				const std::string &profiler_id = "",
				unsigned int num_samples = 1000);

	// For InMemoryTransport which doesn't measure RTT
	void rttClear() {
		m_rtt.min_rtt = 0.0f;
		m_rtt.max_rtt = 0.0f;
		m_rtt.avg_rtt = 0.0f;
		m_rtt.jitter_min = 0.0f;
		m_rtt.jitter_max = 0.0f;
		m_rtt.jitter_avg = 0.0f;
	}
private:
	friend class PeerTable;
	Peer(const Address &address) :
		m_address(address),
		m_receive_time(porting::getTimeMs())
	{}

	void init(session_t peer_id) {
		m_peer_id = peer_id;
	}

	// Must be called before the peer is published to other threads.
	// This peer takes ownership of the data.
	void setPeerData(std::unique_ptr<IPeerData> data) {
		sanity_check(!m_peer_data);
		sanity_check(data && data->getOwner());
		m_peer_data = std::move(data);
	}

	// Address of the peer
	Address m_address;

	session_t m_peer_id = PEER_ID_INEXISTENT;
	struct {
		std::atomic<float> jitter_min = FLT_MAX;
		std::atomic<float> jitter_max = 0.0f;
		std::atomic<float> jitter_avg = -1.0f;
		std::atomic<float> min_rtt = FLT_MAX;
		std::atomic<float> max_rtt = 0.0f;
		std::atomic<float> avg_rtt = -1.0f;
	} m_rtt;

	float m_last_rtt = -1.0f;

	/*
		Until the peer has communicated with us using their assigned peer id
		the connection is considered half-open.
		During this time we inhibit re-sending any reliables or pings. This
		is to avoid spending too many resources on a potential DoS attack
		and to make sure Minetest servers are not useful for UDP amplificiation.
	*/
	std::atomic<bool> m_half_open = true;

	// Time of last receive
	std::atomic<u64> m_receive_time;

	std::unique_ptr<IPeerData> m_peer_data;
};

} // namespace con
