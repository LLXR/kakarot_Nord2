/* QLogic qed NIC Driver
 * Copyright (c) 2015-2017  QLogic Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and /or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "qed_hsi.h"
#include "qed_hw.h"
#include "qed_init_ops.h"
#include "qed_reg_addr.h"

/* General constants */
#define QM_PQ_MEM_4KB(pq_size)	(pq_size ? DIV_ROUND_UP((pq_size + 1) *	\
							QM_PQ_ELEMENT_SIZE, \
							0x1000) : 0)
#define QM_PQ_SIZE_256B(pq_size)	(pq_size ? DIV_ROUND_UP(pq_size, \
								0x100) - 1 : 0)
#define QM_INVALID_PQ_ID                        0xffff
/* Feature enable */
#define QM_BYPASS_EN                            1
#define QM_BYTE_CRD_EN                          1
/* Other PQ constants */
#define QM_OTHER_PQS_PER_PF                     4
/* WFQ constants */
#define QM_WFQ_UPPER_BOUND		62500000
#define QM_WFQ_VP_PQ_VOQ_SHIFT          0
#define QM_WFQ_VP_PQ_PF_SHIFT           5
#define QM_WFQ_INC_VAL(weight)          ((weight) * 0x9000)
#define QM_WFQ_MAX_INC_VAL                      43750000

/* RL constants */
#define QM_RL_UPPER_BOUND                       62500000
#define QM_RL_PERIOD                            5               /* in us */
#define QM_RL_PERIOD_CLK_25M            (25 * QM_RL_PERIOD)
#define QM_RL_MAX_INC_VAL                       43750000
#define QM_RL_INC_VAL(rate)		max_t(u32,	\
					      (u32)(((rate ? rate : \
						      1000000) *    \
						     QM_RL_PERIOD * \
						     101) / (8 * 100)), 1)
/* AFullOprtnstcCrdMask constants */
#define QM_OPLUSR_LINE_VOQ_DEF           1
#define QM_OPLUSR_FW_STOP_DEF            0
#define QM_OPLUSR_PQ_EMPTY_DEF           1
/* Command Queue constants */
#define PBF_CMDQ_PURE_LB_LINES                          150
#define PBF_CMDQ_LINES_RT_OFFSET(voq)           (		 \
		PBF_REG_YCMD_QS_NUM_LINES_VOQ0_RT_OFFSET + voq * \
		(PBF_REG_YCMD_QS_NUM_LINES_VOQ1_RT_OFFSET -	 \
		 PBF_REG_YCMD_QS_NUM_LINES_VOQ0_RT_OFFSET))
#define PBF_BTB_GUARANTEED_RT_OFFSET(voq)       (	      \
		PBF_REG_BTB_GUARANTEED_VOQ0_RT_OFFSET + voq * \
		(PBF_REG_BTB_GUARANTEED_VOQ1_RT_OFFSET -      \
		 PBF_REG_BTB_GUARANTEED_VOQ0_RT_OFFSET))
#define QM_VOQ_LINE_CRD(pbf_cmd_lines)          ((((pbf_cmd_lines) - \
						   4) *		     \
						  2) | QM_LINE_CRD_REG_SIGN_BIT)
/* BTB: blocks constants (block size = 256B) */
#define BTB_JUMBO_PKT_BLOCKS            38
#define BTB_HEADROOM_BLOCKS                     BTB_JUMBO_PKT_BLOCKS
#define BTB_PURE_LB_FACTOR                      10
#define BTB_PURE_LB_RATIO                       7
/* QM stop command constants */
#define QM_STOP_PQ_MASK_WIDTH           32
#define QM_STOP_CMD_ADDR                2
#define QM_STOP_CMD_STRUCT_SIZE         2
#define QM_STOP_CMD_PAUSE_MASK_OFFSET   0
#define QM_STOP_CMD_PAUSE_MASK_SHIFT    0
#define QM_STOP_CMD_PAUSE_MASK_MASK     -1
#define QM_STOP_CMD_GROUP_ID_OFFSET     1
#define QM_STOP_CMD_GROUP_ID_SHIFT      16
#define QM_STOP_CMD_GROUP_ID_MASK       15
#define QM_STOP_CMD_PQ_TYPE_OFFSET      1
#define QM_STOP_CMD_PQ_TYPE_SHIFT       24
#define QM_STOP_CMD_PQ_TYPE_MASK        1
#define QM_STOP_CMD_MAX_POLL_COUNT      100
#define QM_STOP_CMD_POLL_PERIOD_US      500

/* QM command macros */
#define QM_CMD_STRUCT_SIZE(cmd)			cmd ## \
	_STRUCT_SIZE
#define QM_CMD_SET_FIELD(var, cmd, field,				  \
			 value)        SET_FIELD(var[cmd ## _ ## field ## \
						     _OFFSET],		  \
						 cmd ## _ ## field,	  \
						 value)
/* QM: VOQ macros */
#define PHYS_VOQ(port, tc, max_phys_tcs_per_port) ((port) *	\
						   (max_phys_tcs_per_port) + \
						   (tc))
#define LB_VOQ(port)				( \
		MAX_PHYS_VOQS + (port))
#define VOQ(port, tc, max_phy_tcs_pr_port)	\
	((tc) <		\
	 LB_TC ? PHYS_VOQ(port,		\
			  tc,			 \
			  max_phy_tcs_pr_port) \
		: LB_VOQ(port))
/******************** INTERNAL IMPLEMENTATION *********************/
/* Prepare PF RL enable/disable runtime init values */
static void qed_enable_pf_rl(struct qed_hwfn *p_hwfn, bool pf_rl_en)
{
	STORE_RT_REG(p_hwfn, QM_REG_RLPFENABLE_RT_OFFSET, pf_rl_en ? 1 : 0);
	if (pf_rl_en) {
		/* Enable RLs for all VOQs */
		STORE_RT_REG(p_hwfn, QM_REG_RLPFVOQENABLE_RT_OFFSET,
			     (1 << MAX_NUM_VOQS) - 1);
		/* Write RL period */
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLPFPERIOD_RT_OFFSET, QM_RL_PERIOD_CLK_25M);
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLPFPERIODTIMER_RT_OFFSET,
			     QM_RL_PERIOD_CLK_25M);

		/* Set credit threshold for QM bypass flow */
		if (QM_BYPASS_EN)
			STORE_RT_REG(p_hwfn,
				     QM_REG_AFULLQMBYPTHRPFRL_RT_OFFSET,
				     QM_RL_UPPER_BOUND);
	}
}

