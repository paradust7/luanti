// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

namespace con
{

enum rtt_stat_type : int {
	MIN_RTT,
	MAX_RTT,
	AVG_RTT,
	MIN_JITTER,
	MAX_JITTER,
	AVG_JITTER
};

enum rate_stat_type : int {
	CUR_DL_RATE,
	AVG_DL_RATE,
	CUR_INC_RATE,
	AVG_INC_RATE,
	CUR_LOSS_RATE,
	AVG_LOSS_RATE,
};

} // namespace con
