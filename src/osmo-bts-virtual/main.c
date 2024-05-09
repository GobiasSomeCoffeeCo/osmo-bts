/* Main program for Virtual OsmoBTS */

/* (C) 2015 by Harald Welte <laforge@gnumonks.org>
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
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/application.h>
#include <osmocom/vty/telnet_interface.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/ports.h>

#include <osmo-bts/gsm_data.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/vty.h>
#include <osmo-bts/bts_model.h>
#include <osmo-bts/l1sap.h>
#include <osmo-bts/phy_link.h>
#include "virtual_um.h"
#include "l1_if.h"

/* dummy, since no direct dsp support */
uint32_t trx_get_hlayer1(const struct gsm_bts_trx *trx)
{
	return 0;
}

int bts_model_init(struct gsm_bts *bts)
{
	struct bts_virt_priv *bts_virt = talloc_zero(bts, struct bts_virt_priv);
	bts->model_priv = bts_virt;
	bts->variant = BTS_OSMO_VIRTUAL;
	bts->support.ciphers = CIPHER_A5(1) | CIPHER_A5(2) | CIPHER_A5(3);
	bts->gprs.cell.support.gprs_codings = NM_IPAC_MASK_GPRS_CODING_CS
					    | NM_IPAC_MASK_GPRS_CODING_MCS;

	/* order alphabetically */
	osmo_bts_set_feature(bts->features, BTS_FEAT_CBCH);
	osmo_bts_set_feature(bts->features, BTS_FEAT_EGPRS);
	osmo_bts_set_feature(bts->features, BTS_FEAT_GPRS);
	osmo_bts_set_feature(bts->features, BTS_FEAT_OML_ALERTS);
	osmo_bts_set_feature(bts->features, BTS_FEAT_SPEECH_F_AMR);
	osmo_bts_set_feature(bts->features, BTS_FEAT_SPEECH_F_EFR);
	osmo_bts_set_feature(bts->features, BTS_FEAT_SPEECH_F_V1);
	osmo_bts_set_feature(bts->features, BTS_FEAT_SPEECH_H_AMR);
	osmo_bts_set_feature(bts->features, BTS_FEAT_SPEECH_H_V1);

	return 0;
}

int bts_model_trx_init(struct gsm_bts_trx *trx)
{
	/* Frequency bands indicated to the BSC */
	trx->support.freq_bands = NM_IPAC_F_FREQ_BAND_PGSM
				| NM_IPAC_F_FREQ_BAND_EGSM
				| NM_IPAC_F_FREQ_BAND_RGSM
				| NM_IPAC_F_FREQ_BAND_DCS
				| NM_IPAC_F_FREQ_BAND_PCS
				| NM_IPAC_F_FREQ_BAND_850
				| NM_IPAC_F_FREQ_BAND_480
				| NM_IPAC_F_FREQ_BAND_450;

	/* Channel types and modes indicated to the BSC */
	trx->support.chan_types = NM_IPAC_MASK_CHANT_COMMON
				| NM_IPAC_F_CHANT_BCCH_SDCCH4_CBCH
				| NM_IPAC_F_CHANT_SDCCH8_CBCH
				| NM_IPAC_F_CHANT_PDCHF
				| NM_IPAC_F_CHANT_TCHF_PDCHF;
	trx->support.chan_modes = NM_IPAC_MASK_CHANM_SPEECH
				| NM_IPAC_MASK_CHANM_CSD_NT
				| NM_IPAC_MASK_CHANM_CSD_T;
	/* TODO: missing rate adaptation for TCH/F14.4 (see OS#6167) */
	trx->support.chan_modes &= ~NM_IPAC_F_CHANM_CSD_T_14k4;
	trx->support.chan_modes &= ~NM_IPAC_F_CHANM_CSD_NT_14k4;

	return 0;
}

void bts_model_print_help()
{
	LOGP(DLGLOBAL, LOGL_NOTICE, "Unimplemented %s\n", __func__);
}

int bts_model_handle_options(int argc, char **argv)
{
	int num_errors = 0;

	while (1) {
		int option_idx = 0, c;
		static const struct option long_options[] = {
			/* specific to this hardware */
			{ 0, 0, 0, 0 }
		};

		c = getopt_long(argc, argv, "",
				long_options, &option_idx);
		if (c == -1)
			break;

		switch (c) {
		default:
			num_errors++;
			break;
		}
	}

	return num_errors;
}

void bts_model_abis_close(struct gsm_bts *bts)
{
	/* for now, we simply terminate the program and re-spawn */
	bts_shutdown(bts, "Abis close");
}

void bts_model_phy_link_set_defaults(struct phy_link *plink)
{
	plink->u.virt.bts_mcast_group = talloc_strdup(plink, DEFAULT_BTS_MCAST_GROUP);
	plink->u.virt.bts_mcast_port = DEFAULT_BTS_MCAST_PORT;
	plink->u.virt.ms_mcast_group = talloc_strdup(plink, DEFAULT_MS_MCAST_GROUP);
	plink->u.virt.ms_mcast_port = DEFAULT_MS_MCAST_PORT;
	plink->u.virt.ttl = -1; /* initialize to -1 to prevent us setting the TTL */
}

void bts_model_phy_instance_set_defaults(struct phy_instance *pinst)
{
	LOGP(DLGLOBAL, LOGL_NOTICE, "Unimplemented %s\n", __func__);
}

int bts_model_ts_disconnect(struct gsm_bts_trx_ts *ts)
{
	LOGP(DLGLOBAL, LOGL_NOTICE, "Unimplemented %s\n", __func__);
	return -ENOTSUP;
}

void bts_model_ts_connect(struct gsm_bts_trx_ts *ts, enum gsm_phys_chan_config as_pchan)
{
	LOGP(DLGLOBAL, LOGL_NOTICE, "Unimplemented %s\n", __func__);
}

int main(int argc, char **argv)
{
	return bts_main(argc, argv);
}