/* Prepare PF WFQ enable/disable runtime init values */
static void qed_enable_pf_wfq(struct qed_hwfn *p_hwfn, bool pf_wfq_en)
{
	STORE_RT_REG(p_hwfn, QM_REG_WFQPFENABLE_RT_OFFSET, pf_wfq_en ? 1 : 0);

	/* Set credit threshold for QM bypass flow */
	if (pf_wfq_en && QM_BYPASS_EN)
		STORE_RT_REG(p_hwfn,
			     QM_REG_AFULLQMBYPTHRPFWFQ_RT_OFFSET,
			     QM_WFQ_UPPER_BOUND);
}

/* Prepare VPORT RL enable/disable runtime init values */
static void qed_enable_vport_rl(struct qed_hwfn *p_hwfn, bool vport_rl_en)
{
	STORE_RT_REG(p_hwfn, QM_REG_RLGLBLENABLE_RT_OFFSET,
		     vport_rl_en ? 1 : 0);
	if (vport_rl_en) {
		/* Write RL period (use timer 0 only) */
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLGLBLPERIOD_0_RT_OFFSET,
			     QM_RL_PERIOD_CLK_25M);
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLGLBLPERIODTIMER_0_RT_OFFSET,
			     QM_RL_PERIOD_CLK_25M);

		/* Set credit threshold for QM bypass flow */
		if (QM_BYPASS_EN)
			STORE_RT_REG(p_hwfn,
				     QM_REG_AFULLQMBYPTHRGLBLRL_RT_OFFSET,
				     QM_RL_UPPER_BOUND);
	}
}

/* Prepare VPORT WFQ enable/disable runtime init values */
static void qed_enable_vport_wfq(struct qed_hwfn *p_hwfn, bool vport_wfq_en)
{
	STORE_RT_REG(p_hwfn, QM_REG_WFQVPENABLE_RT_OFFSET,
		     vport_wfq_en ? 1 : 0);

	/* Set credit threshold for QM bypass flow */
	if (vport_wfq_en && QM_BYPASS_EN)
		STORE_RT_REG(p_hwfn,
			     QM_REG_AFULLQMBYPTHRVPWFQ_RT_OFFSET,
			     QM_WFQ_UPPER_BOUND);
}

/* Prepare runtime init values to allocate PBF command queue lines for
 * the specified VOQ.
 */
static void qed_cmdq_lines_voq_rt_init(struct qed_hwfn *p_hwfn,
				       u8 voq, u16 cmdq_lines)
{
	u32 qm_line_crd;

	qm_line_crd = QM_VOQ_LINE_CRD(cmdq_lines);
	OVERWRITE_RT_REG(p_hwfn, PBF_CMDQ_LINES_RT_OFFSET(voq),
			 (u32)cmdq_lines);
	STORE_RT_REG(p_hwfn, QM_REG_VOQCRDLINE_RT_OFFSET + voq, qm_line_crd);
	STORE_RT_REG(p_hwfn, QM_REG_VOQINITCRDLINE_RT_OFFSET + voq,
		     qm_line_crd);
}

/* Prepare runtime init values to allocate PBF command queue lines. */
static void qed_cmdq_lines_rt_init(
	struct qed_hwfn *p_hwfn,
	u8 max_ports_per_engine,
	u8 max_phys_tcs_per_port,
	struct init_qm_port_params port_params[MAX_NUM_PORTS])
{
	u8 tc, voq, port_id, num_tcs_in_port;

	/* Clear PBF lines for all VOQs */
	for (voq = 0; voq < MAX_NUM_VOQS; voq++)
		STORE_RT_REG(p_hwfn, PBF_CMDQ_LINES_RT_OFFSET(voq), 0);
	for (port_id = 0; port_id < max_ports_per_engine; port_id++) {
		if (port_params[port_id].active) {
			u16 phys_lines, phys_lines_per_tc;

			/* find #lines to divide between active phys TCs */
			phys_lines = port_params[port_id].num_pbf_cmd_lines -
				     PBF_CMDQ_PURE_LB_LINES;
			/* find #lines per active physical TC */
			num_tcs_in_port = 0;
			for (tc = 0; tc < NUM_OF_PHYS_TCS; tc++) {
				if (((port_params[port_id].active_phys_tcs >>
				      tc) & 0x1) == 1)
					num_tcs_in_port++;
			}

			phys_lines_per_tc = phys_lines / num_tcs_in_port;
			/* init registers per active TC */
			for (tc = 0; tc < NUM_OF_PHYS_TCS; tc++) {
				if (((port_params[port_id].active_phys_tcs >>
				      tc) & 0x1) != 1)
					continue;

				voq = PHYS_VOQ(port_id, tc,
					       max_phys_tcs_per_port);
				qed_cmdq_lines_voq_rt_init(p_hwfn, voq,
							   phys_lines_per_tc);
			}

			/* init registers for pure LB TC */
			qed_cmdq_lines_voq_rt_init(p_hwfn, LB_VOQ(port_id),
						   PBF_CMDQ_PURE_LB_LINES);
		}
	}
}

static void qed_btb_blocks_rt_init(
	struct qed_hwfn *p_hwfn,
	u8 max_ports_per_engine,
	u8 max_phys_tcs_per_port,
	struct init_qm_port_params port_params[MAX_NUM_PORTS])
{
	u32 usable_blocks, pure_lb_blocks, phys_blocks;
	u8 tc, voq, port_id, num_tcs_in_port;

