/*
 * layer 1 primitive handling and interface
 *
 * Copyright (C) 2013  Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2015  Alexander Chemeris <Alexander.Chemeris@fairwaves.co>
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
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/bits.h>
#include <osmocom/core/fsm.h>
#include <osmocom/codec/ecu.h>
#include <osmocom/gsm/abis_nm.h>
#include <osmocom/gsm/rsl.h>

#include <osmo-bts/logging.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/oml.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/l1sap.h>
#include <osmo-bts/bts_model.h>
#include <osmo-bts/amr.h>
#include <osmo-bts/abis.h>
#include <osmo-bts/scheduler.h>
#include <osmo-bts/pcu_if.h>
#include <osmo-bts/nm_common_fsm.h>
#include <osmo-bts/handover.h>

#include "l1_if.h"
#include "trx_if.h"
#include "trx_provision_fsm.h"

#define RF_DISABLED_mdB to_mdB(-10)

static const uint8_t transceiver_chan_types[_GSM_PCHAN_MAX] = {
	[GSM_PCHAN_NONE]                = 8,
	[GSM_PCHAN_CCCH]                = 4,
	[GSM_PCHAN_CCCH_SDCCH4]         = 5,
	[GSM_PCHAN_CCCH_SDCCH4_CBCH]    = 5,
	[GSM_PCHAN_TCH_F]               = 1,
	[GSM_PCHAN_TCH_H]               = 3,
	[GSM_PCHAN_SDCCH8_SACCH8C]      = 7,
	[GSM_PCHAN_SDCCH8_SACCH8C_CBCH] = 7,
	[GSM_PCHAN_PDCH]                = 13,
	/* [GSM_PCHAN_TCH_F_PDCH] not needed here, see trx_set_ts_as_pchan() */
	[GSM_PCHAN_UNKNOWN]             = 0,
};

enum gsm_phys_chan_config transceiver_chan_type_2_pchan(uint8_t type)
{
	int i;
	for (i = 0; i < _GSM_PCHAN_MAX; i++) {
		if (transceiver_chan_types[i] == type)
			return (enum gsm_phys_chan_config) i;
	}
	return GSM_PCHAN_UNKNOWN;
}

struct trx_l1h *trx_l1h_alloc(void *tall_ctx, struct phy_instance *pinst)
{
	struct trx_l1h *l1h;
	l1h = talloc_zero(tall_ctx, struct trx_l1h);
	l1h->phy_inst = pinst;
	l1h->provision_fi = osmo_fsm_inst_alloc(&trx_prov_fsm, l1h, l1h, LOGL_INFO, NULL);
	OSMO_ASSERT(osmo_fsm_inst_update_id_f_sanitize(l1h->provision_fi, '-', phy_instance_name(pinst)) == 0);
	trx_if_init(l1h);
	return l1h;
}

int bts_model_lchan_deactivate(struct gsm_lchan *lchan)
{
	int rc;
	/* set lchan inactive */
	lchan_set_state(lchan, LCHAN_S_NONE);

	/* Disable it on the scheduler: */
	rc = trx_sched_set_lchan(lchan, gsm_lchan2chan_nr(lchan), LID_DEDIC, false);

	/* Reactivate CCCH due to SI3 update in RSL */
	if (lchan->rel_act_kind == LCHAN_REL_ACT_REACT) {
		lchan->rel_act_kind = LCHAN_REL_ACT_RSL;
		trx_sched_set_lchan(lchan, gsm_lchan2chan_nr(lchan), LID_DEDIC, true);
		lchan_set_state(lchan, LCHAN_S_ACTIVE);
		return rc;
	}
	return rc;
}

int bts_model_lchan_deactivate_sacch(struct gsm_lchan *lchan)
{
	return trx_sched_set_lchan(lchan, gsm_lchan2chan_nr(lchan), LID_SACCH, false);
}

