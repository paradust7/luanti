// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "peer.h"
#include "profiler.h"

#include <string>

namespace con
{

void Peer::rttStatistics(float rtt, const std::string &profiler_id,
		unsigned int num_samples)
{
	if (m_last_rtt > 0) {
		/* set min max values */
		if (rtt < m_rtt.min_rtt)
			m_rtt.min_rtt = rtt;
		if (rtt >= m_rtt.max_rtt)
			m_rtt.max_rtt = rtt;

		/* do average calculation */
		if (m_rtt.avg_rtt < 0)
			m_rtt.avg_rtt  = rtt;
		else
			m_rtt.avg_rtt  = m_rtt.avg_rtt * (num_samples/(num_samples-1)) +
								rtt * (1/num_samples);

		/* do jitter calculation */

		//just use some neutral value at beginning
		float jitter = m_rtt.jitter_min;

		if (rtt > m_last_rtt)
			jitter = rtt-m_last_rtt;

		if (rtt <= m_last_rtt)
			jitter = m_last_rtt - rtt;

		if (jitter < m_rtt.jitter_min)
			m_rtt.jitter_min = jitter;
		if (jitter >= m_rtt.jitter_max)
			m_rtt.jitter_max = jitter;

		if (m_rtt.jitter_avg < 0)
			m_rtt.jitter_avg  = jitter;
		else
			m_rtt.jitter_avg  = m_rtt.jitter_avg * (num_samples/(num_samples-1)) +
								jitter * (1/num_samples);

		if (!profiler_id.empty()) {
			g_profiler->graphAdd(profiler_id + " RTT [ms]", rtt * 1000.f);
			g_profiler->graphAdd(profiler_id + " jitter [ms]", jitter * 1000.f);
		}
	}
	/* save values required for next loop */
	m_last_rtt = rtt;
}

} // namespace con