	for (port_id = 0; port_id < max_ports_per_engine; port_id++) {
		u32 temp;

		if (!port_params[port_id].active)
			continue;

		/* Subtract headroom blocks */
		usable_blocks = port_params[port_id].num_btb_blocks -
				BTB_HEADROOM_BLOCKS;

		/* find blocks per physical TC */
		num_tcs_in_port = 0;
		for (tc = 0; tc < NUM_OF_PHYS_TCS; tc++) {
			if (((port_params[port_id].active_phys_tcs >>
			      tc) & 0x1) == 1)
				num_tcs_in_port++;
		}

		pure_lb_blocks = (usable_blocks * BTB_PURE_LB_FACTOR) /
				 (num_tcs_in_port * BTB_PURE_LB_FACTOR +
				  BTB_PURE_LB_RATIO);
		pure_lb_blocks = max_t(u32, BTB_JUMBO_PKT_BLOCKS,
				       pure_lb_blocks / BTB_PURE_LB_FACTOR);
		phys_blocks = (usable_blocks - pure_lb_blocks) /
			      num_tcs_in_port;

		/* Init physical TCs */
		for (tc = 0; tc < NUM_OF_PHYS_TCS; tc++) {
			if (((port_params[port_id].active_phys_tcs >>
			      tc) & 0x1) != 1)
				continue;

			voq = PHYS_VOQ(port_id, tc,
				       max_phys_tcs_per_port);
			STORE_RT_REG(p_hwfn, PBF_BTB_GUARANTEED_RT_OFFSET(voq),
				     phys_blocks);
		}

		/* Init pure LB TC */
		temp = LB_VOQ(port_id);
		STORE_RT_REG(p_hwfn, PBF_BTB_GUARANTEED_RT_OFFSET(temp),
			     pure_lb_blocks);
	}
}

/* Prepare Tx PQ mapping runtime init values for the specified PF */
static void qed_tx_pq_map_rt_init(
	struct qed_hwfn *p_hwfn,
	struct qed_ptt *p_ptt,
	struct qed_qm_pf_rt_init_params *p_params,
	u32 base_mem_addr_4kb)
{
	struct init_qm_vport_params *vport_params = p_params->vport_params;
	u16 num_pqs = p_params->num_pf_pqs + p_params->num_vf_pqs;
	u16 first_pq_group = p_params->start_pq / QM_PF_QUEUE_GROUP_SIZE;
	u16 last_pq_group = (p_params->start_pq + num_pqs - 1) /
			    QM_PF_QUEUE_GROUP_SIZE;
	u16 i, pq_id, pq_group;

	/* A bit per Tx PQ indicating if the PQ is associated with a VF */
	u32 tx_pq_vf_mask[MAX_QM_TX_QUEUES / QM_PF_QUEUE_GROUP_SIZE] = { 0 };
	u32 num_tx_pq_vf_masks = MAX_QM_TX_QUEUES / QM_PF_QUEUE_GROUP_SIZE;
	u32 pq_mem_4kb = QM_PQ_MEM_4KB(p_params->num_pf_cids);
	u32 vport_pq_mem_4kb = QM_PQ_MEM_4KB(p_params->num_vf_cids);
	u32 mem_addr_4kb = base_mem_addr_4kb;

	/* Set mapping from PQ group to PF */
	for (pq_group = first_pq_group; pq_group <= last_pq_group; pq_group++)
		STORE_RT_REG(p_hwfn, QM_REG_PQTX2PF_0_RT_OFFSET + pq_group,
			     (u32)(p_params->pf_id));
	/* Set PQ sizes */
	STORE_RT_REG(p_hwfn, QM_REG_MAXPQSIZE_0_RT_OFFSET,
		     QM_PQ_SIZE_256B(p_params->num_pf_cids));
	STORE_RT_REG(p_hwfn, QM_REG_MAXPQSIZE_1_RT_OFFSET,
		     QM_PQ_SIZE_256B(p_params->num_vf_cids));

	/* Go over all Tx PQs */
	for (i = 0, pq_id = p_params->start_pq; i < num_pqs; i++, pq_id++) {
		u8 voq = VOQ(p_params->port_id, p_params->pq_params[i].tc_id,
			     p_params->max_phys_tcs_per_port);
		bool is_vf_pq = (i >= p_params->num_pf_pqs);
		struct qm_rf_pq_map tx_pq_map;

		bool rl_valid = p_params->pq_params[i].rl_valid &&
				(p_params->pq_params[i].vport_id <
				 MAX_QM_GLOBAL_RLS);

		/* Update first Tx PQ of VPORT/TC */
		u8 vport_id_in_pf = p_params->pq_params[i].vport_id -
				    p_params->start_vport;
		u16 *pq_ids = &vport_params[vport_id_in_pf].first_tx_pq_id[0];
		u16 first_tx_pq_id = pq_ids[p_params->pq_params[i].tc_id];

		if (first_tx_pq_id == QM_INVALID_PQ_ID) {
			/* Create new VP PQ */
			pq_ids[p_params->pq_params[i].tc_id] = pq_id;
			first_tx_pq_id = pq_id;

			/* Map VP PQ to VOQ and PF */
			STORE_RT_REG(p_hwfn,
				     QM_REG_WFQVPMAP_RT_OFFSET +
				     first_tx_pq_id,
				     (voq << QM_WFQ_VP_PQ_VOQ_SHIFT) |
				     (p_params->pf_id <<
				      QM_WFQ_VP_PQ_PF_SHIFT));
		}

		if (p_params->pq_params[i].rl_valid && !rl_valid)
			DP_NOTICE(p_hwfn,
				  "Invalid VPORT ID for rate limiter configuration");
		/* Fill PQ map entry */
		memset(&tx_pq_map, 0, sizeof(tx_pq_map));
		SET_FIELD(tx_pq_map.reg, QM_RF_PQ_MAP_PQ_VALID, 1);
		SET_FIELD(tx_pq_map.reg,
			  QM_RF_PQ_MAP_RL_VALID, rl_valid ? 1 : 0);
		SET_FIELD(tx_pq_map.reg, QM_RF_PQ_MAP_VP_PQ_ID, first_tx_pq_id);
		SET_FIELD(tx_pq_map.reg, QM_RF_PQ_MAP_RL_ID,
			  rl_valid ?
			  p_params->pq_params[i].vport_id : 0);
		SET_FIELD(tx_pq_map.reg, QM_RF_PQ_MAP_VOQ, voq);
		SET_FIELD(tx_pq_map.reg, QM_RF_PQ_MAP_WRR_WEIGHT_GROUP,
			  p_params->pq_params[i].wrr_group);
		/* Write PQ map entry to CAM */
		STORE_RT_REG(p_hwfn, QM_REG_TXPQMAP_RT_OFFSET + pq_id,
			     *((u32 *)&tx_pq_map));
		/* Set base address */
		STORE_RT_REG(p_hwfn,
			     QM_REG_BASEADDRTXPQ_RT_OFFSET + pq_id,
			     mem_addr_4kb);

		/* If VF PQ, add indication to PQ VF mask */
		if (is_vf_pq) {
			tx_pq_vf_mask[pq_id /
				      QM_PF_QUEUE_GROUP_SIZE] |=
			    BIT((pq_id % QM_PF_QUEUE_GROUP_SIZE));
			mem_addr_4kb += vport_pq_mem_4kb;
		} else {
			mem_addr_4kb += pq_mem_4kb;
		}
	}

	/* Store Tx PQ VF mask to size select register */
	for (i = 0; i < num_tx_pq_vf_masks; i++)
		if (tx_pq_vf_mask[i])
			STORE_RT_REG(p_hwfn,
				     QM_REG_MAXPQSIZETXSEL_0_RT_OFFSET + i,
				     tx_pq_vf_mask[i]);
}