int l1if_trx_start_power_ramp(struct gsm_bts_trx *trx, ramp_compl_cb_t ramp_compl_cb)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;

	if (l1h->config.forced_max_power_red == -1)
		return power_ramp_start(trx, get_p_nominal_mdBm(trx), 0, ramp_compl_cb);
	else
		return power_ramp_start(trx, get_p_max_out_mdBm(trx) - to_mdB(l1h->config.forced_max_power_red), 1, ramp_compl_cb);
}

/* Sets the nominal power, in dB */
void l1if_trx_set_nominal_power(struct gsm_bts_trx *trx, int nominal_power)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	bool nom_pwr_changed = trx->nominal_power != nominal_power;

	trx->nominal_power = nominal_power;
	trx->power_params.trx_p_max_out_mdBm = to_mdB(nominal_power);
	/* If we receive ultra-low  nominal Tx power (<0dBm), make sure to update where we are */
	trx->power_params.p_total_cur_mdBm = OSMO_MIN(trx->power_params.p_total_cur_mdBm,
						      trx->power_params.trx_p_max_out_mdBm);

	/* If TRX is not yet powered, delay ramping until it's ON */
	if (!nom_pwr_changed || !pinst->phy_link->u.osmotrx.powered ||
	    trx->mo.nm_state.administrative == NM_STATE_UNLOCKED)
		return;

	/* We are already ON and we got new information about nominal power, so
	 * let's make sure we adapt the tx power to it
	 */
	l1if_trx_start_power_ramp(trx, NULL);
}

static void l1if_setpower_att_cb(struct trx_l1h *l1h, int power_att_db, int rc)
{
	struct phy_instance *pinst = l1h->phy_inst;
	struct gsm_bts_trx *trx = pinst->trx;

	LOGPPHI(pinst, DL1C, LOGL_DEBUG, "l1if_setpower_att_cb(power_att_db=%d, rc=%d)\n", power_att_db, rc);

	power_trx_change_compl(trx, get_p_max_out_mdBm(trx) - to_mdB(power_att_db));
}

/*
 * activation/configuration/deactivation of transceiver's TRX
 */

/* initialize the layer1 */
static int trx_init(struct gsm_bts_trx *trx)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
	int rc;

	rc = osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CFG_ENABLE, (void*)(intptr_t)true);
	if (rc != 0)
		return osmo_fsm_inst_dispatch(trx->mo.fi, NM_EV_OPSTART_NACK,
					      (void*)(intptr_t)NM_NACK_CANT_PERFORM);
	/* Send OPSTART ack */
	return osmo_fsm_inst_dispatch(trx->mo.fi, NM_EV_OPSTART_ACK, NULL);
}

/* Deact RF on transceiver */
int bts_model_trx_deact_rf(struct gsm_bts_trx *trx)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;

	return trx_if_cmd_rfmute(l1h, true);
}

/* deactivate transceiver */
void bts_model_trx_close(struct gsm_bts_trx *trx)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;

	osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CLOSE, NULL);

	/* Set to Operational State: Disabled */
	osmo_fsm_inst_dispatch(trx->mo.fi, NM_EV_DISABLE, NULL);
	osmo_fsm_inst_dispatch(trx->bb_transc.mo.fi, NM_EV_DISABLE, NULL);
}

void bts_model_abis_close(struct gsm_bts *bts)
{
	/* Go into shutdown state deactivating transceivers until Abis link
	 * becomes up again */
	bts_shutdown_ext(bts, "Abis close", false, true);
}

int bts_model_adjst_ms_pwr(struct gsm_lchan *lchan)
{
	/* we always implement the power control loop in osmo-bts software, as
	 * there is no automatism in the underlying osmo-trx */
	return 0;
}

/* set bts attributes */
static uint8_t trx_set_bts(struct gsm_bts *bts)
{
	struct phy_instance *pinst = trx_phy_instance(bts->c0);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
	uint8_t bsic = bts->bsic;
	struct gsm_bts_trx *trx;

	/* ARFCN for C0 is assigned during Set BTS Attr, see oml.c */
	osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CFG_ARFCN, (void *)(intptr_t)pinst->trx->arfcn);

	llist_for_each_entry(trx, &bts->trx_list, list) {
		pinst = trx_phy_instance(trx);
		l1h = pinst->u.osmotrx.hdl;

		osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CFG_BSIC, (void*)(intptr_t)bsic);
	}

	return 0;
}

