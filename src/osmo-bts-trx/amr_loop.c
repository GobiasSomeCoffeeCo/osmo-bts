/* AMR link adaptation loop (see 3GPP TS 45.009, section 3) */

/* (C) 2013 by Andreas Eversberg <jolly@eversberg.eu>
 * (C) 2022 by sysmocom - s.m.f.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>

#include <osmo-bts/gsm_data.h>
#include <osmo-bts/logging.h>
#include <osmocom/gsm/gsm_utils.h>

#include "amr_loop.h"

void trx_loop_amr_input(struct l1sched_chan_state *chan_state,
			const struct l1sched_meas_set *meas_set)
{
	const struct gsm_lchan *lchan = chan_state->lchan;
	const struct amr_multirate_conf *cfg = &lchan->tch.amr_mr;
	int lqual_cb = meas_set->ci_cb; /* cB (centibel) */

	/* check if loop is enabled */
	if (!chan_state->amr_loop)
		return;

	/* wait for MS to use the requested codec */
	if (chan_state->ul_ft != chan_state->dl_cmr)
		return;

	/* count per-block C/I samples for further averaging */
	if (lchan->type == GSM_LCHAN_TCH_H) {
		chan_state->lqual_cb_num += 2;
		chan_state->lqual_cb_sum += (lqual_cb + lqual_cb);
	} else {
		chan_state->lqual_cb_num++;
		chan_state->lqual_cb_sum += lqual_cb;
	}

	/* count frames */
	if (chan_state->lqual_cb_num < 48)
		return;

	/* calculate average (reuse lqual_cb variable) */
	lqual_cb = chan_state->lqual_cb_sum / chan_state->lqual_cb_num;

	LOGPLCHAN(lchan, DLOOP, LOGL_DEBUG, "AMR link quality (C/I) is %d cB, "
		  "codec mode=%d\n", lqual_cb, chan_state->ul_ft);

	/* reset the link quality measurements */
	chan_state->lqual_cb_num = 0;
	chan_state->lqual_cb_sum = 0;

	if (chan_state->dl_cmr > 0) {
		/* The threshold/hysteresis is in 0.5 dB steps, convert to cB:
		 * 1dB is 10cB, so 0.5dB is 5cB - this is why we multiply by 5. */
		const int thresh_lower_cb = cfg->mode[chan_state->dl_cmr - 1].threshold * 5;

		/* Degrade if the link quality is below THR_MX_Dn(i - 1) */
		if (lqual_cb < thresh_lower_cb) {
			LOGPLCHAN(lchan, DLOOP, LOGL_INFO, "Degrading AMR codec mode: "
				  "%d -> %d due to link quality %d cB < THR_MX_Dn=%d cB\n",
				  chan_state->dl_cmr, chan_state->dl_cmr - 1,
				  lqual_cb, thresh_lower_cb);
			chan_state->dl_cmr--;
		}
	} else if (chan_state->dl_cmr < chan_state->codecs - 1) {
		/* The threshold/hysteresis is in 0.5 dB steps, convert to cB:
		 * 1dB is 10cB, so 0.5dB is 5cB - this is why we multiply by 5. */
		const int thresh_upper_cb = cfg->mode[chan_state->dl_cmr].threshold * 5 \
					  + cfg->mode[chan_state->dl_cmr].hysteresis * 5;

		/* Upgrade if the link quality is above THR_MX_Up(i) */
		if (lqual_cb > thresh_upper_cb) {
			LOGPLCHAN(lchan, DLOOP, LOGL_INFO, "Upgrading AMR codec mode: "
				  "%d -> %d due to link quality %d cB > THR_MX_Up=%d cB\n",
				  chan_state->dl_cmr, chan_state->dl_cmr + 1,
				  lqual_cb, thresh_upper_cb);
			chan_state->dl_cmr++;
		}
	}
}

void trx_loop_amr_set(struct l1sched_chan_state *chan_state, int loop)
{
	if (chan_state->amr_loop == loop)
		return;
	if (!chan_state->amr_loop) {
		/* reset the link quality measurements */
		chan_state->lqual_cb_num = 0;
		chan_state->lqual_cb_sum = 0;
	}

	chan_state->amr_loop = loop;
}