/* Prepare Other PQ mapping runtime init values for the specified PF */
static void qed_other_pq_map_rt_init(struct qed_hwfn *p_hwfn,
				     u8 port_id,
				     u8 pf_id,
				     u32 num_pf_cids,
				     u32 num_tids, u32 base_mem_addr_4kb)
{
	u32 pq_size, pq_mem_4kb, mem_addr_4kb;
	u16 i, pq_id, pq_group;

	/* a single other PQ group is used in each PF,
	 * where PQ group i is used in PF i.
	 */
	pq_group = pf_id;
	pq_size = num_pf_cids + num_tids;
	pq_mem_4kb = QM_PQ_MEM_4KB(pq_size);
	mem_addr_4kb = base_mem_addr_4kb;

	/* Map PQ group to PF */
	STORE_RT_REG(p_hwfn, QM_REG_PQOTHER2PF_0_RT_OFFSET + pq_group,
		     (u32)(pf_id));
	/* Set PQ sizes */
	STORE_RT_REG(p_hwfn, QM_REG_MAXPQSIZE_2_RT_OFFSET,
		     QM_PQ_SIZE_256B(pq_size));

	/* Set base address */
	for (i = 0, pq_id = pf_id * QM_PF_QUEUE_GROUP_SIZE;
	     i < QM_OTHER_PQS_PER_PF; i++, pq_id++) {
		STORE_RT_REG(p_hwfn,
			     QM_REG_BASEADDROTHERPQ_RT_OFFSET + pq_id,
			     mem_addr_4kb);
		mem_addr_4kb += pq_mem_4kb;
	}
}

/* Prepare PF WFQ runtime init values for the specified PF.
 * Return -1 on error.
 */
static int qed_pf_wfq_rt_init(struct qed_hwfn *p_hwfn,
			      struct qed_qm_pf_rt_init_params *p_params)
{
	u16 num_tx_pqs = p_params->num_pf_pqs + p_params->num_vf_pqs;
	u32 crd_reg_offset;
	u32 inc_val;
	u16 i;

	if (p_params->pf_id < MAX_NUM_PFS_BB)
		crd_reg_offset = QM_REG_WFQPFCRD_RT_OFFSET;
	else
		crd_reg_offset = QM_REG_WFQPFCRD_MSB_RT_OFFSET;
	crd_reg_offset += p_params->pf_id % MAX_NUM_PFS_BB;

	inc_val = QM_WFQ_INC_VAL(p_params->pf_wfq);
	if (!inc_val || inc_val > QM_WFQ_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid PF WFQ weight configuration\n");
		return -1;
	}

	for (i = 0; i < num_tx_pqs; i++) {
		u8 voq = VOQ(p_params->port_id, p_params->pq_params[i].tc_id,
			     p_params->max_phys_tcs_per_port);

		OVERWRITE_RT_REG(p_hwfn,
				 crd_reg_offset + voq * MAX_NUM_PFS_BB,
				 QM_WFQ_CRD_REG_SIGN_BIT);
	}

	STORE_RT_REG(p_hwfn,
		     QM_REG_WFQPFUPPERBOUND_RT_OFFSET + p_params->pf_id,
		     QM_WFQ_UPPER_BOUND | QM_WFQ_CRD_REG_SIGN_BIT);
	STORE_RT_REG(p_hwfn, QM_REG_WFQPFWEIGHT_RT_OFFSET + p_params->pf_id,
		     inc_val);
	return 0;
}

/* Prepare PF RL runtime init values for the specified PF.
 * Return -1 on error.
 */
static int qed_pf_rl_rt_init(struct qed_hwfn *p_hwfn, u8 pf_id, u32 pf_rl)
{
	u32 inc_val = QM_RL_INC_VAL(pf_rl);

	if (inc_val > QM_RL_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid PF rate limit configuration\n");
		return -1;
	}
	STORE_RT_REG(p_hwfn, QM_REG_RLPFCRD_RT_OFFSET + pf_id,
		     QM_RL_CRD_REG_SIGN_BIT);
	STORE_RT_REG(p_hwfn, QM_REG_RLPFUPPERBOUND_RT_OFFSET + pf_id,
		     QM_RL_UPPER_BOUND | QM_RL_CRD_REG_SIGN_BIT);
	STORE_RT_REG(p_hwfn, QM_REG_RLPFINCVAL_RT_OFFSET + pf_id, inc_val);
	return 0;
}

/* Prepare VPORT WFQ runtime init values for the specified VPORTs.
 * Return -1 on error.
 */
static int qed_vp_wfq_rt_init(struct qed_hwfn *p_hwfn,
			      u8 num_vports,
			      struct init_qm_vport_params *vport_params)
{
	u32 inc_val;
	u8 tc, i;

	/* Go over all PF VPORTs */
	for (i = 0; i < num_vports; i++) {

		if (!vport_params[i].vport_wfq)
			continue;

		inc_val = QM_WFQ_INC_VAL(vport_params[i].vport_wfq);
		if (inc_val > QM_WFQ_MAX_INC_VAL) {
			DP_NOTICE(p_hwfn,
				  "Invalid VPORT WFQ weight configuration\n");
			return -1;
		}

		/* each VPORT can have several VPORT PQ IDs for
		 * different TCs
		 */
		for (tc = 0; tc < NUM_OF_TCS; tc++) {
			u16 vport_pq_id = vport_params[i].first_tx_pq_id[tc];

			if (vport_pq_id != QM_INVALID_PQ_ID) {
				STORE_RT_REG(p_hwfn,
					     QM_REG_WFQVPCRD_RT_OFFSET +
					     vport_pq_id,
					     QM_WFQ_CRD_REG_SIGN_BIT);
				STORE_RT_REG(p_hwfn,
					     QM_REG_WFQVPWEIGHT_RT_OFFSET +
					     vport_pq_id, inc_val);
			}
		}
	}