/* set trx attributes */
static uint8_t trx_set_trx(struct gsm_bts_trx *trx)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
	struct phy_link *plink = pinst->phy_link;
	uint16_t arfcn = trx->arfcn;

	/* ARFCN for C0 is assigned during Set BTS Attr, see oml.c */
	if (trx != trx->bts->c0)
		osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CFG_ARFCN, (void *)(intptr_t)arfcn);

	/* Begin to ramp up the power if power reduction is set by OML and TRX
	   is already running. Otherwise skip, power ramping will be started
	   after TRX is running */
	if (plink->u.osmotrx.powered && l1h->config.forced_max_power_red == -1 &&
	    trx->mo.nm_state.administrative == NM_STATE_UNLOCKED)
		power_ramp_start(pinst->trx, get_p_nominal_mdBm(pinst->trx), 0, NULL);

	return 0;
}

/* set ts attributes */
static uint8_t trx_set_ts_as_pchan(struct gsm_bts_trx_ts *ts,
				   enum gsm_phys_chan_config pchan)
{
	struct phy_instance *pinst = trx_phy_instance(ts->trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
	uint8_t tn = ts->nr;
	uint8_t slottype;
	int rc;

	/* ignore disabled slots */
	if (!(l1h->config.slotmask & (1 << tn)))
		return NM_NACK_RES_NOTAVAIL;

	/* set physical channel. For dynamic timeslots, the caller should have
	 * decided on a more specific PCHAN type already. */
	OSMO_ASSERT(pchan != GSM_PCHAN_TCH_F_PDCH);
	OSMO_ASSERT(pchan != GSM_PCHAN_OSMO_DYN);
	rc = trx_sched_set_pchan(ts, pchan);
	if (rc)
		return NM_NACK_RES_NOTAVAIL;

	/* activate lchans for [CBCH/]BCCH/CCCH */
	switch (pchan) {
	case GSM_PCHAN_SDCCH8_SACCH8C_CBCH:
		/* using RSL_CHAN_OSMO_CBCH4 is correct here, because the scheduler
		 * does not distinguish between SDCCH/4+CBCH abd SDCCH/8+CBCH. */
		trx_sched_set_lchan(&ts->lchan[CBCH_LCHAN],
				    RSL_CHAN_OSMO_CBCH4, LID_DEDIC, true);
		break;
	case GSM_PCHAN_CCCH_SDCCH4_CBCH:
		trx_sched_set_lchan(&ts->lchan[CBCH_LCHAN],
				    RSL_CHAN_OSMO_CBCH4, LID_DEDIC, true);
		/* fall-through */
	case GSM_PCHAN_CCCH_SDCCH4:
	case GSM_PCHAN_CCCH:
		trx_sched_set_bcch_ccch(&ts->lchan[CCCH_LCHAN], true);
		ts->lchan[CCCH_LCHAN].rel_act_kind = LCHAN_REL_ACT_OML;
		lchan_set_state(&ts->lchan[CCCH_LCHAN], LCHAN_S_ACTIVE);
		break;
	default:
		break;
	}

	slottype = transceiver_chan_types[pchan];


	struct trx_prov_ev_cfg_ts_data data = { .tn = tn, .slottype = slottype };
	if (ts->tsc_set != 0) {
		/* On TRXC we use 3GPP compliant numbering, so +1 */
		data.tsc_set = ts->tsc_set + 1;
		data.tsc_val = ts->tsc;
		data.tsc_valid = true;
	}

	osmo_fsm_inst_dispatch(l1h->provision_fi, TRX_PROV_EV_CFG_TS, &data);

	return 0;
}

static uint8_t trx_set_ts(struct gsm_bts_trx_ts *ts)
{
	enum gsm_phys_chan_config pchan;

	/* For dynamic timeslots, pick the pchan type that should currently be
	 * active. This should only be called during init, PDCH transitions
	 * will call trx_set_ts_as_pchan() directly. */
	switch (ts->pchan) {
	case GSM_PCHAN_TCH_F_PDCH:
		OSMO_ASSERT((ts->flags & TS_F_PDCH_PENDING_MASK) == 0);
		pchan = (ts->flags & TS_F_PDCH_ACTIVE)? GSM_PCHAN_PDCH
			                              : GSM_PCHAN_TCH_F;
		break;
	case GSM_PCHAN_OSMO_DYN:
		OSMO_ASSERT(ts->dyn.pchan_is == ts->dyn.pchan_want);
		pchan = ts->dyn.pchan_is;
		break;
	default:
		pchan = ts->pchan;
		break;
	}

	return trx_set_ts_as_pchan(ts, pchan);
}


/*
 * primitive handling
 */

/* enable ciphering */
static int l1if_set_ciphering(struct gsm_lchan *lchan, uint8_t chan_nr, int downlink)
{
	/* ignore the request when the channel is not active */
	if (lchan->state != LCHAN_S_ACTIVE)
		return -EINVAL;

	if (!downlink) {
		/* set uplink */
		trx_sched_set_cipher(lchan, chan_nr, false);
		lchan->ciph_state = LCHAN_CIPH_RX_CONF;
	} else {
		/* set downlink and also set uplink, if not already */
		if (lchan->ciph_state != LCHAN_CIPH_RX_CONF)
			trx_sched_set_cipher(lchan, chan_nr, false);
		trx_sched_set_cipher(lchan, chan_nr, true);
		lchan->ciph_state = LCHAN_CIPH_RXTX_CONF;
	}

	return 0;
}

static int mph_info_chan_confirm(struct gsm_bts_trx *trx, uint8_t chan_nr,
				 enum osmo_mph_info_type type, uint8_t cause)
{
	struct osmo_phsap_prim l1sap;