	return 0;
}

static int qed_vport_rl_rt_init(struct qed_hwfn *p_hwfn,
				u8 start_vport,
				u8 num_vports,
				struct init_qm_vport_params *vport_params)
{
	u8 i, vport_id;

	if (start_vport + num_vports >= MAX_QM_GLOBAL_RLS) {
		DP_NOTICE(p_hwfn,
			  "Invalid VPORT ID for rate limiter configuration\n");
		return -1;
	}

	/* Go over all PF VPORTs */
	for (i = 0, vport_id = start_vport; i < num_vports; i++, vport_id++) {
		u32 inc_val = QM_RL_INC_VAL(vport_params[i].vport_rl);

		if (inc_val > QM_RL_MAX_INC_VAL) {
			DP_NOTICE(p_hwfn,
				  "Invalid VPORT rate-limit configuration\n");
			return -1;
		}

		STORE_RT_REG(p_hwfn,
			     QM_REG_RLGLBLCRD_RT_OFFSET + vport_id,
			     QM_RL_CRD_REG_SIGN_BIT);
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLGLBLUPPERBOUND_RT_OFFSET + vport_id,
			     QM_RL_UPPER_BOUND | QM_RL_CRD_REG_SIGN_BIT);
		STORE_RT_REG(p_hwfn,
			     QM_REG_RLGLBLINCVAL_RT_OFFSET + vport_id,
			     inc_val);
	}

	return 0;
}

static bool qed_poll_on_qm_cmd_ready(struct qed_hwfn *p_hwfn,
				     struct qed_ptt *p_ptt)
{
	u32 reg_val, i;

	for (i = 0, reg_val = 0; i < QM_STOP_CMD_MAX_POLL_COUNT && reg_val == 0;
	     i++) {
		udelay(QM_STOP_CMD_POLL_PERIOD_US);
		reg_val = qed_rd(p_hwfn, p_ptt, QM_REG_SDMCMDREADY);
	}

	/* Check if timeout while waiting for SDM command ready */
	if (i == QM_STOP_CMD_MAX_POLL_COUNT) {
		DP_VERBOSE(p_hwfn, NETIF_MSG_HW,
			   "Timeout when waiting for QM SDM command ready signal\n");
		return false;
	}

	return true;
}

static bool qed_send_qm_cmd(struct qed_hwfn *p_hwfn,
			    struct qed_ptt *p_ptt,
			    u32 cmd_addr, u32 cmd_data_lsb, u32 cmd_data_msb)
{
	if (!qed_poll_on_qm_cmd_ready(p_hwfn, p_ptt))
		return false;

	qed_wr(p_hwfn, p_ptt, QM_REG_SDMCMDADDR, cmd_addr);
	qed_wr(p_hwfn, p_ptt, QM_REG_SDMCMDDATALSB, cmd_data_lsb);
	qed_wr(p_hwfn, p_ptt, QM_REG_SDMCMDDATAMSB, cmd_data_msb);
	qed_wr(p_hwfn, p_ptt, QM_REG_SDMCMDGO, 1);
	qed_wr(p_hwfn, p_ptt, QM_REG_SDMCMDGO, 0);

	return qed_poll_on_qm_cmd_ready(p_hwfn, p_ptt);
}

/******************** INTERFACE IMPLEMENTATION *********************/
u32 qed_qm_pf_mem_size(u8 pf_id,
		       u32 num_pf_cids,
		       u32 num_vf_cids,
		       u32 num_tids, u16 num_pf_pqs, u16 num_vf_pqs)
{
	return QM_PQ_MEM_4KB(num_pf_cids) * num_pf_pqs +
	       QM_PQ_MEM_4KB(num_vf_cids) * num_vf_pqs +
	       QM_PQ_MEM_4KB(num_pf_cids + num_tids) * QM_OTHER_PQS_PER_PF;
}

int qed_qm_common_rt_init(
	struct qed_hwfn *p_hwfn,
	struct qed_qm_common_rt_init_params *p_params)
{
	/* init AFullOprtnstcCrdMask */
	u32 mask = (QM_OPLUSR_LINE_VOQ_DEF <<
		    QM_RF_OPLUSRTUNISTIC_MASK_LINEVOQ_SHIFT) |
		   (QM_BYTE_CRD_EN << QM_RF_OPLUSRTUNISTIC_MASK_BYTEVOQ_SHIFT) |
		   (p_params->pf_wfq_en <<
		    QM_RF_OPLUSRTUNISTIC_MASK_PFWFQ_SHIFT) |
		   (p_params->vport_wfq_en <<
		    QM_RF_OPLUSRTUNISTIC_MASK_VPWFQ_SHIFT) |
		   (p_params->pf_rl_en <<
		    QM_RF_OPLUSRTUNISTIC_MASK_PFRL_SHIFT) |
		   (p_params->vport_rl_en <<
		    QM_RF_OPLUSRTUNISTIC_MASK_VPQCNRL_SHIFT) |
		   (QM_OPLUSR_FW_STOP_DEF <<
		    QM_RF_OPLUSRTUNISTIC_MASK_FWPAUSE_SHIFT) |
		   (QM_OPLUSR_PQ_EMPTY_DEF <<
		    QM_RF_OPLUSRTUNISTIC_MASK_QUEUEEMPTY_SHIFT);

	STORE_RT_REG(p_hwfn, QM_REG_AFULLOPRTNSTCCRDMASK_RT_OFFSET, mask);
	qed_enable_pf_rl(p_hwfn, p_params->pf_rl_en);
	qed_enable_pf_wfq(p_hwfn, p_params->pf_wfq_en);
	qed_enable_vport_rl(p_hwfn, p_params->vport_rl_en);
	qed_enable_vport_wfq(p_hwfn, p_params->vport_wfq_en);
	qed_cmdq_lines_rt_init(p_hwfn,
			       p_params->max_ports_per_engine,
			       p_params->max_phys_tcs_per_port,
			       p_params->port_params);
	qed_btb_blocks_rt_init(p_hwfn,
			       p_params->max_ports_per_engine,
			       p_params->max_phys_tcs_per_port,
			       p_params->port_params);
	return 0;
}

int qed_qm_pf_rt_init(struct qed_hwfn *p_hwfn,
		      struct qed_ptt *p_ptt,
		      struct qed_qm_pf_rt_init_params *p_params)
{
	struct init_qm_vport_params *vport_params = p_params->vport_params;
	u32 other_mem_size_4kb = QM_PQ_MEM_4KB(p_params->num_pf_cids +
					       p_params->num_tids) *
				 QM_OTHER_PQS_PER_PF;
	u8 tc, i;

	/* Clear first Tx PQ ID array for each VPORT */
	for (i = 0; i < p_params->num_vports; i++)
		for (tc = 0; tc < NUM_OF_TCS; tc++)
			vport_params[i].first_tx_pq_id[tc] = QM_INVALID_PQ_ID;

	/* Map Other PQs (if any) */
	qed_other_pq_map_rt_init(p_hwfn, p_params->port_id, p_params->pf_id,
				 p_params->num_pf_cids, p_params->num_tids, 0);

	/* Map Tx PQs */
	qed_tx_pq_map_rt_init(p_hwfn, p_ptt, p_params, other_mem_size_4kb);

	if (p_params->pf_wfq)
		if (qed_pf_wfq_rt_init(p_hwfn, p_params))
			return -1;

	if (qed_pf_rl_rt_init(p_hwfn, p_params->pf_id, p_params->pf_rl))
		return -1;

	if (qed_vp_wfq_rt_init(p_hwfn, p_params->num_vports, vport_params))
		return -1;

	if (qed_vport_rl_rt_init(p_hwfn, p_params->start_vport,
				 p_params->num_vports, vport_params))
		return -1;

	return 0;
}

int qed_init_pf_wfq(struct qed_hwfn *p_hwfn,
		    struct qed_ptt *p_ptt, u8 pf_id, u16 pf_wfq)
{
	u32 inc_val = QM_WFQ_INC_VAL(pf_wfq);

	if (!inc_val || inc_val > QM_WFQ_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid PF WFQ weight configuration\n");
		return -1;
	}

	qed_wr(p_hwfn, p_ptt, QM_REG_WFQPFWEIGHT + pf_id * 4, inc_val);
	return 0;
}

int qed_init_pf_rl(struct qed_hwfn *p_hwfn,
		   struct qed_ptt *p_ptt, u8 pf_id, u32 pf_rl)
{
	u32 inc_val = QM_RL_INC_VAL(pf_rl);

	if (inc_val > QM_RL_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid PF rate limit configuration\n");
		return -1;
	}

	qed_wr(p_hwfn, p_ptt,
	       QM_REG_RLPFCRD + pf_id * 4,
	       QM_RL_CRD_REG_SIGN_BIT);
	qed_wr(p_hwfn, p_ptt, QM_REG_RLPFINCVAL + pf_id * 4, inc_val);

	return 0;
}

int qed_init_vport_wfq(struct qed_hwfn *p_hwfn,
		       struct qed_ptt *p_ptt,
		       u16 first_tx_pq_id[NUM_OF_TCS], u16 vport_wfq)
{
	u16 vport_pq_id;
	u32 inc_val;
	u8 tc;

	inc_val = QM_WFQ_INC_VAL(vport_wfq);
	if (!inc_val || inc_val > QM_WFQ_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid VPORT WFQ weight configuration\n");
		return -1;
	}

	for (tc = 0; tc < NUM_OF_TCS; tc++) {
		vport_pq_id = first_tx_pq_id[tc];
		if (vport_pq_id != QM_INVALID_PQ_ID)
			qed_wr(p_hwfn, p_ptt,
			       QM_REG_WFQVPWEIGHT + vport_pq_id * 4,
			       inc_val);
	}

	return 0;
}

int qed_init_vport_rl(struct qed_hwfn *p_hwfn,
		      struct qed_ptt *p_ptt, u8 vport_id, u32 vport_rl)
{
	u32 inc_val = QM_RL_INC_VAL(vport_rl);

	if (vport_id >= MAX_QM_GLOBAL_RLS) {
		DP_NOTICE(p_hwfn,
			  "Invalid VPORT ID for rate limiter configuration\n");
		return -1;
	}

	if (inc_val > QM_RL_MAX_INC_VAL) {
		DP_NOTICE(p_hwfn, "Invalid VPORT rate-limit configuration\n");
		return -1;
	}

	qed_wr(p_hwfn, p_ptt,
	       QM_REG_RLGLBLCRD + vport_id * 4,
	       QM_RL_CRD_REG_SIGN_BIT);
	qed_wr(p_hwfn, p_ptt, QM_REG_RLGLBLINCVAL + vport_id * 4, inc_val);

	return 0;
}

bool qed_send_qm_stop_cmd(struct qed_hwfn *p_hwfn,
			  struct qed_ptt *p_ptt,
			  bool is_release_cmd,
			  bool is_tx_pq, u16 start_pq, u16 num_pqs)
{
	u32 cmd_arr[QM_CMD_STRUCT_SIZE(QM_STOP_CMD)] = { 0 };
	u32 pq_mask = 0, last_pq = start_pq + num_pqs - 1, pq_id;

	/* Set command's PQ type */
	QM_CMD_SET_FIELD(cmd_arr, QM_STOP_CMD, PQ_TYPE, is_tx_pq ? 0 : 1);

	for (pq_id = start_pq; pq_id <= last_pq; pq_id++) {
		/* Set PQ bit in mask (stop command only) */
		if (!is_release_cmd)
			pq_mask |= (1 << (pq_id % QM_STOP_PQ_MASK_WIDTH));

		/* If last PQ or end of PQ mask, write command */
		if ((pq_id == last_pq) ||
		    (pq_id % QM_STOP_PQ_MASK_WIDTH ==
		     (QM_STOP_PQ_MASK_WIDTH - 1))) {
			QM_CMD_SET_FIELD(cmd_arr, QM_STOP_CMD,
					 PAUSE_MASK, pq_mask);
			QM_CMD_SET_FIELD(cmd_arr, QM_STOP_CMD,
					 GROUP_ID,
					 pq_id / QM_STOP_PQ_MASK_WIDTH);
			if (!qed_send_qm_cmd(p_hwfn, p_ptt, QM_STOP_CMD_ADDR,
					     cmd_arr[0], cmd_arr[1]))
				return false;
			pq_mask = 0;
		}
	}

	return true;
}