	memset(&l1sap, 0, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_MPH_INFO, PRIM_OP_CONFIRM,
		NULL);
	l1sap.u.info.type = type;
	l1sap.u.info.u.act_cnf.chan_nr = chan_nr;
	l1sap.u.info.u.act_cnf.cause = cause;

	return l1sap_up(trx, &l1sap);
}

int l1if_mph_time_ind(struct gsm_bts *bts, uint32_t fn)
{
	struct osmo_phsap_prim l1sap;

	memset(&l1sap, 0, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_MPH_INFO,
		PRIM_OP_INDICATION, NULL);
	l1sap.u.info.type = PRIM_INFO_TIME;
	l1sap.u.info.u.time_ind.fn = fn;

	if (!bts->c0)
		return -EINVAL;

	return l1sap_up(bts->c0, &l1sap);
}

/* primitive from common part */
int bts_model_l1sap_down(struct gsm_bts_trx *trx, struct osmo_phsap_prim *l1sap)
{
	struct msgb *msg = l1sap->oph.msg;
	uint8_t chan_nr;
	int rc = 0;
	struct gsm_lchan *lchan;

	switch (OSMO_PRIM_HDR(&l1sap->oph)) {
	case OSMO_PRIM(PRIM_PH_DATA, PRIM_OP_REQUEST):
		if (!msg)
			break;
		/* put data into scheduler's queue */
		return trx_sched_ph_data_req(trx, l1sap);
	case OSMO_PRIM(PRIM_TCH, PRIM_OP_REQUEST):
		if (!msg)
			break;
		/* put data into scheduler's queue */
		return trx_sched_tch_req(trx, l1sap);
	case OSMO_PRIM(PRIM_MPH_INFO, PRIM_OP_REQUEST):
		switch (l1sap->u.info.type) {
		case PRIM_INFO_ACT_CIPH:
			chan_nr = l1sap->u.info.u.ciph_req.chan_nr;
			break;
		case PRIM_INFO_ACT_UL_ACC:
		case PRIM_INFO_DEACT_UL_ACC:
			chan_nr = l1sap->u.info.u.ulacc_req.chan_nr;
			break;
		default:
			/* u.act_req used by PRIM_INFO_{ACTIVATE,DEACTIVATE,MODIFY} */
			chan_nr = l1sap->u.info.u.act_req.chan_nr;
		}
		lchan = get_lchan_by_chan_nr(trx, chan_nr);
		if (OSMO_UNLIKELY(lchan == NULL)) {
			LOGP(DL1C, LOGL_ERROR,
			     "Rx MPH-INFO.req (type=0x%02x) for non-existent lchan (%s)\n",
			     l1sap->u.info.type, rsl_chan_nr_str(chan_nr));
			rc = -ENODEV;
			break;
		}

		switch (l1sap->u.info.type) {
		case PRIM_INFO_ACT_CIPH:
			if (l1sap->u.info.u.ciph_req.uplink)
				l1if_set_ciphering(lchan, chan_nr, 0);
			if (l1sap->u.info.u.ciph_req.downlink)
				l1if_set_ciphering(lchan, chan_nr, 1);
			break;
		case PRIM_INFO_ACT_UL_ACC:
			trx_sched_set_ul_access(lchan, chan_nr, true);
			break;
		case PRIM_INFO_DEACT_UL_ACC:
			trx_sched_set_ul_access(lchan, chan_nr, false);
			break;
		case PRIM_INFO_ACTIVATE:
			if ((chan_nr & 0xE0) == 0x80) {
				LOGPLCHAN(lchan, DL1C, LOGL_ERROR, "Cannot activate"
					  " channel %s\n", rsl_chan_nr_str(chan_nr));
				rc = -EPERM;
				break;
			}

			/* activate dedicated channel */
			trx_sched_set_lchan(lchan, chan_nr, LID_DEDIC, true);
			/* activate associated channel */
			trx_sched_set_lchan(lchan, chan_nr, LID_SACCH, true);
			/* set mode */
			trx_sched_set_mode(lchan->ts, chan_nr,
					   lchan->rsl_cmode, lchan->tch_mode,
					   lchan->tch.amr_mr.num_modes,
					   lchan->tch.amr_mr.mode[0].mode,
					   lchan->tch.amr_mr.mode[1].mode,
					   lchan->tch.amr_mr.mode[2].mode,
					   lchan->tch.amr_mr.mode[3].mode,
					   amr_get_initial_mode(lchan),
					   (lchan->ho.active == HANDOVER_ENABLED) ||
					   rsl_chan_rt_is_asci(lchan->rsl_chan_rt));
			/* set lchan active */
			lchan_set_state(lchan, LCHAN_S_ACTIVE);
			/* set initial ciphering */
			l1if_set_ciphering(lchan, chan_nr, 0);
			l1if_set_ciphering(lchan, chan_nr, 1);
			if (lchan->encr.alg_id)
				lchan->ciph_state = LCHAN_CIPH_RXTX_CONF;
			else
				lchan->ciph_state = LCHAN_CIPH_NONE;

			/* confirm */
			mph_info_chan_confirm(trx, chan_nr, PRIM_INFO_ACTIVATE, 0);
			break;
		case PRIM_INFO_MODIFY:
			/* change mode */
			trx_sched_set_mode(lchan->ts, chan_nr,
					   lchan->rsl_cmode, lchan->tch_mode,
					   lchan->tch.amr_mr.num_modes,
					   lchan->tch.amr_mr.mode[0].mode,
					   lchan->tch.amr_mr.mode[1].mode,
					   lchan->tch.amr_mr.mode[2].mode,
					   lchan->tch.amr_mr.mode[3].mode,
					   amr_get_initial_mode(lchan),
					   0);
			/* update ciphering params */
			l1if_set_ciphering(lchan, chan_nr, 0);
			l1if_set_ciphering(lchan, chan_nr, 1);
			if (lchan->encr.alg_id)
				lchan->ciph_state = LCHAN_CIPH_RXTX_CONF;
			else
				lchan->ciph_state = LCHAN_CIPH_NONE;
			break;
		case PRIM_INFO_DEACTIVATE:
			if ((chan_nr & 0xE0) == 0x80) {
				LOGPLCHAN(lchan, DL1C, LOGL_ERROR, "Cannot deactivate"
					  " channel %s\n", rsl_chan_nr_str(chan_nr));
				rc = -EPERM;
				break;
			}
			/* deactivate associated channel */
			bts_model_lchan_deactivate_sacch(lchan);
			if (!l1sap->u.info.u.act_req.sacch_only) {
				/* deactivate dedicated channel */
				lchan_deactivate(lchan);
				/* confirm only on dedicated channel */
				mph_info_chan_confirm(trx, chan_nr, PRIM_INFO_DEACTIVATE, 0);
			}
			break;
		default:
			LOGP(DL1C, LOGL_NOTICE, "unknown MPH-INFO.req %d\n",
				l1sap->u.info.type);
			rc = -EINVAL;
			goto done;
		}
		break;
	default:
		LOGP(DL1C, LOGL_NOTICE, "unknown prim %d op %d\n",
			l1sap->oph.primitive, l1sap->oph.operation);
		rc = -EINVAL;
		goto done;
	}

done:
	if (msg)
		msgb_free(msg);
	return rc;
}


/*
 * oml handling
 */

/* callback from OML */
int bts_model_check_oml(struct gsm_bts *bts, uint8_t msg_type,
			struct tlv_parsed *old_attr, struct tlv_parsed *new_attr,
			void *obj)
{
	/* FIXME: check if the attributes are valid */
	return 0;
}

/* callback from OML */
int bts_model_apply_oml(struct gsm_bts *bts, const struct msgb *msg,
			struct gsm_abis_mo *mo, void *obj)
{
	struct abis_om_fom_hdr *foh = msgb_l3(msg);
	int rc;