static void
qed_set_tunnel_type_enable_bit(unsigned long *var, int bit, bool enable)
{
	if (enable)
		set_bit(bit, var);
	else
		clear_bit(bit, var);
}

#define PRS_ETH_TUNN_FIC_FORMAT	-188897008

void qed_set_vxlan_dest_port(struct qed_hwfn *p_hwfn,
			     struct qed_ptt *p_ptt, u16 dest_port)
{
	qed_wr(p_hwfn, p_ptt, PRS_REG_VXLAN_PORT, dest_port);
	qed_wr(p_hwfn, p_ptt, NIG_REG_VXLAN_CTRL, dest_port);
	qed_wr(p_hwfn, p_ptt, PBF_REG_VXLAN_PORT, dest_port);
}

void qed_set_vxlan_enable(struct qed_hwfn *p_hwfn,
			  struct qed_ptt *p_ptt, bool vxlan_enable)
{
	unsigned long reg_val = 0;
	u8 shift;

	reg_val = qed_rd(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN);
	shift = PRS_REG_ENCAPSULATION_TYPE_EN_VXLAN_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, vxlan_enable);

	qed_wr(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN, reg_val);

	if (reg_val)
		qed_wr(p_hwfn, p_ptt, PRS_REG_OUTPUT_FORMAT_4_0,
		       PRS_ETH_TUNN_FIC_FORMAT);

	reg_val = qed_rd(p_hwfn, p_ptt, NIG_REG_ENC_TYPE_ENABLE);
	shift = NIG_REG_ENC_TYPE_ENABLE_VXLAN_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, vxlan_enable);

	qed_wr(p_hwfn, p_ptt, NIG_REG_ENC_TYPE_ENABLE, reg_val);

	qed_wr(p_hwfn, p_ptt, DORQ_REG_L2_EDPM_TUNNEL_VXLAN_EN,
	       vxlan_enable ? 1 : 0);
}

void qed_set_gre_enable(struct qed_hwfn *p_hwfn, struct qed_ptt *p_ptt,
			bool eth_gre_enable, bool ip_gre_enable)
{
	unsigned long reg_val = 0;
	u8 shift;

	reg_val = qed_rd(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN);
	shift = PRS_REG_ENCAPSULATION_TYPE_EN_ETH_OVER_GRE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, eth_gre_enable);

	shift = PRS_REG_ENCAPSULATION_TYPE_EN_IP_OVER_GRE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, ip_gre_enable);
	qed_wr(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN, reg_val);
	if (reg_val)
		qed_wr(p_hwfn, p_ptt, PRS_REG_OUTPUT_FORMAT_4_0,
		       PRS_ETH_TUNN_FIC_FORMAT);

	reg_val = qed_rd(p_hwfn, p_ptt, NIG_REG_ENC_TYPE_ENABLE);
	shift = NIG_REG_ENC_TYPE_ENABLE_ETH_OVER_GRE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, eth_gre_enable);

	shift = NIG_REG_ENC_TYPE_ENABLE_IP_OVER_GRE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, ip_gre_enable);
	qed_wr(p_hwfn, p_ptt, NIG_REG_ENC_TYPE_ENABLE, reg_val);

	qed_wr(p_hwfn, p_ptt, DORQ_REG_L2_EDPM_TUNNEL_GRE_ETH_EN,
	       eth_gre_enable ? 1 : 0);
	qed_wr(p_hwfn, p_ptt, DORQ_REG_L2_EDPM_TUNNEL_GRE_IP_EN,
	       ip_gre_enable ? 1 : 0);
}

void qed_set_geneve_dest_port(struct qed_hwfn *p_hwfn,
			      struct qed_ptt *p_ptt, u16 dest_port)
{
	qed_wr(p_hwfn, p_ptt, PRS_REG_NGE_PORT, dest_port);
	qed_wr(p_hwfn, p_ptt, NIG_REG_NGE_PORT, dest_port);
	qed_wr(p_hwfn, p_ptt, PBF_REG_NGE_PORT, dest_port);
}

void qed_set_geneve_enable(struct qed_hwfn *p_hwfn,
			   struct qed_ptt *p_ptt,
			   bool eth_geneve_enable, bool ip_geneve_enable)
{
	unsigned long reg_val = 0;
	u8 shift;

	reg_val = qed_rd(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN);
	shift = PRS_REG_ENCAPSULATION_TYPE_EN_ETH_OVER_GENEVE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, eth_geneve_enable);

	shift = PRS_REG_ENCAPSULATION_TYPE_EN_IP_OVER_GENEVE_ENABLE_SHIFT;
	qed_set_tunnel_type_enable_bit(&reg_val, shift, ip_geneve_enable);

	qed_wr(p_hwfn, p_ptt, PRS_REG_ENCAPSULATION_TYPE_EN, reg_val);
	if (reg_val)
		qed_wr(p_hwfn, p_ptt, PRS_REG_OUTPUT_FORMAT_4_0,
		       PRS_ETH_TUNN_FIC_FORMAT);

	qed_wr(p_hwfn, p_ptt, NIG_REG_NGE_ETH_ENABLE,
	       eth_geneve_enable ? 1 : 0);
	qed_wr(p_hwfn, p_ptt, NIG_REG_NGE_IP_ENABLE, ip_geneve_enable ? 1 : 0);

	/* EDPM with geneve tunnel not supported in BB_B0 */
	if (QED_IS_BB_B0(p_hwfn->cdev))
		return;

	qed_wr(p_hwfn, p_ptt, DORQ_REG_L2_EDPM_TUNNEL_NGE_ETH_EN,
	       eth_geneve_enable ? 1 : 0);
	qed_wr(p_hwfn, p_ptt, DORQ_REG_L2_EDPM_TUNNEL_NGE_IP_EN,
	       ip_geneve_enable ? 1 : 0);
}

#define T_ETH_PACKET_ACTION_GFT_EVENTID  23
#define PARSER_ETH_CONN_GFT_ACTION_CM_HDR  272
#define T_ETH_PACKET_MATCH_RFS_EVENTID 25
#define PARSER_ETH_CONN_CM_HDR 0
#define CAM_LINE_SIZE sizeof(u32)
#define RAM_LINE_SIZE sizeof(u64)
#define REG_SIZE sizeof(u32)

void qed_set_rfs_mode_disable(struct qed_hwfn *p_hwfn,
			      struct qed_ptt *p_ptt, u16 pf_id)
{
	u32 hw_addr = PRS_REG_GFT_PROFILE_MASK_RAM +
		      pf_id * RAM_LINE_SIZE;

	/*stop using gft logic */
	qed_wr(p_hwfn, p_ptt, PRS_REG_SEARCH_GFT, 0);
	qed_wr(p_hwfn, p_ptt, PRS_REG_CM_HDR_GFT, 0x0);
	qed_wr(p_hwfn, p_ptt, PRS_REG_GFT_CAM + CAM_LINE_SIZE * pf_id, 0);
	qed_wr(p_hwfn, p_ptt, hw_addr, 0);
	qed_wr(p_hwfn, p_ptt, hw_addr + 4, 0);
}

void qed_set_rfs_mode_enable(struct qed_hwfn *p_hwfn, struct qed_ptt *p_ptt,
			     u16 pf_id, bool tcp, bool udp,
			     bool ipv4, bool ipv6)
{
	union gft_cam_line_union camline;
	struct gft_ram_line ramline;
	u32 rfs_cm_hdr_event_id;

	rfs_cm_hdr_event_id = qed_rd(p_hwfn, p_ptt, PRS_REG_CM_HDR_GFT);

	if (!ipv6 && !ipv4)
		DP_NOTICE(p_hwfn,
			  "set_rfs_mode_enable: must accept at least on of - ipv4 or ipv6");
	if (!tcp && !udp)
		DP_NOTICE(p_hwfn,
			  "set_rfs_mode_enable: must accept at least on of - udp or tcp");

	rfs_cm_hdr_event_id |= T_ETH_PACKET_MATCH_RFS_EVENTID <<
					PRS_REG_CM_HDR_GFT_EVENT_ID_SHIFT;
	rfs_cm_hdr_event_id |= PARSER_ETH_CONN_CM_HDR <<
					PRS_REG_CM_HDR_GFT_CM_HDR_SHIFT;
	qed_wr(p_hwfn, p_ptt, PRS_REG_CM_HDR_GFT, rfs_cm_hdr_event_id);

	/* Configure Registers for RFS mode */
	qed_wr(p_hwfn, p_ptt, PRS_REG_SEARCH_GFT, 1);
	qed_wr(p_hwfn, p_ptt, PRS_REG_LOAD_L2_FILTER, 0);
	camline.cam_line_mapped.camline = 0;

	/* Cam line is now valid!! */
	SET_FIELD(camline.cam_line_mapped.camline,
		  GFT_CAM_LINE_MAPPED_VALID, 1);

	/* filters are per PF!! */
	SET_FIELD(camline.cam_line_mapped.camline,
		  GFT_CAM_LINE_MAPPED_PF_ID_MASK,
		  GFT_CAM_LINE_MAPPED_PF_ID_MASK_MASK);
	SET_FIELD(camline.cam_line_mapped.camline,
		  GFT_CAM_LINE_MAPPED_PF_ID, pf_id);
	if (!(tcp && udp)) {
		SET_FIELD(camline.cam_line_mapped.camline,
			  GFT_CAM_LINE_MAPPED_UPPER_PROTOCOL_TYPE_MASK,
			  GFT_CAM_LINE_MAPPED_UPPER_PROTOCOL_TYPE_MASK_MASK);
		if (tcp)
			SET_FIELD(camline.cam_line_mapped.camline,
				  GFT_CAM_LINE_MAPPED_UPPER_PROTOCOL_TYPE,
				  GFT_PROFILE_TCP_PROTOCOL);
		else
			SET_FIELD(camline.cam_line_mapped.camline,
				  GFT_CAM_LINE_MAPPED_UPPER_PROTOCOL_TYPE,
				  GFT_PROFILE_UDP_PROTOCOL);
	}

	if (!(ipv4 && ipv6)) {
		SET_FIELD(camline.cam_line_mapped.camline,
			  GFT_CAM_LINE_MAPPED_IP_VERSION_MASK, 1);
		if (ipv4)
			SET_FIELD(camline.cam_line_mapped.camline,
				  GFT_CAM_LINE_MAPPED_IP_VERSION,
				  GFT_PROFILE_IPV4);
		else
			SET_FIELD(camline.cam_line_mapped.camline,
				  GFT_CAM_LINE_MAPPED_IP_VERSION,
				  GFT_PROFILE_IPV6);
	}

	/* Write characteristics to cam */
	qed_wr(p_hwfn, p_ptt, PRS_REG_GFT_CAM + CAM_LINE_SIZE * pf_id,
	       camline.cam_line_mapped.camline);
	camline.cam_line_mapped.camline = qed_rd(p_hwfn, p_ptt,
						 PRS_REG_GFT_CAM +
						 CAM_LINE_SIZE * pf_id);

	/* Write line to RAM - compare to filter 4 tuple */
	ramline.lo = 0;
	ramline.hi = 0;
	SET_FIELD(ramline.hi, GFT_RAM_LINE_DST_IP, 1);
	SET_FIELD(ramline.hi, GFT_RAM_LINE_SRC_IP, 1);
	SET_FIELD(ramline.hi, GFT_RAM_LINE_OVER_IP_PROTOCOL, 1);
	SET_FIELD(ramline.lo, GFT_RAM_LINE_ETHERTYPE, 1);
	SET_FIELD(ramline.lo, GFT_RAM_LINE_SRC_PORT, 1);
	SET_FIELD(ramline.lo, GFT_RAM_LINE_DST_PORT, 1);

	/* Each iteration write to reg */
	qed_wr(p_hwfn, p_ptt,
	       PRS_REG_GFT_PROFILE_MASK_RAM + RAM_LINE_SIZE * pf_id,
	       ramline.lo);
	qed_wr(p_hwfn, p_ptt,
	       PRS_REG_GFT_PROFILE_MASK_RAM + RAM_LINE_SIZE * pf_id + 4,
	       ramline.hi);

	/* Set default profile so that no filter match will happen */
	qed_wr(p_hwfn, p_ptt,
	       PRS_REG_GFT_PROFILE_MASK_RAM +
	       RAM_LINE_SIZE * PRS_GFT_CAM_LINES_NO_MATCH,
	       ramline.lo);
	qed_wr(p_hwfn, p_ptt,
	       PRS_REG_GFT_PROFILE_MASK_RAM +
	       RAM_LINE_SIZE * PRS_GFT_CAM_LINES_NO_MATCH + 4,
	       ramline.hi);
}