	switch (foh->msg_type) {
	case NM_MT_SET_BTS_ATTR:
		rc = trx_set_bts(obj);
		break;
	case NM_MT_SET_RADIO_ATTR:
		rc = trx_set_trx(obj);
		break;
	case NM_MT_SET_CHAN_ATTR:
		rc = trx_set_ts(obj);
		break;
	default:
		rc = 0;
		break;
	}

	return rc;
}

/* callback from OML */
int bts_model_opstart(struct gsm_bts *bts, struct gsm_abis_mo *mo,
		      void *obj)
{
	struct gsm_bts_trx *trx;
	int rc;

	switch (mo->obj_class) {
	case NM_OC_SITE_MANAGER:
	case NM_OC_BTS:
	case NM_OC_BASEB_TRANSC:
	case NM_OC_CHANNEL:
	case NM_OC_GPRS_NSE:
	case NM_OC_GPRS_CELL:
	case NM_OC_GPRS_NSVC:
		rc = osmo_fsm_inst_dispatch(mo->fi, NM_EV_OPSTART_ACK, NULL);
		break;
	case NM_OC_RADIO_CARRIER:
		/* activate transceiver */
		trx = (struct gsm_bts_trx *) obj;
		rc = trx_init(trx);
		break;
	default:
		rc = oml_mo_opstart_nack(mo, NM_NACK_OBJCLASS_NOTSUPP);
	}
	return rc;
}

static void bts_model_chg_adm_state_ramp_compl_cb(struct gsm_bts_trx *trx)
{
	LOGPTRX(trx, DL1C, LOGL_INFO, "power ramp due to ADM STATE change finished\n");
	trx->mo.procedure_pending = 0;
	if (trx->mo.nm_state.administrative == NM_STATE_LOCKED) {
		bts_model_trx_deact_rf(trx);
		pcu_tx_info_ind();
	}
}

int bts_model_chg_adm_state(struct gsm_bts *bts, struct gsm_abis_mo *mo,
			    void *obj, uint8_t adm_state)
{
	struct gsm_bts_trx *trx;
	struct phy_instance *pinst;
	struct trx_l1h *l1h;
	int rc = 0;

	switch (mo->obj_class) {
	case NM_OC_RADIO_CARRIER:
		trx = (struct gsm_bts_trx *) obj;
		pinst = trx_phy_instance(trx);
		l1h = pinst->u.osmotrx.hdl;

		/* Begin to ramp the power if TRX is already running. Otherwise
		 * skip, power ramping will be started after TRX is running.
		 * We still want to make sure to update RFMUTE status on the
		 * other side. */
		if (!pinst->phy_link->u.osmotrx.powered) {
			trx_if_cmd_rfmute(l1h, adm_state != NM_STATE_UNLOCKED);
			break;
		}

		if (mo->procedure_pending) {
			LOGPTRX(trx, DL1C, LOGL_INFO,
				"ADM change received while previous one still WIP\n");

			if (mo->nm_state.administrative == NM_STATE_LOCKED &&
			    adm_state == NM_STATE_UNLOCKED) {
				/* Previous change was UNLOCKED->LOCKED, so we
				 * were ramping down and we didn't mute RF
				 * yet, so now simply skip old ramp down compl
				 * cb, skip RF unmute and go for ramp up
				 * directly. */
				goto ramp_up;
			} else if (mo->nm_state.administrative == NM_STATE_UNLOCKED &&
			    adm_state == NM_STATE_LOCKED) {
				/* Previous change was LOCKED->UNLOCKED, so we
				 * simply need to skip ramping up and start
				 * ramping down instead, muting RF at the
				 * end as usual. Fall through usual procedure
				 * below. */
			} else if (mo->nm_state.administrative == adm_state) {
				OSMO_ASSERT(0);
			}
		}
		switch (adm_state) {
		case NM_STATE_LOCKED:
			mo->procedure_pending = 1;
			rc = power_ramp_start(trx, RF_DISABLED_mdB, 1, bts_model_chg_adm_state_ramp_compl_cb);
			break;
		case NM_STATE_UNLOCKED:
			mo->procedure_pending = 1;
			trx_if_cmd_rfmute(l1h, false);
ramp_up:
			rc = l1if_trx_start_power_ramp(trx, bts_model_chg_adm_state_ramp_compl_cb);
			if (rc == 0) {
				mo->nm_state.administrative = adm_state;
				pcu_tx_info_ind();
				return oml_mo_statechg_ack(mo);
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (rc == 0) {
		mo->nm_state.administrative = adm_state;
		return oml_mo_statechg_ack(mo);
	} else
		return oml_mo_statechg_nack(mo, NM_NACK_REQ_NOT_GRANT);
}

int bts_model_oml_estab(struct gsm_bts *bts)
{
	return 0;
}

int bts_model_change_power(struct gsm_bts_trx *trx, int p_trxout_mdBm)
{
	struct phy_instance *pinst = trx_phy_instance(trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
	int power_att = (get_p_max_out_mdBm(trx) - p_trxout_mdBm) / 1000;
	return trx_if_cmd_setpower_att(l1h, power_att, l1if_setpower_att_cb);
}

int bts_model_ts_disconnect(struct gsm_bts_trx_ts *ts)
{
	/* no action required, signal completion right away. */
	cb_ts_disconnected(ts);
	return 0;
}

void bts_model_ts_connect(struct gsm_bts_trx_ts *ts,
			 enum gsm_phys_chan_config as_pchan)
{
	int rc;
	LOGP(DL1C, LOGL_DEBUG, "%s bts_model_ts_connect(as_pchan=%s)\n",
	     gsm_ts_name(ts), gsm_pchan_name(as_pchan));

	rc = trx_set_ts_as_pchan(ts, as_pchan);
	if (rc)
		cb_ts_connected(ts, rc);

	/* cb_ts_connected will be called in l1if_setslot_cb once we receive RSP SETSLOT */
}
