// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2013 - 2021 Intel Corporation. */

#include "i40e.h"

/*********************notification routines***********************/

/**
 * i40e_vc_vf_broadcast
 * @pf: pointer to the PF structure
 * @v_opcode: operation code
 * @v_retval: return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * send a message to all VFs on a given PF
 **/
static void i40e_vc_vf_broadcast(struct i40e_pf *pf,
				 enum virtchnl_ops v_opcode,
				 i40e_status v_retval, u8 *msg,
				 u16 msglen)
{
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf = pf->vf;
	int i;

	for (i = 0; i < pf->num_alloc_vfs; i++, vf++) {
		int abs_vf_id = vf->vf_id + (int)hw->func_caps.vf_base_id;
		/* Not all vfs are enabled so skip the ones that are not */
		if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states) &&
		    !test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
			continue;

		/* Ignore return value on purpose - a given VF may fail, but
		 * we need to keep going and send to all of them
		 */
		i40e_aq_send_msg_to_vf(hw, abs_vf_id, v_opcode, v_retval,
				       msg, msglen, NULL);
	}
}

/**
 * i40e_vc_link_speed2mbps - Convert AdminQ link_speed bit represented
 * to integer value of Mbps
 * @link_speed: the speed to convert
 *
 * Returns the speed as direct value of Mbps.
 **/
static INLINE u32
i40e_vc_link_speed2mbps(enum i40e_aq_link_speed link_speed)
{
	switch (link_speed) {
	case I40E_LINK_SPEED_100MB:
		return SPEED_100;
	case I40E_LINK_SPEED_1GB:
		return SPEED_1000;
	case I40E_LINK_SPEED_2_5GB:
		return SPEED_2500;
	case I40E_LINK_SPEED_5GB:
		return SPEED_5000;
	case I40E_LINK_SPEED_10GB:
		return SPEED_10000;
	case I40E_LINK_SPEED_20GB:
		return SPEED_20000;
	case I40E_LINK_SPEED_25GB:
		return SPEED_25000;
	case I40E_LINK_SPEED_40GB:
		return SPEED_40000;
	case I40E_LINK_SPEED_UNKNOWN:
		return SPEED_UNKNOWN;
	}
	return SPEED_UNKNOWN;
}

/**
 * i40e_set_vf_link_state
 * @vf: pointer to the VF structure
 * @pfe: pointer to PF event structure
 * @ls: pointer to link status structure
 *
 * set a link state on a single vf
 **/
static void
i40e_set_vf_link_state(struct i40e_vf *vf,
		       struct virtchnl_pf_event *pfe,
		       struct i40e_link_status *ls)
{
	u8 link_status = ls->link_info & I40E_AQ_LINK_UP;

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	if (vf->link_forced)
		link_status = vf->link_up;
#endif

	if (vf->driver_caps & VIRTCHNL_VF_CAP_ADV_LINK_SPEED) {
		pfe->event_data.link_event_adv.link_speed =
			link_status ? i40e_vc_link_speed2mbps(ls->link_speed) :
				0;
		pfe->event_data.link_event_adv.link_status = link_status;
	} else {
		pfe->event_data.link_event.link_speed =
			link_status ?
				i40e_virtchnl_link_speed(ls->link_speed) :
				VIRTCHNL_LINK_SPEED_UNKNOWN;
		pfe->event_data.link_event.link_status = link_status;
	}
}

/**
 * i40e_vc_notify_vf_link_state
 * @vf: pointer to the VF structure
 *
 * send a link status message to a single VF
 **/
static void i40e_vc_notify_vf_link_state(struct i40e_vf *vf)
{
	struct virtchnl_pf_event pfe;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_link_status *ls = &pf->hw.phy.link_info;
	int abs_vf_id = vf->vf_id + (int)hw->func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;

	i40e_set_vf_link_state(vf, &pfe, ls);

	i40e_aq_send_msg_to_vf(hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       I40E_SUCCESS, (u8 *)&pfe, sizeof(pfe), NULL);
}

/**
 * i40e_vc_notify_link_state
 * @pf: pointer to the PF structure
 *
 * send a link status message to all VFs on a given PF
 **/
void i40e_vc_notify_link_state(struct i40e_pf *pf)
{
	int i;

	for (i = 0; i < pf->num_alloc_vfs; i++)
		i40e_vc_notify_vf_link_state(&pf->vf[i]);
}

/**
 * i40e_vc_notify_reset
 * @pf: pointer to the PF structure
 *
 * indicate a pending reset to all VFs on a given PF
 **/
void i40e_vc_notify_reset(struct i40e_pf *pf)
{
	struct virtchnl_pf_event pfe;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	i40e_vc_vf_broadcast(pf, VIRTCHNL_OP_EVENT, I40E_SUCCESS,
			     (u8 *)&pfe, sizeof(struct virtchnl_pf_event));
}

/**
 * i40e_restore_all_vfs_msi_state - restore VF MSI state after PF FLR
 * @pdev: pointer to a pci_dev structure
 *
 * Called when recovering from a PF FLR to restore interrupt capability to
 * the VFs.
 */
void i40e_restore_all_vfs_msi_state(struct pci_dev *pdev)
{
	struct pci_dev *vfdev;
	u16 vf_id;
	int pos;

	/* Continue only if this is a PF */
	if (!pdev->is_physfn)
		return;

	if (!pci_num_vf(pdev))
		return;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_SRIOV);
	if (pos) {
		pci_read_config_word(pdev, pos + PCI_SRIOV_VF_DID, &vf_id);
		vfdev = pci_get_device(pdev->vendor, vf_id, NULL);
		while (vfdev) {
			if (vfdev->is_virtfn && vfdev->physfn == pdev)
				pci_restore_msi_state(vfdev);
			vfdev = pci_get_device(pdev->vendor, vf_id, vfdev);
		}
	}
}

/**
 * i40e_vc_notify_vf_reset
 * @vf: pointer to the VF structure
 *
 * indicate a pending reset to the given VF
 **/
void i40e_vc_notify_vf_reset(struct i40e_vf *vf)
{
	struct virtchnl_pf_event pfe;
	int abs_vf_id;

	/* validate the request */
	if (!vf || vf->vf_id >= vf->pf->num_alloc_vfs)
		return;

	/* verify if the VF is in either init or active before proceeding */
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states) &&
	    !test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
		return;

	if (ktime_get_ns() - vf->reset_timestamp < I40E_VF_RESET_TIME_MIN)
		usleep_range(30000, 60000);

	abs_vf_id = vf->vf_id + (int)vf->pf->hw.func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	i40e_aq_send_msg_to_vf(&vf->pf->hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       I40E_SUCCESS, (u8 *)&pfe,
			       sizeof(struct virtchnl_pf_event), NULL);
}
/***********************misc routines*****************************/

/**
 * i40e_vc_reset_vf
 * @vf: pointer to the VF info
 * @notify_vf: notify vf about reset or not
 *
 * Reset VF handler.
 **/
static inline void i40e_vc_reset_vf(struct i40e_vf *vf, bool notify_vf)
{
	struct i40e_pf *pf = vf->pf;
	int i;

	if (notify_vf)
		i40e_vc_notify_vf_reset(vf);

	/* We want to ensure that an actual reset occurs initiated after this
	 * function was called. However, we do not want to wait forever, so
	 * we'll give a reasonable time and print a message if we failed to
	 * ensure a reset.
	 */
	for (i = 0; i < 20; i++) {
		/* If pf is in vfs releasing state reset vf is impossible,
		 * so leave it.
		 */
		if (test_bit(__I40E_VFS_RELEASING, pf->state))
			return;

		if (i40e_reset_vf(vf, false))
			return;

		usleep_range(10000, 20000);
	}

	if (notify_vf)
		dev_warn(&vf->pf->pdev->dev,
			 "Failed to initiate reset for VF %d after 200 milliseconds\n",
			 vf->vf_id);
	else
		dev_dbg(&vf->pf->pdev->dev,
			"Failed to initiate reset for VF %d after 200 milliseconds\n",
			vf->vf_id);
}

/**
 * i40e_vc_isvalid_vsi_id
 * @vf: pointer to the VF info
 * @vsi_id: VF relative VSI id
 *
 * check for the valid VSI id
 **/
static inline bool i40e_vc_isvalid_vsi_id(struct i40e_vf *vf, u16 vsi_id)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);

	return (vsi && (vsi->vf_id == vf->vf_id));
}

/**
 * i40e_vc_isvalid_queue_id
 * @vf: pointer to the VF info
 * @vsi_id: vsi id
 * @qid: vsi relative queue id
 *
 * check for the valid queue id
 **/
static inline bool i40e_vc_isvalid_queue_id(struct i40e_vf *vf, u16 vsi_id,
					    u16 qid)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);

	return (vsi && (qid < vsi->alloc_queue_pairs));
}

/**
 * i40e_vc_isvalid_vector_id
 * @vf: pointer to the VF info
 * @vector_id: VF relative vector id
 *
 * check for the valid vector id
 **/
static inline bool i40e_vc_isvalid_vector_id(struct i40e_vf *vf, u32 vector_id)
{
	struct i40e_pf *pf = vf->pf;

	return vector_id < pf->hw.func_caps.num_msix_vectors_vf;
}

/***********************vf resource mgmt routines*****************/

/**
 * i40e_vc_get_pf_queue_id
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as provided by the FW
 * @vsi_queue_id: vsi relative queue id
 *
 * return PF relative queue id
 **/
static u16 i40e_vc_get_pf_queue_id(struct i40e_vf *vf, u16 vsi_id,
				   u8 vsi_queue_id)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);
	u16 pf_queue_id = I40E_QUEUE_END_OF_LIST;

	if (!vsi)
		return pf_queue_id;

	if (le16_to_cpu(vsi->info.mapping_flags) &
	    I40E_AQ_VSI_QUE_MAP_NONCONTIG)
		pf_queue_id =
			le16_to_cpu(vsi->info.queue_mapping[vsi_queue_id]);
	else
		pf_queue_id = le16_to_cpu(vsi->info.queue_mapping[0]) +
			      vsi_queue_id;

	return pf_queue_id;
}

/**
 * i40e_get_real_pf_qid
 * @vf: pointer to the VF info
 * @vsi_id: vsi id
 * @queue_id: queue number
 *
 * wrapper function to get pf_queue_id handling ADq code as well
 **/
static u16 i40e_get_real_pf_qid(struct i40e_vf *vf, u16 vsi_id, u16 queue_id)
{
	int i;

	if (vf->adq_enabled) {
		/* Although VF considers all the queues(can be 1 to 16) as its
		 * own but they may actually belong to different VSIs(up to 4).
		 * We need to find which queues belongs to which VSI.
		 */
		for (i = 0; i < vf->num_tc; i++) {
			if (queue_id < vf->ch[i].num_qps) {
				vsi_id = vf->ch[i].vsi_id;
				break;
			}
			/* find right queue id which is relative to a
			 * given VSI.
			 */
			queue_id -= vf->ch[i].num_qps;
		}
	}

	return i40e_vc_get_pf_queue_id(vf, vsi_id, queue_id);
}

/**
 * i40e_config_irq_link_list
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as given by the FW
 * @vecmap: irq map info
 *
 * configure irq link list from the map
 **/
static void i40e_config_irq_link_list(struct i40e_vf *vf, u16 vsi_id,
				      struct virtchnl_vector_map *vecmap)
{
	unsigned long linklistmap = 0, tempmap;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u16 vsi_queue_id, pf_queue_id;
	enum i40e_queue_type qtype;
	u16 next_q, vector_id, size;
	u32 reg, reg_idx;
	u16 itr_idx = 0;

	vector_id = vecmap->vector_id;
	/* setup the head */
	if (0 == vector_id)
		reg_idx = I40E_VPINT_LNKLST0(vf->vf_id);
	else
		reg_idx = I40E_VPINT_LNKLSTN(
		     ((pf->hw.func_caps.num_msix_vectors_vf - 1) * vf->vf_id) +
		     (vector_id - 1));

	if (vecmap->rxq_map == 0 && vecmap->txq_map == 0) {
		/* Special case - No queues mapped on this vector */
		wr32(hw, reg_idx, I40E_VPINT_LNKLST0_FIRSTQ_INDX_MASK);
		goto irq_list_done;
	}
	tempmap = vecmap->rxq_map;
	for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
		linklistmap |= (BIT(I40E_VIRTCHNL_SUPPORTED_QTYPES *
				    vsi_queue_id));
	}

	tempmap = vecmap->txq_map;
	for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
		linklistmap |= (BIT(I40E_VIRTCHNL_SUPPORTED_QTYPES *
				     vsi_queue_id + 1));
	}

	size = I40E_MAX_VSI_QP * I40E_VIRTCHNL_SUPPORTED_QTYPES;
	next_q = find_first_bit(&linklistmap, size);
	if (unlikely(next_q == size))
		goto irq_list_done;

	vsi_queue_id = next_q / I40E_VIRTCHNL_SUPPORTED_QTYPES;
	qtype = next_q % I40E_VIRTCHNL_SUPPORTED_QTYPES;
	pf_queue_id = i40e_get_real_pf_qid(vf, vsi_id, vsi_queue_id);
	reg = ((qtype << I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_SHIFT) | pf_queue_id);

	wr32(hw, reg_idx, reg);

	while (next_q < size) {
		switch (qtype) {
		case I40E_QUEUE_TYPE_RX:
			reg_idx = I40E_QINT_RQCTL(pf_queue_id);
			itr_idx = vecmap->rxitr_idx;
			break;
		case I40E_QUEUE_TYPE_TX:
			reg_idx = I40E_QINT_TQCTL(pf_queue_id);
			itr_idx = vecmap->txitr_idx;
			break;
		default:
			break;
		}

		next_q = find_next_bit(&linklistmap, size, next_q + 1);
		if (next_q < size) {
			vsi_queue_id = next_q / I40E_VIRTCHNL_SUPPORTED_QTYPES;
			qtype = next_q % I40E_VIRTCHNL_SUPPORTED_QTYPES;
			pf_queue_id = i40e_get_real_pf_qid(vf,
							   vsi_id,
							   vsi_queue_id);
		} else {
			pf_queue_id = I40E_QUEUE_END_OF_LIST;
			qtype = I40E_QUEUE_TYPE_RX;
		}

		/* format for the RQCTL & TQCTL regs is same */
		reg = (vector_id) |
		    (qtype << I40E_QINT_RQCTL_NEXTQ_TYPE_SHIFT) |
		    (pf_queue_id << I40E_QINT_RQCTL_NEXTQ_INDX_SHIFT) |
		    BIT(I40E_QINT_RQCTL_CAUSE_ENA_SHIFT) |
		    (itr_idx << I40E_QINT_RQCTL_ITR_INDX_SHIFT);
		wr32(hw, reg_idx, reg);
	}

	/* if the vf is running in polling mode and using interrupt zero,
	 * need to disable auto-mask on enabling zero interrupt for VFs.
	 */
	if ((vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RX_POLLING) &&
	    (vector_id == 0)) {
		reg = rd32(hw, I40E_GLINT_CTL);
		if (!(reg & I40E_GLINT_CTL_DIS_AUTOMASK_VF0_MASK)) {
			reg |= I40E_GLINT_CTL_DIS_AUTOMASK_VF0_MASK;
			wr32(hw, I40E_GLINT_CTL, reg);
		}
	}

irq_list_done:
	i40e_flush(hw);
}

/**
 * i40e_config_vsi_tx_queue
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as provided by the FW
 * @vsi_queue_id: vsi relative queue index
 * @info: config. info
 *
 * configure tx queue
 **/
static int i40e_config_vsi_tx_queue(struct i40e_vf *vf, u16 vsi_id,
				    u16 vsi_queue_id,
				    struct virtchnl_txq_info *info)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_hmc_obj_txq tx_ctx;
	struct i40e_vsi *vsi;
	u16 pf_queue_id;
	int ret = 0, i;
	u32 qtx_ctl;

	if (!i40e_vc_isvalid_vsi_id(vf, info->vsi_id)) {
		ret = -ENOENT;
		goto error_context;
	}
	pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id, vsi_queue_id);
	vsi = i40e_find_vsi_from_id(pf, vsi_id);
	if (!vsi) {
		ret = -ENOENT;
		goto error_context;
	}

	/* clear the context structure first */
	memset(&tx_ctx, 0, sizeof(struct i40e_hmc_obj_txq));

	/* only set the required fields */
	tx_ctx.base = info->dma_ring_addr / 128;
	tx_ctx.qlen = info->ring_len;

	if (vsi->tc_config.enabled_tc == 1) {
		tx_ctx.rdylist = le16_to_cpu(vsi->info.qs_handle[0]);
	} else {
		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
			/* If queue is assigned to this TC */
			if (vsi->tc_config.tc_info[i].qoffset <=
			    vsi_queue_id && vsi_queue_id <
			    vsi->tc_config.tc_info[i].qoffset +
			    vsi->tc_config.tc_info[i].qcount)
				break;
		}

		/* If queue was somehow assigned to nonextisting queue set,
		 * or queue did not find it's TC, assign it to queue set 0
		 */
		if (i >= I40E_MAX_TRAFFIC_CLASS ||
		    le16_to_cpu(vsi->info.qs_handle[i]) ==
		    I40E_AQ_VSI_QS_HANDLE_INVALID)
			tx_ctx.rdylist = le16_to_cpu(vsi->info.qs_handle[0]);
		else
			tx_ctx.rdylist = le16_to_cpu(vsi->info.qs_handle[i]);
	}

	tx_ctx.rdylist_act = 0;
	tx_ctx.head_wb_ena = info->headwb_enabled;
	tx_ctx.head_wb_addr = info->dma_headwb_addr;

	/* clear the context in the HMC */
	ret = i40e_clear_lan_tx_queue_context(hw, pf_queue_id);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to clear VF LAN Tx queue context %d, error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_context;
	}

	/* set the context in the HMC */
	ret = i40e_set_lan_tx_queue_context(hw, pf_queue_id, &tx_ctx);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to set VF LAN Tx queue context %d error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_context;
	}

	/* associate this queue with the PCI VF function */
	qtx_ctl = I40E_QTX_CTL_VF_QUEUE;
	qtx_ctl |= ((hw->pf_id << I40E_QTX_CTL_PF_INDX_SHIFT)
		    & I40E_QTX_CTL_PF_INDX_MASK);
	qtx_ctl |= (((vf->vf_id + hw->func_caps.vf_base_id)
		     << I40E_QTX_CTL_VFVM_INDX_SHIFT)
		    & I40E_QTX_CTL_VFVM_INDX_MASK);
	wr32(hw, I40E_QTX_CTL(pf_queue_id), qtx_ctl);
	i40e_flush(hw);

error_context:
	return ret;
}

/**
 * i40e_config_vsi_rx_queue
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI  as provided by the FW
 * @vsi_queue_id: vsi relative queue index
 * @info: config. info
 *
 * configure rx queue
 **/
static int i40e_config_vsi_rx_queue(struct i40e_vf *vf, u16 vsi_id,
				    u16 vsi_queue_id,
				    struct virtchnl_rxq_info *info)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_hmc_obj_rxq rx_ctx;
	struct i40e_vsi *vsi = pf->vsi[vf->lan_vsi_idx];
	u16 pf_queue_id;
	int ret = 0;

	pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id, vsi_queue_id);

	/* clear the context structure first */
	memset(&rx_ctx, 0, sizeof(struct i40e_hmc_obj_rxq));

	/* only set the required fields */
	rx_ctx.base = info->dma_ring_addr / 128;
	rx_ctx.qlen = info->ring_len;

	if (info->splithdr_enabled) {
		rx_ctx.hsplit_0 = I40E_RX_SPLIT_L2      |
				  I40E_RX_SPLIT_IP      |
				  I40E_RX_SPLIT_TCP_UDP |
				  I40E_RX_SPLIT_SCTP;
		/* header length validation */
		if (info->hdr_size > ((2 * 1024) - 64)) {
			ret = -EINVAL;
			goto error_param;
		}
		rx_ctx.hbuff = info->hdr_size >> I40E_RXQ_CTX_HBUFF_SHIFT;

		/* set splitalways mode 10b */
		rx_ctx.dtype = I40E_RX_DTYPE_HEADER_SPLIT;
	}

	/* databuffer length validation */
	if (info->databuffer_size > ((16 * 1024) - 128)) {
		ret = -EINVAL;
		goto error_param;
	}
	rx_ctx.dbuff = info->databuffer_size >> I40E_RXQ_CTX_DBUFF_SHIFT;

	/* max pkt. length validation */
	if (info->max_pkt_size >= (16 * 1024) || info->max_pkt_size < 64) {
		ret = -EINVAL;
		goto error_param;
	}
	rx_ctx.rxmax = info->max_pkt_size;

	/* if port/outer VLAN is configured increase the max packet size */
	if (i40e_is_vid(&vsi->info))
		rx_ctx.rxmax += VLAN_HLEN;

	/* enable 32bytes desc always */
	rx_ctx.dsize = 1;

	/* default values */
	rx_ctx.lrxqthresh = 1;
	rx_ctx.crcstrip = 1;
	rx_ctx.prefena = 1;
	rx_ctx.l2tsel = 1;

	/* clear the context in the HMC */
	ret = i40e_clear_lan_rx_queue_context(hw, pf_queue_id);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to clear VF LAN Rx queue context %d, error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_param;
	}

	/* set the context in the HMC */
	ret = i40e_set_lan_rx_queue_context(hw, pf_queue_id, &rx_ctx);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to set VF LAN Rx queue context %d error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_param;
	}

error_param:
	return ret;
}

/**
 * i40e_validate_vf
 * @pf: the physical function
 * @vf_id: VF identifier
 *
 * Check that the VF is enabled and the vsi exists.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_validate_vf(struct i40e_pf *pf, int vf_id)
{
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev,
			"Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto err_out;
	}
	vf = &pf->vf[vf_id];
	vsi = i40e_find_vsi_from_id(pf, vf->lan_vsi_id);
	if (!vsi)
		ret = -EINVAL;
err_out:
	return ret;
}

#ifdef HAVE_NDO_SET_VF_LINK_STATE

/**
 * i40e_set_spoof_settings
 * @vsi: VF VSI to configure
 * @sec_flag: the spoof check flag to enable or disable
 * @enable: enable or disable
 *
 * This function sets the spoof check settings
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_spoof_settings(struct i40e_vsi *vsi, u8 sec_flag,
				   bool enable)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vsi_context ctxt;
	int ret = 0;

	vsi->info.valid_sections = CPU_TO_LE16(I40E_AQ_VSI_PROP_SECURITY_VALID);
	if (enable)
		vsi->info.sec_flags |= sec_flag;
	else
		vsi->info.sec_flags &= ~sec_flag;

	memset(&ctxt, 0, sizeof(ctxt));
	ctxt.seid = vsi->seid;
	ctxt.pf_num = vsi->back->hw.pf_id;
	ctxt.info = vsi->info;
	ret = i40e_aq_update_vsi_params(hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d updating VSI parameters\n",
			ret);
		ret = -EIO;
	}
	return ret;
}

/**
 * i40e_configure_vf_loopback
 * @vsi: VF VSI to configure
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function configures the VF VSI with the loopback settings
 *
 * Returns 0 on success, negative on failure
 *
 **/
static int i40e_configure_vf_loopback(struct i40e_vsi *vsi, int vf_id,
				      bool enable)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_vsi_context ctxt;
	int ret = 0;

	vsi->info.valid_sections = CPU_TO_LE16(I40E_AQ_VSI_PROP_SWITCH_VALID);
	if (enable)
		vsi->info.switch_id |=
				CPU_TO_LE16(I40E_AQ_VSI_SW_ID_FLAG_ALLOW_LB);
	else
		vsi->info.switch_id &=
				~CPU_TO_LE16(I40E_AQ_VSI_SW_ID_FLAG_ALLOW_LB);

	memset(&ctxt, 0, sizeof(ctxt));
	ctxt.seid = vsi->seid;
	ctxt.pf_num = vsi->back->hw.pf_id;
	ctxt.info = vsi->info;
	ret = i40e_aq_update_vsi_params(&pf->hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d configuring loopback for VF %d\n",
			ret, vf_id);
		ret = -EIO;
	}
	return ret;
}

/**
 * i40e_configure_vf_outer_vlan_stripping
 * @vsi: VF VSI to configure
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function enables or disables outer vlan stripping on the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_configure_vf_outer_vlan_stripping(struct i40e_vsi *vsi,
						  int vf_id,
						  bool enable)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_vsi_context ctxt;
	int ret = 0;
	u8 flag;

	vsi->info.valid_sections = cpu_to_le16(I40E_AQ_VSI_PROP_VLAN_VALID);
	if (enable) {
		/* Don't enable vlan stripping if outer vlan is set */
		if (vsi->info.outer_vlan) {
			dev_err(&pf->pdev->dev,
				"Cannot enable vlan stripping when port VLAN is set\n");
			ret = -EINVAL;
			goto err_out;
		}
		flag = I40E_AQ_VSI_OVLAN_EMOD_SHOW_ALL;
	} else {
		flag = I40E_AQ_VSI_OVLAN_EMOD_NOTHING;
	}
	vsi->info.outer_vlan_flags = I40E_AQ_VSI_OVLAN_MODE_ALL |
		(flag << I40E_AQ_VSI_OVLAN_EMOD_SHIFT) |
		(I40E_AQ_VSI_OVLAN_CTRL_ENA << I40E_AQ_VSI_OVLAN_EMOD_SHIFT);
	ctxt.seid = vsi->seid;
	ctxt.info = vsi->info;
	ret = i40e_aq_update_vsi_params(&pf->hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d configuring vlan stripping for VF %d\n",
			ret, vf_id);
		ret = -EIO;
	}
err_out:
	return ret;
}

/**
 * i40e_configure_vf_vlan_stripping
 * @vsi: VF VSI to configure
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function enables or disables vlan stripping on the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_configure_vf_vlan_stripping(struct i40e_vsi *vsi, int vf_id,
					    bool enable)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_vsi_context ctxt;
	int ret = 0;
	u8 flag;

	if (i40e_is_double_vlan(&pf->hw))
		return i40e_configure_vf_outer_vlan_stripping(vsi, vf_id,
							      enable);

	vsi->info.valid_sections = cpu_to_le16(I40E_AQ_VSI_PROP_VLAN_VALID);
	if (enable) {
		/* Don't enable vlan stripping if port vlan is set */
		if (vsi->info.pvid) {
			dev_err(&pf->pdev->dev,
				"Cannot enable vlan stripping when port VLAN is set\n");
			ret = -EINVAL;
			goto err_out;
		}
		flag = I40E_AQ_VSI_PVLAN_EMOD_STR_BOTH;
	} else {
		flag = I40E_AQ_VSI_PVLAN_EMOD_NOTHING;
	}
	vsi->info.port_vlan_flags = I40E_AQ_VSI_PVLAN_MODE_ALL | flag;
	ctxt.seid = vsi->seid;
	ctxt.info = vsi->info;
	ret = i40e_aq_update_vsi_params(&pf->hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d configuring vlan stripping for VF %d\n",
			ret, vf_id);
		ret = -EIO;
	}
err_out:
	return ret;
}

/**
 * i40e_configure_vf_promisc_mode
 * @vf: VF
 * @vsi: VF VSI to configure
 * @promisc_mode: promisc mode to configure
 *
 * This function configures the requested promisc mode for a vf
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_configure_vf_promisc_mode(struct i40e_vf *vf,
					  struct i40e_vsi *vsi,
					  u8 promisc_mode)
{
	struct i40e_pf *pf = vsi->back;
	int ret = 0;

	if (promisc_mode & VFD_PROMISC_MULTICAST) {
		ret = i40e_aq_set_vsi_multicast_promiscuous(&pf->hw, vsi->seid,
							    true, NULL);
		if (ret)
			goto err;
		vf->promisc_mode |= VFD_PROMISC_MULTICAST;
	} else {
		ret = i40e_aq_set_vsi_multicast_promiscuous(&pf->hw, vsi->seid,
							    false, NULL);
		if (ret)
			goto err;
		vf->promisc_mode &= ~VFD_PROMISC_MULTICAST;
	}
	if (promisc_mode & VFD_PROMISC_UNICAST) {
		ret = i40e_aq_set_vsi_unicast_promiscuous(&pf->hw, vsi->seid,
							  true, NULL, true);
		if (ret)
			goto err;
		vf->promisc_mode |= VFD_PROMISC_UNICAST;
	} else {
		ret = i40e_aq_set_vsi_unicast_promiscuous(&pf->hw, vsi->seid,
							  false, NULL, true);
		if (ret)
			goto err;
		vf->promisc_mode &= ~VFD_PROMISC_UNICAST;
	}
err:
	if (ret)
		dev_err(&pf->pdev->dev, "Error %d configuring promisc mode for VF %d\n",
			ret, vf->vf_id);

	return ret;
}

/**
 * i40e_add_ingress_egress_mirror
 * @src_vsi: VSI to mirror from
 * @mirror_vsi: VSI to mirror to
 * @rule_type: rule type to configure
 * @rule_id: rule id to store
 *
 * This function adds the requested ingress/egress mirror for a vsi
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_add_ingress_egress_mirror(struct i40e_vsi *src_vsi,
					  struct i40e_vsi *mirror_vsi,
					  u16 rule_type, u16 *rule_id)
{
	u16 dst_seid, rules_used, rules_free, sw_seid;
	struct i40e_pf *pf = src_vsi->back;
	int ret, num = 0, cnt = 1;
	int *vsi_ingress_vlan;
	int *vsi_egress_vlan;
	__le16 *mr_list;

	mr_list = kcalloc(cnt, sizeof(__le16), GFP_KERNEL);
	if (!mr_list) {
		ret = -ENOMEM;
		goto err_out;
	}

	if (src_vsi->type == I40E_VSI_MAIN) {
		vsi_ingress_vlan = &pf->ingress_vlan;
		vsi_egress_vlan = &pf->egress_vlan;
	} else {
		vsi_ingress_vlan = &pf->vf[src_vsi->vf_id].ingress_vlan;
		vsi_egress_vlan = &pf->vf[src_vsi->vf_id].egress_vlan;
	}

	if (I40E_IS_MIRROR_VLAN_ID_VALID(*vsi_ingress_vlan)) {
		if (src_vsi->type == I40E_VSI_MAIN)
			dev_err(&pf->pdev->dev,
				"PF already has an ingress mirroring configured, only one rule per PF is supported!\n");
		else
			dev_err(&pf->pdev->dev,
				"VF=%d already has an ingress mirroring configured, only one rule per VF is supported!\n",
				src_vsi->vf_id);
		ret = -EPERM;
		goto err_out;
	} else if (I40E_IS_MIRROR_VLAN_ID_VALID(*vsi_egress_vlan)) {
		if (src_vsi->type == I40E_VSI_MAIN)
			dev_err(&pf->pdev->dev,
				"PF already has an egress mirroring configured, only one rule per PF is supported!\n");
		else
			dev_err(&pf->pdev->dev,
				"VF=%d already has an egress mirroring configured, only one rule per VF is supported!\n",
				src_vsi->vf_id);
		ret = -EPERM;
		goto err_out;
	}

	sw_seid = src_vsi->uplink_seid;
	dst_seid = mirror_vsi->seid;
	mr_list[num] = CPU_TO_LE16(src_vsi->seid);
	ret = i40e_aq_add_mirrorrule(&pf->hw, sw_seid,
				     rule_type, dst_seid,
				     cnt, mr_list, NULL,
				     rule_id, &rules_used,
				     &rules_free);
	kfree(mr_list);
err_out:
	return ret;
}

/**
 * i40e_del_ingress_egress_mirror
 * @src_vsi: the mirrored VSI
 * @rule_type: rule type to configure
 * @rule_id : rule id to delete
 *
 * This function deletes the ingress/egress mirror on a VSI
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_del_ingress_egress_mirror(struct i40e_vsi *src_vsi,
					  u16 rule_type, u16 rule_id)
{
	u16 rules_used, rules_free, sw_seid;
	struct i40e_pf *pf = src_vsi->back;
	int ret;

	sw_seid = src_vsi->uplink_seid;
	ret = i40e_aq_delete_mirrorrule(&pf->hw, sw_seid, rule_type,
					rule_id, 0, NULL, NULL,
					&rules_used, &rules_free);
	return ret;
}

/**
 * i40e_restore_ingress_egress_mirror
 * @src_vsi: the mirrored VSI
 * @mirror: VSI to mirror to
 * @rule_type: rule type to configure
 * @rule_id : rule id to delete
 *
 * This function restores the configured ingress/egress mirrors
 *
 * Returns 0 on success, negative on failure
 **/
int i40e_restore_ingress_egress_mirror(struct i40e_vsi *src_vsi,
				       int mirror, u16 rule_type, u16 *rule_id)
{
	struct i40e_vsi *mirror_vsi;
	struct i40e_vf *mirror_vf;
	struct i40e_pf *pf;
	int ret = 0;

	pf = src_vsi->back;

	/* validate the mirror */
	ret = i40e_validate_vf(pf, mirror);
	if (ret)
		goto err_out;
	mirror_vf = &pf->vf[mirror];
	mirror_vsi = pf->vsi[mirror_vf->lan_vsi_idx];
	ret = i40e_add_ingress_egress_mirror(src_vsi, mirror_vsi, rule_type,
					     rule_id);

err_out:
	return ret;
}

/**
 * i40e_configure_vf_link
 * @vf: VF
 * @link: link state to configure
 *
 * This function configures the requested link state for a VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_configure_vf_link(struct i40e_vf *vf, u8 link)
{
	struct virtchnl_pf_event pfe;
	struct i40e_link_status *ls;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw;
	int abs_vf_id;
	int ret = 0;

	hw = &pf->hw;
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;
	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;
	ls = &pf->hw.phy.link_info;
	switch (link) {
	case VFD_LINKSTATE_AUTO:
		vf->link_forced = false;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	case VFD_LINKSTATE_ON:
		vf->link_forced = true;
		vf->link_up = true;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	case VFD_LINKSTATE_OFF:
		vf->link_forced = true;
		vf->link_up = false;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	default:
		ret = -EINVAL;
		goto error_out;
	}

	/* Notify the VF of its new link state */
	i40e_aq_send_msg_to_vf(hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       I40E_SUCCESS, (u8 *)&pfe, sizeof(pfe), NULL);
error_out:
	return ret;
}

/**
 * i40e_vf_del_vlan_mirror
 * @vf: pointer to the VF structure
 * @vsi: pointer to the VSI structure
 *
 * Delete configured mirror vlans
 *
 * Returns 0 on success, negative on failure
 *
 **/
static int i40e_vf_del_vlan_mirror(struct i40e_vf *vf, struct i40e_vsi *vsi)
{
	u16 rules_used, rules_free, vid;
	struct i40e_pf *pf = vf->pf;
	int ret = 0, num = 0, cnt;
	__le16 *mr_list;

	cnt = bitmap_weight(vf->mirror_vlans, VLAN_N_VID);
	if (cnt) {
		mr_list = kcalloc(cnt, sizeof(__le16), GFP_KERNEL);
		if (!mr_list)
			return -ENOMEM;

		for_each_set_bit(vid, vf->mirror_vlans, VLAN_N_VID) {
			mr_list[num] = CPU_TO_LE16(vid);
			num++;
		}

		ret = i40e_aq_delete_mirrorrule(&pf->hw, vsi->uplink_seid,
						I40E_AQC_MIRROR_RULE_TYPE_VLAN,
						vf->vlan_rule_id, cnt, mr_list,
						NULL, &rules_used,
						&rules_free);

		vf->vlan_rule_id = 0;
		kfree(mr_list);
	}

	return ret;
}

/**
 * i40e_apply_vsi_tc_bw
 * @vf: pointer to the VF structure
 * @share: array representing 8 elements of vfd share
 *
 * Apply VSI BW credits per TC.
 *
 * Returns 0 on success, negative on failure
 *
 **/
static int i40e_apply_vsi_tc_bw(struct i40e_vf *vf, u8 *share)
{
	struct i40e_aqc_configure_vsi_tc_bw_data bw_data = {0};
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = pf->vsi[vf->lan_vsi_idx];
	int ret, i;

	if (!share)
		return -EINVAL;

	/* Reapply share option */
	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
		if (BIT(i) & vsi->tc_config.enabled_tc) {
			bw_data.tc_valid_bits |= BIT(i);
			bw_data.tc_bw_credits[i] = 1;
			if (share[i])
				bw_data.tc_bw_credits[i] = share[i];
		}
	}

	if (unlikely(bw_data.tc_valid_bits == 0)) {
		/* This shouldn't happen, log this */
		dev_info(&pf->pdev->dev,
			 "No valid bits provided for VF %d, can't change share settings",
			 vf->vf_id);
		ret = -EINVAL;
		goto err;
	}

	ret = i40e_aq_config_vsi_tc_bw(&pf->hw, vsi->seid,
				       &bw_data, NULL);
	if (ret) {
		dev_info(&pf->pdev->dev,
			 "AQ command Config VSI BW allocation per TC failed = %d\n",
			 ret);
		goto err;
	}

	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
		vsi->info.qs_handle[i] = bw_data.qs_handles[i];
		vsi->tc_config.tc_info[i].tc_bw_credits =
			vf->tc_info.max_tc_tx_rate[i];
	}
	i40e_vsi_get_bw_info(vsi);
err:
	return ret;
}

/**
 * i40e_restore_vfd_config
 * @vf: pointer to the VF structure
 * @vsi: VF VSI to be configured
 *
 * Restore the VF-d config as per the stored configuration
 *
 * Returns 0 on success, negative on failure
 *
 **/
static int i40e_restore_vfd_config(struct i40e_vf *vf, struct i40e_vsi *vsi)
{
	struct i40e_pf *pf = vf->pf;
	int ret = 0, cnt = 0;
	u8 sec_flag;
	u16 vid;

	/* Restore all VF-d configuration on reset */
	for_each_set_bit(vid, vf->trunk_vlans, VLAN_N_VID) {
		ret = i40e_vsi_add_vlan(vsi, vid);
		if (ret)
			goto err_out;
	}

	cnt = bitmap_weight(vf->mirror_vlans, VLAN_N_VID);
	if (cnt) {
		u16 rule_type = I40E_AQC_MIRROR_RULE_TYPE_VLAN;
		u16 rule_id, rules_used, rules_free;
		u16 sw_seid = vsi->uplink_seid;
		u16 dst_seid = vsi->seid;
		__le16 *mr_list;
		int num = 0;

		mr_list = kcalloc(cnt, sizeof(__le16), GFP_KERNEL);
		if (!mr_list)
			return -ENOMEM;
		for_each_set_bit(vid, vf->mirror_vlans, VLAN_N_VID) {
			mr_list[num] = CPU_TO_LE16(vid);
			num++;
		}
		ret = i40e_aq_add_mirrorrule(&pf->hw, sw_seid, rule_type,
					     dst_seid, cnt, mr_list, NULL,
					     &rule_id, &rules_used,
					     &rules_free);
		if (!ret)
			vf->vlan_rule_id = rule_id;
		kfree(mr_list);
	}

	sec_flag = I40E_AQ_VSI_SEC_FLAG_ENABLE_MAC_CHK;
	ret = i40e_set_spoof_settings(vsi, sec_flag, vf->mac_anti_spoof);
	if (ret)
		goto err_out;

	if (vf->vlan_anti_spoof) {
		sec_flag = I40E_AQ_VSI_SEC_FLAG_ENABLE_VLAN_CHK;
		ret = i40e_set_spoof_settings(vsi, sec_flag, true);
		if (ret)
			goto err_out;
	}

	ret = i40e_configure_vf_loopback(vsi, vf->vf_id, vf->loopback);
	if (ret) {
		vf->loopback = false;
		goto err_out;
	}

	if (!vf->vlan_stripping) {
		ret = i40e_configure_vf_vlan_stripping(vsi, vf->vf_id, false);
		if (ret) {
			vf->vlan_stripping = true;
			goto err_out;
		}
	}

	if (vf->promisc_mode) {
		ret = i40e_configure_vf_promisc_mode(vf, vsi, vf->promisc_mode);
		if (ret) {
			vf->promisc_mode = VFD_PROMISC_OFF;
			goto err_out;
		}
	}

	if (vf->link_forced) {
		u8 link;

		link = (vf->link_up ? VFD_LINKSTATE_ON : VFD_LINKSTATE_OFF);
		ret = i40e_configure_vf_link(vf, link);
		if (ret) {
			vf->link_forced = false;
			goto err_out;
		}
	}

	if (vf->bw_share_applied && vf->bw_share) {
		struct i40e_aqc_configure_vsi_tc_bw_data bw_data = {0};
		int i;

		bw_data.tc_valid_bits = 1;
		bw_data.tc_bw_credits[0] = vf->bw_share;

		ret = i40e_aq_config_vsi_tc_bw(&pf->hw, vsi->seid, &bw_data,
					       NULL);
		if (ret) {
			dev_info(&pf->pdev->dev,
				 "AQ command Config VSI BW allocation per TC failed = %d\n",
				 pf->hw.aq.asq_last_status);
			vf->bw_share_applied = false;
			goto err_out;
		}

		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++)
			vsi->info.qs_handle[i] = bw_data.qs_handles[i];
	}

	if (vf->tc_info.applied) {
		i40e_apply_vsi_tc_bw(vf, vf->tc_info.applied_tc_share);

		ret = i40e_vsi_configure_tc_max_bw(vsi);
		if (ret)
			dev_info(&pf->pdev->dev,
				 "AQ command Config VSI BW allocation per TC failed = %d\n",
				 ret);
	}

err_out:
	return ret;
}

/**
 * i40e_copy_mac_list_sync
 * @vsi: pointer to the vsi structure
 * @mac_list: pointer to head of mac list
 *
 * This function copy mac addresses to mac_list
 **/
static int i40e_copy_mac_list_sync(struct i40e_vsi *vsi,
				   struct list_head *mac_list)
{
	struct i40e_mac_filter *f;
	struct vfd_macaddr *elem;
	int ret = 0, bkt;

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
		elem = kzalloc(sizeof(*elem), GFP_ATOMIC);
		if (!elem) {
			ret = -ENOMEM;
			goto error_unlock;
		}
		INIT_LIST_HEAD(&elem->list);
		ether_addr_copy(elem->mac, f->macaddr);
		list_add_tail(&elem->list, mac_list);
	}
error_unlock:
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	return ret;
}

static bool i40e_find_vmmac_on_list(struct i40e_vf *vf, const u8 *macaddr);

/**
 * i40e_retain_mac_list
 * @pf: pointer to the PF structure
 * @vf_id: VF identifier
 * @vsi_idx: vsi idx
 *
 * This function do backup of vf mac_list without broadcast and default
 * lan address before vsi release
 **/
static int i40e_retain_mac_list(struct i40e_pf *pf, int vf_id, u16 vsi_idx)
{
	struct vfd_macaddr *tmp, *pos;
	struct list_head *mac_list;
	u8 broadcast[ETH_ALEN];
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vsi_idx];
	mac_list = &pf->mac_list[vf_id];
	eth_broadcast_addr(broadcast);
	INIT_LIST_HEAD(mac_list);

	ret = i40e_copy_mac_list_sync(vsi, mac_list);
	if (ret)
		goto err_copy;

	list_for_each_entry_safe(tmp, pos, mac_list, list) {
		if (ether_addr_equal(tmp->mac, broadcast) ||
		    ether_addr_equal(tmp->mac, vf->default_lan_addr.addr) ||
		    i40e_find_vmmac_on_list(vf, tmp->mac)) {
			list_del(&tmp->list);
			kfree(tmp);
		}
	}
err_copy:
	return ret;
}

/**
 * i40e_merge_macs
 * @vf: pointer to the VF info
 * @vsi: pointer to the vsi structure
 * @mac_list: pointer to head of mac list
 * @force: true if continue merge if any problem occurred, otherwise false
 *
 * This function merge mac addresses from mac_list to vsi
 **/
static int i40e_merge_macs(struct i40e_vf *vf, struct i40e_vsi *vsi,
			   struct list_head *mac_list, bool force)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_mac_filter *f;
	struct vfd_macaddr *elem;
	int ret = 0;

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	list_for_each_entry(elem, mac_list, list) {
		f = i40e_find_mac(vsi, elem->mac);
		if (!f) {
			f = i40e_add_mac_filter(vsi, elem->mac);
			if (!f) {
				if (force) {
					dev_info(&pf->pdev->dev,
						 "Unable to add MAC filter %pM for VF %d\n",
						 elem->mac, vf->vf_id);
				} else {
					dev_err(&pf->pdev->dev,
						"Unable to add MAC filter %pM for VF %d\n",
						elem->mac, vf->vf_id);
					ret = I40E_ERR_PARAM;
					break;
				}
			}
		}
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);
	return ret;
}

/**
 * i40e_free_macs
 * @mac_list: pointer to head of mac list
 *
 * This function release mac addresses list
 **/
static void i40e_free_macs(struct list_head *mac_list)
{
	struct vfd_macaddr *elem, *tmp;

	list_for_each_entry_safe(elem, tmp, mac_list, list) {
		list_del(&elem->list);
		kfree(elem);
	}
}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

/**
 * i40e_alloc_vsi_res
 * @vf: pointer to the VF info
 * @idx: VSI index, applies only for ADq mode, zero otherwise
 *
 * alloc VF vsi context & resources
 **/
static int i40e_alloc_vsi_res(struct i40e_vf *vf, u8 idx)
{
	struct i40e_mac_filter *f = NULL;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;
	u64 max_tx_rate = 0;
	int ret = 0;

	vsi = i40e_vsi_setup(pf, I40E_VSI_SRIOV, pf->vsi[pf->lan_vsi]->seid,
			     vf->vf_id);

	if (!vsi) {
		dev_err(&pf->pdev->dev,
			"add vsi failed for VF %d, aq_err %d\n",
			vf->vf_id, pf->hw.aq.asq_last_status);
		ret = -ENOENT;
		goto error_alloc_vsi_res;
	}

	if (!idx) {
		u64 hena = i40e_pf_get_default_rss_hena(pf);
		bool trunk_conf = false;
		u8 broadcast[ETH_ALEN];
		u16 vid;

		for_each_set_bit(vid, vf->trunk_vlans, VLAN_N_VID) {
			if (vid != vf->port_vlan_id)
				trunk_conf = true;
		}
		vf->lan_vsi_idx = vsi->idx;
		vf->lan_vsi_id = vsi->id;
		/* If the port VLAN has been configured and then the
		 * VF driver was removed then the VSI port VLAN
		 * configuration was destroyed.  Check if there is
		 * a port VLAN and restore the VSI configuration if
		 * needed.
		 */
		if (vf->port_vlan_id && !trunk_conf)
			i40e_vsi_add_pvid(vsi, vf->port_vlan_id);

		spin_lock_bh(&vsi->mac_filter_hash_lock);
		if (is_valid_ether_addr(vf->default_lan_addr.addr)) {
			f = i40e_add_mac_filter(vsi,
						vf->default_lan_addr.addr);
			if (!f)
				dev_info(&pf->pdev->dev,
					"Could not add MAC filter %pM for VF %d\n",
					vf->default_lan_addr.addr, vf->vf_id);
		}
		eth_broadcast_addr(broadcast);
		f = i40e_add_mac_filter(vsi, broadcast);
		if (!f)
			dev_info(&pf->pdev->dev,
				 "Could not allocate VF broadcast filter\n");

		spin_unlock_bh(&vsi->mac_filter_hash_lock);
#ifdef HAVE_NDO_SET_VF_LINK_STATE
		/* restore pre-reset mac_list */
		i40e_merge_macs(vf, vsi, &pf->mac_list[vf->vf_id], true);
		i40e_free_macs(&pf->mac_list[vf->vf_id]);
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
		wr32(&pf->hw, I40E_VFQF_HENA1(0, vf->vf_id), (u32)hena);
		wr32(&pf->hw, I40E_VFQF_HENA1(1, vf->vf_id), (u32)(hena >> 32));
		/* program mac filter only for VF VSI */
		ret = i40e_sync_vsi_filters(vsi);
		if (ret)
			dev_err(&pf->pdev->dev, "Unable to program ucast filters\n");
	}

	/* storing VSI index and id for ADq and don't apply the mac filter */
	if (vf->adq_enabled) {
		vf->ch[idx].vsi_idx = vsi->idx;
		vf->ch[idx].vsi_id = vsi->id;
	}

	/* Set VF bandwidth if specified */
	if (vf->tx_rate) {
		max_tx_rate = vf->tx_rate;
	} else if (vf->ch[idx].max_tx_rate) {
		max_tx_rate = vf->ch[idx].max_tx_rate;
	}

	if (max_tx_rate) {
		max_tx_rate = div_u64(max_tx_rate, I40E_BW_CREDIT_DIVISOR);
		ret = i40e_aq_config_vsi_bw_limit(&pf->hw, vsi->seid,
						  max_tx_rate, 0, NULL);
		if (ret)
			dev_err(&pf->pdev->dev, "Unable to set tx rate, VF %d, error code %d.\n",
				vf->vf_id, ret);
	}
#ifdef HAVE_NDO_SET_VF_LINK_STATE
	ret = i40e_restore_vfd_config(vf, vsi);
	if (ret)
		dev_err(&pf->pdev->dev,
			"Failed to restore VF-d config error %d\n", ret);
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

error_alloc_vsi_res:
	return ret;
}

/**
 * i40e_map_pf_queues_to_vsi
 * @vf: pointer to the VF info
 *
 * PF maps LQPs to a VF by programming VSILAN_QTABLE & VPLAN_QTABLE. This
 * function takes care of first part VSILAN_QTABLE, mapping pf queues to VSI.
 **/
static void i40e_map_pf_queues_to_vsi(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg, num_tc = 1; /* VF has at least one traffic class */
	u16 vsi_id, qps;
	int i, j;

	if (vf->adq_enabled)
		num_tc = vf->num_tc;

	for (i = 0; i < num_tc; i++) {
		if (vf->adq_enabled) {
			qps = vf->ch[i].num_qps;
			vsi_id =  vf->ch[i].vsi_id;
		} else {
			qps = pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs;
			vsi_id = vf->lan_vsi_id;
		}

		for (j = 0; j < 7; j++) {
			if (j * 2 >= qps) {
				/* end of list */
				reg = 0x07FF07FF;
			} else {
				u16 qid = i40e_vc_get_pf_queue_id(vf,
								  vsi_id,
								  j * 2);
				reg = qid;
				qid = i40e_vc_get_pf_queue_id(vf, vsi_id,
							      (j * 2) + 1);
				reg |= qid << 16;
			}
			i40e_write_rx_ctl(hw,
					  I40E_VSILAN_QTABLE(j, vsi_id),
					  reg);
		}
	}
}

/**
 * i40e_map_pf_to_vf_queues
 * @vf: pointer to the VF info
 *
 * PF maps LQPs to a VF by programming VSILAN_QTABLE & VPLAN_QTABLE. This
 * function takes care of the second part VPLAN_QTABLE & completes VF mappings.
 **/
static void i40e_map_pf_to_vf_queues(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg, total_qps = 0;
	u32 qps, num_tc = 1; /* VF has at least one traffic class */
	u16 vsi_id, qid;
	int i, j;

	if (vf->adq_enabled)
		num_tc = vf->num_tc;

	for (i = 0; i < num_tc; i++) {
		const u32 queue_mapping_size = ARRAY_SIZE
			(pf->vsi[vf->lan_vsi_idx]->info.queue_mapping);

		if (vf->adq_enabled) {
			qps = vf->ch[i].num_qps;
			vsi_id =  vf->ch[i].vsi_id;
		} else {
			qps = pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs;
			vsi_id = vf->lan_vsi_id;
		}

		qps = min(queue_mapping_size, qps);

		for (j = 0; j < qps; j++) {
			qid = i40e_vc_get_pf_queue_id(vf, vsi_id, j);

			reg = (qid & I40E_VPLAN_QTABLE_QINDEX_MASK);
			wr32(hw, I40E_VPLAN_QTABLE(total_qps, vf->vf_id),
			     reg);
			total_qps++;
		}
	}
}

/**
 * i40e_enable_vf_mappings
 * @vf: pointer to the VF info
 *
 * enable VF mappings
 **/
static void i40e_enable_vf_mappings(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg;

	/* Tell the hardware we're using noncontiguous mapping. HW requires
	 * that VF queues be mapped using this method, even when they are
	 * contiguous in real life
	 */
	i40e_write_rx_ctl(hw, I40E_VSILAN_QBASE(vf->lan_vsi_id),
			  I40E_VSILAN_QBASE_VSIQTABLE_ENA_MASK);

	/* enable VF vplan_qtable mappings */
	reg = I40E_VPLAN_MAPENA_TXRX_ENA_MASK;
	wr32(hw, I40E_VPLAN_MAPENA(vf->vf_id), reg);

	i40e_map_pf_to_vf_queues(vf);
	i40e_map_pf_queues_to_vsi(vf);

	i40e_flush(hw);
}

/**
 * i40e_disable_vf_mappings
 * @vf: pointer to the VF info
 *
 * disable VF mappings
 **/
static void i40e_disable_vf_mappings(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	int i;

	/* disable qp mappings */
	wr32(hw, I40E_VPLAN_MAPENA(vf->vf_id), 0);
	for (i = 0; i < I40E_MAX_VSI_QP; i++)
		wr32(hw, I40E_VPLAN_QTABLE(i, vf->vf_id),
		     I40E_QUEUE_END_OF_LIST);
	i40e_flush(hw);
}

/**
 * i40e_add_vmvlan_to_list
 * @vf: pointer to the VF info
 * @vfl:  pointer to the VF VLAN tag filters list
 * @vlan_idx: vlan_id index in VLAN tag filters list
 *
 * add VLAN tag into the VLAN list for VM
 **/
static i40e_status
i40e_add_vmvlan_to_list(struct i40e_vf *vf,
			struct virtchnl_vlan_filter_list *vfl,
			u16 vlan_idx)
{
	struct i40e_vm_vlan *vlan_elem;

	vlan_elem = kzalloc(sizeof(*vlan_elem), GFP_KERNEL);
	if (!vlan_elem)
		return I40E_ERR_NO_MEMORY;
	vlan_elem->vlan = vfl->vlan_id[vlan_idx];
	vlan_elem->vsi_id = vfl->vsi_id;
	INIT_LIST_HEAD(&vlan_elem->list);
	vf->num_vlan++;
	list_add(&vlan_elem->list, &vf->vm_vlan_list);
	return I40E_SUCCESS;
}

/**
 * i40e_del_vmvlan_from_list
 * @vsi: pointer to the VSI structure
 * @vf: pointer to the VF info
 * @vlan: VLAN tag to be removed from the list
 *
 * delete VLAN tag from the VLAN list for VM
 **/
static void i40e_del_vmvlan_from_list(struct i40e_vsi *vsi,
				      struct i40e_vf *vf, u16 vlan)
{
	struct i40e_vm_vlan *entry, *tmp;

	list_for_each_entry_safe(entry, tmp,
				 &vf->vm_vlan_list, list) {
		if (vlan == entry->vlan) {
			i40e_vsi_kill_vlan(vsi, vlan);
			vf->num_vlan--;
			list_del(&entry->list);
			kfree(entry);
			break;
		}
	}
}

/**
 * i40e_free_vmvlan_list
 * @vsi: pointer to the VSI structure
 * @vf: pointer to the VF info
 *
 * remove whole list of VLAN tags for VM
 **/
static void i40e_free_vmvlan_list(struct i40e_vsi *vsi, struct i40e_vf *vf)
{
	struct i40e_vm_vlan *entry, *tmp;

	if (list_empty(&vf->vm_vlan_list))
		return;

	list_for_each_entry_safe(entry, tmp,
				 &vf->vm_vlan_list, list) {
		if (vsi)
			i40e_vsi_kill_vlan(vsi, entry->vlan);
		list_del(&entry->list);
		kfree(entry);
	}
	vf->num_vlan = 0;
}

/**
 * i40e_add_vmmac_to_list
 * @vf: pointer to the VF info
 * @macaddr: pointer to the MAC address
 *
 * add MAC address into the MAC list for VM
 **/
static i40e_status i40e_add_vmmac_to_list(struct i40e_vf *vf,
					  const u8 *macaddr)
{
	struct i40e_vm_mac *mac_elem;

	mac_elem = kzalloc(sizeof(*mac_elem), GFP_ATOMIC);

	if (!mac_elem)
		return I40E_ERR_NO_MEMORY;
	ether_addr_copy(mac_elem->macaddr, macaddr);
	INIT_LIST_HEAD(&mac_elem->list);
	list_add(&mac_elem->list, &vf->vm_mac_list);
	return I40E_SUCCESS;
}

/**
 * i40e_del_vmmac_from_list
 * @vf: pointer to the VF info
 * @macaddr: pointer to the MAC address
 *
 * delete MAC address from the MAC list for VM
 **/
static void i40e_del_vmmac_from_list(struct i40e_vf *vf, const u8 *macaddr)
{
	struct i40e_vm_mac *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &vf->vm_mac_list, list) {
		if (ether_addr_equal(macaddr, entry->macaddr)) {
			list_del(&entry->list);
			kfree(entry);
			break;
		}
	}
}

#ifdef HAVE_NDO_SET_VF_LINK_STATE
/**
 * i40e_find_vmmac_on_list
 * @vf: pointer to the VF info
 * @macaddr: pointer to the MAC address
 *
 * Search MAC address on MAC list
 **/
static bool i40e_find_vmmac_on_list(struct i40e_vf *vf, const u8 *macaddr)
{
	struct i40e_vm_mac *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &vf->vm_mac_list, list) {
		if (ether_addr_equal(macaddr, entry->macaddr))
			return true;
	}
	return false;
}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

/**
 * i40e_free_vmmac_list
 * @vf: pointer to the VF info
 *
 * remove whole list of MAC addresses for VM
 **/
static void i40e_free_vmmac_list(struct i40e_vf *vf)
{
	struct i40e_vm_mac *entry, *tmp;

	if (list_empty(&vf->vm_mac_list))
		return;

	list_for_each_entry_safe(entry, tmp, &vf->vm_mac_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
}

/**
 * i40e_free_vf_res
 * @vf: pointer to the VF info
 *
 * free VF resources
 **/
static void i40e_free_vf_res(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg_idx, reg;
	int i, j, msix_vf;

	/* Start by disabling VF's configuration API to prevent the OS from
	 * accessing the VF's VSI after it's freed / invalidated.
	 */
	clear_bit(I40E_VF_STATE_INIT, &vf->vf_states);

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	/* Release vlan mirror */
	if (vf->lan_vsi_idx) {
		i40e_vf_del_vlan_mirror(vf, pf->vsi[vf->lan_vsi_idx]);
		if (!test_bit(__I40E_VFS_RELEASING, pf->state))
			i40e_retain_mac_list(pf, vf->vf_id, vf->lan_vsi_idx);
	}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

	/* It's possible the VF had requeuested more queues than the default so
	 * do the accounting here when we're about to free them.
	 */
	if (vf->num_queue_pairs > I40E_DEFAULT_QUEUES_PER_VF) {
		pf->queues_left +=
			vf->num_queue_pairs - I40E_DEFAULT_QUEUES_PER_VF;
	}

	/* free vsi & disconnect it from the parent uplink */
	if (vf->lan_vsi_idx) {
		i40e_vsi_release(pf->vsi[vf->lan_vsi_idx]);
		vf->lan_vsi_idx = 0;
		vf->lan_vsi_id = 0;
	}

	/* do the accounting and remove additional ADq VSI's */
	if (vf->adq_enabled && vf->ch[0].vsi_idx) {
		for (j = 0; j < vf->num_tc; j++) {
			/* At this point VSI0 is already released so don't
			 * release it again and only clear their values in
			 * structure variables
			 */
			if (j)
				i40e_vsi_release(pf->vsi[vf->ch[j].vsi_idx]);
			vf->ch[j].vsi_idx = 0;
			vf->ch[j].vsi_id = 0;
		}
	}
	msix_vf = pf->hw.func_caps.num_msix_vectors_vf;

	/* disable interrupts so the VF starts in a known state */
	for (i = 0; i < msix_vf; i++) {
		/* format is same for both registers */
		if (0 == i)
			reg_idx = I40E_VFINT_DYN_CTL0(vf->vf_id);
		else
			reg_idx = I40E_VFINT_DYN_CTLN(((msix_vf - 1) *
						      (vf->vf_id))
						     + (i - 1));
		wr32(hw, reg_idx, I40E_VFINT_DYN_CTLN_CLEARPBA_MASK);
		i40e_flush(hw);
	}

	/* clear the irq settings */
	for (i = 0; i < msix_vf; i++) {
		/* format is same for both registers */
		if (0 == i)
			reg_idx = I40E_VPINT_LNKLST0(vf->vf_id);
		else
			reg_idx = I40E_VPINT_LNKLSTN(((msix_vf - 1) *
						      (vf->vf_id))
						     + (i - 1));
		reg = (I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_MASK |
		       I40E_VPINT_LNKLSTN_FIRSTQ_INDX_MASK);
		wr32(hw, reg_idx, reg);
		i40e_flush(hw);
	}

	i40e_free_vmvlan_list(NULL, vf);
	i40e_free_vmmac_list(vf);

	/* reset some of the state variables keeping track of the resources */
	vf->num_queue_pairs = 0;
	clear_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states);
	clear_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states);
}

/**
 * i40e_alloc_vf_res
 * @vf: pointer to the VF info
 *
 * allocate VF resources
 **/
static int i40e_alloc_vf_res(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	int total_queue_pairs = 0;
	int ret, idx;

	if (vf->num_req_queues &&
	    vf->num_req_queues <= pf->queues_left + I40E_DEFAULT_QUEUES_PER_VF)
		pf->num_vf_qps = vf->num_req_queues;
	else
		pf->num_vf_qps = I40E_DEFAULT_QUEUES_PER_VF;

	/* allocate hw vsi context & associated resources */
	ret = i40e_alloc_vsi_res(vf, 0);
	if (ret)
		goto error_alloc;
	total_queue_pairs += pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs;

	/* allocate additional VSIs based on tc information for ADq */
	if (vf->adq_enabled) {
		if (pf->queues_left >=
		    (I40E_MAX_VF_QUEUES - I40E_DEFAULT_QUEUES_PER_VF)) {
			/* TC 0 always belongs to VF VSI */
			for (idx = 1; idx < vf->num_tc; idx++) {
				ret = i40e_alloc_vsi_res(vf, idx);
				if (ret)
					goto error_alloc;
			}
			/* send correct number of queues */
			total_queue_pairs = I40E_MAX_VF_QUEUES;
		} else {
			dev_info(&pf->pdev->dev, "VF %d: Not enough queues to allocate, disabling ADq\n",
				 vf->vf_id);
			vf->adq_enabled = false;
		}
	}

	/* We account for each VF to get a default number of queue pairs.  If
	 * the VF has now requested more, we need to account for that to make
	 * certain we never request more queues than we actually have left in
	 * HW.
	 */
	if (total_queue_pairs > I40E_DEFAULT_QUEUES_PER_VF)
		pf->queues_left -=
			total_queue_pairs - I40E_DEFAULT_QUEUES_PER_VF;

	if (vf->trusted)
		set_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);
	else
		clear_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);

	/* store the total qps number for the runtime
	 * VF req validation
	 */
	vf->num_queue_pairs = total_queue_pairs;

	/* set default queue type for the VF */
	vf->queue_type = VFD_QUEUE_TYPE_RSS;
	/* VF is now completely initialized */
	set_bit(I40E_VF_STATE_INIT, &vf->vf_states);

error_alloc:
	if (ret)
		i40e_free_vf_res(vf);

	return ret;
}

#define VF_DEVICE_STATUS 0xAA
#define VF_TRANS_PENDING_MASK 0x20
/**
 * i40e_quiesce_vf_pci
 * @vf: pointer to the VF structure
 *
 * Wait for VF PCI transactions to be cleared after reset. Returns -EIO
 * if the transactions never clear.
 **/
static int i40e_quiesce_vf_pci(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	int vf_abs_id, i;
	u32 reg;

	vf_abs_id = vf->vf_id + hw->func_caps.vf_base_id;

	wr32(hw, I40E_PF_PCI_CIAA,
	     VF_DEVICE_STATUS | (vf_abs_id << I40E_PF_PCI_CIAA_VF_NUM_SHIFT));
	for (i = 0; i < 100; i++) {
		reg = rd32(hw, I40E_PF_PCI_CIAD);
		if ((reg & VF_TRANS_PENDING_MASK) == 0)
			return 0;
		udelay(1);
	}
	return -EIO;
}

static inline int i40e_getnum_vf_vsi_vlan_filters(struct i40e_vsi *vsi);
static inline void i40e_get_vlan_list_sync(struct i40e_vsi *vsi, int *num_vlans,
					   s16 **vlan_list);
static inline i40e_status
i40e_set_vsi_promisc(struct i40e_vf *vf, u16 seid, bool multi_enable,
		     bool unicast_enable, s16 *vl, int num_vlans);

/**
 * i40e_config_vf_promiscuous_mode
 * @vf: pointer to the VF info
 * @vsi_id: VSI id
 * @allmulti: set MAC L2 layer multicast promiscuous enable/disable
 * @alluni: set MAC L2 layer unicast promiscuous enable/disable
 *
 * Called from the VF to configure the promiscuous mode of
 * VF vsis and from the VF reset path to reset promiscuous mode.
 **/
static i40e_status i40e_config_vf_promiscuous_mode(struct i40e_vf *vf,
						   u16 vsi_id,
						   bool allmulti,
						   bool alluni)
{
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;
	int num_vlans;
	s16 *vl;

	vsi = i40e_find_vsi_from_id(pf, vsi_id);
	if (!i40e_vc_isvalid_vsi_id(vf, vsi_id) || !vsi)
		return I40E_ERR_PARAM;

	if (vf->port_vlan_id) {
		aq_ret = i40e_set_vsi_promisc(vf, vsi->seid, allmulti,
					      alluni, &vf->port_vlan_id, 1);
		return aq_ret;
	} else if (i40e_getnum_vf_vsi_vlan_filters(vsi)) {
		i40e_get_vlan_list_sync(vsi, &num_vlans, &vl);

		if (!vl)
			return I40E_ERR_NO_MEMORY;

		aq_ret = i40e_set_vsi_promisc(vf, vsi->seid, allmulti, alluni,
					      vl, num_vlans);
		kfree(vl);
		return aq_ret;
	}
	/* no vlans to set on, set on vsi */
	aq_ret = i40e_set_vsi_promisc(vf, vsi->seid, allmulti, alluni,
				      NULL, 0);
	return aq_ret;
}

/**
 * i40e_sync_vfr_reset
 * @hw: pointer to hw struct
 * @vf_id: VF identifier
 *
 * Before trigger hardware reset, we need to know if no other process has
 * reserved the hardware for any reset operations. This check is done by
 * examining the status of the ADMINQ bit in VF interrupt register.
 **/
static int i40e_sync_vfr_reset(struct i40e_hw *hw, int vf_id)
{
	u32 reg;
	int i;

	for (i = 0; i < I40E_VFR_WAIT_COUNT; i++) {
		reg = rd32(hw, I40E_VFINT_ICR0_ENA(vf_id)) &
			   I40E_VFINT_ICR0_ADMINQ_MASK;
		if (reg)
			return 0;

		usleep_range(100, 200);
	}

	return -EAGAIN;
}

/**
 * i40e_trigger_vf_reset
 * @vf: pointer to the VF structure
 * @flr: VFLR was issued or not
 *
 * Trigger hardware to start a reset for a particular VF. Expects the caller
 * to wait the proper amount of time to allow hardware to reset the VF before
 * it cleans up and restores VF functionality.
 **/
static void i40e_trigger_vf_reset(struct i40e_vf *vf, bool flr)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg, reg_idx, bit_idx;
	bool vf_active;
	u32 radq;

	/* warn the VF */
	vf_active = test_and_clear_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);

	/* Disable VF's configuration API during reset. The flag is re-enabled
	 * in i40e_alloc_vf_res(), when it's safe again to access VF's VSI.
	 * It's normally disabled in i40e_free_vf_res(), but it's safer
	 * to do it earlier to give some time to finish to any VF config
	 * functions that may still be running at this point.
	 */
	clear_bit(I40E_VF_STATE_INIT, &vf->vf_states);

	/* In the case of a VFLR, the HW has already reset the VF and we
	 * just need to clean up, so don't hit the VFRTRIG register.
	 */
	if (!flr) {
		/* Sync VFR reset before trigger next one */
		radq = rd32(hw, I40E_VFINT_ICR0_ENA(vf->vf_id)) &
			    I40E_VFINT_ICR0_ADMINQ_MASK;
		if (vf_active && !radq)
			/* waiting for finish reset by virtual driver */
			if (i40e_sync_vfr_reset(hw, vf->vf_id))
				dev_info(&pf->pdev->dev,
					 "Reset VF %d never finished\n",
					 vf->vf_id);

		/* Reset VF using VPGEN_VFRTRIG reg. It is also setting
		 * in progress state in rstat1 register.
		 */
		reg = rd32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id));
		reg |= I40E_VPGEN_VFRTRIG_VFSWR_MASK;
		wr32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id), reg);
	}
	/* clear the VFLR bit in GLGEN_VFLRSTAT */
	reg_idx = (hw->func_caps.vf_base_id + vf->vf_id) / 32;
	bit_idx = (hw->func_caps.vf_base_id + vf->vf_id) % 32;
	wr32(hw, I40E_GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
	i40e_flush(hw);

	if (i40e_quiesce_vf_pci(vf))
		dev_err(&pf->pdev->dev, "VF %d PCI transactions stuck\n",
			vf->vf_id);
}

/**
 * i40e_cleanup_reset_vf
 * @vf: pointer to the VF structure
 *
 * Cleanup a VF after the hardware reset is finished. Expects the caller to
 * have verified whether the reset is finished properly, and ensure the
 * minimum amount of wait time has passed.
 **/
static void i40e_cleanup_reset_vf(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg;

	/* disable promisc modes in case they were enabled */
	i40e_config_vf_promiscuous_mode(vf, vf->lan_vsi_id, false, false);

	/* free VF resources to begin resetting the VSI state */
	i40e_free_vf_res(vf);

	/* Enable hardware by clearing the reset bit in the VPGEN_VFRTRIG reg.
	 * By doing this we allow HW to access VF memory at any point. If we
	 * did it any sooner, HW could access memory while it was being freed
	 * in i40e_free_vf_res(), causing an IOMMU fault.
	 *
	 * On the other hand, this needs to be done ASAP, because the VF driver
	 * is waiting for this to happen and may report a timeout. It's
	 * harmless, but it gets logged into Guest OS kernel log, so best avoid
	 * it.
	 */
	reg = rd32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id));
	reg &= ~I40E_VPGEN_VFRTRIG_VFSWR_MASK;
	wr32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id), reg);

	/* reallocate VF resources to finish resetting the VSI state */
	if (!i40e_alloc_vf_res(vf)) {
		int abs_vf_id = vf->vf_id + (int)hw->func_caps.vf_base_id;
		i40e_enable_vf_mappings(vf);
		set_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);
		clear_bit(I40E_VF_STATE_DISABLED, &vf->vf_states);
		/* Do not notify the client during VF init */
		if (!test_and_clear_bit(I40E_VF_STATE_PRE_ENABLE,
					&vf->vf_states))
			i40e_notify_client_of_vf_reset(pf, abs_vf_id);
		vf->num_vlan = 0;
	}

	/* Tell the VF driver the reset is done. This needs to be done only
	 * after VF has been fully initialized, because the VF driver may
	 * request resources immediately after setting this flag.
	 */
	wr32(hw, I40E_VFGEN_RSTAT1(vf->vf_id), VIRTCHNL_VFR_VFACTIVE);
}

/**
 * i40e_reset_vf
 * @vf: pointer to the VF structure
 * @flr: VFLR was issued or not
 *
 * Returns true if the VF is reset, false otherwise.
 **/
bool i40e_reset_vf(struct i40e_vf *vf, bool flr)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	bool rsd = false;
	u32 reg;
	int i;

	if (test_bit(__I40E_VF_RESETS_DISABLED, pf->state))
		return true;

	/* If the VFs have been disabled, this means something else is
	 * resetting the VF, so we shouldn't continue.
	 * This is a global state of a PF, so it is possible that,
	 * a different VF is in reset.
	 */
	if (test_and_set_bit(__I40E_VF_DISABLE, pf->state))
		return false;

	i40e_trigger_vf_reset(vf, flr);

	/* poll VPGEN_VFRSTAT reg to make sure
	 * that reset is complete
	 */
	for (i = 0; i < 10; i++) {
		/* VF reset requires driver to first reset the VF and then
		 * poll the status register to make sure that the reset
		 * completed successfully. Due to internal HW FIFO flushes,
		 * we must wait 10ms before the register will be valid.
		 */
		usleep_range(10000, 20000);
		reg = rd32(hw, I40E_VPGEN_VFRSTAT(vf->vf_id));
		if (reg & I40E_VPGEN_VFRSTAT_VFRD_MASK) {
			rsd = true;
			break;
		}
	}

	if (flr)
		usleep_range(10000, 20000);

	if (!rsd)
		dev_err(&pf->pdev->dev, "VF reset check timeout on VF %d\n",
			vf->vf_id);
	usleep_range(10000, 20000);

	/* On initial reset, we don't have any queues to disable */
	if (vf->lan_vsi_idx != 0)
		i40e_vsi_stop_rings(pf->vsi[vf->lan_vsi_idx]);

	i40e_cleanup_reset_vf(vf);

	i40e_flush(hw);
	usleep_range(20000, 40000);
	vf->reset_timestamp = ktime_get_ns();
	clear_bit(__I40E_VF_DISABLE, pf->state);

	return true;
}

/**
 * i40e_reset_all_vfs
 * @pf: pointer to the PF structure
 * @flr: VFLR was issued or not
 *
 * Reset all allocated VFs in one go. First, tell the hardware to reset each
 * VF, then do all the waiting in one chunk, and finally finish restoring each
 * VF after the wait. This is useful during PF routines which need to reset
 * all VFs, as otherwise it must perform these resets in a serialized fashion.
 *
 * Returns true if any VFs were reset, and false otherwise.
 **/
bool i40e_reset_all_vfs(struct i40e_pf *pf, bool flr)
{
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int i, v;
	u32 reg;

	/* If we don't have any VFs, then there is nothing to reset */
	if (!pf->num_alloc_vfs)
		return false;

	/* If VFs have been disabled, there is no need to reset */
	if (test_and_set_bit(__I40E_VF_DISABLE, pf->state))
		return false;

	/* Begin reset on all VFs at once */
	for (v = 0; v < pf->num_alloc_vfs; v++)
		i40e_trigger_vf_reset(&pf->vf[v], flr);

	/* HW requires some time to make sure it can flush the FIFO for a VF
	 * when it resets it. Poll the VPGEN_VFRSTAT register for each VF in
	 * sequence to make sure that it has completed. We'll keep track of
	 * the VFs using a simple iterator that increments once that VF has
	 * finished resetting.
	 */
	for (i = 0, v = 0; i < 10 && v < pf->num_alloc_vfs; i++) {
		usleep_range(10000, 20000);

		/* Check each VF in sequence, beginning with the VF to fail
		 * the previous check.
		 */
		while (v < pf->num_alloc_vfs) {
			vf = &pf->vf[v];
			reg = rd32(hw, I40E_VPGEN_VFRSTAT(vf->vf_id));
			if (!(reg & I40E_VPGEN_VFRSTAT_VFRD_MASK))
				break;

			/* If the current VF has finished resetting, move on
			 * to the next VF in sequence.
			 */
			v++;
		}
	}

	if (flr)
		usleep_range(10000, 20000);

	/* Display a warning if at least one VF didn't manage to reset in
	 * time, but continue on with the operation.
	 */
	if (v < pf->num_alloc_vfs)
		dev_err(&pf->pdev->dev, "VF reset check timeout on VF %d\n",
			pf->vf[v].vf_id);
	usleep_range(10000, 20000);

	/* Begin disabling all the rings associated with VFs, but do not wait
	 * between each VF.
	 */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		/* On initial reset, we don't have any queues to disable */
		if (pf->vf[v].lan_vsi_idx == 0)
			continue;

		i40e_vsi_stop_rings_no_wait(pf->vsi[pf->vf[v].lan_vsi_idx]);
	}

	/* Now that we've notified HW to disable all of the VF rings, wait
	 * until they finish.
	 */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		/* On initial reset, we don't have any queues to disable */
		if (pf->vf[v].lan_vsi_idx == 0)
			continue;

		i40e_vsi_wait_queues_disabled(pf->vsi[pf->vf[v].lan_vsi_idx]);
	}

	/* Hw may need up to 50ms to finish disabling the RX queues. We
	 * minimize the wait by delaying only once for all VFs.
	 */
	mdelay(50);

	/* Finish the reset on each VF */
	for (v = 0; v < pf->num_alloc_vfs; v++)
		i40e_cleanup_reset_vf(&pf->vf[v]);

	i40e_flush(hw);
	usleep_range(20000, 40000);
	clear_bit(__I40E_VF_DISABLE, pf->state);

	return true;
}

#ifdef HAVE_NDO_SET_VF_LINK_STATE
static int i40e_set_pf_egress_mirror(struct pci_dev *pdev, const int mirror);
static int i40e_set_pf_ingress_mirror(struct pci_dev *pdev, const int mirror);
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

/**
 * i40e_free_vfs
 * @pf: pointer to the PF structure
 *
 * free VF resources
 **/
void i40e_free_vfs(struct i40e_pf *pf)
{
	struct i40e_hw *hw = &pf->hw;
	u32 reg_idx, bit_idx;
	int i, tmp, vf_id;
#ifdef HAVE_NDO_SET_VF_LINK_STATE
	struct i40e_vsi *src_vsi;
	u16 rule_type, rule_id;
	int ret;
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

	if (!pf->vf)
		return;

	set_bit(__I40E_VFS_RELEASING, pf->state);

	while (test_and_set_bit(__I40E_VF_DISABLE, pf->state))
		usleep_range(1000, 2000);

	i40e_notify_client_of_vf_enable(pf, 0);

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	if (pf->egress_vlan != I40E_NO_VF_MIRROR)
		i40e_set_pf_egress_mirror(pf->pdev, I40E_NO_VF_MIRROR);
	if (pf->ingress_vlan != I40E_NO_VF_MIRROR)
		i40e_set_pf_ingress_mirror(pf->pdev, I40E_NO_VF_MIRROR);

	/* At start we need to clear all ingress and egress mirroring setup.
	 * We can contiune when we remove all mirroring.
	 */
	for (i = 0; i < pf->num_alloc_vfs; i++) {
		src_vsi = pf->vsi[pf->vf[i].lan_vsi_idx];
		if (I40E_IS_MIRROR_VLAN_ID_VALID(pf->vf[i].ingress_vlan)) {
			rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_EGRESS;
			rule_id = pf->vf[i].ingress_rule_id;
			ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
							     rule_id);
			if (ret)
				dev_warn(&pf->pdev->dev,
					 "Error %s when tried to remove ingress mirror on VF %d",
					 i40e_aq_str
					 (hw, hw->aq.asq_last_status),
					 pf->vf[i].vf_id);
		}
		if (I40E_IS_MIRROR_VLAN_ID_VALID(pf->vf[i].egress_vlan)) {
			rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_INGRESS;
			rule_id = pf->vf[i].egress_rule_id;
			ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
							     rule_id);
			if (ret)
				dev_warn(&pf->pdev->dev,
					 "Error %s when tried to remove egress mirror on VF %d",
					 i40e_aq_str
					 (hw, hw->aq.asq_last_status),
					 pf->vf[i].vf_id);
		}
	}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

	/* Disable IOV before freeing resources. This lets any VF drivers
	 * running in the host get themselves cleaned up before we yank
	 * the carpet out from underneath their feet.
	 */
	if (!pci_vfs_assigned(pf->pdev))
		pci_disable_sriov(pf->pdev);
	else
		dev_warn(&pf->pdev->dev, "VFs are assigned - not disabling SR-IOV\n");

	/* Amortize wait time by stopping all VFs at the same time */
	for (i = 0; i < pf->num_alloc_vfs; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			continue;

		i40e_vsi_stop_rings_no_wait(pf->vsi[pf->vf[i].lan_vsi_idx]);
	}

	for (i = 0; i < pf->num_alloc_vfs; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			continue;

		i40e_vsi_wait_queues_disabled(pf->vsi[pf->vf[i].lan_vsi_idx]);
	}

	/* free up VF resources */
	tmp = pf->num_alloc_vfs;
	pf->num_alloc_vfs = 0;
	for (i = 0; i < tmp; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			i40e_free_vf_res(&pf->vf[i]);
		/* disable qp mappings */
		i40e_disable_vf_mappings(&pf->vf[i]);
	}
#ifdef HAVE_NDO_SET_VF_LINK_STATE
	if (pf->vfd_obj) {
		destroy_vfd_sysfs(pf->pdev, pf->vfd_obj);
		pf->vfd_obj = NULL;
	}
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

	kfree(pf->vf);
	pf->vf = NULL;

	/* This check is for when the driver is unloaded while VFs are
	 * assigned. Setting the number of VFs to 0 through sysfs is caught
	 * before this function ever gets called.
	 */
	if (!pci_vfs_assigned(pf->pdev)) {
		/* Acknowledge VFLR for all VFS. Without this, VFs will fail to
		 * work correctly when SR-IOV gets re-enabled.
		 */
		for (vf_id = 0; vf_id < tmp; vf_id++) {
			reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
			bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
			wr32(hw, I40E_GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
		}
	}
	clear_bit(__I40E_VF_DISABLE, pf->state);
	clear_bit(__I40E_VFS_RELEASING, pf->state);
}

#ifdef CONFIG_PCI_IOV
/**
 * i40e_alloc_vfs
 * @pf: pointer to the PF structure
 * @num_alloc_vfs: number of VFs to allocate
 *
 * allocate VF resources
 **/
int i40e_alloc_vfs(struct i40e_pf *pf, u16 num_alloc_vfs)
{
	struct i40e_vf *vfs;
	int i, ret = 0;

	/* Disable interrupt 0 so we don't try to handle the VFLR. */
	i40e_irq_dynamic_disable_icr0(pf);

	/* Check to see if we're just allocating resources for extant VFs */
	if (pci_num_vf(pf->pdev) != num_alloc_vfs) {
		ret = pci_enable_sriov(pf->pdev, num_alloc_vfs);
		if (ret) {
			pf->flags &= ~I40E_FLAG_VEB_MODE_ENABLED;
			pf->num_alloc_vfs = 0;
			goto err_iov;
		}
	}
	/* allocate memory */
	vfs = kcalloc(num_alloc_vfs, sizeof(struct i40e_vf), GFP_KERNEL);
	if (!vfs) {
		ret = -ENOMEM;
		goto err_alloc;
	}
	pf->vf = vfs;

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	/* set vfd ops */
	vfd_ops = &i40e_vfd_ops;
	/* create the sriov kobjects */
	pf->vfd_obj = create_vfd_sysfs(pf->pdev, num_alloc_vfs);
#endif /* HAVE_NDO_SET_VF_LINK_STATE */

	/* apply default profile */
	for (i = 0; i < num_alloc_vfs; i++) {
		vfs[i].pf = pf;
		vfs[i].parent_type = I40E_SWITCH_ELEMENT_TYPE_VEB;
		vfs[i].vf_id = i;

#ifdef HAVE_NDO_SET_VF_LINK_STATE
		/* setup default mirror values */
		vfs[i].ingress_vlan = I40E_NO_VF_MIRROR;
		vfs[i].egress_vlan = I40E_NO_VF_MIRROR;
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
		/* assign default loopback value */
		vfs[i].loopback = true;
		/* assign default mac anti spoof value for untrusted VF */
		vfs[i].mac_anti_spoof = true;
		/* assign default allow_untagged value */
		vfs[i].allow_untagged = true;
		/* assign default allow_bcast value */
		vfs[i].allow_bcast = true;
		/* assign default vlan_stripping value */
		vfs[i].vlan_stripping = true;
		/* assign default capabilities */
		set_bit(I40E_VIRTCHNL_VF_CAP_L2, &vfs[i].vf_caps);
		set_bit(I40E_VF_STATE_PRE_ENABLE, &vfs[i].vf_states);
		INIT_LIST_HEAD(&vfs[i].vm_vlan_list);
		INIT_LIST_HEAD(&vfs[i].vm_mac_list);
	}
	pf->num_alloc_vfs = num_alloc_vfs;

	/* VF resources get allocated during reset */
	i40e_reset_all_vfs(pf, false);

	i40e_notify_client_of_vf_enable(pf, num_alloc_vfs);

err_alloc:
	if (ret)
		i40e_free_vfs(pf);
err_iov:
	/* Re-enable interrupt 0. */
	i40e_irq_dynamic_enable_icr0(pf);
	return ret;
}

#endif
#if defined(HAVE_SRIOV_CONFIGURE) || defined(HAVE_RHEL6_SRIOV_CONFIGURE)
/**
 * i40e_pci_sriov_enable
 * @pdev: pointer to a pci_dev structure
 * @num_vfs: number of VFs to allocate
 *
 * Enable or change the number of VFs
 **/
static int i40e_pci_sriov_enable(struct pci_dev *pdev, int num_vfs)
{
#ifdef CONFIG_PCI_IOV
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int pre_existing_vfs = pci_num_vf(pdev);
	int err = 0;

	if (test_bit(__I40E_TESTING, pf->state)) {
		dev_warn(&pdev->dev,
			 "Cannot enable SR-IOV virtual functions while the device is undergoing diagnostic testing\n");
		err = -EPERM;
		goto err_out;
	}

	if (pre_existing_vfs && pre_existing_vfs != num_vfs)
		i40e_free_vfs(pf);
	else if (pre_existing_vfs && pre_existing_vfs == num_vfs)
		goto out;

	if (num_vfs > pf->num_req_vfs) {
		dev_warn(&pdev->dev, "Unable to enable %d VFs. Limited to %d VFs due to device resource constraints.\n",
			 num_vfs, pf->num_req_vfs);
		err = -EPERM;
		goto err_out;
	}

	dev_info(&pdev->dev, "Allocating %d VFs.\n", num_vfs);
	err = i40e_alloc_vfs(pf, num_vfs);
	if (err) {
		dev_warn(&pdev->dev, "Failed to enable SR-IOV: %d\n", err);
		goto err_out;
	}

out:
	return num_vfs;

err_out:
	return err;
#endif
	return 0;
}

/**
 * i40e_pci_sriov_configure
 * @pdev: pointer to a pci_dev structure
 * @num_vfs: number of vfs to allocate
 *
 * Enable or change the number of VFs. Called when the user updates the number
 * of VFs in sysfs.
 **/
int i40e_pci_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	if (num_vfs) {
		if (!(pf->flags & I40E_FLAG_VEB_MODE_ENABLED)) {
			pf->flags |= I40E_FLAG_VEB_MODE_ENABLED;
			i40e_do_reset_safe(pf, I40E_PF_RESET_AND_REBUILD_FLAG);
		}
		ret = i40e_pci_sriov_enable(pdev, num_vfs);
		goto sriov_configure_out;
	}
	if (!pci_vfs_assigned(pdev)) {
		i40e_free_vfs(pf);
		pf->flags &= ~I40E_FLAG_VEB_MODE_ENABLED;
		i40e_do_reset_safe(pf, I40E_PF_RESET_AND_REBUILD_FLAG);
	} else {
		dev_warn(&pdev->dev, "Unable to free VFs because some are assigned to VMs.\n");
		ret = -EINVAL;
		goto sriov_configure_out;
	}
sriov_configure_out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}
#endif

/***********************virtual channel routines******************/

/**
 * i40e_vc_send_msg_to_vf_ex
 * @vf: pointer to the VF info
 * @v_opcode: virtual channel opcode
 * @v_retval: virtual channel return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 * @is_quiet: true for not printing unsuccessful return values, false otherwise
 *
 * send msg to VF
 **/
static int i40e_vc_send_msg_to_vf_ex(struct i40e_vf *vf, u32 v_opcode,
				     u32 v_retval, u8 *msg, u16 msglen,
				     bool is_quiet)
{
	struct i40e_pf *pf;
	struct i40e_hw *hw;
	int abs_vf_id;
	i40e_status aq_ret;

	/* validate the request */
	if (!vf || vf->vf_id >= vf->pf->num_alloc_vfs)
		return -EINVAL;

	pf = vf->pf;
	hw = &pf->hw;
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;

	/* single place to detect unsuccessful return values */
	if (v_retval && !is_quiet) {
		vf->num_invalid_msgs++;
		dev_info(&pf->pdev->dev, "VF %d failed opcode %d, retval: %d\n",
			 vf->vf_id, v_opcode, v_retval);
		if (vf->num_invalid_msgs >
		    I40E_DEFAULT_NUM_INVALID_MSGS_ALLOWED) {
			dev_err(&pf->pdev->dev,
				"Number of invalid messages exceeded for VF %d\n",
				vf->vf_id);
			dev_err(&pf->pdev->dev, "Use PF Control I/F to enable the VF\n");
			set_bit(I40E_VF_STATE_DISABLED, &vf->vf_states);
		}
	} else {
		vf->num_valid_msgs++;
		/* reset the invalid counter, if a valid message is received. */
		vf->num_invalid_msgs = 0;
	}

	aq_ret = i40e_aq_send_msg_to_vf(hw, abs_vf_id, v_opcode, v_retval,
					msg, msglen, NULL);
	if (aq_ret) {
		dev_info(&pf->pdev->dev,
			 "Unable to send the message to VF %d aq_err %d\n",
			 vf->vf_id, pf->hw.aq.asq_last_status);
		return -EIO;
	}

	return 0;
}

/**
 * i40e_vc_send_msg_to_vf
 * @vf: pointer to the VF info
 * @v_opcode: virtual channel opcode
 * @v_retval: virtual channel return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * send msg to VF
 **/
static int i40e_vc_send_msg_to_vf(struct i40e_vf *vf, u32 v_opcode,
				  u32 v_retval, u8 *msg, u16 msglen)
{
	return i40e_vc_send_msg_to_vf_ex(vf, v_opcode, v_retval,
					 msg, msglen, false);
}

/**
 * i40e_vc_send_resp_to_vf
 * @vf: pointer to the VF info
 * @opcode: operation code
 * @retval: return value
 *
 * send resp msg to VF
 **/
static int i40e_vc_send_resp_to_vf(struct i40e_vf *vf,
				   enum virtchnl_ops opcode,
				   i40e_status retval)
{
	return i40e_vc_send_msg_to_vf(vf, opcode, retval, NULL, 0);
}

/**
 * i40e_sync_vf_state
 * @vf: pointer to the VF info
 * @state: VF state
 *
 * Called from a VF message to synchronize the service with a potential
 * VF reset state
 **/
static bool i40e_sync_vf_state(struct i40e_vf *vf, enum i40e_vf_states state)
{
	int i;

	/* When handling some messages, it needs vf state to be set.
	 * It is possible that this flag is cleared during vf reset,
	 * so there is a need to wait until the end of the reset to
	 * handle the request message correctly.
	 */
	for (i = 0; i < I40E_VF_STATE_WAIT_COUNT; i++) {
		if (test_bit(state, &vf->vf_states))
			return true;
		usleep_range(10000, 20000);
	}

	return test_bit(state, &vf->vf_states);
}

/**
 * i40e_vc_get_version_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to request the API version used by the PF
 **/
static int i40e_vc_get_version_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_version_info info = {
		VIRTCHNL_VERSION_MAJOR, VIRTCHNL_VERSION_MINOR
	};

	vf->vf_ver = *(struct virtchnl_version_info *)msg;
	/* VFs running the 1.0 API expect to get 1.0 back or they will cry. */
	if (VF_IS_V10(&vf->vf_ver))
		info.minor = VIRTCHNL_VERSION_MINOR_NO_VF_CAPS;
	return i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_VERSION,
				      I40E_SUCCESS, (u8 *)&info,
				      sizeof(struct virtchnl_version_info));
}

#ifdef __TC_MQPRIO_MODE_MAX
/**
 * i40e_del_qch - delete all the additional VSIs created as a part of ADq
 * @vf: pointer to VF structure
 **/
static void i40e_del_qch(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	int i;

	/* first element in the array belongs to primary VF VSI and we shouldn't
	 * delete it. We should however delete the rest of the VSIs created
	 */
	for (i = 1; i < vf->num_tc; i++) {
		if (vf->ch[i].vsi_idx) {
			i40e_vsi_release(pf->vsi[vf->ch[i].vsi_idx]);
			vf->ch[i].vsi_idx = 0;
			vf->ch[i].vsi_id = 0;
		}
	}
}
#endif /* __TC_MQPRIO_MODE_MAX */

/**
 * i40e_vc_get_vf_resources_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to request its resources
 **/
static int i40e_vc_get_vf_resources_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vf_resource *vfres = NULL;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;
	int num_vsis = 1;
	int len = 0;
	int ret;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_INIT)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	len = (sizeof(struct virtchnl_vf_resource) +
	       sizeof(struct virtchnl_vsi_resource) * num_vsis);

	vfres = kzalloc(len, GFP_KERNEL);
	if (!vfres) {
		aq_ret = I40E_ERR_NO_MEMORY;
		len = 0;
		goto err;
	}
	if (VF_IS_V11(&vf->vf_ver))
		vf->driver_caps = *(u32 *)msg;
	else
		vf->driver_caps = VIRTCHNL_VF_OFFLOAD_L2 |
				  VIRTCHNL_VF_OFFLOAD_RSS_REG |
				  VIRTCHNL_VF_OFFLOAD_VLAN;

	vfres->vf_cap_flags = VIRTCHNL_VF_OFFLOAD_L2 | VIRTCHNL_VF_OFFLOAD_VLAN;
#ifdef VIRTCHNL_VF_CAP_ADV_LINK_SPEED
	vfres->vf_cap_flags |= VIRTCHNL_VF_CAP_ADV_LINK_SPEED;
#endif /* VIRTCHNL_VF_CAP_ADV_LINK_SPEED */

	vsi = pf->vsi[vf->lan_vsi_idx];

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_PF;
	} else {
		if ((pf->hw_features & I40E_HW_RSS_AQ_CAPABLE) &&
		    (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_AQ))
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_AQ;
		else
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_REG;
	}
	if (pf->hw_features & I40E_HW_MULTIPLE_TCP_UDP_RSS_PCTYPE) {
		if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2)
			vfres->vf_cap_flags |=
				VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2;
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP;

	if ((pf->hw_features & I40E_HW_OUTER_UDP_CSUM_CAPABLE) &&
	    (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM))
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RX_POLLING) {
		if (pf->flags & I40E_FLAG_MFP_ENABLED) {
			dev_err(&pf->pdev->dev,
				"VF %d requested polling mode: this feature is supported only when the device is running in single function per port (SFP) mode\n",
				 vf->vf_id);
			aq_ret = I40E_ERR_PARAM;
			goto err;
		}
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RX_POLLING;
	}

	if (pf->hw_features & I40E_HW_WB_ON_ITR_CAPABLE) {
		if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_WB_ON_ITR)
			vfres->vf_cap_flags |=
					VIRTCHNL_VF_OFFLOAD_WB_ON_ITR;
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_REQ_QUEUES)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_REQ_QUEUES;

#ifdef __TC_MQPRIO_MODE_MAX
	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ADQ)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ADQ;
#endif /* __TC_MQPRIO_MODE_MAX */

	vfres->num_vsis = num_vsis;
	vfres->num_queue_pairs = vf->num_queue_pairs;
	vfres->max_vectors = pf->hw.func_caps.num_msix_vectors_vf;
	vfres->rss_key_size = I40E_HKEY_ARRAY_SIZE;
	vfres->rss_lut_size = I40E_VF_HLUT_ARRAY_SIZE;

	if (vf->lan_vsi_idx) {
		vfres->vsi_res[0].vsi_id = vf->lan_vsi_id;
		vfres->vsi_res[0].vsi_type = VIRTCHNL_VSI_SRIOV;
		vfres->vsi_res[0].num_queue_pairs = vsi->alloc_queue_pairs;
		/* VFs only use TC 0 */
		vfres->vsi_res[0].qset_handle
					  = LE16_TO_CPU(vsi->info.qs_handle[0]);
		ether_addr_copy(vfres->vsi_res[0].default_mac_addr,
				vf->default_lan_addr.addr);
	}
	set_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);
	set_bit(I40E_VF_STATE_RESOURCES_LOADED, &vf->vf_states);
	/* if vf is in base mode, keep only the base capabilities that are
	 * negotiated
	 */
	if (pf->vf_base_mode_only)
		vfres->vf_cap_flags &= VF_BASE_MODE_OFFLOADS;
err:
	/* send the response back to the VF */
	ret = i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_VF_RESOURCES,
				     aq_ret, (u8 *)vfres, len);

	kfree(vfres);
	return ret;
}

/**
 * i40e_getnum_vf_vsi_vlan_filters
 * @vsi: pointer to the vsi
 *
 * called to get the number of VLANs offloaded on this VF
 **/
static inline int i40e_getnum_vf_vsi_vlan_filters(struct i40e_vsi *vsi)
{
	struct i40e_mac_filter *f;
	int num_vlans = 0, bkt;

	hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
		if (f->vlan >= 0 && f->vlan <= I40E_MAX_VLANID)
			num_vlans++;
	}

	return num_vlans;
}

/**
 * i40e_get_vlan_list_sync
 * @vsi: pointer to the vsi
 * @num_vlans: number of vlans present in mac_filter_hash, returned to caller
 * @vlan_list: list of vlans present in mac_filter_hash, returned to caller.
 *	       This array is allocated here, but has to be freed in caller.
 *
 * Called to get number of vlans and vlan list present in mac_filter_hash.
 **/

static inline void i40e_get_vlan_list_sync(struct i40e_vsi *vsi, int *num_vlans,
					   s16 **vlan_list)
{
	struct i40e_mac_filter *f;
	int bkt;
	int i;

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	*num_vlans = i40e_getnum_vf_vsi_vlan_filters(vsi);
	*vlan_list = kcalloc(*num_vlans, sizeof(**vlan_list),
			     GFP_ATOMIC);
	if (!(*vlan_list))
		goto err;

	i = 0;
	hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
		if (f->vlan < 0 || f->vlan > I40E_MAX_VLANID)
			continue;
		(*vlan_list)[i++] = f->vlan;
	}
err:
	spin_unlock_bh(&vsi->mac_filter_hash_lock);
}

/**
 * i40e_set_vsi_promisc
 * @vf: pointer to the vf struct
 * @seid: vsi number
 * @multi_enable: set MAC L2 layer multicast promiscuous enable/disable
 *		  for a given VLAN
 * @unicast_enable: set MAC L2 layer unicast promiscuous enable/disable
 *		    for a given VLAN
 * @vl: List of vlans - apply filter for given vlans
 * @num_vlans: Number of elements in vl
 **/
static inline i40e_status
i40e_set_vsi_promisc(struct i40e_vf *vf, u16 seid, bool multi_enable,
		     bool unicast_enable, s16 *vl, int num_vlans)
{
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	int i;

	/* No vlan to set promisc on, set on vsi */
	if (!num_vlans || !vl) {
		aq_ret = i40e_aq_set_vsi_multicast_promiscuous(hw, seid,
							       multi_enable,
							       NULL);
		if (aq_ret) {
			int aq_err = pf->hw.aq.asq_last_status;

			dev_err(&pf->pdev->dev,
				"VF %d failed to set multicast promiscuous mode err %s aq_err %s\n",
				vf->vf_id,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));

			return aq_ret;
		}

		aq_ret = i40e_aq_set_vsi_unicast_promiscuous(hw, seid,
							     unicast_enable,
							     NULL, true);

		if (aq_ret) {
			int aq_err = pf->hw.aq.asq_last_status;

			dev_err(&pf->pdev->dev,
				"VF %d failed to set unicast promiscuous mode err %s aq_err %s\n",
				vf->vf_id,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));
		}

		return aq_ret;
	}

	for (i = 0; i < num_vlans; i++) {
		aq_ret = i40e_aq_set_vsi_mc_promisc_on_vlan(hw, seid,
							    multi_enable,
							    vl[i], NULL);
		if (aq_ret) {
			int aq_err = pf->hw.aq.asq_last_status;

			dev_err(&pf->pdev->dev,
				"VF %d failed to set multicast promiscuous mode err %s aq_err %s\n",
				vf->vf_id,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));
		}

		aq_ret = i40e_aq_set_vsi_uc_promisc_on_vlan(hw, seid,
							    unicast_enable,
							    vl[i], NULL);
		if (aq_ret) {
			int aq_err = pf->hw.aq.asq_last_status;

			dev_err(&pf->pdev->dev,
				"VF %d failed to set unicast promiscuous mode err %s aq_err %s\n",
				vf->vf_id,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));
		}
	}
	return aq_ret;
}

/**
 * i40e_vc_config_promiscuous_mode_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to configure the promiscuous mode of
 * VF vsis
 **/
static int i40e_vc_config_promiscuous_mode_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_promisc_info *info =
	    (struct virtchnl_promisc_info *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	bool allmulti = false;
	bool alluni = false;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err_out;
	}
	if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		dev_err(&pf->pdev->dev,
			"Unprivileged VF %d is attempting to configure promiscuous mode\n",
			vf->vf_id);
		if (pf->vf_base_mode_only)
			dev_err(&pf->pdev->dev, "VF %d is in base mode only, promiscuous mode is not be supported\n",
				vf->vf_id);

		/* Lie to the VF on purpose, because this is an error we can
		 * ignore. Unprivileged VF is not a virtual channel error.
		 */
		aq_ret = I40E_SUCCESS;
		goto err_out;
	}

	if (info->flags > I40E_MAX_VF_PROMISC_FLAGS) {
		aq_ret = I40E_ERR_PARAM;
		goto err_out;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, info->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto err_out;
	}

	/* Multicast promiscuous handling*/
	if (info->flags & FLAG_VF_MULTICAST_PROMISC)
		allmulti = true;

	if (info->flags & FLAG_VF_UNICAST_PROMISC)
		alluni = true;

	aq_ret = i40e_config_vf_promiscuous_mode(vf, info->vsi_id, allmulti,
						 alluni);
	if (aq_ret)
		goto err_out;

	if (allmulti) {
		if (!test_and_set_bit(I40E_VF_STATE_MC_PROMISC,
				      &vf->vf_states))
			dev_info(&pf->pdev->dev,
				 "VF %d successfully set multicast promiscuous mode\n",
				 vf->vf_id);
	} else if (test_and_clear_bit(I40E_VF_STATE_MC_PROMISC,
				      &vf->vf_states))
		dev_info(&pf->pdev->dev,
			 "VF %d successfully unset multicast promiscuous mode\n",
			 vf->vf_id);

	if (alluni) {
		if (!test_and_set_bit(I40E_VF_STATE_UC_PROMISC,
				      &vf->vf_states))
			dev_info(&pf->pdev->dev,
				 "VF %d successfully set unicast promiscuous mode\n",
				 vf->vf_id);
	} else if (test_and_clear_bit(I40E_VF_STATE_UC_PROMISC,
				      &vf->vf_states))
		dev_info(&pf->pdev->dev,
			 "VF %d successfully unset unicast promiscuous mode\n",
			 vf->vf_id);

err_out:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf,
				       VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE,
				       aq_ret);
}

/**
 * i40e_vc_config_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to configure the rx/tx
 * queues
 **/
static int i40e_vc_config_queues_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vsi_queue_config_info *qci =
	    (struct virtchnl_vsi_queue_config_info *)msg;
	struct virtchnl_queue_pair_info *qpi;
	i40e_status aq_ret = I40E_SUCCESS;
	u16 vsi_id, vsi_queue_id = 0;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;
	int i, j = 0, idx = 0;
	u16 num_qps_all = 0;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, qci->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (qci->num_queue_pairs > I40E_MAX_VF_QUEUES) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (vf->adq_enabled) {
		for (i = 0; i < vf->num_tc; i++)
			num_qps_all += vf->ch[i].num_qps;
		if (num_qps_all != qci->num_queue_pairs) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}
	}

	vsi_id = qci->vsi_id;

	for (i = 0; i < qci->num_queue_pairs; i++) {
		qpi = &qci->qpair[i];

		if (!vf->adq_enabled) {
			if (!i40e_vc_isvalid_queue_id(vf, vsi_id,
						      qpi->txq.queue_id)) {
				aq_ret = I40E_ERR_PARAM;
				goto error_param;
			}

			vsi_queue_id = qpi->txq.queue_id;

			if (qpi->txq.vsi_id != qci->vsi_id ||
			    qpi->rxq.vsi_id != qci->vsi_id ||
			    qpi->rxq.queue_id != vsi_queue_id) {
				aq_ret = I40E_ERR_PARAM;
				goto error_param;
			}
		}

		if (vf->adq_enabled) {
			if (idx >= ARRAY_SIZE(vf->ch)) {
				aq_ret = I40E_ERR_NO_AVAILABLE_VSI;
				goto error_param;
			}
			vsi_id = vf->ch[idx].vsi_id;
		}

		if (i40e_config_vsi_rx_queue(vf, vsi_id, vsi_queue_id,
					     &qpi->rxq) ||
		    i40e_config_vsi_tx_queue(vf, vsi_id, vsi_queue_id,
					     &qpi->txq)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		/* For ADq there can be up to 4 VSIs with max 4 queues each.
		 * VF does not know about these additional VSIs and all
		 * it cares is about its own queues. PF configures these queues
		 * to its appropriate VSIs based on TC mapping
		 */
		if (vf->adq_enabled) {
			if (idx >= ARRAY_SIZE(vf->ch)) {
				aq_ret = I40E_ERR_NO_AVAILABLE_VSI;
				goto error_param;
			}
			if (j == (vf->ch[idx].num_qps - 1)) {
				idx++;
				j = 0; /* resetting the queue count */
				vsi_queue_id = 0;
			} else {
				j++;
				vsi_queue_id++;
			}
		}
	}

	/* set vsi num_queue_pairs in use to num configured by VF */
	if (!vf->adq_enabled) {
		pf->vsi[vf->lan_vsi_idx]->num_queue_pairs =
			qci->num_queue_pairs;
	} else {
		for (i = 0; i < vf->num_tc; i++) {
			vsi = pf->vsi[vf->ch[i].vsi_idx];
			vsi->num_queue_pairs = vf->ch[i].num_qps;

			if (i40e_update_adq_vsi_queues(vsi, i)) {
				aq_ret = I40E_ERR_CONFIG;
				goto error_param;
			}
		}
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_VSI_QUEUES,
				       aq_ret);
}

/**
 * i40e_validate_queue_map
 * @vf: pointer to the VF info
 * @vsi_id: vsi id
 * @queuemap: Tx or Rx queue map
 *
 * check if Tx or Rx queue map is valid
 **/
static int i40e_validate_queue_map(struct i40e_vf *vf, u16 vsi_id,
				   unsigned long queuemap)
{
	u16 vsi_queue_id, queue_id;

	for_each_set_bit(vsi_queue_id, &queuemap, I40E_MAX_VSI_QP) {
		if (vf->adq_enabled) {
			vsi_id = vf->ch[vsi_queue_id / I40E_MAX_VF_VSI].vsi_id;
			queue_id = (vsi_queue_id % I40E_DEFAULT_QUEUES_PER_VF);
		} else {
			queue_id = vsi_queue_id;
		}

		if (!i40e_vc_isvalid_queue_id(vf, vsi_id, queue_id))
			return -EINVAL;
	}

	return 0;
}

/**
 * i40e_vc_config_irq_map_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to configure the irq to
 * queue map
 **/
static int i40e_vc_config_irq_map_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_irq_map_info *irqmap_info =
	    (struct virtchnl_irq_map_info *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct virtchnl_vector_map *map;
	u16 vsi_id;
	int i;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (irqmap_info->num_vectors >
	    vf->pf->hw.func_caps.num_msix_vectors_vf) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < irqmap_info->num_vectors; i++) {
		map = &irqmap_info->vecmap[i];
		/* validate msg params */
		if (!i40e_vc_isvalid_vector_id(vf, map->vector_id) ||
		    !i40e_vc_isvalid_vsi_id(vf, map->vsi_id)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}
		vsi_id = map->vsi_id;

		if (i40e_validate_queue_map(vf, vsi_id, map->rxq_map)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		if (i40e_validate_queue_map(vf, vsi_id, map->txq_map)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		i40e_config_irq_link_list(vf, vsi_id, map);
	}
error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_IRQ_MAP,
				       aq_ret);
}

/**
 * i40e_ctrl_vf_tx_rings
 * @vsi: the SRIOV VSI being configured
 * @q_map: bit map of the queues to be enabled
 * @enable: start or stop the queue
 **/
static int i40e_ctrl_vf_tx_rings(struct i40e_vsi *vsi, unsigned long q_map,
				 bool enable)
{
	struct i40e_pf *pf = vsi->back;
	int ret = 0;
	u16 q_id;

	for_each_set_bit(q_id, &q_map, I40E_MAX_VF_QUEUES) {
		ret = i40e_control_wait_tx_q(vsi->seid, pf,
					     vsi->base_queue + q_id,
					     false /*is xdp*/, enable);
		if (ret)
			break;
	}
	return ret;
}

/**
 * i40e_ctrl_vf_rx_rings
 * @vsi: the SRIOV VSI being configured
 * @q_map: bit map of the queues to be enabled
 * @enable: start or stop the queue
 **/
static int i40e_ctrl_vf_rx_rings(struct i40e_vsi *vsi, unsigned long q_map,
				 bool enable)
{
	struct i40e_pf *pf = vsi->back;
	int ret = 0;
	u16 q_id;

	for_each_set_bit(q_id, &q_map, I40E_MAX_VF_QUEUES) {
		ret = i40e_control_wait_rx_q(pf, vsi->base_queue + q_id,
					     enable);
		if (ret)
			break;
	}
	return ret;
}

/**
 * i40e_vc_isvalid_vqs_bitmaps - validate Rx/Tx queue bitmaps from VIRTCHNL
 * @vqs: virtchnl_queue_select structure containing bitmaps to validate
 *
 * Returns true if bitmaps are valid, else false
 */
static bool i40e_vc_isvalid_vqs_bitmaps(struct virtchnl_queue_select *vqs)
{
	if ((!vqs->rx_queues && !vqs->tx_queues) ||
	    vqs->rx_queues >= BIT(I40E_MAX_VF_QUEUES) ||
	    vqs->tx_queues >= BIT(I40E_MAX_VF_QUEUES))
		return false;

	return true;
}

/**
 * i40e_vc_enable_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to enable all or specific queue(s)
 **/
static int i40e_vc_enable_queues_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	int i;

	if (vf->pf_ctrl_disable) {
		aq_ret = I40E_ERR_PARAM;
		dev_err(&pf->pdev->dev,
			"Admin has disabled VF %d via sysfs, will not enable queues",
			 vf->vf_id);
		goto error_param;
	}
	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vqs_bitmaps(vqs)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	/* Use the queue bit map sent by the VF */
	if (i40e_ctrl_vf_rx_rings(pf->vsi[vf->lan_vsi_idx], vqs->rx_queues,
				  true)) {
		aq_ret = I40E_ERR_TIMEOUT;
		goto error_param;
	}
	if (i40e_ctrl_vf_tx_rings(pf->vsi[vf->lan_vsi_idx], vqs->tx_queues,
				  true)) {
		aq_ret = I40E_ERR_TIMEOUT;
		goto error_param;
	}

	/* need to start the rings for additional ADq VSI's as well */
	if (vf->adq_enabled) {
		/* zero belongs to LAN VSI */
		for (i = 1; i < vf->num_tc; i++) {
			if (i40e_ctrl_vf_rx_rings(pf->vsi[vf->ch[i].vsi_idx],
						  vqs->rx_queues, true)) {
				aq_ret = I40E_ERR_TIMEOUT;
				goto error_param;
			}
			if (i40e_ctrl_vf_tx_rings(pf->vsi[vf->ch[i].vsi_idx],
						  vqs->tx_queues, true)) {
				aq_ret = I40E_ERR_TIMEOUT;
				goto error_param;
			}
		}
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_QUEUES,
				       aq_ret);
}

/**
 * i40e_vc_disable_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to disable all or specific
 * queue(s)
 **/
static int i40e_vc_disable_queues_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vqs_bitmaps(vqs)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	/* Use the queue bit map sent by the VF */
	if (i40e_ctrl_vf_tx_rings(pf->vsi[vf->lan_vsi_idx], vqs->tx_queues,
				  false)) {
		aq_ret = I40E_ERR_TIMEOUT;
		goto error_param;
	}
	if (i40e_ctrl_vf_rx_rings(pf->vsi[vf->lan_vsi_idx], vqs->rx_queues,
				  false)) {
		aq_ret = I40E_ERR_TIMEOUT;
		goto error_param;
	}
error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_QUEUES,
				       aq_ret);
}

/**
 * i40e_check_enough_queue - find enough queue
 * @vf: pointer to the VF info
 * @needed: the number of items needed
 *
 * Returns the base item index of the queue, or negative for error
 **/
static int i40e_check_enough_queue(struct i40e_vf *vf, u16 needed)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = pf->vsi[vf->lan_vsi_idx];
	u16 i, cur_queues, more, pool_size;
	struct i40e_lump_tracking *pile;

	cur_queues = vsi->alloc_queue_pairs;

	/* if current allocated queues is enough for need */
	if (cur_queues >= needed)
		return vsi->base_queue;

	pile = pf->qp_pile;
	if (cur_queues > 0) {
		/*
		 * if queues of allocated not zero, just check if
		 * there is enough queues behind the allocated queues
		 * for more.
		 */
		more = needed - cur_queues;
		for (i = vsi->base_queue + cur_queues;
			i < pile->num_entries; i++) {
			if (pile->list[i] & I40E_PILE_VALID_BIT)
				break;

			if (more-- == 1)
				/* there is enough */
				return vsi->base_queue;
		}
	}

	pool_size = 0;
	for (i = 0; i < pile->num_entries; i++) {
		if (pile->list[i] & I40E_PILE_VALID_BIT) {
			pool_size = 0;
			continue;
		}
		if (needed <= ++pool_size)
			/* there is enough */
			return i;
	}

	return -ENOMEM;
}

static int i40e_set_vf_num_queues(struct i40e_vf *vf, int num_queues)
{
	int cur_pairs = vf->num_queue_pairs;
	struct i40e_pf *pf = vf->pf;
	int max_size;

	if (num_queues > I40E_MAX_VF_QUEUES) {
		dev_err(&pf->pdev->dev, "Unable to configure %d VF queues, the maximum is %d\n",
			num_queues,
			I40E_MAX_VF_QUEUES);
		return -EINVAL;
	} else if (num_queues - cur_pairs > pf->queues_left) {
		dev_warn(&pf->pdev->dev, "Unable to configure %d VF queues, only %d available\n",
			 num_queues - cur_pairs,
			 pf->queues_left);
		return -EINVAL;
	} else if (i40e_check_enough_queue(vf, num_queues) < 0) {
		dev_warn(&pf->pdev->dev, "VF requested %d more queues, but there is not enough for it.\n",
			 num_queues - cur_pairs);
		return -EINVAL;
	}

	max_size = i40e_max_lump_qp(pf);
	if (max_size < 0) {
		dev_err(&pf->pdev->dev, "Unable to configure %d VF queues, pile=<null>\n",
			num_queues);
		return -EINVAL;
	}

	if (num_queues > max_size) {
		dev_err(&pf->pdev->dev, "Unable to configure %d VF queues, only %d available\n",
			num_queues, max_size);
		return -EINVAL;
	}

	/* successful request */
	vf->num_req_queues = num_queues;
	i40e_vc_reset_vf(vf, true);
	return 0;
}

/**
 * i40e_vc_request_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * VFs get a default number of queues but can use this message to request a
 * different number.  If the request is successful, PF will reset the VF and
 * return 0.  If unsuccessful, PF will send message informing VF of number of
 * available queues and return result of sending VF a message.
 **/
static int i40e_vc_request_queues_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vf_res_request *vfres =
		(struct virtchnl_vf_res_request *)msg;
	u16 req_pairs = vfres->num_queue_pairs;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE))
		return -EINVAL;

	return i40e_set_vf_num_queues(vf, req_pairs);

}

/**
 * i40e_vc_get_stats_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to get vsi stats
 **/
static int i40e_vc_get_stats_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_eth_stats stats;
	struct i40e_vsi *vsi;

	memset(&stats, 0, sizeof(struct i40e_eth_stats));

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}
	i40e_update_eth_stats(vsi);
	stats = vsi->eth_stats;

error_param:
	/* send the response back to the VF */
	return i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_STATS, aq_ret,
				      (u8 *)&stats, sizeof(stats));
}

#define I40E_MAX_MACVLAN_PER_HW 3072
#define I40E_MAX_MACVLAN_PER_PF(num_ports) (I40E_MAX_MACVLAN_PER_HW /	\
	(num_ports))
/* If the VF is not trusted restrict the number of MAC/VLAN it can program
 * MAC filters: 16 for multicast, 1 for MAC, 1 for broadcast
 */
#define I40E_VC_MAX_MAC_ADDR_PER_VF (16 + 1 + 1)
#define I40E_VC_MAX_VLAN_PER_VF 16

#define I40E_VC_MAX_MACVLAN_PER_TRUSTED_VF(vf_num, num_ports)		\
({	typeof(vf_num) vf_num_ = (vf_num);				\
	typeof(num_ports) num_ports_ = (num_ports);			\
	((I40E_MAX_MACVLAN_PER_PF(num_ports_) - vf_num_ *		\
	I40E_VC_MAX_MAC_ADDR_PER_VF) / vf_num_) +			\
	I40E_VC_MAX_MAC_ADDR_PER_VF; })
/**
 * i40e_check_vf_permission
 * @vf: pointer to the VF info
 * @al: MAC address list from virtchnl
 * @is_quiet: set true for printing msg without opcode info, false otherwise
 *
 * Check that the given list of MAC addresses is allowed. Will return -EPERM
 * if any address in the list is not valid. Checks the following conditions:
 *
 * 1) broadcast and zero addresses are never valid
 * 2) unicast addresses are not allowed if the VMM has administratively set
 *    the VF MAC address, unless the VF is marked as privileged.
 * 3) There is enough space to add all the addresses.
 *
 * Note that to guarantee consistency, it is expected this function be called
 * while holding the mac_filter_hash_lock, as otherwise the current number of
 * addresses might not be accurate.
 **/
static inline int i40e_check_vf_permission(struct i40e_vf *vf,
					   struct virtchnl_ether_addr_list *al,
					   bool *is_quiet)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vsi *vsi = pf->vsi[vf->lan_vsi_idx];
	int mac2add_cnt = 0;
	int i;

	if (!is_quiet)
		return -EINVAL;

	*is_quiet = false;
	for (i = 0; i < al->num_elements; i++) {
		struct i40e_mac_filter *f;
		u8 *addr = al->list[i].addr;

		if (is_broadcast_ether_addr(addr) ||
		    is_zero_ether_addr(addr)) {
			dev_err(&pf->pdev->dev, "invalid VF MAC addr %pM\n", addr);
			return I40E_ERR_INVALID_MAC_ADDR;
		}

		/* If the host VMM administrator has set the VF MAC address
		 * administratively via the ndo_set_vf_mac command then deny
		 * permission to the VF to add or delete unicast MAC addresses.
		 * Unless the VF is privileged and then it can do whatever.
		 * The VF may request to set the MAC address filter already
		 * assigned to it so do not return an error in that case.
		 */
		if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps) &&
		    !is_multicast_ether_addr(addr) && vf->pf_set_mac &&
		    !ether_addr_equal(addr, vf->default_lan_addr.addr)) {
			dev_err(&pf->pdev->dev,
				"VF attempting to override administratively set MAC address\n");
			*is_quiet = true;
			return -EPERM;
		}

		/* count filters that really will be added */
		f = i40e_find_mac(vsi, addr);
		if (!f)
			++mac2add_cnt;
	}

	/* If this VF is not privileged, then we can't add more than a limited
	 * number of addresses. Check to make sure that the additions do not
	 * push us over the limit.
	 */
	if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		if ((i40e_count_filters(vsi) + mac2add_cnt) >
		    I40E_VC_MAX_MAC_ADDR_PER_VF) {
			dev_err(&pf->pdev->dev,
				"Cannot add more MAC addresses, VF is not trusted, switch the VF to trusted to add more functionality\n");
			if (pf->vf_base_mode_only)
				dev_err(&pf->pdev->dev, "VF %d is in base mode only, cannot add more than %d filters\n",
					vf->vf_id,
					I40E_VC_MAX_MAC_ADDR_PER_VF);
			return -EPERM;
		}
	/* If this VF is trusted, it can use more resources than untrusted.
	 * However to ensure that every trusted VF has appropriate number of
	 * resources, divide whole pool of resources per port and then across
	 * all VFs.
	 */
	} else {
		if ((i40e_count_filters(vsi) + mac2add_cnt) >
		    I40E_VC_MAX_MACVLAN_PER_TRUSTED_VF(pf->num_alloc_vfs,
						       hw->num_ports)) {
			dev_err(&pf->pdev->dev,
				"Cannot add more MAC addresses, trusted VF exhausted it's resources\n");
			return -EPERM;
		}
	}
	return 0;
}

/**
 * i40e_check_vf_vlan_cap
 * @vf: pointer to the VF info
 *
 * Check if VF can add another VLAN filter.
 */
static i40e_status
i40e_check_vf_vlan_cap(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;

	if ((vf->num_vlan + 1 > I40E_VC_MAX_VLAN_PER_VF) &&
	    !test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		dev_err(&pf->pdev->dev,
			"VF is not trusted, switch the VF to trusted to add more VLAN addresses\n");
		if (pf->vf_base_mode_only)
			dev_err(&pf->pdev->dev, "VF %d is in base mode only, cannot add more than %d vlans\n",
				vf->vf_id, I40E_VC_MAX_VLAN_PER_VF);

		return I40E_ERR_CONFIG;
	}

	return I40E_SUCCESS;
}

/**
 * i40e_vc_ether_addr_type - get type of virtchnl_ether_addr
 * @vc_ether_addr: used to extract the type
 */
static inline u8
i40e_vc_ether_addr_type(struct virtchnl_ether_addr *vc_ether_addr)
{
	return vc_ether_addr->type & VIRTCHNL_ETHER_ADDR_TYPE_MASK;
}

/**
 * i40e_is_vc_addr_legacy
 * @vc_ether_addr: VIRTCHNL structure that contains MAC and type
 *
 * check if the MAC address is from an older VF
 */
static inline bool
i40e_is_vc_addr_legacy(struct virtchnl_ether_addr __maybe_unused *vc_ether_addr)
{
	return i40e_vc_ether_addr_type(vc_ether_addr) ==
		VIRTCHNL_ETHER_ADDR_LEGACY;
}

/**
 * i40e_is_vc_addr_primary
 * @vc_ether_addr: VIRTCHNL structure that contains MAC and type
 *
 * check if the MAC address is the VF's primary MAC
 * This function should only be called when the MAC address in
 * virtchnl_ether_addr is a valid unicast MAC
 */
static inline bool
i40e_is_vc_addr_primary(struct virtchnl_ether_addr __maybe_unused *vc_ether_addr)
{
	return i40e_vc_ether_addr_type(vc_ether_addr) ==
		VIRTCHNL_ETHER_ADDR_PRIMARY;
}

/**
 * i40e_is_legacy_umac_expired
 * @time_last_added_umac: time since the last delete of VFs default MAC
 *
 * check if last added legacy unicast MAC expired
 */
static inline bool
i40e_is_legacy_umac_expired(unsigned long time_last_added_umac)
{
#define I40E_LEGACY_VF_MAC_CHANGE_EXPIRE_TIME	msecs_to_jiffies(3000)
	return time_is_before_jiffies(time_last_added_umac +
				      I40E_LEGACY_VF_MAC_CHANGE_EXPIRE_TIME);
}

/**
 * i40e_update_vf_mac_addr
 * @vf: VF to update
 * @vc_ether_addr: structure from VIRTCHNL with MAC to add
 *
 * update the VF's cached hardware MAC if allowed
 */
static void
i40e_update_vf_mac_addr(struct i40e_vf *vf,
			struct virtchnl_ether_addr *vc_ether_addr)
{
	u8 *mac_addr = vc_ether_addr->addr;

	if (!is_valid_ether_addr(mac_addr))
		return;

	/* if request to add MAC filter is a primary request
	 * update its default MAC address with the requested one
	 *
	 * if it is a legacy request then check if current default is empty
	 * if so update the default MAC
	 * otherwise save it in case it is followed by a delete request
	 * meaning VF wants to change its default MAC which will be updated
	 * in the delete path
	 */
	if (i40e_is_vc_addr_primary(vc_ether_addr)) {
		ether_addr_copy(vf->default_lan_addr.addr, mac_addr);
	} else {
		if (is_zero_ether_addr(vf->default_lan_addr.addr)) {
			ether_addr_copy(vf->default_lan_addr.addr, mac_addr);
		} else {
			ether_addr_copy(vf->legacy_last_added_umac.addr,
					mac_addr);
			vf->legacy_last_added_umac.time_modified = jiffies;
		}
	}
}

/**
 * i40e_add_vf_mac_filters
 * @vf: pointer to the VF info
 * @is_quiet: set true for printing msg without opcode info, false otherwise
 * @al: pointer to the address list of MACs to add
 *
 * add guest mac address filter
 **/
static int i40e_add_vf_mac_filters(struct i40e_vf *vf, bool *is_quiet,
				   struct virtchnl_ether_addr_list *al)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	i40e_status ret = I40E_SUCCESS;
	int i;

	vsi = pf->vsi[vf->lan_vsi_idx];

	/* Lock once, because all function inside for loop accesses VSI's
	 * MAC filter list which needs to be protected using same lock.
	 */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	ret = i40e_check_vf_permission(vf, al, is_quiet);
	if (ret) {
		spin_unlock_bh(&vsi->mac_filter_hash_lock);
		goto error_param;
	}

	/* add new addresses to the list */
	for (i = 0; i < al->num_elements; i++) {
		struct i40e_mac_filter *f;

		f = i40e_find_mac(vsi, al->list[i].addr);
		if (!f) {
			f = i40e_add_mac_filter(vsi, al->list[i].addr);
			if (!f) {
				dev_err(&pf->pdev->dev,
					"Unable to add MAC filter %pM for VF %d\n",
					al->list[i].addr, vf->vf_id);
				ret = I40E_ERR_PARAM;
				spin_unlock_bh(&vsi->mac_filter_hash_lock);
				goto error_param;
			}

			ret = i40e_add_vmmac_to_list(vf, al->list[i].addr);
			if (ret) {
				spin_unlock_bh(&vsi->mac_filter_hash_lock);
				goto error_param;
			}
		}

		i40e_update_vf_mac_addr(vf, &al->list[i]);
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);
error_param:
	return ret;
}

/**
 * i40e_vc_add_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * add guest mac address filter
 **/
static int i40e_vc_add_mac_addr_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_ether_addr_list *al =
	    (struct virtchnl_ether_addr_list *)msg;
	bool is_quiet = false;
	i40e_status ret = I40E_SUCCESS;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, al->vsi_id)) {
		ret = I40E_ERR_PARAM;
		goto error_param;
	}

	ret = i40e_add_vf_mac_filters(vf, &is_quiet, al);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_msg_to_vf_ex(vf, VIRTCHNL_OP_ADD_ETH_ADDR,
					 ret, NULL, 0, is_quiet);
}

/**
 * i40e_vf_clear_default_mac_addr - clear VF default MAC
 * @vf: pointer to the VF info
 * @is_legacy_unimac: is request to delete a legacy request
 *
 * clear VFs default MAC address
 **/
static void i40e_vf_clear_default_mac_addr(struct i40e_vf *vf,
					   bool is_legacy_unimac)
{
	eth_zero_addr(vf->default_lan_addr.addr);

	if (is_legacy_unimac) {
		unsigned long time_added =
			vf->legacy_last_added_umac.time_modified;

		if (!i40e_is_legacy_umac_expired(time_added)) {
			ether_addr_copy(vf->default_lan_addr.addr,
					vf->legacy_last_added_umac.addr);
		}
	}
}

/**
 * i40e_del_vf_mac_filters
 * @vf: pointer to the VF info
 * @al: pointer to the address list of MACs to delete
 *
 * remove guest mac address filters
 **/
static int i40e_del_vf_mac_filters(struct i40e_vf *vf,
				   struct virtchnl_ether_addr_list *al)
{
	bool was_unimac_deleted = false;
	bool is_legacy_unimac = false;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	i40e_status ret = I40E_SUCCESS;
	int i;

	vsi = pf->vsi[vf->lan_vsi_idx];

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	/* delete addresses from the list */
	for (i = 0; i < al->num_elements; i++) {
		if (ether_addr_equal(al->list[i].addr,
				     vf->default_lan_addr.addr) &&
		    (vf->trusted || !vf->pf_set_mac)) {
			was_unimac_deleted = true;
			is_legacy_unimac =
				i40e_is_vc_addr_legacy(&al->list[i]);
		}

		if (is_broadcast_ether_addr(al->list[i].addr) ||
		    is_zero_ether_addr(al->list[i].addr) ||
		    i40e_del_mac_filter(vsi, al->list[i].addr)) {
			dev_err(&pf->pdev->dev, "Invalid MAC addr %pM for VF %d\n",
				al->list[i].addr, vf->vf_id);
			ret = I40E_ERR_INVALID_MAC_ADDR;
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_param;
		}

		i40e_del_vmmac_from_list(vf, al->list[i].addr);
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	if (was_unimac_deleted)
		i40e_vf_clear_default_mac_addr(vf, is_legacy_unimac);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);
error_param:
	return ret;
}

/**
 * i40e_vc_del_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * remove guest mac address filter
 **/
static int i40e_vc_del_mac_addr_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_ether_addr_list *al =
	    (struct virtchnl_ether_addr_list *)msg;
	i40e_status ret = I40E_SUCCESS;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, al->vsi_id)) {
		ret = I40E_ERR_PARAM;
		goto error_param;
	}

	ret = i40e_del_vf_mac_filters(vf, al);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DEL_ETH_ADDR, ret);
}

/**
 * i40e_vc_add_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * program guest vlan id
 **/
static int i40e_vc_add_vlan_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vlan_filter_list *vfl =
	    (struct virtchnl_vlan_filter_list *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	int ret;
	u16 i;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, vfl->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	for (i = 0; i < vfl->num_elements; i++) {
		if (i40e_is_vid(&vsi->info) &&
		    vfl->vlan_id[i]) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		if (vfl->vlan_id[i] > I40E_MAX_VLANID) {
			aq_ret = I40E_ERR_PARAM;
			dev_err(&pf->pdev->dev,
				"invalid VF VLAN id %d\n", vfl->vlan_id[i]);
			goto error_param;
		}
	}

	i40e_vlan_stripping_enable(vsi);

	for (i = 0; i < vfl->num_elements; i++) {
		aq_ret = i40e_check_vf_vlan_cap(vf);
		if (aq_ret)
			goto error_param;
		/* VLANs are configured by PF, omit check VLAN 0
		 * as it's already added by HW.
		 */
		if (vfl->vlan_id[i] && vf->trunk_set_by_pf) {
			dev_err(&pf->pdev->dev, "Failed to add VLAN id %d for VF %d, as PF has already configured VF's trunk\n",
				vfl->vlan_id[i], vf->vf_id);
			aq_ret = I40E_ERR_CONFIG;
			goto error_param;
		}
		ret = i40e_vsi_add_vlan(vsi, vfl->vlan_id[i]);

		if (!ret && vfl->vlan_id[i]) {
			aq_ret = i40e_add_vmvlan_to_list(vf, vfl, i);
			if (aq_ret)
				goto error_param;
		}
		if (test_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_uc_promisc_on_vlan(&pf->hw, vsi->seid,
							   true,
							   vfl->vlan_id[i],
							   NULL);
		if (test_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_mc_promisc_on_vlan(&pf->hw, vsi->seid,
							   true,
							   vfl->vlan_id[i],
							   NULL);

		if (ret)
			dev_err(&pf->pdev->dev,
				"Unable to add VLAN filter %d for VF %d, error %d\n",
				vfl->vlan_id[i], vf->vf_id, ret);
	}
error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ADD_VLAN, aq_ret);
}

/**
 * i40e_vc_remove_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * remove programmed guest vlan id
 **/
static int i40e_vc_remove_vlan_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vlan_filter_list *vfl =
	    (struct virtchnl_vlan_filter_list *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 i;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, vfl->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps) &&
	    bitmap_weight(vf->trunk_vlans, VLAN_N_VID))
		/* Silently fail the request if the VF is untrusted and trunk
		 * VLANs are configured.
		 */
		goto error_param;

	for (i = 0; i < vfl->num_elements; i++) {
		if (vfl->vlan_id[i] > I40E_MAX_VLANID) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (i40e_is_vid(&vsi->info)) {
		if (vfl->num_elements > 1 || vfl->vlan_id[0])
			aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < vfl->num_elements; i++) {
		i40e_del_vmvlan_from_list(vsi, vf, vfl->vlan_id[i]);

		if (test_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_uc_promisc_on_vlan(&pf->hw, vsi->seid,
							   false,
							   vfl->vlan_id[i],
							   NULL);
		if (test_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_mc_promisc_on_vlan(&pf->hw, vsi->seid,
							   false,
							   vfl->vlan_id[i],
							   NULL);
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DEL_VLAN, aq_ret);
}

/**
 * i40e_vc_config_rss_key
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Configure the VF's RSS key
 **/
static int i40e_vc_config_rss_key(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_rss_key *vrk =
		(struct virtchnl_rss_key *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, vrk->vsi_id) ||
	    vrk->key_len != I40E_HKEY_ARRAY_SIZE) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_config_rss(vsi, vrk->key, NULL, 0);
err:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_KEY,
				       aq_ret);
}

/**
 * i40e_vc_config_rss_lut
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Configure the VF's RSS LUT
 **/
static int i40e_vc_config_rss_lut(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_rss_lut *vrl =
		(struct virtchnl_rss_lut *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 i;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE) ||
	    !i40e_vc_isvalid_vsi_id(vf, vrl->vsi_id) ||
	    vrl->lut_entries != I40E_VF_HLUT_ARRAY_SIZE) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	for (i = 0; i < vrl->lut_entries; i++)
		if (vrl->lut[i] >= vf->num_queue_pairs) {
			aq_ret = I40E_ERR_PARAM;
			goto err;
		}

	vsi = pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_config_rss(vsi, NULL, vrl->lut, I40E_VF_HLUT_ARRAY_SIZE);
	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_LUT,
				       aq_ret);
}

/**
 * i40e_vc_get_rss_hena
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Return the RSS HENA bits allowed by the hardware
 **/
static int i40e_vc_get_rss_hena(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_rss_hena *vrh = NULL;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	int len = 0;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}
	len = sizeof(struct virtchnl_rss_hena);

	vrh = kzalloc(len, GFP_KERNEL);
	if (!vrh) {
		aq_ret = I40E_ERR_NO_MEMORY;
		len = 0;
		goto err;
	}
	vrh->hena = i40e_pf_get_default_rss_hena(pf);
err:
	/* send the response back to the VF */
	aq_ret = i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_RSS_HENA_CAPS,
					aq_ret, (u8 *)vrh, len);
	kfree(vrh);
	return aq_ret;
}

/**
 * i40e_vc_set_rss_hena
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Set the RSS HENA bits for the VF
 **/
static int i40e_vc_set_rss_hena(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_rss_hena *vrh =
		(struct virtchnl_rss_hena *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA1(0, vf->vf_id), (u32)vrh->hena);
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA1(1, vf->vf_id),
			  (u32)(vrh->hena >> 32));

	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_SET_RSS_HENA, aq_ret);
}

/**
 * i40e_vc_enable_vlan_stripping
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Enable vlan header stripping for the VF
 **/
static int i40e_vc_enable_vlan_stripping(struct i40e_vf *vf, u8 *msg)
{
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_vsi *vsi;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	vsi = vf->pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_vlan_stripping_enable(vsi);
	if (!aq_ret)
		vf->vlan_stripping = true;
	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_VLAN_STRIPPING,
				       aq_ret);
}

/**
 * i40e_vc_disable_vlan_stripping
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Disable vlan header stripping for the VF
 **/
static int i40e_vc_disable_vlan_stripping(struct i40e_vf *vf, u8 *msg)
{
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_vsi *vsi;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	vsi = vf->pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_vlan_stripping_disable(vsi);
	if (!aq_ret)
		vf->vlan_stripping = false;
	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING,
				       aq_ret);
}

#ifdef __TC_MQPRIO_MODE_MAX
/**
 * i40e_validate_cloud_filter
 * @vf: pointer to the VF info
 * @tc_filter: pointer to virtchnl_filter
 *
 * This function validates cloud filter programmed as TC filter for ADq
 **/
static int i40e_validate_cloud_filter(struct i40e_vf *vf,
				      struct virtchnl_filter *tc_filter)
{
	struct virtchnl_l4_spec mask = tc_filter->mask.tcp_spec;
	struct virtchnl_l4_spec data = tc_filter->data.tcp_spec;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	struct i40e_mac_filter *f;
	struct hlist_node *h;
	bool found = false;
	int bkt;

	if (!tc_filter->action) {
		dev_info(&pf->pdev->dev,
			 "VF %d: Currently ADq doesn't support Drop Action\n",
			 vf->vf_id);
		goto err;
	}

	/* action_meta is TC number here to which the filter is applied */
	if (tc_filter->action_meta > I40E_MAX_VF_VSI) {
		dev_info(&pf->pdev->dev, "VF %d: Invalid TC number %u\n",
			 vf->vf_id, tc_filter->action_meta);
		goto err;
	}

	/* Check filter if it's programmed for advanced mode or basic mode.
	 * There are two ADq modes (for VF only),
	 * 1. Basic mode: intended to allow as many filter options as possible
	 *		  to be added to a VF in Non-trusted mode. Main goal is
	 *		  to add filters to its own MAC and VLAN id.
	 * 2. Advanced mode: is for allowing filters to be applied other than
	 *		  its own MAC or VLAN. This mode requires the VF to be
	 *		  Trusted.
	 */
	if (mask.dst_mac[0] && !mask.dst_ip[0]) {
		vsi = pf->vsi[vf->lan_vsi_idx];
		f = i40e_find_mac(vsi, data.dst_mac);

		if (!f) {
			dev_info(&pf->pdev->dev,
				 "Destination MAC %pM doesn't belong to VF %d\n",
				 data.dst_mac, vf->vf_id);
			goto err;
		}

		if (mask.vlan_id) {
			hash_for_each_safe(vsi->mac_filter_hash, bkt, h, f,
					   hlist) {
				if (f->vlan == ntohs(data.vlan_id)) {
					found = true;
					break;
				}
			}
			if (!found) {
				dev_info(&pf->pdev->dev,
					 "VF %d doesn't have any VLAN id %u\n",
					 vf->vf_id, ntohs(data.vlan_id));
				goto err;
			}
		}
	} else {
		/* Check if VF is trusted */
		if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
			dev_err(&pf->pdev->dev,
				"VF %d not trusted, make VF trusted to add advanced mode ADq cloud filters\n",
				vf->vf_id);
			return I40E_ERR_CONFIG;
		}
	}

	if (mask.dst_mac[0] & data.dst_mac[0]) {
		if (is_broadcast_ether_addr(data.dst_mac) ||
		    is_zero_ether_addr(data.dst_mac)) {
			dev_info(&pf->pdev->dev, "VF %d: Invalid Dest MAC addr %pM\n",
				 vf->vf_id, data.dst_mac);
			goto err;
		}
	}

	if (mask.src_mac[0] & data.src_mac[0]) {
		if (is_broadcast_ether_addr(data.src_mac) ||
		    is_zero_ether_addr(data.src_mac)) {
			dev_info(&pf->pdev->dev, "VF %d: Invalid Source MAC addr %pM\n",
				 vf->vf_id, data.src_mac);
			goto err;
		}
	}

	if (mask.dst_port & data.dst_port) {
		if (!data.dst_port) {
			dev_info(&pf->pdev->dev, "VF %d: Invalid Dest port\n",
				 vf->vf_id);
			goto err;
		}
	}

	if (mask.src_port & data.src_port) {
		if (!data.src_port) {
			dev_info(&pf->pdev->dev, "VF %d: Invalid Source port\n",
				 vf->vf_id);
			goto err;
		}
	}

	if (tc_filter->flow_type != VIRTCHNL_TCP_V6_FLOW &&
	    tc_filter->flow_type != VIRTCHNL_TCP_V4_FLOW) {
		dev_info(&pf->pdev->dev, "VF %d: Invalid Flow type\n",
			 vf->vf_id);
		goto err;
	}

	if (mask.vlan_id & data.vlan_id) {
		if (ntohs(data.vlan_id) > I40E_MAX_VLANID) {
			dev_info(&pf->pdev->dev, "VF %d: invalid VLAN ID\n",
				 vf->vf_id);
			goto err;
		}
	}

	return I40E_SUCCESS;
err:
	return I40E_ERR_CONFIG;
}

/**
 * i40e_find_vf_vsi_from_seid - searches for the vsi with the given seid
 * @vf: pointer to the VF info
 * @seid: seid of the vsi it is searching for
 **/
static struct i40e_vsi *i40e_find_vf_vsi_from_seid(struct i40e_vf *vf, u16 seid)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	int i;

	for (i = 0; i < vf->num_tc ; i++) {
		vsi = i40e_find_vsi_from_id(pf, vf->ch[i].vsi_id);
		if (vsi && vsi->seid == seid)
			return vsi;
	}
	return NULL;
}

/**
 * i40e_del_all_cloud_filters
 * @vf: pointer to the VF info
 *
 * This function deletes all cloud filters
 **/
static void i40e_del_all_cloud_filters(struct i40e_vf *vf)
{
	struct i40e_cloud_filter *cfilter = NULL;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	struct hlist_node *node;
	int ret;

	hlist_for_each_entry_safe(cfilter, node,
				  &vf->cloud_filter_list, cloud_node) {
		vsi = i40e_find_vf_vsi_from_seid(vf, cfilter->seid);

		if (!vsi) {
			dev_err(&pf->pdev->dev, "VF %d: no VSI found for matching %u seid, can't delete cloud filter\n",
				vf->vf_id, cfilter->seid);
			continue;
		}

		if (cfilter->dst_port)
			ret = i40e_add_del_cloud_filter_big_buf(vsi, cfilter,
								false);
		else
			ret = i40e_add_del_cloud_filter(vsi, cfilter, false);
		if (ret)
			dev_err(&pf->pdev->dev,
				"VF %d: Failed to delete cloud filter, err %s aq_err %s\n",
				vf->vf_id, i40e_stat_str(&pf->hw, ret),
				i40e_aq_str(&pf->hw,
					    pf->hw.aq.asq_last_status));

		hlist_del(&cfilter->cloud_node);
		kfree(cfilter);
		vf->num_cloud_filters--;
	}
}

/**
 * i40e_vc_del_cloud_filter
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * This function deletes a cloud filter programmed as TC filter for ADq
 **/
static int i40e_vc_del_cloud_filter(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_filter *vcf = (struct virtchnl_filter *)msg;
	struct virtchnl_l4_spec mask = vcf->mask.tcp_spec;
	struct virtchnl_l4_spec tcf = vcf->data.tcp_spec;
	struct i40e_cloud_filter cfilter, *cf = NULL;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	struct hlist_node *node;
	int i, ret;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	if (!vf->adq_enabled) {
		dev_info(&pf->pdev->dev,
			 "VF %d: ADq not enabled, can't apply cloud filter\n",
			 vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	if (i40e_validate_cloud_filter(vf, vcf)) {
		dev_info(&pf->pdev->dev,
			 "VF %d: Invalid input, can't apply cloud filter\n",
			 vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	memset(&cfilter, 0, sizeof(cfilter));
	/* parse destination mac address */
	for (i = 0; i < ETH_ALEN; i++)
		cfilter.dst_mac[i] = mask.dst_mac[i] & tcf.dst_mac[i];

	/* parse source mac address */
	for (i = 0; i < ETH_ALEN; i++)
		cfilter.src_mac[i] = mask.src_mac[i] & tcf.src_mac[i];

	cfilter.vlan_id = mask.vlan_id & tcf.vlan_id;
	cfilter.dst_port = mask.dst_port & tcf.dst_port;
	cfilter.src_port = mask.src_port & tcf.src_port;

	switch (vcf->flow_type) {
	case VIRTCHNL_TCP_V4_FLOW:
		cfilter.n_proto = ETH_P_IP;
		if (mask.dst_ip[0] & tcf.dst_ip[0])
			memcpy(&cfilter.ip.v4.dst_ip, tcf.dst_ip,
			       ARRAY_SIZE(tcf.dst_ip));
		else if (mask.src_ip[0] & tcf.dst_ip[0])
			memcpy(&cfilter.ip.v4.src_ip, tcf.src_ip,
			       ARRAY_SIZE(tcf.dst_ip));
		break;
	case VIRTCHNL_TCP_V6_FLOW:
		cfilter.n_proto = ETH_P_IPV6;
		if (mask.dst_ip[3] & tcf.dst_ip[3])
			memcpy(&cfilter.ip.v6.dst_ip6, tcf.dst_ip,
			       sizeof(cfilter.ip.v6.dst_ip6));
		if (mask.src_ip[3] & tcf.src_ip[3])
			memcpy(&cfilter.ip.v6.src_ip6, tcf.src_ip,
			       sizeof(cfilter.ip.v6.src_ip6));
		break;
	default:
		/* TC filter can be configured based on different combinations
		 * and in this case IP is not a part of filter config
		 */
		dev_info(&pf->pdev->dev, "VF %d: Flow type not configured\n",
			 vf->vf_id);
	}

	/* get the vsi to which the tc belongs to */
	vsi = pf->vsi[vf->ch[vcf->action_meta].vsi_idx];
	cfilter.seid = vsi->seid;
	cfilter.flags = vcf->field_flags;

	/* Deleting TC filter */
	if (tcf.dst_port)
		ret = i40e_add_del_cloud_filter_big_buf(vsi, &cfilter, false);
	else
		ret = i40e_add_del_cloud_filter(vsi, &cfilter, false);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"VF %d: Failed to delete cloud filter, err %s aq_err %s\n",
			vf->vf_id, i40e_stat_str(&pf->hw, ret),
			i40e_aq_str(&pf->hw, pf->hw.aq.asq_last_status));
		goto err;
	}

	hlist_for_each_entry_safe(cf, node,
				  &vf->cloud_filter_list, cloud_node) {
		if (cf->seid != cfilter.seid)
			continue;
		if (mask.dst_port)
			if (cfilter.dst_port != cf->dst_port)
				continue;
		if (mask.dst_mac[0])
			if (!ether_addr_equal(cf->src_mac, cfilter.src_mac))
				continue;
		/* for ipv4 data to be valid, only first byte of mask is set */
		if (cfilter.n_proto == ETH_P_IP && mask.dst_ip[0])
			if (memcmp(&cfilter.ip.v4.dst_ip, &cf->ip.v4.dst_ip,
				   ARRAY_SIZE(tcf.dst_ip)))
				continue;
		/* for ipv6, mask is set for all sixteen bytes (4 words) */
		if (cfilter.n_proto == ETH_P_IPV6 && mask.dst_ip[3])
			if (memcmp(&cfilter.ip.v6.dst_ip6, &cf->ip.v6.dst_ip6,
				   sizeof(cfilter.ip.v6.src_ip6)))
				continue;
		if (mask.vlan_id)
			if (cfilter.vlan_id != cf->vlan_id)
				continue;

		hlist_del(&cf->cloud_node);
		kfree(cf);
		vf->num_cloud_filters--;
	}

err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DEL_CLOUD_FILTER,
				       aq_ret);
}

/**
 * i40e_vc_add_cloud_filter
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * This function adds a cloud filter programmed as TC filter for ADq
 **/
static int i40e_vc_add_cloud_filter(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_filter *vcf = (struct virtchnl_filter *)msg;
	struct virtchnl_l4_spec mask = vcf->mask.tcp_spec;
	struct virtchnl_l4_spec tcf = vcf->data.tcp_spec;
	struct i40e_cloud_filter *cfilter = NULL;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	char err_msg_buf[100];
	bool is_quiet = false;
	u16 err_msglen = 0;
	u8 *err_msg = NULL;
	int i, ret;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err_out;
	}

	if (!vf->adq_enabled) {
		dev_info(&pf->pdev->dev,
			 "VF %d: ADq is not enabled, can't apply cloud filter\n",
			 vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
		goto err_out;
	}

	if (pf->fdir_pf_active_filters ||
	    (!hlist_empty(&pf->fdir_filter_list))) {
		aq_ret = I40E_ERR_PARAM;
		err_msglen = strlcpy(err_msg_buf,
				     "Flow Director Sideband filters exists, turn ntuple off to configure cloud filters",
				     sizeof(err_msg_buf));
		err_msg = err_msg_buf;
		is_quiet = true;
		goto err_out;
	}

	if (i40e_validate_cloud_filter(vf, vcf)) {
		dev_info(&pf->pdev->dev,
			 "VF %d: Invalid input/s, can't apply cloud filter\n",
			 vf->vf_id);
			aq_ret = I40E_ERR_PARAM;
			goto err_out;
	}

	cfilter = kzalloc(sizeof(*cfilter), GFP_KERNEL);
	if (!cfilter)
		return -ENOMEM;

	/* parse destination mac address */
	for (i = 0; i < ETH_ALEN; i++)
		cfilter->dst_mac[i] = mask.dst_mac[i] & tcf.dst_mac[i];

	/* parse source mac address */
	for (i = 0; i < ETH_ALEN; i++)
		cfilter->src_mac[i] = mask.src_mac[i] & tcf.src_mac[i];

	cfilter->vlan_id = mask.vlan_id & tcf.vlan_id;
	cfilter->dst_port = mask.dst_port & tcf.dst_port;
	cfilter->src_port = mask.src_port & tcf.src_port;

	switch (vcf->flow_type) {
	case VIRTCHNL_TCP_V4_FLOW:
		cfilter->n_proto = ETH_P_IP;
		if (mask.dst_ip[0] & tcf.dst_ip[0])
			memcpy(&cfilter->ip.v4.dst_ip, tcf.dst_ip,
			       ARRAY_SIZE(tcf.dst_ip));
		else if (mask.src_ip[0] & tcf.dst_ip[0])
			memcpy(&cfilter->ip.v4.src_ip, tcf.src_ip,
			       ARRAY_SIZE(tcf.dst_ip));
		break;
	case VIRTCHNL_TCP_V6_FLOW:
		cfilter->n_proto = ETH_P_IPV6;
		if (mask.dst_ip[3] & tcf.dst_ip[3])
			memcpy(&cfilter->ip.v6.dst_ip6, tcf.dst_ip,
			       sizeof(cfilter->ip.v6.dst_ip6));
		if (mask.src_ip[3] & tcf.src_ip[3])
			memcpy(&cfilter->ip.v6.src_ip6, tcf.src_ip,
			       sizeof(cfilter->ip.v6.src_ip6));
		break;
	default:
		/* TC filter can be configured based on different combinations
		 * and in this case IP is not a part of filter config
		 */
		dev_info(&pf->pdev->dev, "VF %d: Flow type not configured\n",
			 vf->vf_id);
	}

	/* get the VSI to which the TC belongs to */
	vsi = pf->vsi[vf->ch[vcf->action_meta].vsi_idx];
	cfilter->seid = vsi->seid;
	cfilter->flags = vcf->field_flags;

	/* Adding cloud filter programmed as TC filter */
	if (tcf.dst_port)
		ret = i40e_add_del_cloud_filter_big_buf(vsi, cfilter, true);
	else
		ret = i40e_add_del_cloud_filter(vsi, cfilter, true);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"VF %d: Failed to add cloud filter, err %s aq_err %s\n",
			vf->vf_id, i40e_stat_str(&pf->hw, ret),
			i40e_aq_str(&pf->hw, pf->hw.aq.asq_last_status));
		goto err_free;
	}

	INIT_HLIST_NODE(&cfilter->cloud_node);
	hlist_add_head(&cfilter->cloud_node, &vf->cloud_filter_list);
	/* release the pointer passing it to the collection */
	cfilter = NULL;
	vf->num_cloud_filters++;
err_free:
	kfree(cfilter);
err_out:
	return i40e_vc_send_msg_to_vf_ex(vf, VIRTCHNL_OP_ADD_CLOUD_FILTER,
					 aq_ret, err_msg, err_msglen,
					 is_quiet);
}

/**
 * i40e_is_ok_to_alloc_vsi - check resources to create new vsi for tc
 * @pf: board private structure
 * @pile: the pile of resource to search
 * @qp_needed: the number of queue pairs needed
 * @num_vsi: count of new vsi
 *
 * Returns true if there is enough resources, otherwise false
 **/
static bool i40e_is_ok_to_alloc_vsi(struct i40e_pf *pf,
				    struct i40e_lump_tracking *pile,
				    u16 qp_needed, u8 num_vsi)
{
	u16 i = 0, qp_free = 0;

	if (!pile || qp_needed == 0)
		return false;

	/* Start from beginning because earlier areas may have been freed */
	while (i < pile->num_entries) {
		/* Skip already allocated entries */
		if (pile->list[i] & I40E_PILE_VALID_BIT) {
			i++;
			continue;
		}

		/* Do we have enough in this lump? */
		for (qp_free = 0; (qp_free < qp_needed) &&
		     ((i + qp_free) < pile->num_entries); qp_free++) {
			if (pile->list[i + qp_free] & I40E_PILE_VALID_BIT)
				break;
		}
		if (qp_free >= qp_needed)
			break;

		/* Not enough, so skip over it and continue looking */
		i += qp_free;
	}

	if (qp_free < qp_needed)
		return false;

	/* Quick scan to look for free VSIs */
	if (pf->next_vsi + num_vsi >= pf->num_alloc_vsi) {
		i = 0;
		while (i < pf->next_vsi && pf->vsi[i])
			i++;
		if (i + num_vsi >= pf->num_alloc_vsi)
			return false;
	}
	return true;
}

/**
 * i40e_vc_add_qch_msg: Add queue channel and enable ADq
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 **/
static int i40e_vc_add_qch_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_tc_info *tci = (struct virtchnl_tc_info *)msg;
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;
	struct i40e_link_status *ls;
	int i, adq_request_qps = 0;
	u32 speed;

	ls = &pf->hw.phy.link_info;
	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	/* ADq cannot be applied if spoof check is ON */
	if (vf->mac_anti_spoof) {
		dev_err(&pf->pdev->dev,
			"Spoof check is ON, turn OFF both MAC and VLAN anti spoof to enable ADq\n");
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	if (!(vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ADQ)) {
		dev_err(&pf->pdev->dev,
			"VF %d attempting to enable ADq, but hasn't properly negotiated that capability\n",
			vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	/* max number of traffic classes for VF currently capped at 4 */
	if (!tci->num_tc || tci->num_tc > I40E_MAX_VF_VSI) {
		dev_err(&pf->pdev->dev,
			"VF %d trying to set %u TCs, valid range 1-%u TCs per VF\n",
			vf->vf_id, tci->num_tc, I40E_MAX_VF_VSI);
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	/* validate queues for each TC */
	for (i = 0; i < tci->num_tc; i++)
		if (!tci->list[i].count ||
		    tci->list[i].count > I40E_DEFAULT_QUEUES_PER_VF) {
			dev_err(&pf->pdev->dev,
				"VF %d: TC %d trying to set %u queues, valid range 1-%u queues per TC\n",
				vf->vf_id, i, tci->list[i].count,
				I40E_DEFAULT_QUEUES_PER_VF);
			aq_ret = I40E_ERR_PARAM;
			goto err;
		}

	/* need Max VF queues but already have default number of queues */
	adq_request_qps = I40E_MAX_VF_QUEUES - I40E_DEFAULT_QUEUES_PER_VF;

	if (tci->num_tc > 1 &&
	    !(i40e_is_ok_to_alloc_vsi(pf, pf->qp_pile,
				      (tci->num_tc - 1) * vf->num_queue_pairs,
				      tci->num_tc - 1))) {
		dev_err(&pf->pdev->dev, "Lack of resources to allocate %d TCs for VF %d\n",
			tci->num_tc, vf->vf_id);
		aq_ret = I40E_ERR_CONFIG;
		goto err;
	}

	if (pf->queues_left < adq_request_qps) {
		dev_err(&pf->pdev->dev,
			"No queues left to allocate to VF %d\n",
			vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
		goto err;
	} else {
		/* we need to allocate max VF queues to enable ADq so as to
		 * make sure ADq enabled VF always gets back queues when it
		 * goes through a reset.
		 */
		vf->num_queue_pairs = I40E_MAX_VF_QUEUES;
	}

	/* get link speed in MB to validate rate limit */
	speed = i40e_vc_link_speed2mbps(ls->link_speed);
	if (speed == SPEED_UNKNOWN) {
		dev_err(&pf->pdev->dev, "Cannot detect link speed\n");
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	/* parse data from the queue channel info */
	vf->num_tc = tci->num_tc;
	for (i = 0; i < vf->num_tc; i++) {
		if (tci->list[i].max_tx_rate) {
			if (tci->list[i].max_tx_rate > speed) {
				dev_err(&pf->pdev->dev,
					"Invalid max tx rate %llu specified for VF %d.",
					tci->list[i].max_tx_rate,
					vf->vf_id);
				aq_ret = I40E_ERR_PARAM;
				goto err;
			} else {
				vf->ch[i].max_tx_rate =
					tci->list[i].max_tx_rate;
			}
		}
		vf->ch[i].num_qps = tci->list[i].count;
	}

	/* set this flag only after making sure all inputs are sane */
	vf->adq_enabled = true;

	/* reset the VF in order to allocate resources */
	i40e_vc_reset_vf(vf, true);

	return I40E_SUCCESS;

	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_CHANNELS,
				       aq_ret);
}

/**
 * i40e_vc_del_qch_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 **/
static int i40e_vc_del_qch_msg(struct i40e_vf *vf, u8 *msg)
{
	i40e_status aq_ret = I40E_SUCCESS;
	struct i40e_pf *pf = vf->pf;

	if (!i40e_sync_vf_state(vf, I40E_VF_STATE_ACTIVE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	if (vf->adq_enabled) {
		i40e_del_all_cloud_filters(vf);
		i40e_del_qch(vf);
		vf->adq_enabled = false;
		vf->num_tc = 0;
		dev_info(&pf->pdev->dev,
			 "Deleting Queue Channels and cloud filters for ADq on VF %d\n",
			 vf->vf_id);
	} else {
		dev_info(&pf->pdev->dev, "VF %d trying to delete queue channels but ADq isn't enabled\n",
			 vf->vf_id);
		aq_ret = I40E_ERR_PARAM;
	}

	/* reset the VF in order to allocate resources */
	i40e_vc_reset_vf(vf, true);

	return I40E_SUCCESS;

err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_CHANNELS,
				       aq_ret);
}
#endif /* __TC_MQPRIO_MODE_MAX */

/**
 * i40e_vc_process_vf_msg
 * @pf: pointer to the PF structure
 * @vf_id: source VF id
 * @v_opcode: operation code
 * @v_retval: unused return value code
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the common aeq/arq handler to
 * process request from VF
 **/
int i40e_vc_process_vf_msg(struct i40e_pf *pf, s16 vf_id, u32 v_opcode,
			   u32 __always_unused v_retval, u8 *msg, u16 msglen)
{
	struct i40e_hw *hw = &pf->hw;
	int local_vf_id = vf_id - (s16)hw->func_caps.vf_base_id;
	struct i40e_vf *vf;
	int ret;

	pf->vf_aq_requests++;
	if (local_vf_id < 0 || local_vf_id >= pf->num_alloc_vfs)
		return -EINVAL;
	vf = &(pf->vf[local_vf_id]);

	/* Check if VF is disabled. */
	if (test_bit(I40E_VF_STATE_DISABLED, &vf->vf_states))
		return I40E_ERR_PARAM;

	/* perform basic checks on the msg */
	ret = virtchnl_vc_validate_vf_msg(&vf->vf_ver, v_opcode, msg, msglen);

	if (ret) {
		i40e_vc_send_resp_to_vf(vf, v_opcode, I40E_ERR_PARAM);
		dev_err(&pf->pdev->dev, "Invalid message from VF %d, opcode %d, len %d\n",
			local_vf_id, v_opcode, msglen);
		switch (ret) {
		case VIRTCHNL_STATUS_ERR_PARAM:
			return -EPERM;
		default:
			return -EINVAL;
		}
	}

	switch (v_opcode) {
	case VIRTCHNL_OP_VERSION:
		ret = i40e_vc_get_version_msg(vf, msg);
		break;
	case VIRTCHNL_OP_GET_VF_RESOURCES:
		ret = i40e_vc_get_vf_resources_msg(vf, msg);
		i40e_vc_notify_vf_link_state(vf);
		break;
	case VIRTCHNL_OP_RESET_VF:
		clear_bit(I40E_VF_STATE_RESOURCES_LOADED, &vf->vf_states);
		i40e_vc_reset_vf(vf, false);
		ret = 0;
		break;
	case VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE:
		ret = i40e_vc_config_promiscuous_mode_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_VSI_QUEUES:
		ret = i40e_vc_config_queues_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_IRQ_MAP:
		ret = i40e_vc_config_irq_map_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ENABLE_QUEUES:
		ret = i40e_vc_enable_queues_msg(vf, msg);
		i40e_vc_notify_vf_link_state(vf);
		break;
	case VIRTCHNL_OP_DISABLE_QUEUES:
		ret = i40e_vc_disable_queues_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ADD_ETH_ADDR:
		ret = i40e_vc_add_mac_addr_msg(vf, msg);
		break;
	case VIRTCHNL_OP_DEL_ETH_ADDR:
		ret = i40e_vc_del_mac_addr_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ADD_VLAN:
		ret = i40e_vc_add_vlan_msg(vf, msg);
		break;
	case VIRTCHNL_OP_DEL_VLAN:
		ret = i40e_vc_remove_vlan_msg(vf, msg);
		break;
	case VIRTCHNL_OP_GET_STATS:
		ret = i40e_vc_get_stats_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_KEY:
		ret = i40e_vc_config_rss_key(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_LUT:
		ret = i40e_vc_config_rss_lut(vf, msg);
		break;
	case VIRTCHNL_OP_GET_RSS_HENA_CAPS:
		ret = i40e_vc_get_rss_hena(vf, msg);
		break;
	case VIRTCHNL_OP_SET_RSS_HENA:
		ret = i40e_vc_set_rss_hena(vf, msg);
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING:
		ret = i40e_vc_enable_vlan_stripping(vf, msg);
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING:
		ret = i40e_vc_disable_vlan_stripping(vf, msg);
		break;
	case VIRTCHNL_OP_REQUEST_QUEUES:
		ret = i40e_vc_request_queues_msg(vf, msg);
		break;
#ifdef __TC_MQPRIO_MODE_MAX
	case VIRTCHNL_OP_ENABLE_CHANNELS:
		ret = i40e_vc_add_qch_msg(vf, msg);
		break;
	case VIRTCHNL_OP_DISABLE_CHANNELS:
		ret = i40e_vc_del_qch_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ADD_CLOUD_FILTER:
		ret = i40e_vc_add_cloud_filter(vf, msg);
		break;
	case VIRTCHNL_OP_DEL_CLOUD_FILTER:
		ret = i40e_vc_del_cloud_filter(vf, msg);
		break;
#endif /* __TC_MQPRIO_MODE_MAX */
	case VIRTCHNL_OP_UNKNOWN:
	default:
		dev_err(&pf->pdev->dev, "Unsupported opcode %d from VF %d\n",
			v_opcode, local_vf_id);
		ret = i40e_vc_send_resp_to_vf(vf, v_opcode,
					      I40E_ERR_NOT_IMPLEMENTED);
		break;
	}

	return ret;
}

/**
 * i40e_vc_process_vflr_event
 * @pf: pointer to the PF structure
 *
 * called from the vlfr irq handler to
 * free up VF resources and state variables
 **/
int i40e_vc_process_vflr_event(struct i40e_pf *pf)
{
	struct i40e_hw *hw = &pf->hw;
	u32 reg, reg_idx, bit_idx;
	struct i40e_vf *vf;
	int vf_id;

	if (!test_bit(__I40E_VFLR_EVENT_PENDING, pf->state))
		return 0;

	/* Re-enable the VFLR interrupt cause here, before looking for which
	 * VF got reset. Otherwise, if another VF gets a reset while the
	 * first one is being processed, that interrupt will be lost, and
	 * that VF will be stuck in reset forever.
	 */
	reg = rd32(hw, I40E_PFINT_ICR0_ENA);
	reg |= I40E_PFINT_ICR0_ENA_VFLR_MASK;
	wr32(hw, I40E_PFINT_ICR0_ENA, reg);
	i40e_flush(hw);

	clear_bit(__I40E_VFLR_EVENT_PENDING, pf->state);
	for (vf_id = 0; vf_id < pf->num_alloc_vfs; vf_id++) {
		reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
		bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
		/* read GLGEN_VFLRSTAT register to find out the flr VFs */
		vf = &pf->vf[vf_id];
		reg = rd32(hw, I40E_GLGEN_VFLRSTAT(reg_idx));
		if (reg & BIT(bit_idx))
			/* i40e_reset_vf will clear the bit in GLGEN_VFLRSTAT */
			i40e_reset_vf(vf, true);
	}

	return 0;
}

#ifdef IFLA_VF_MAX

/**
 * i40e_set_vf_mac
 * @vf: the VF
 * @vsi: VF VSI to configure
 * @mac: the mac address
 *
 * This function allows the administrator to set the mac address for the VF
 *
 * Returns 0 on success, negative on failure
 *
 **/
static int i40e_set_vf_mac(struct i40e_vf *vf, struct i40e_vsi *vsi,
			   const u8 *mac)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_mac_filter *f;
	struct hlist_node *h;
	int ret = 0;
	int bkt;
	u8 i;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	if (is_multicast_ether_addr(mac)) {
		dev_err(&pf->pdev->dev,
			"Invalid Ethernet address %pM for VF %d\n",
			mac, vf->vf_id);
		ret = -EINVAL;
		goto error_param;
	}

	/* When the VF is resetting wait until it is done.
	 * It can take up to 200 milliseconds,
	 * but wait for up to 300 milliseconds to be safe.
	 * Acquire the vsi pointer only after the VF has been
	 * properly initialized.
	 */
	for (i = 0; i < 15; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &vf->vf_states))
			break;
		msleep(20);
	}
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf->vf_id);
		ret = -EAGAIN;
		goto error_param;
	}
	vsi = pf->vsi[vf->lan_vsi_idx];

	/* Lock once because below invoked function add/del_filter requires
	 * mac_filter_hash_lock to be held
	 */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	/* delete the temporary mac address */
	if (!is_zero_ether_addr(vf->default_lan_addr.addr))
		i40e_del_mac_filter(vsi, vf->default_lan_addr.addr);

	/* Delete all the filters for this VSI - we're going to kill it
	 * anyway.
	 */
	hash_for_each_safe(vsi->mac_filter_hash, bkt, h, f, hlist)
		__i40e_del_filter(vsi, f);

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program mac filter */
	vsi->flags |= I40E_VSI_FLAG_FILTER_CHANGED;
	set_bit(__I40E_MACVLAN_SYNC_PENDING, vsi->back->state);
	if (i40e_sync_vsi_filters(vsi)) {
		dev_err(&pf->pdev->dev, "Unable to program ucast filters\n");
		ret = -EIO;
		goto error_param;
	}

	ether_addr_copy(vf->default_lan_addr.addr, mac);

	i40e_free_vmvlan_list(NULL, vf);

	if (is_zero_ether_addr(mac)) {
		vf->pf_set_mac = false;
		dev_info(&pf->pdev->dev, "Removing MAC on VF %d\n", vf->vf_id);
	} else {
		vf->pf_set_mac = true;
		dev_info(&pf->pdev->dev, "Setting MAC %pM on VF %d\n",
			 mac, vf->vf_id);
	}

	/* Force the VF interface down so it has to bring up with new MAC
	 * address
	 */
	i40e_vc_reset_vf(vf, true);
	dev_info(&pf->pdev->dev, "Bring down and up the VF interface to make this change effective.\n");
error_param:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_ndo_set_vf_mac
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @mac: mac address
 *
 * program VF mac address
 **/
int i40e_ndo_set_vf_mac(struct net_device *netdev, int vf_id, u8 *mac)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_param;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_set_vf_mac(vf, vsi, mac);
error_param:
	return ret;
}

/**
 * i40e_ndo_set_vf_port_vlan
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @vlan_id: mac address
 * @qos: priority setting
 * @vlan_proto: vlan protocol
 *
 * program VF vlan id and/or qos
 **/
#ifdef IFLA_VF_VLAN_INFO_MAX
int i40e_ndo_set_vf_port_vlan(struct net_device *netdev, int vf_id,
			      u16 vlan_id, u8 qos, __be16 vlan_proto)
#else
int i40e_ndo_set_vf_port_vlan(struct net_device *netdev,
			      int vf_id, u16 vlan_id, u8 qos)
#endif /* IFLA_VF_VLAN_INFO_MAX */
{
	u16 vlanprio = vlan_id | (qos << I40E_VLAN_PRIORITY_SHIFT);
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	bool allmulti = false, alluni = false;
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	__le16 *pvid;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_pvid;

	if ((vlan_id > I40E_MAX_VLANID) || (qos > 7)) {
		dev_err(&pf->pdev->dev, "Invalid VF Parameters\n");
		ret = -EINVAL;
		goto error_pvid;
	}
#ifdef IFLA_VF_VLAN_INFO_MAX

	if (vlan_proto != htons(ETH_P_8021Q)) {
		dev_err(&pf->pdev->dev, "VF VLAN protocol is not supported\n");
		ret = -EPROTONOSUPPORT;
		goto error_pvid;
	}
#endif

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error_pvid;
	}

	pvid = i40e_get_current_vid(vsi);

	if (le16_to_cpu(*pvid) == vlanprio) {
#ifdef HAVE_NDO_SET_VF_LINK_STATE
		/* if vlan is being removed then clear trunk_vlan */
		if (!(*pvid))
			memset(vf->trunk_vlans, 0,
			       BITS_TO_LONGS(VLAN_N_VID) * sizeof(long));
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
		goto error_pvid;
	}

	i40e_vlan_stripping_enable(vsi);
	/* do VF reset to renegotiate its capabilities and reinitialize */
	i40e_vc_reset_vf(vf, true);
	/* During reset the VF got a new VSI, so refresh the pointer. */
	vsi = pf->vsi[vf->lan_vsi_idx];
	pvid = i40e_get_current_vid(vsi);

	/* Locked once because multiple functions below iterate list */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	/* Check for condition where there was already a port VLAN ID
	 * filter set and now it is being deleted by setting it to zero.
	 * Additionally check for the condition where there was a port
	 * VLAN but now there is a new and different port VLAN being set.
	 * Before deleting all the old VLAN filters we must add new ones
	 * with -1 (I40E_VLAN_ANY) or otherwise we're left with all our
	 * MAC addresses deleted.
	 */
	if ((!(vlan_id || qos) ||
	     vlanprio != le16_to_cpu(*pvid)) &&
	    *pvid) {
		ret = i40e_add_vlan_all_mac(vsi, 0);
		if (ret) {
			dev_info(&vsi->back->pdev->dev,
				 "add VF VLAN failed, ret=%d aq_err=%d\n", ret,
				 vsi->back->hw.aq.asq_last_status);
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_pvid;
		}
	}

	if (*pvid) {
		s16 mask = VLAN_VID_MASK;

		mask &= ~(0x1);
		/* remove all filters on the old VLAN */
		i40e_rm_vlan_all_mac(vsi, (le16_to_cpu(*pvid) &
					   mask));
	}

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* disable promisc modes in case they were enabled */
	ret = i40e_config_vf_promiscuous_mode(vf, vf->lan_vsi_id,
					      allmulti, alluni);
	if (ret) {
		dev_err(&pf->pdev->dev, "Unable to config VF promiscuous mode\n");
		goto error_pvid;
	}

	if (vlan_id || qos) {
		ret = i40e_vsi_add_pvid(vsi, vlanprio);
		if (ret) {
			dev_info(&vsi->back->pdev->dev,
				 "add VF VLAN failed, ret=%d aq_err=%d\n", ret,
				 vsi->back->hw.aq.asq_last_status);
			goto error_pvid;
		}
		/* as there is no MacVlan pair left, set
		 * allow_untagged to off
		 */
		vf->allow_untagged = false;
	} else {
		i40e_vsi_remove_pvid(vsi);
#ifdef HAVE_NDO_SET_VF_LINK_STATE
		/* if vlan is being removed then clear also trunk_vlan */
		if (!(*pvid))
			memset(vf->trunk_vlans, 0,
			       BITS_TO_LONGS(VLAN_N_VID) * sizeof(long));
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
		vf->allow_untagged = true;
	}
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	if (vlan_id) {
#ifdef HAVE_NDO_SET_VF_LINK_STATE
		int tmp;

#endif /* HAVE_NDO_SET_VF_LINK_STATE */
		dev_info(&pf->pdev->dev, "Setting VLAN %d, QOS 0x%x on VF %d\n",
			 vlan_id, qos, vf_id);

		/* add new VLAN filter for each MAC */
		ret = i40e_add_vlan_all_mac(vsi, vlan_id);
		if (ret) {
			dev_info(&vsi->back->pdev->dev,
				 "add VF VLAN failed, ret=%d aq_err=%d\n", ret,
				 vsi->back->hw.aq.asq_last_status);
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_pvid;
		}
#ifdef HAVE_NDO_SET_VF_LINK_STATE
		/* only pvid should be present in trunk */
		clear_bit(le16_to_cpu(*pvid), vf->trunk_vlans);
		for_each_set_bit(tmp, vf->trunk_vlans,
				 BITS_TO_LONGS(VLAN_N_VID) * sizeof(long)) {
			if (tmp != 0)
				i40e_rm_vlan_all_mac(vsi, tmp);
		}
		memset(vf->trunk_vlans, 0,
		       BITS_TO_LONGS(VLAN_N_VID) * sizeof(long));
		set_bit(le16_to_cpu(*pvid), vf->trunk_vlans);

		vf->allow_untagged = false;
		vsi->flags |= I40E_VSI_FLAG_FILTER_CHANGED;
		set_bit(__I40E_MACVLAN_SYNC_PENDING, vsi->back->state);
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
	}

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	if (test_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states))
		alluni = true;

	if (test_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states))
		allmulti = true;

	/* The Port VLAN needs to be saved across resets the same as the
	 * default LAN MAC address.
	 */
	vf->port_vlan_id = le16_to_cpu(*pvid);
	if (*pvid) {
		ret = i40e_config_vf_promiscuous_mode(vf,
						      vsi->id,
						      allmulti,
						      alluni);
		if (ret) {
			dev_err(&pf->pdev->dev, "Unable to config vf promiscuous mode\n");
			goto error_pvid;
		}
	}

	/* Schedule the worker thread to take care of applying changes */
	i40e_service_event_schedule(vsi->back);

error_pvid:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_ndo_set_vf_bw
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @min_tx_rate: Minimum Tx rate
 * @max_tx_rate: Maximum Tx rate
 *
 * configure VF Tx rate
 **/
#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
int i40e_ndo_set_vf_bw(struct net_device *netdev, int vf_id, int min_tx_rate,
		       int max_tx_rate)
#else
int i40e_ndo_set_vf_bw(struct net_device *netdev, int vf_id, int max_tx_rate)
#endif
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error;
	}

	ret = i40e_set_bw_limit(vsi, vsi->seid, max_tx_rate);
	if (ret)
		goto error;

	vf->tx_rate = max_tx_rate;
error:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_ndo_enable_vf
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @enable: true to enable & false to disable
 *
 * enable/disable VF
 **/
int i40e_ndo_enable_vf(struct net_device *netdev, int vf_id, bool enable)
{
	return -EOPNOTSUPP;
}

/**
 * i40e_ndo_get_vf_config
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @ivi: VF configuration structure
 *
 * return VF configuration
 **/
int i40e_ndo_get_vf_config(struct net_device *netdev,
			   int vf_id, struct ifla_vf_info *ivi)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_vf *vf;
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_param;

	vf = &pf->vf[vf_id];
	/* first vsi is always the LAN vsi */
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		ret = -ENOENT;
		goto error_param;
	}

	ivi->vf = vf_id;

	ether_addr_copy(ivi->mac, vf->default_lan_addr.addr);

#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
	ivi->max_tx_rate = vf->tx_rate;
	ivi->min_tx_rate = 0;
#else
	ivi->tx_rate = vf->tx_rate;
#endif
	if (vsi->info.pvid) {
		ivi->vlan = le16_to_cpu(vsi->info.pvid) & I40E_VLAN_MASK;
		ivi->qos = (le16_to_cpu(vsi->info.pvid) &
			    I40E_PRIORITY_MASK) >> I40E_VLAN_PRIORITY_SHIFT;
	} else {
		ivi->vlan = le16_to_cpu(vsi->info.outer_vlan) & I40E_VLAN_MASK;
		ivi->qos = (le16_to_cpu(vsi->info.outer_vlan) &
			    I40E_PRIORITY_MASK) >> I40E_VLAN_PRIORITY_SHIFT;
	}

#ifdef HAVE_NDO_SET_VF_LINK_STATE
	if (vf->link_forced == false)
		ivi->linkstate = IFLA_VF_LINK_STATE_AUTO;
	else if (vf->link_up == true)
		ivi->linkstate = IFLA_VF_LINK_STATE_ENABLE;
	else
		ivi->linkstate = IFLA_VF_LINK_STATE_DISABLE;
#endif
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
	ivi->spoofchk = vf->mac_anti_spoof;
#endif
#ifdef HAVE_NDO_SET_VF_TRUST
	ivi->trusted = vf->trusted;
#endif /* HAVE_NDO_SET_VF_TRUST */
	ret = 0;

error_param:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

#ifdef HAVE_NDO_SET_VF_LINK_STATE
/**
 * i40e_ndo_set_vf_link_state
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @link: required link state
 *
 * Set the link state of a specified VF, regardless of physical link state
 **/
int i40e_ndo_set_vf_link_state(struct net_device *netdev, int vf_id, int link)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_link_status *ls = &pf->hw.phy.link_info;
	struct virtchnl_pf_event pfe;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int abs_vf_id;
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto error_out;
	}

	vf = &pf->vf[vf_id];
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;

	switch (link) {
	case IFLA_VF_LINK_STATE_AUTO:
		vf->link_forced = false;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	case IFLA_VF_LINK_STATE_ENABLE:
		vf->link_forced = true;
		vf->link_up = true;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		vf->link_forced = true;
		vf->link_up = false;
		i40e_set_vf_link_state(vf, &pfe, ls);
		break;
	default:
		ret = -EINVAL;
		goto error_out;
	}

	/* Notify the VF of its new link state */
	i40e_aq_send_msg_to_vf(hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       I40E_SUCCESS, (u8 *)&pfe, sizeof(pfe), NULL);

error_out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

#endif /* HAVE_NDO_SET_VF_LINK_STATE */
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
/**
 * i40e_ndo_set_vf_spoofchk
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @enable: flag to enable or disable feature
 *
 * Enable or disable VF spoof checking
 **/
int i40e_ndo_set_vf_spoofchk(struct net_device *netdev, int vf_id, bool enable)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_vsi_context ctxt;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto out;
	}

	vf = &(pf->vf[vf_id]);
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto out;
	}

	if (enable == vf->mac_anti_spoof)
		goto out;

	vf->mac_anti_spoof = enable;
	memset(&ctxt, 0, sizeof(ctxt));
	ctxt.seid = pf->vsi[vf->lan_vsi_idx]->seid;
	ctxt.pf_num = pf->hw.pf_id;
	ctxt.info.valid_sections = cpu_to_le16(I40E_AQ_VSI_PROP_SECURITY_VALID);
	if (enable)
		ctxt.info.sec_flags |= I40E_AQ_VSI_SEC_FLAG_ENABLE_MAC_CHK;
	ret = i40e_aq_update_vsi_params(hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d updating VSI parameters\n",
			ret);
		ret = -EIO;
	}
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

#endif /* HAVE_VF_SPOOFCHK_CONFIGURE */
#ifdef HAVE_NDO_SET_VF_TRUST
/**
 * i40e_ndo_set_vf_trust
 * @netdev: network interface device structure of the pf
 * @vf_id: VF identifier
 * @setting: trust setting
 *
 * Enable or disable VF trust setting
 **/
int i40e_ndo_set_vf_trust(struct net_device *netdev, int vf_id, bool setting)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vf *vf;
	int ret = 0;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto out;
	}

	if (pf->flags & I40E_FLAG_MFP_ENABLED) {
		dev_err(&pf->pdev->dev, "Trusted VF not supported in MFP mode.\n");
		ret = -EINVAL;
		goto out;
	}

	vf = &pf->vf[vf_id];

	/* if vf is in base mode, make it untrusted */
	if (pf->vf_base_mode_only)
		setting = false;
	if (setting == vf->trusted)
		goto out;

	vf->trusted = setting;
	i40e_vc_reset_vf(vf, true);
	dev_info(&pf->pdev->dev, "VF %u is now %strusted\n",
		 vf_id, setting ? "" : "un");

#ifdef __TC_MQPRIO_MODE_MAX
	if (vf->adq_enabled) {
		if (!vf->trusted) {
			dev_info(&pf->pdev->dev,
				 "VF %u no longer Trusted, deleting all cloud filters\n",
				 vf_id);
			i40e_del_all_cloud_filters(vf);
		}
	}
#endif /* __TC_MQPRIO_MODE_MAX */

out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}
#endif /* HAVE_NDO_SET_VF_TRUST */
#ifdef HAVE_VF_STATS

/**
 * i40e_get_vf_stats - populate some stats for the VF
 * @netdev: the netdev of the PF
 * @vf_id: the host OS identifier (0-127)
 * @vf_stats: pointer to the OS memory to be initialized
 */
int i40e_get_vf_stats(struct net_device *netdev, int vf_id,
		      struct ifla_vf_stats *vf_stats)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_eth_stats *stats;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;

	/* validate the request */
	if (i40e_validate_vf(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d in reset. Try again.\n", vf_id);
		return -EBUSY;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi)
		return -EINVAL;

	i40e_update_eth_stats(vsi);
	stats = &vsi->eth_stats;

	memset(vf_stats, 0, sizeof(*vf_stats));

	vf_stats->rx_packets = stats->rx_unicast + stats->rx_broadcast +
		stats->rx_multicast;
	vf_stats->tx_packets = stats->tx_unicast + stats->tx_broadcast +
		stats->tx_multicast;
	vf_stats->rx_bytes   = stats->rx_bytes;
	vf_stats->tx_bytes   = stats->tx_bytes;
	vf_stats->broadcast  = stats->rx_broadcast;
	vf_stats->multicast  = stats->rx_multicast;
#ifdef HAVE_VF_STATS_DROPPED
	vf_stats->rx_dropped = stats->rx_discards;
	vf_stats->tx_dropped = stats->tx_discards;
#endif

	return 0;
}
#endif /* HAVE_VF_STATS */
#endif /* IFLA_VF_MAX */
#ifdef HAVE_NDO_SET_VF_LINK_STATE

/**
 * i40e_get_vlan_anti_spoof
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: on success, true if enabled, false if not
 *
 * This function queries if VLAN anti spoof is enabled or not
 *
 * Returns 0 on success, negative on failure.
 **/
static int i40e_get_vlan_anti_spoof(struct pci_dev *pdev, int vf_id,
				    bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	if ((vsi->info.valid_sections &
	    CPU_TO_LE16(I40E_AQ_VSI_PROP_SECURITY_VALID)) &&
	    (vsi->info.sec_flags & I40E_AQ_VSI_SEC_FLAG_ENABLE_VLAN_CHK))
		*enable = true;
	else
		*enable = false;
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_vlan_anti_spoof
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable/disable
 *
 * This function enables or disables VLAN anti-spoof
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_vlan_anti_spoof(struct pci_dev *pdev, int vf_id,
				    const bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	u8 sec_flag;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}
	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	sec_flag = I40E_AQ_VSI_SEC_FLAG_ENABLE_VLAN_CHK;
	ret = i40e_set_spoof_settings(vsi, sec_flag, enable);
	if (!ret)
		vf->vlan_anti_spoof = enable;
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_mac_anti_spoof
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: on success, true if enabled, false if not
 *
 * This function queries if MAC anti spoof is enabled or not.
 *
 * Returns 0 on success, negative error on failure.
 **/
static int i40e_get_mac_anti_spoof(struct pci_dev *pdev, int vf_id,
				   bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	*enable = vf->mac_anti_spoof;
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_mac_anti_spoof
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable/disable
 *
 * This function enables or disables MAC anti-spoof
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_mac_anti_spoof(struct pci_dev *pdev, int vf_id,
				   const bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	u8 sec_flag;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	sec_flag = I40E_AQ_VSI_SEC_FLAG_ENABLE_MAC_CHK;
	ret = i40e_set_spoof_settings(vsi, sec_flag, enable);
	if (!ret)
		vf->mac_anti_spoof = enable;
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_trunk - Gets the configured VLAN filters
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @trunk_vlans: trunk vlans
 *
 * Gets the active trunk vlans
 *
 * Returns the number of active vlans filters on success,
 * negative on failure
 **/
static int i40e_get_trunk(struct pci_dev *pdev, int vf_id,
			  unsigned long *trunk_vlans)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	/* checking if pvid has been set through netdev */
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (i40e_is_vid(&vsi->info)) {
		memset(trunk_vlans, 0,
		       BITS_TO_LONGS(VLAN_N_VID) * sizeof(long));
		if (vsi->info.pvid)
			set_bit(le16_to_cpu(vsi->info.pvid), trunk_vlans);
		else
			set_bit(le16_to_cpu(vsi->info.outer_vlan), trunk_vlans);
	} else {
		bitmap_copy(trunk_vlans, vf->trunk_vlans, VLAN_N_VID);
	}

	bitmap_copy(trunk_vlans, vf->trunk_vlans, VLAN_N_VID);
	ret = bitmap_weight(trunk_vlans, VLAN_N_VID);
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_trunk - Configure VLAN filters
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @vlan_bitmap: vlans to filter on
 *
 * Applies the VLAN filters
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_trunk(struct pci_dev *pdev, int vf_id,
			  const unsigned long *vlan_bitmap)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;
	u16 vid;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_vlan_stripping_enable(vsi);

	/* checking if pvid has been set through netdev */
	vid = __le16_to_cpu(*i40e_get_current_vid(vsi));

	if (vid) {
		i40e_vsi_remove_pvid(vsi);
		/* Remove pvid and vlan 0 from trunk */
		clear_bit(vid, vf->trunk_vlans);
		clear_bit(0, vf->trunk_vlans);
	}

	if (bitmap_weight(vlan_bitmap, VLAN_N_VID) && !vf->trunk_set_by_pf)
		i40e_free_vmvlan_list(vsi, vf);

	/* Add vlans */
	for_each_set_bit(vid, vlan_bitmap, VLAN_N_VID) {
		if (!test_bit(vid, vf->trunk_vlans)) {
			ret = i40e_vsi_add_vlan(vsi, vid);
			if (ret)
				goto out;
		}
	}

	/* If to empty trunk filter is added, remove I40E_VLAN_ANY.
	 * Removal of this filter sets allow_untagged to false.
	 */
	if (bitmap_weight(vlan_bitmap, VLAN_N_VID) &&
	    !bitmap_weight(vf->trunk_vlans, VLAN_N_VID)) {
		vf->allow_untagged = false;
		vf->trunk_set_by_pf = true;
	}

	/* If deleting all vlan filters, check if we have VLAN 0 filters
	 * existing. If we don't, add filters to allow all traffic i.e
	 * VLAN tag = -1 before deleting all filters because the in the
	 * delete all filters flow, we check if there are VLAN 0 filters
	 * and then replace them with filters of VLAN id = -1
	 */
	if (!bitmap_weight(vlan_bitmap, VLAN_N_VID)) {
		vf->allow_untagged = true;
		vf->trunk_set_by_pf = false;
	}

	/* Del vlans */
	for_each_set_bit(vid, vf->trunk_vlans, VLAN_N_VID) {
		if (!test_bit(vid, vlan_bitmap))
			i40e_vsi_kill_vlan(vsi, vid);
	}
	/* Copy over the updated bitmap */
	bitmap_copy(vf->trunk_vlans, vlan_bitmap, VLAN_N_VID);
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_mirror - Gets the configured  VLAN mirrors
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mirror_vlans: mirror vlans
 *
 * Gets the active mirror vlans
 *
 * Returns the number of active mirror vlans on success,
 * negative on failure
 **/
static int i40e_get_mirror(struct pci_dev *pdev, int vf_id,
			   unsigned long *mirror_vlans)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	bitmap_copy(mirror_vlans, vf->mirror_vlans, VLAN_N_VID);
	ret = bitmap_weight(mirror_vlans, VLAN_N_VID);
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_mirror - Configure VLAN mirrors
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @vlan_bitmap: vlans to configure as mirrors
 *
 * Configures the mirror vlans
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_mirror(struct pci_dev *pdev, int vf_id,
			   const unsigned long *vlan_bitmap)
{
	u16 vid, sw_seid, dst_seid, rule_id, rule_type;
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int ret, num = 0, cnt, add, status;
	u16 rules_used, rules_free;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	__le16 *mr_list;

	DECLARE_BITMAP(num_vlans, VLAN_N_VID);

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	sw_seid = vsi->uplink_seid;
	dst_seid = vsi->seid;
	rule_type = I40E_AQC_MIRROR_RULE_TYPE_VLAN;
	bitmap_xor(num_vlans, vf->mirror_vlans, vlan_bitmap, VLAN_N_VID);
	cnt = bitmap_weight(num_vlans, VLAN_N_VID);
	if (!cnt)
		goto out;
	mr_list = kcalloc(cnt, sizeof(__le16), GFP_KERNEL);
	if (!mr_list) {
		ret = -ENOMEM;
		goto out;
	}

	/* figure out if adding or deleting */
	bitmap_and(num_vlans, vlan_bitmap, num_vlans, VLAN_N_VID);
	add = bitmap_weight(num_vlans, VLAN_N_VID);
	if (add) {
		/* Add mirrors */
		for_each_set_bit(vid, vlan_bitmap, VLAN_N_VID) {
			if (!test_bit(vid, vf->mirror_vlans)) {
				mr_list[num] = CPU_TO_LE16(vid);
				num++;
			}
		}
		status = i40e_aq_add_mirrorrule(&pf->hw, sw_seid,
						rule_type, dst_seid,
						cnt, mr_list, NULL,
						&rule_id, &rules_used,
						&rules_free);

		if (pf->hw.aq.asq_last_status == I40E_AQ_RC_ENOSPC)
			dev_warn(&pdev->dev, "Not enough resources to assign a mirror rule. Maximum limit of mirrored VLANs is 192.\n");

		if (status == I40E_ERR_ADMIN_QUEUE_ERROR && cnt == 1) {
			dev_warn(&pdev->dev, "Unable to add vlan mirror rule to VF %d.\n", vf_id);
			ret = -EPERM;
			goto err_free;
		}

		if (status) {
			ret = -EINVAL;
			goto err_free;
		}
		vf->vlan_rule_id = rule_id;
	} else {
		/* Del mirrors */
		for_each_set_bit(vid, vf->mirror_vlans, VLAN_N_VID) {
			if (!test_bit(vid, vlan_bitmap)) {
				mr_list[num] = CPU_TO_LE16(vid);
				num++;
			}
		}
		status = i40e_aq_delete_mirrorrule(&pf->hw, sw_seid, rule_type,
						   vf->vlan_rule_id, cnt, mr_list,
						   NULL, &rules_used,
						   &rules_free);
		if (status) {
			ret = -EINVAL;
			goto err_free;
		}
	}

	/* Copy over the updated bitmap */
	bitmap_copy(vf->mirror_vlans, vlan_bitmap, VLAN_N_VID);
err_free:
	kfree(mr_list);
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_allow_untagged
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @on: on or off
 *
 * This functions checks if the untagged packets
 * are allowed or not.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_allow_untagged(struct pci_dev *pdev, int vf_id,
				   bool *on)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	*on = vf->allow_untagged;
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_allow_untagged
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @on: on or off
 *
 * This functions allows or stops untagged packets
 * on the VF.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_allow_untagged(struct pci_dev *pdev, int vf_id,
				   const bool on)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	if (i40e_is_vid(&vsi->info) && on)
		dev_info(&pf->pdev->dev,
			 "VF has port VLAN configured, setting allow_untagged to on\n");

	i40e_service_event_schedule(vsi->back);
	vf->allow_untagged = on;

	vsi->flags |= I40E_VSI_FLAG_FILTER_CHANGED;
	set_bit(__I40E_MACVLAN_SYNC_PENDING, vsi->back->state);
out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_loopback
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function checks loopback is enabled
 *
 * Returns 1 if enabled, 0 if disabled
 **/
static int i40e_get_loopback(struct pci_dev *pdev, int vf_id, bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	*enable = vf->loopback;
err_out:
	return ret;
}

/**
 * i40e_set_loopback
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function enables or disables loopback
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_loopback(struct pci_dev *pdev, int vf_id,
			     const bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_configure_vf_loopback(vsi, vf_id, enable);
	if (!ret)
		vf->loopback = enable;
err_out:
	return ret;
}

/**
 * i40e_get_vlan_strip
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable or disable
 *
 * This function checks if vlan stripping is enabled or not
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_vlan_strip(struct pci_dev *pdev, int vf_id, bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	*enable = vf->vlan_stripping;
err_out:
	return ret;
}

/**
 * i40e_set_vlan_strip
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable/disable
 *
 * This function enables or disables VLAN stripping on a VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_vlan_strip(struct pci_dev *pdev, int vf_id,
			       const bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_configure_vf_vlan_stripping(vsi, vf_id, enable);
	if (ret)
		goto err_out;
	vf->vlan_stripping = enable;

	if (enable)
		ret = i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_VLAN_STRIPPING,
					      I40E_SUCCESS);
	else
		ret = i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING,
					      I40E_SUCCESS);
err_out:
	return ret;
}

/**
 * i40e_reset_vf_stats
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 *
 * This function resets all the stats for the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_reset_vf_stats(struct pci_dev *pdev, int vf_id)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_vsi_reset_stats(vsi);

err_out:
	return ret;
}

/**
 * i40e_get_vf_bw_share
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @bw_share: bw share of the VF
 *
 * This function retrieves the bw share configured for the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_vf_bw_share(struct pci_dev *pdev, int vf_id, u8 *bw_share)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;

	vf = &pf->vf[vf_id];

	if (vf->tc_bw_share_req) {
		ret = -EPERM;
		goto err_out;
	}

	if (vf->bw_share_applied)
		*bw_share = vf->bw_share;
	else
		ret = -EINVAL;

err_out:
	return ret;
}

/**
 * i40e_store_vf_bw_share
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @bw_share: bw share of the VF
 *
 * This function stores bw share configured for the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_store_vf_bw_share(struct pci_dev *pdev, int vf_id, u8 bw_share)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];

	if (vf->tc_bw_share_req)
		return -EPERM;

	vf->bw_share = bw_share;

	/* this tracking bool is set to true when 'apply' attribute is used */
	vf->bw_share_applied = false;
	pf->vf_bw_applied = false;
err_out:
	return ret;
}

/**
 * i40e_get_link_state
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enabled: link state
 * @link_speed: link speed of the VF
 *
 * Gets the status of link and the link speed
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_link_state(struct pci_dev *pdev, int vf_id, bool *enabled,
			       enum vfd_link_speed *link_speed)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_link_status *ls;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	ls = &pf->hw.phy.link_info;
	if (vf->link_forced)
		*enabled = vf->link_up;
	else
		*enabled = ls->link_info & I40E_AQ_LINK_UP;
	switch (ls->link_speed) {
	case I40E_LINK_SPEED_UNKNOWN:
		*link_speed = VFD_LINK_SPEED_UNKNOWN;
		break;
	case I40E_LINK_SPEED_100MB:
		*link_speed = VFD_LINK_SPEED_100MB;
		break;
	case I40E_LINK_SPEED_1GB:
		*link_speed = VFD_LINK_SPEED_1GB;
		break;
	case I40E_LINK_SPEED_2_5GB:
		*link_speed = VFD_LINK_SPEED_2_5GB;
		break;
	case I40E_LINK_SPEED_5GB:
		*link_speed = VFD_LINK_SPEED_5GB;
		break;
	case I40E_LINK_SPEED_10GB:
		*link_speed = VFD_LINK_SPEED_10GB;
		break;
	case I40E_LINK_SPEED_20GB:
		*link_speed = VFD_LINK_SPEED_20GB;
		break;
	case I40E_LINK_SPEED_25GB:
		*link_speed = VFD_LINK_SPEED_25GB;
		break;
	case I40E_LINK_SPEED_40GB:
		*link_speed = VFD_LINK_SPEED_40GB;
		break;
	default:
		*link_speed = VFD_LINK_SPEED_UNKNOWN;
	}
err_out:
	return ret;
}

/**
 * i40e_set_link_state
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @link: the link state to configure
 *
 * Configures link for a vf
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_link_state(struct pci_dev *pdev, int vf_id, const u8 link)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_out;
	vf = &pf->vf[vf_id];
	ret = i40e_configure_vf_link(vf, link);
error_out:
	return ret;
}

#ifdef CONFIG_DCB
/**
 * i40e_enable_vf_queues
 * @vsi: PCI device information struct
 * @enable: VF identifier
 *
 * Disable/enable VF queues.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_enable_vf_queues(struct i40e_vsi *vsi, bool enable)
{
	struct i40e_pf *pf = vsi->back;
	int vf_id = -1, v, ret;
	unsigned long q_map;
	struct i40e_vf *vf;

	if (!pf->vf)
		return 0;

	for (v = 0; v < pf->num_alloc_vfs; v++) {
		if (pf->vsi[pf->vf[v].lan_vsi_idx] == vsi) {
			vf_id = v;
			break;
		}
	}

	if (vf_id == -1) {
		ret = -ENOENT;
		goto err_out;
	}

	vf = &pf->vf[vf_id];
	q_map = BIT(vsi->num_queue_pairs) - 1;
	if (!enable) {
		ret = i40e_set_link_state(pf->pdev, vf_id, VFD_LINKSTATE_OFF);
		if (ret)
			goto err_out;
	}
	ret = i40e_ctrl_vf_tx_rings(vsi, q_map, enable);
	if (ret)
		goto err_out;
	ret = i40e_ctrl_vf_rx_rings(vsi, q_map, enable);
	if (ret)
		goto err_out;

	if (enable) {
		i40e_vc_notify_vf_reset(vf);
		i40e_reset_vf(vf, false);
		ret = i40e_set_link_state(pf->pdev, vf_id, VFD_LINKSTATE_AUTO);
	}
err_out:
	return ret;
}
#endif /* CONFIG_DCB */

/**
 * i40e_set_vf_enable
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable/disable
 *
 * This function enables or disables a VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_vf_enable(struct pci_dev *pdev, int vf_id,
			      const bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	unsigned long q_map;
	struct i40e_vf *vf;
	int ret, tmp;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];

	/* allow the VF to get enabled */
	if (enable) {
		vf->pf_ctrl_disable = false;
		/* reset needed to reinit VF resources */
		i40e_vc_reset_vf(vf, true);
		ret = i40e_set_link_state(pdev, vf_id, VFD_LINKSTATE_AUTO);
	} else {
		vsi = pf->vsi[vf->lan_vsi_idx];
		q_map = BIT(vsi->num_queue_pairs) - 1;

		/* force link down to prevent tx hangs */
		ret = i40e_set_link_state(pdev, vf_id, VFD_LINKSTATE_OFF);
		if (ret)
			goto err_out;
		vf->pf_ctrl_disable = true;

		/* Try to stop both Tx&Rx rings even if one of the calls fails
		 * to ensure we stop the rings even in case of errors.
		 * If any of them returns with an error then the first
		 * error that occurred will be returned.
		 */
		tmp = i40e_ctrl_vf_tx_rings(vsi, q_map, enable);
		ret = i40e_ctrl_vf_rx_rings(vsi, q_map, enable);

		ret = tmp ? tmp : ret;
	}

err_out:
	return ret;
}

static int i40e_get_vf_enable(struct pci_dev *pdev, int vf_id, bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];
	*enable = !vf->pf_ctrl_disable;
	return 0;
}

/**
 * i40e_get_rx_bytes
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @rx_bytes: pointer to the caller's rx_bytes variable
 *
 * This function gets the received bytes on the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_rx_bytes(struct pci_dev *pdev, int vf_id,
			     u64 *rx_bytes)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*rx_bytes = vsi->eth_stats.rx_bytes;
err_out:
	return ret;
}

/**
 * i40e_get_rx_dropped
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @rx_dropped: pointer to the caller's rx_dropped variable
 *
 * This function gets the dropped received bytes on the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_rx_dropped(struct pci_dev *pdev, int vf_id,
			       u64 *rx_dropped)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*rx_dropped = vsi->eth_stats.rx_discards;
err_out:
	return ret;
}

/**
 * i40e_get_rx_packets
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @rx_packets: pointer to the caller's rx_packets variable
 *
 * This function gets the number of packets received on the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_rx_packets(struct pci_dev *pdev, int vf_id,
			       u64 *rx_packets)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*rx_packets = vsi->eth_stats.rx_unicast + vsi->eth_stats.rx_multicast +
		      vsi->eth_stats.rx_broadcast;
err_out:
	return ret;
}

/**
 * i40e_get_tx_bytes
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tx_bytes: pointer to the caller's tx_bytes variable
 *
 * This function gets the transmitted bytes by the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_tx_bytes(struct pci_dev *pdev, int vf_id,
			     u64 *tx_bytes)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*tx_bytes = vsi->eth_stats.tx_bytes;
err_out:
	return ret;
}

/**
 * i40e_get_tx_dropped
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tx_dropped: pointer to the caller's tx_dropped variable
 *
 * This function gets the dropped tx bytes by the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_tx_dropped(struct pci_dev *pdev, int vf_id,
			       u64 *tx_dropped)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*tx_dropped = vsi->eth_stats.tx_discards;
err_out:
	return ret;
}

/**
 * i40e_get_tx_packets
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tx_packets: pointer to the caller's tx_packets variable
 *
 * This function gets the number of packets transmitted by the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_tx_packets(struct pci_dev *pdev, int vf_id,
			       u64 *tx_packets)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*tx_packets = vsi->eth_stats.tx_unicast + vsi->eth_stats.tx_multicast +
		      vsi->eth_stats.tx_broadcast;
err_out:
	return ret;
}

/**
 * i40e_get_tx_errors
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tx_errors: pointer to the caller's tx_errors variable
 *
 * This function gets the number of packets transmitted by the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_tx_errors(struct pci_dev *pdev, int vf_id,
			      u64 *tx_errors)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	i40e_update_eth_stats(vsi);
	*tx_errors = vsi->eth_stats.tx_errors;
err_out:
	return ret;
}

/**
 * i40e_get_mac
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mac: the default mac address
 *
 * This function gets the default mac address
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_mac(struct pci_dev *pdev, int vf_id, u8 *mac)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	ether_addr_copy(mac, vf->default_lan_addr.addr);
err_out:
	return ret;
}

/**
 * i40e_set_mac
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mac: the default mac address to set
 *
 * This function sets the default mac address for the VF
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_mac(struct pci_dev *pdev, int vf_id, const u8 *mac)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_set_vf_mac(vf, vsi, mac);
err_out:
	return ret;
}

/**
 * i40e_get_promisc
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @promisc_mode: current promiscuous mode
 *
 * This function gets the current promiscuous mode configuration.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_promisc(struct pci_dev *pdev, int vf_id, u8 *promisc_mode)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	*promisc_mode = vf->promisc_mode;
err_out:
	return ret;
}

/**
 * i40e_set_promisc
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @promisc_mode: promiscuous mode to be set
 *
 * This function sets the promiscuous mode configuration.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_promisc(struct pci_dev *pdev, int vf_id,
			    const u8 promisc_mode)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_configure_vf_promisc_mode(vf, vsi, promisc_mode);
err:
	return ret;
}

/**
 * i40e_get_ingress_mirror - Gets the configured ingress mirror
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mirror: pointer to return the ingress mirror
 *
 * Gets the ingress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_ingress_mirror(struct pci_dev *pdev, int vf_id, int *mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	*mirror = vf->ingress_vlan;
err_out:
	return ret;
}

/**
 * i40e_set_ingress_mirror - Configure ingress mirror
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mirror: mirror vf
 *
 * Configures the ingress mirror
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_ingress_mirror(struct pci_dev *pdev, int vf_id,
				   const int mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *src_vsi, *mirror_vsi;
	struct i40e_vf *vf, *mirror_vf;
	u16 rule_type, rule_id;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];

	/* The Admin Queue mirroring rules refer to the traffic
	 * directions from the perspective of the switch, not the VSI
	 * we apply the mirroring rule on - so the behaviour of a VSI
	 * ingress mirror is classified as an egress rule
	 */
	rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_EGRESS;
	src_vsi = pf->vsi[vf->lan_vsi_idx];
	if (mirror == I40E_NO_VF_MIRROR) {
		/* Del mirrors */
		rule_id = vf->ingress_rule_id;
		ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
						     rule_id);
		if (ret)
			goto err_out;
		vf->ingress_vlan = I40E_NO_VF_MIRROR;
	} else {
		/* validate the mirror */
		ret = i40e_validate_vf(pf, mirror);
		if (ret)
			goto err_out;
		mirror_vf = &pf->vf[mirror];
		mirror_vsi = pf->vsi[mirror_vf->lan_vsi_idx];

		/* Add mirrors */
		ret = i40e_add_ingress_egress_mirror(src_vsi, mirror_vsi,
						     rule_type, &rule_id);
		if (ret)
			goto err_out;
		vf->ingress_vlan = mirror;
		vf->ingress_rule_id = rule_id;
	}
err_out:
	return ret;
}

/**
 * i40e_get_egress_mirror - Gets the configured egress mirror
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mirror: pointer to return the egress mirror
 *
 * Gets the egress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_egress_mirror(struct pci_dev *pdev, int vf_id, int *mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];
	*mirror = vf->egress_vlan;
err_out:
	return ret;
}

/**
 * i40e_set_egress_mirror - Configure egress mirror
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @mirror: mirror vf
 *
 * Configures the egress mirror
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_egress_mirror(struct pci_dev *pdev, int vf_id,
				  const int mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *src_vsi, *mirror_vsi;
	struct i40e_vf *vf, *mirror_vf;
	u16 rule_type, rule_id;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err_out;
	vf = &pf->vf[vf_id];

	/* The Admin Queue mirroring rules refer to the traffic
	 * directions from the perspective of the switch, not the VSI
	 * we apply the mirroring rule on - so the behaviour of a VSI
	 * egress mirror is classified as an ingress rule
	 */
	rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_INGRESS;
	src_vsi = pf->vsi[vf->lan_vsi_idx];
	if (mirror == I40E_NO_VF_MIRROR) {
		/* Del mirrors */
		rule_id = vf->egress_rule_id;
		ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
						     rule_id);
		if (ret)
			goto err_out;
		vf->egress_vlan = I40E_NO_VF_MIRROR;
	} else {
		/* validate the mirror */
		ret = i40e_validate_vf(pf, mirror);
		if (ret)
			goto err_out;
		mirror_vf = &pf->vf[mirror];
		mirror_vsi = pf->vsi[mirror_vf->lan_vsi_idx];

		/* Add mirrors */
		ret = i40e_add_ingress_egress_mirror(src_vsi, mirror_vsi,
						     rule_type, &rule_id);
		if (ret)
			goto err_out;
		vf->egress_vlan = mirror;
		vf->egress_rule_id = rule_id;
	}
err_out:
	return ret;
}

/*
 * i40e_get_mac_list
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @list_head: list of mac addresses
 *
 * This function returns the list of mac address configured on the VF. It is
 * the responsibility of the caller to free the allocated list when finished.
 *
 * Returns 0 on success, negative on failure
 */
static int i40e_get_mac_list(struct pci_dev *pdev, int vf_id,
			     struct list_head *mac_list)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	ret = i40e_copy_mac_list_sync(vsi, mac_list);
error_out:
	return ret;
}

#define I40E_MAC_FILTERS_LIMIT (PAGE_SIZE / (3 * ETH_ALEN))
/* determined by kernel: ((1024 - header) / (3 * ETH_ALEN)) = 51 */
#define I40E_MAC_LISTING_LIMIT 51

/*
 * i40e_add_macs_to_list
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @list_head: list of mac addresses
 *
 * This function adds a list of mac addresses for a VF
 *
 * Returns 0 on success, negative on failure
 */
static int i40e_add_macs_to_list(struct pci_dev *pdev, int vf_id,
				 struct list_head *mac_list)
{
	unsigned int mac_num_allowed, mac_num_list = 0;
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int off_limits_count = 0, idx = 0;
	struct i40e_mac_filter *f;
	struct vfd_macaddr *tmp;
	char *off_limits = NULL;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret, bkt;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	hash_for_each(vsi->mac_filter_hash, bkt, f, hlist)
		mac_num_list++;

	mac_num_allowed = I40E_MAC_FILTERS_LIMIT - mac_num_list;
	off_limits = kzalloc(I40E_MAC_LISTING_LIMIT * 3 * ETH_ALEN,
			     GFP_ATOMIC);
	if (!off_limits) {
		spin_unlock_bh(&vsi->mac_filter_hash_lock);
		return -ENOMEM;
	}

	list_for_each_entry(tmp, mac_list, list) {
		f = i40e_find_mac(vsi, tmp->mac);
		if (!f && mac_num_allowed) {
			f = i40e_add_mac_filter(vsi, tmp->mac);
			if (!f) {
				dev_err(&pf->pdev->dev,
					"Unable to add MAC filter %pM for VF %d\n",
					tmp->mac, vf->vf_id);
				ret = I40E_ERR_PARAM;
				spin_unlock_bh(&vsi->mac_filter_hash_lock);
				goto error_out;
			}
			mac_num_allowed--;
		} else if (!f && !mac_num_allowed) {
			if (!off_limits_count) {
				idx = scnprintf(off_limits,
						3 * ETH_ALEN,
						"%pM", tmp->mac);
				off_limits_count++;
			} else if (off_limits_count + 1 >=
				   I40E_MAC_LISTING_LIMIT) {
				scnprintf(&off_limits[idx],
					  3 * ETH_ALEN + 1,
					  ",%pM", tmp->mac);
				dev_warn(&pf->pdev->dev,
					 "No more MAC addresses can be added. <%s> not added\n",
					 off_limits);
				off_limits_count = 0;
				idx = 0;
			} else {
				idx += scnprintf(&off_limits[idx],
						 3 * ETH_ALEN + 1,
						 ",%pM", tmp->mac);
				off_limits_count++;
			}
		}
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	if (off_limits_count)
		dev_warn(&pf->pdev->dev,
			 "No more MAC addresses can be added. <%s> not added\n",
			 off_limits);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);
error_out:
	kfree(off_limits);

	return ret;
}

/*
 * i40e_rem_macs_from_list
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @list_head: list of mac addresses
 *
 * This function removes a list of mac addresses from a VF
 *
 * Returns 0 on success, negative on failure
 */
static int i40e_rem_macs_from_list(struct pci_dev *pdev, int vf_id,
				   struct list_head *mac_list)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct vfd_macaddr *tmp;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_out;
	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	spin_lock_bh(&vsi->mac_filter_hash_lock);
	list_for_each_entry(tmp, mac_list, list) {
		if (i40e_del_mac_filter(vsi, tmp->mac)) {
			ret = I40E_ERR_INVALID_MAC_ADDR;
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_out;
		}
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);
error_out:
	return ret;
}

/*
 * i40e_set_pf_qos_apply
 * @pdev: PCI device information struct
 *
 * This function applies the bw shares stored across all VFs.
 * If there are VFs with share configured per traffic class, it configures
 * VEB's TC bandwidth.
 *
 * Returns 0 on success, negative on failure
 */
static int i40e_set_pf_qos_apply(struct pci_dev *pdev)
{
	struct i40e_aqc_configure_vsi_tc_bw_data bw_data;
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int i, j, ret = 0, total_share = 0;
	struct i40e_vf *vf = pf->vf;
	struct i40e_vsi *vsi;
#ifdef CONFIG_DCB
	u16 total_mib_bw[I40E_MAX_TRAFFIC_CLASS] = {0};
	bool reconfig_vf_vsi = false;
	u8 enabled_tc = 0;
	s16 total_bw = 0;
#endif

	for (i = 0; i < pf->num_alloc_vfs; i++, vf++)
		total_share += vf->bw_share;

	/* verify BW share distribution */
	if (total_share > 100) {
		dev_err(&pdev->dev, "Total share is greater than 100 percent");
		return I40E_ERR_PARAM;
	}

	memset(&bw_data, 0,
	       sizeof(struct i40e_aqc_configure_vsi_tc_bw_data));
	for (i = 0; i < pf->num_alloc_vfs; i++) {
		ret = i40e_validate_vf(pf, vf->vf_id);
		if (ret)
			continue;
		vf = &pf->vf[i];
		if (vf->tc_bw_share_req)
			continue;
		if (!vf->bw_share)
			continue;
		if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
			dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
				vf->vf_id);
			ret = I40E_ERR_PARAM;
			goto error_param;
		}
		vsi = pf->vsi[vf->lan_vsi_idx];
		bw_data.tc_valid_bits = 1;
		bw_data.tc_bw_credits[0] = vf->bw_share;

		ret = i40e_aq_config_vsi_tc_bw(&pf->hw, vsi->seid,
					       &bw_data, NULL);
		if (ret) {
			dev_info(&pf->pdev->dev,
				 "AQ command Config VSI BW allocation per TC failed = %d\n",
				 pf->hw.aq.asq_last_status);
			vf->bw_share_applied = false;
			return -EINVAL;
		}

		for (j = 0; j < I40E_MAX_TRAFFIC_CLASS; j++)
			vsi->info.qs_handle[j] = bw_data.qs_handles[j];

		/* set the tracking bool to true */
		vf->bw_share_applied = true;
	}
	pf->vf_bw_applied = true;

#ifdef CONFIG_DCB
	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++)
		if (pf->dcb_user_up_map[i] !=
		    I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY)
			enabled_tc |= BIT(pf->dcb_user_up_map[i]);

	if (pf->dcb_user_reconfig) {
		/* first gather what is set by user */
		for (i = 0, vf = pf->vf; i < pf->num_alloc_vfs; i++, vf++)
			for (j = 0; j < I40E_MAX_TRAFFIC_CLASS; j++) {
				total_bw += vf->tc_info.requested_tc_share[j];
				total_mib_bw[j] +=
					vf->tc_info.requested_tc_share[j];
			}

		/* set missing mib_bw to 100 if it's missing */
		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++)
			if (total_mib_bw[i] == 0 && (enabled_tc & BIT(i))) {
				total_mib_bw[i] = 100;
				total_bw += 100;
			}

		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
			if (total_mib_bw[i] > 100) {
				dev_err(&pdev->dev, "Cannot apply ETS settings, sum of VF share settings for TC %d is different than 100",
					i);
				return I40E_ERR_PARAM;
			}
			if (unlikely(total_bw == 0)) {
				dev_err(&pdev->dev, "Cannot apply ETS settings, total bandwidth used is 0");
				return I40E_ERR_PARAM;
			}
			/* accommodate for total_bw */
			total_mib_bw[i] = total_mib_bw[i] * 100 / total_bw;
		}

		/* assign remaining bw to TC0 */
		total_bw = 0;
		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++)
			total_bw += total_mib_bw[i];
		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
			if (((s16)(total_mib_bw[i]) + 100 - total_bw) > 0) {
				total_mib_bw[i] += 100 - total_bw;
				break;
			}
		}

		for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
			pf->dcb_mib_bw_map[i] = total_mib_bw[i];
			total_mib_bw[i] = 0;
		}

		/* quiesce VFs */
		vf = pf->vf;
		for (i = 0; i < pf->num_alloc_vfs; i++, vf++)
			i40e_enable_vf_queues(pf->vsi[vf->lan_vsi_idx], false);
		/* Configure port to ETS */
		i40e_update_ets(pf);
		pf->dcb_user_reconfig = false;
		vf = pf->vf;
		/* unquiesce VFs */
		for (i = 0; i < pf->num_alloc_vfs; i++, vf++)
			if (!vf->pf_ctrl_disable)
				i40e_enable_vf_queues(pf->vsi[vf->lan_vsi_idx],
						      true);
	}

	/* Reconfig VF VSI for TC */
	for (i = 0, vf = pf->vf; i < pf->num_alloc_vfs; i++, vf++) {
		total_bw = 0;

		for (j = 0; j < I40E_MAX_TRAFFIC_CLASS; j++) {
			/* TC must be continuous */
			if (!(enabled_tc & BIT(j)) &&
			    vf->tc_info.requested_tc_share[j]) {
				dev_info(&pdev->dev, "User tried to set non continuous TC, Not setting TC on VF %d",
					 vf->vf_id);
				for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++)
					vf->tc_info.requested_tc_share[j] = 0;
				continue;
			}
			total_bw += vf->tc_info.requested_tc_share[j];
		}

		ret = i40e_vsi_config_tc(pf->vsi[vf->lan_vsi_idx],
					 enabled_tc);
		if (ret) {
			dev_info(&pdev->dev,
				 "Failed configuring TC for VSI seid=%d\n",
				 pf->vsi[vf->lan_vsi_idx]->seid);
			/* Will try to configure as many components */
		} else {
			reconfig_vf_vsi = true;
			vf->tc_info.applied = true;
		}
	}

	/* exhaust whole TC BW, redistribute remaining TC BW to every VF,
	 * which does not have share assigned to it.
	 */
	vf = pf->vf;
	for (i = 0; i < pf->num_alloc_vfs; i++, vf++) {
		if (!vf->tc_info.applied)
			continue;
		for (j = 0; j < I40E_MAX_TRAFFIC_CLASS; j++)
			total_mib_bw[j] += vf->tc_info.requested_tc_share[j];
	}

	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
		total_bw = 0;

		if (!(pf->vsi[pf->lan_vsi]->tc_config.enabled_tc & BIT(i)))
			break;

		vf = pf->vf;
		for (j = 0; j < pf->num_alloc_vfs; j++, vf++) {
			if (!vf->tc_info.applied)
				continue;

			if (!vf->tc_info.requested_tc_share[i])
				total_bw++;
		}
		total_mib_bw[i] = 100 - total_mib_bw[i];
		if (total_mib_bw[i] && total_bw) {
			total_mib_bw[i] /= total_bw;
			vf = pf->vf;
			for (j = 0; j < pf->num_alloc_vfs; j++, vf++) {
				if (!vf->tc_info.applied)
					continue;
				if (!vf->tc_info.requested_tc_share[i])
					vf->tc_info.requested_tc_share[i] =
						total_mib_bw[i];
			}
		}
	}

	if (reconfig_vf_vsi) {
		for (i = 0, vf = pf->vf; i < pf->num_alloc_vfs; i++, vf++) {
			if (vf->tc_info.applied)
				ret = i40e_apply_vsi_tc_bw
					(vf, vf->tc_info.requested_tc_share);
			if (ret)
				continue;
			for (j = 0; j < I40E_MAX_TRAFFIC_CLASS; j++)
				vf->tc_info.applied_tc_share[j] =
					vf->tc_info.requested_tc_share[j];
		}
	}
#endif /* CONFIG_DCB */
error_param:
	return ret;
}

/**
 * i40e_get_pf_ingress_mirror - Gets the configured ingress mirror for PF
 * @pdev: PCI device information struct
 * @mirror: pointer to return the ingress mirror
 *
 * Gets the ingress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_pf_ingress_mirror(struct pci_dev *pdev, int *mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	*mirror = pf->ingress_vlan;
	return 0;
}

/**
 * i40e_set_pf_ingress_mirror - Sets the configured ingress mirror for PF
 * @pdev: PCI device information struct
 * @mirror: pointer to return the ingress mirror
 *
 * Gets the ingress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_pf_ingress_mirror(struct pci_dev *pdev, const int mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *src_vsi, *mirror_vsi;
	struct i40e_vf *mirror_vf;
	u16 rule_type, rule_id;
	int ret;

	/* The Admin Queue mirroring rules refer to the traffic
	 * directions from the perspective of the switch, not the VSI
	 * we apply the mirroring rule on - so the behaviour of a VSI
	 * ingress mirror is classified as an egress rule
	 */
	rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_EGRESS;
	src_vsi = pf->vsi[pf->lan_vsi];
	if (mirror == I40E_NO_VF_MIRROR) {
		/* Del mirrors */
		rule_id = pf->ingress_rule_id;
		ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
						     rule_id);
		if (ret)
			goto err_out;
		pf->ingress_vlan = I40E_NO_VF_MIRROR;
	} else {
		/* validate the mirror */
		ret = i40e_validate_vf(pf, mirror);
		if (ret)
			goto err_out;
		mirror_vf = &pf->vf[mirror];
		mirror_vsi = pf->vsi[mirror_vf->lan_vsi_idx];

		/* Add mirrors */
		ret = i40e_add_ingress_egress_mirror(src_vsi, mirror_vsi,
						     rule_type, &rule_id);
		if (ret)
			goto err_out;
		pf->ingress_vlan = mirror;
		pf->ingress_rule_id = rule_id;
	}
err_out:
	return ret;
}

/**
 * i40e_get_pf_egress_mirror - Gets the configured egress mirror for PF
 * @pdev: PCI device information struct
 * @mirror: pointer to return the ingress mirror
 *
 * Gets the ingress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_pf_egress_mirror(struct pci_dev *pdev, int *mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	*mirror = pf->egress_vlan;
	return 0;
}

/**
 * i40e_set_pf_egress_mirror - Sets the configured egress mirror for PF
 * @pdev: PCI device information struct
 * @mirror: pointer to return the ingress mirror
 *
 * Gets the ingress mirror configured
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_pf_egress_mirror(struct pci_dev *pdev, const int mirror)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *src_vsi, *mirror_vsi;
	struct i40e_vf *mirror_vf;
	u16 rule_type, rule_id;
	int ret;

	/* The Admin Queue mirroring rules refer to the traffic
	 * directions from the perspective of the switch, not the VSI
	 * we apply the mirroring rule on - so the behaviour of a VSI
	 * egress mirror is classified as an ingress rule
	 */
	rule_type = I40E_AQC_MIRROR_RULE_TYPE_VPORT_INGRESS;
	src_vsi = pf->vsi[pf->lan_vsi];
	if (mirror == I40E_NO_VF_MIRROR) {
		/* Del mirrors */
		rule_id = pf->egress_rule_id;
		ret = i40e_del_ingress_egress_mirror(src_vsi, rule_type,
						     rule_id);
		if (ret)
			goto err_out;
		pf->egress_vlan = I40E_NO_VF_MIRROR;
	} else {
		/* validate the mirror */
		ret = i40e_validate_vf(pf, mirror);
		if (ret)
			goto err_out;
		mirror_vf = &pf->vf[mirror];
		mirror_vsi = pf->vsi[mirror_vf->lan_vsi_idx];

		/* Add mirrors */
		ret = i40e_add_ingress_egress_mirror(src_vsi, mirror_vsi,
						     rule_type, &rule_id);
		if (ret)
			goto err_out;
		pf->egress_vlan = mirror;
		pf->egress_rule_id = rule_id;
	}
err_out:
	return ret;
}

#define I40E_GL_SWT_L2TAGCTRL(_i) (0x001C0A70 + ((_i) * 4))
#define I40E_GL_SWT_L2TAGCTRL_ETHERTYPE_SHIFT 16
#define OUTER_TAG_IDX 2
static int i40e_get_pf_tpid(struct pci_dev *pdev, u16 *tp_id)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);

	if (!(pf->hw.flags & I40E_HW_FLAG_802_1AD_CAPABLE))
		return -EOPNOTSUPP;

	*tp_id = (u16)(rd32(&pf->hw, I40E_GL_SWT_L2TAGCTRL(OUTER_TAG_IDX)) >>
		      I40E_GL_SWT_L2TAGCTRL_ETHERTYPE_SHIFT);

	return 0;
}

static int i40e_set_pf_tpid(struct pci_dev *pdev, u16 tp_id)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	u16 sw_flags = 0, valid_flags = 0;
	int ret = 0;

	if (!(pf->hw.flags & I40E_HW_FLAG_802_1AD_CAPABLE))
		return -EOPNOTSUPP;

	if (tp_id != ETH_P_8021Q && tp_id != ETH_P_8021AD) {
		dev_err(&pdev->dev,
			"Only TPIDs 0x88a8 and 0x8100 are allowed.\n");
		return -EINVAL;
	}

	pf->hw.first_tag = tp_id;
	dev_info(&pdev->dev,
		 "TPID configuration only supported for PF 0. Please ensure to manually set same TPID on all PFs.\n");
	if (pf->hw.pf_id == 0) {
		ret = i40e_aq_set_switch_config(&pf->hw, sw_flags, valid_flags,
						0, NULL);
		if (ret)
			/* not a fatal problem, just keep going */
			dev_info(&pf->pdev->dev,
				 "couldn't set switch config bits, err %s\n",
				 i40e_stat_str(&pf->hw, ret));
	}

	return ret;
}

static int i40e_get_num_queues(struct pci_dev *pdev, int vf_id, int *num_queues)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	*num_queues = vf->num_queue_pairs;

	return ret;
}

static int i40e_set_num_queues(struct pci_dev *pdev, int vf_id, int num_queues)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	if (test_bit(I40E_VF_STATE_RESOURCES_LOADED, &vf->vf_states)) {
		dev_err(&pdev->dev,
			"Unable to configure %d queues, please unbind the driver for VF %d\n",
			num_queues, vf_id);
		return -EAGAIN;
	}

	return i40e_set_vf_num_queues(vf, num_queues);
}

/**
 * i40e_get_max_tx_rate
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @max_tx_rate: max transmit bandwidth rate
 *
 * This function returns the value of transmit bandwidth, in Mbps,
 * for the specified VF,
 * value 0 means rate limiting is disabled.
 *
 * Returns 0 on success, negative on failure
 */
static int i40e_get_max_tx_rate(struct pci_dev *pdev, int vf_id,
				unsigned int *max_tx_rate)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev,
			 "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error_param;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		ret = -ENOENT;
		goto error_param;
	}

	*max_tx_rate = vf->tx_rate;

error_param:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_set_max_tx_rate
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @max_tx_rate: max transmit bandwidth rate to set
 *
 * This function sets the value of max transmit bandwidth, in Mbps,
 * for the specified VF,
 * value 0 means rate limiting is disabled.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_max_tx_rate(struct pci_dev *pdev, int vf_id,
				unsigned int *max_tx_rate)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev,
			 "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto error;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error;
	}

	ret = i40e_set_bw_limit(vsi, vsi->seid, *max_tx_rate);
	if (ret)
		goto error;

	vf->tx_rate = *max_tx_rate;
error:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

/**
 * i40e_get_trust_state
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: on success, true if enabled, false if not
 *
 * Gets VF trust configure.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_trust_state(struct pci_dev *pdev, int vf_id, bool *enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	*enable = vf->trusted;

	return ret;
}

/**
 * i40e_set_trust_state
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @enable: enable or disable trust
 *
 * Sets the VF trust configure
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_trust_state(struct pci_dev *pdev, int vf_id, bool enable)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	if (test_and_set_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state)) {
		dev_warn(&pf->pdev->dev, "Unable to configure VFs, other operation is pending.\n");
		return -EAGAIN;
	}

	if (pf->flags & I40E_FLAG_MFP_ENABLED) {
		dev_err(&pf->pdev->dev, "Trusted VF not supported in MFP mode.\n");
		ret = -EINVAL;
		goto out;
	}

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;

	vf = &pf->vf[vf_id];
	/* if vf is in base mode, make it untrusted */
	if (pf->vf_base_mode_only)
		enable = false;
	if (enable == vf->trusted)
		goto out;

	vf->trusted = enable;

	/* request PF to sync mac/vlan filters for the VF */
	set_bit(__I40E_MACVLAN_SYNC_PENDING, pf->state);
	pf->vsi[vf->lan_vsi_idx]->flags |= I40E_VSI_FLAG_FILTER_CHANGED;

	i40e_vc_reset_vf(vf, true);
	dev_info(&pf->pdev->dev, "VF %u is now %strusted\n",
		 vf_id, enable ? "" : "un");

#ifdef __TC_MQPRIO_MODE_MAX
	if (vf->adq_enabled) {
		if (!vf->trusted) {
			dev_info(&pf->pdev->dev,
				 "VF %u no longer Trusted, deleting all cloud filters\n",
				 vf_id);
			i40e_del_all_cloud_filters(vf);
		}
	}
#endif /* __TC_MQPRIO_MODE_MAX */

out:
	clear_bit(__I40E_VIRTCHNL_OP_PENDING, pf->state);
	return ret;
}

static int i40e_get_queue_type(struct pci_dev *pdev, int vf_id, u8 *queue_type)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	*queue_type = vf->queue_type;

	return ret;
}

static int i40e_set_queue_type(struct pci_dev *pdev, int vf_id, u8 queue_type)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	if (queue_type != VFD_QUEUE_TYPE_RSS &&
	    queue_type != VFD_QUEUE_TYPE_QOS) {
		dev_err(&pdev->dev,
			"Unable to configure queue_type for VF %d, invalid argument\n",
			vf_id);
		return -EINVAL;
	}
	vf->queue_type = queue_type;

	return ret;
}

/**
 * i40e_get_allow_bcast
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @allow: on success, true if allowed, false if not
 *
 * Gets VF allow_bcast configure.
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_allow_bcast(struct pci_dev *pdev, int vf_id, bool *allow)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		return ret;
	vf = &pf->vf[vf_id];

	*allow = vf->allow_bcast;

	return ret;
}

/**
 * i40e_set_allow_bcast
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @allow: allow or disallow VF broadcast
 *
 * Sets the VF allow_bcast configure
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_allow_bcast(struct pci_dev *pdev, int vf_id, bool allow)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	u8 broadcast[ETH_ALEN];
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto out;

	vf = &pf->vf[vf_id];
	if (allow == vf->allow_bcast)
		goto out;

	vf->allow_bcast = allow;
	eth_broadcast_addr(broadcast);
	vsi = pf->vsi[vf->lan_vsi_idx];

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	if (!allow)
		i40e_del_mac_filter(vsi, broadcast);
	else if (!i40e_add_mac_filter(vsi, broadcast))
		dev_info(&pf->pdev->dev,
			 "Could not allocate VF broadcast filter\n");
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

out:
	return ret;
}

/**
 * i40e_set_pf_qos_tc_max_bw
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @req_bw: Requested bandwidth
 *
 * Set bandwidth assigned for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_pf_qos_tc_max_bw(struct pci_dev *pdev, int tc, u16 req_bw)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	int ret;

	vsi = pf->vsi[pf->lan_vsi];
	ret = i40e_get_link_speed(vsi);
	if (req_bw > ret) {
		dev_err(&pdev->dev, "Failed to set PF max bandwidth. Value must be between 0 and %d",
			ret);
		return -EINVAL;
	}

	if (req_bw % I40E_BW_CREDIT_DIVISOR) {
		dev_err(&pdev->dev, "Failed to set PF max bandwidth. Value must be multiple of %d",
			I40E_BW_CREDIT_DIVISOR);
		return -EINVAL;
	}

	pf->dcb_veb_bw_map[tc] = req_bw / I40E_BW_CREDIT_DIVISOR;
	pf->dcb_user_reconfig = true;

	return 0;
}

/**
 * i40e_get_pf_qos_tc_max_bw
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @req_bw: Requested bandwidth
 *
 * Get bandwidth assigned for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_pf_qos_tc_max_bw(struct pci_dev *pdev, int tc, u16 *req_bw)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;

	vsi = pf->vsi[pf->lan_vsi];

	if (tc > I40E_MAX_TRAFFIC_CLASS ||
	    (!(vsi->tc_config.enabled_tc & BIT(tc)))) {
		dev_err(&pdev->dev, "Invalid TC value. Value must be between 0-7 and TC must be configured");
		return -EINVAL;
	}

	*req_bw = pf->dcb_veb_bw_map[tc] * I40E_BW_CREDIT_DIVISOR;

	return 0;
}

/**
 * i40e_set_pf_qos_tc_lsp
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @on: true if link strict priority is on, false otherwise
 *
 * Set link strict priority for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_pf_qos_tc_lsp(struct pci_dev *pdev, int tc, bool on)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);

	pf->dcb_user_lsp_map[tc] = on;
	pf->dcb_user_reconfig = true;

	return 0;
}

/**
 * i40e_get_pf_qos_tc_lsp
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @on: true if link strict priority is on, false otherwise
 *
 * Get link strict priority for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_pf_qos_tc_lsp(struct pci_dev *pdev, int tc, bool *on)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi =  pf->vsi[pf->lan_vsi];

	if (!(pf->flags & I40E_FLAG_DCB_ENABLED)) {
		dev_err(&pdev->dev, "Port is not configured to DCB");
		return -EPERM;
	}

	if (tc > I40E_MAX_TRAFFIC_CLASS ||
	    (!(vsi->tc_config.enabled_tc & BIT(tc)))) {
		dev_err(&pdev->dev, "Invalid TC value. Value must be between 0-7 and TC must be configured");
		return -EINVAL;
	}

	*on = !!pf->dcb_user_lsp_map[tc];

	return 0;
}

/**
 * i40e_set_pf_qos_tc_priority
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @tc_bitmap: Priority bitmap
 *
 * Set priority bitmap for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_pf_qos_tc_priority(struct pci_dev *pdev, int tc,
				       char tc_bitmap)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	u8 new_up[I40E_MAX_USER_PRIORITY];
	u8 old_up[I40E_MAX_USER_PRIORITY];
	u8 tmp;
	int i;

	/* Check if up is already set by another TC */
	for (i = 0; i < I40E_MAX_USER_PRIORITY; i++) {
		if (BIT(i) & tc_bitmap) {
			tmp = pf->dcb_user_up_map[i];
			if (!(tmp == I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY) ==
			    !(tmp == tc)) {
				dev_err(&pdev->dev, "Failed to set user priority for TC %d. Priority %d already taken by <TC num>",
					tc, i);
				return -EPERM;
			}
			new_up[i] = tc;
			continue;
		}
		new_up[i] = I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY;
	}

	for (i = 0; i < I40E_MAX_USER_PRIORITY; i++) {
		if (pf->dcb_user_up_map[i] == tc)
			old_up[i] = tc;
		else
			old_up[i] = I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY;
	}

	/* enable for change again */
	for (i = 0; i < I40E_MAX_USER_PRIORITY; i++)
		if (new_up[i] == I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY &&
		    old_up[i] != I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY)
			pf->dcb_user_up_map[i] =
				I40E_MULTIPLE_TRAFFIC_CLASS_NO_ENTRY;

	for (i = 0; i < I40E_MAX_USER_PRIORITY; i++)
		if (BIT(i) & tc_bitmap)
			pf->dcb_user_up_map[i] = tc;

	pf->dcb_user_reconfig = true;

	return 0;
}

/**
 * i40e_get_pf_qos_tc_priority
 * @pdev: PCI device information struct
 * @tc: Traffic class number
 * @tc_bitmap: Priority bitmap
 *
 * Get priority bitmap for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_pf_qos_tc_priority(struct pci_dev *pdev, int tc,
				       char *tc_bitmap)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int i;

	*tc_bitmap = 0;
	if (!(pf->flags & I40E_FLAG_DCB_ENABLED)) {
		dev_err(&pdev->dev, "Port is not configured to DCB");
		return -EPERM;
	}

	for (i = 0; i < I40E_MAX_USER_PRIORITY; i++) {
		if (pf->dcb_user_up_map[i] == tc)
			*tc_bitmap |= BIT(i);
	}

	return 0;
}

/**
 * i40e_set_vf_max_tc_tx_rate
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tc: Traffic class number
 * @rate: Max TC tx rate in Mbps for VF
 *
 * Get max transfer speed for VF for given TC
 *
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_vf_max_tc_tx_rate(struct pci_dev *pdev, int vf_id, int tc,
				      int rate)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	ret = i40e_get_link_speed(vsi);
	if (rate > ret || rate < 0) {
		dev_err(&pdev->dev, "Failed to set VF max TC tx rate. Value must be between 0 and %d",
			ret);
		return -EINVAL;
	}

	if (rate % I40E_BW_CREDIT_DIVISOR) {
		dev_err(&pdev->dev, "Failed to set VF max TC tx rate. Value must be multiple of %d",
			I40E_BW_CREDIT_DIVISOR);
		return -EINVAL;
	}

	if (tc > I40E_MAX_TRAFFIC_CLASS || (!(vsi->tc_config.enabled_tc & BIT(tc)))) {
		dev_err(&pdev->dev, "Invalid TC value. Value must be between 0-7 and TC must be configured");
		return -EINVAL;
	}

	vsi->tc_config.tc_info[tc].tc_bw_credits = rate /
		I40E_BW_CREDIT_DIVISOR;
	ret = i40e_vsi_configure_tc_max_bw(vsi);
	if (!ret)
		vf->tc_info.max_tc_tx_rate[tc] =
			vsi->tc_config.tc_info[tc].tc_bw_credits;
err:
	return ret;
}

/**
 * i40e_get_vf_max_tc_tx_rate
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tc: Traffic class number
 * @rate: Max TC tx rate in Mbps for VF
 *
 * Get max transfer speed for VF for given TC
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_vf_max_tc_tx_rate(struct pci_dev *pdev, int vf_id, int tc,
				      int *rate)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	if (tc > I40E_MAX_TRAFFIC_CLASS ||
	    (!(vsi->tc_config.enabled_tc & BIT(tc)))) {
		dev_err(&pdev->dev, "Invalid TC value. Value must be between 0-7 and TC must be configured");
		return -EINVAL;
	}

	*rate = vf->tc_info.max_tc_tx_rate[tc] * I40E_BW_CREDIT_DIVISOR;
err:
	return ret;
}

/**
 * i40e_set_vf_qos_tc_share
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tc: Traffic class number
 * @share: percentage share of TC
 *
 * Set percentage share of TC resources for VF
 * Returns 0 on success, negative on failure
 **/
static int i40e_set_vf_qos_tc_share(struct pci_dev *pdev, int vf_id, int tc,
				    u8 share)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err;

	vf = &pf->vf[vf_id];

	if (vf->tc_info.requested_tc_share[tc] && !share) {
		dev_err(&pdev->dev, "Invalid share value. Can't set share back to 0");
		return -EINVAL;
	}

	if (vf->bw_share_applied)
		return -EPERM;

	vf->tc_info.requested_tc_share[tc] = share;
	vf->tc_bw_share_req = true;
	pf->dcb_user_reconfig = true;
err:
	return ret;
}

/**
 * i40e_get_vf_qos_tc_share
 * @pdev: PCI device information struct
 * @vf_id: VF identifier
 * @tc: Traffic class number
 * @share: percentage share of TC
 *
 * Get percentage share of TC resources for VF
 * Returns 0 on success, negative on failure
 **/
static int i40e_get_vf_qos_tc_share(struct pci_dev *pdev, int vf_id, int tc,
				    u8 *share)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret;

	ret = i40e_validate_vf(pf, vf_id);
	if (ret)
		goto err;

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	if (tc > I40E_MAX_TRAFFIC_CLASS ||
	    (!(vsi->tc_config.enabled_tc & BIT(tc)))) {
		dev_err(&pdev->dev, "Invalid TC value. Value must be between 0-7 and TC must be configured");
		return -EINVAL;
	}

	if (vf->bw_share_applied)
		return -EPERM;

	*share = vf->tc_info.applied_tc_share[tc];
err:
	return ret;
}

const struct vfd_ops i40e_vfd_ops = {
	.get_trunk		= i40e_get_trunk,
	.set_trunk		= i40e_set_trunk,
	.get_vlan_mirror	= i40e_get_mirror,
	.set_vlan_mirror	= i40e_set_mirror,
	.get_mac_anti_spoof	= i40e_get_mac_anti_spoof,
	.set_mac_anti_spoof	= i40e_set_mac_anti_spoof,
	.get_vlan_anti_spoof	= i40e_get_vlan_anti_spoof,
	.set_vlan_anti_spoof	= i40e_set_vlan_anti_spoof,
	.set_allow_untagged	= i40e_set_allow_untagged,
	.get_allow_untagged	= i40e_get_allow_untagged,
	.get_loopback		= i40e_get_loopback,
	.set_loopback		= i40e_set_loopback,
	.get_vlan_strip		= i40e_get_vlan_strip,
	.set_vlan_strip		= i40e_set_vlan_strip,
	.get_rx_bytes		= i40e_get_rx_bytes,
	.get_rx_dropped		= i40e_get_rx_dropped,
	.get_rx_packets		= i40e_get_rx_packets,
	.get_tx_bytes		= i40e_get_tx_bytes,
	.get_tx_dropped		= i40e_get_tx_dropped,
	.get_tx_packets		= i40e_get_tx_packets,
	.get_tx_errors		= i40e_get_tx_errors,
	.get_mac		= i40e_get_mac,
	.set_mac		= i40e_set_mac,
	.get_promisc		= i40e_get_promisc,
	.set_promisc		= i40e_set_promisc,
	.get_ingress_mirror	= i40e_get_ingress_mirror,
	.set_ingress_mirror	= i40e_set_ingress_mirror,
	.get_egress_mirror	= i40e_get_egress_mirror,
	.set_egress_mirror	= i40e_set_egress_mirror,
	.get_link_state		= i40e_get_link_state,
	.set_link_state		= i40e_set_link_state,
	.get_mac_list		= i40e_get_mac_list,
	.add_macs_to_list	= i40e_add_macs_to_list,
	.rem_macs_from_list	= i40e_rem_macs_from_list,
	.get_vf_enable		= i40e_get_vf_enable,
	.set_vf_enable		= i40e_set_vf_enable,
	.reset_stats		= i40e_reset_vf_stats,
	.set_vf_bw_share	= i40e_store_vf_bw_share,
	.get_vf_bw_share	= i40e_get_vf_bw_share,
	.set_pf_qos_apply	= i40e_set_pf_qos_apply,
	.get_pf_ingress_mirror	= i40e_get_pf_ingress_mirror,
	.set_pf_ingress_mirror	= i40e_set_pf_ingress_mirror,
	.get_pf_egress_mirror	= i40e_get_pf_egress_mirror,
	.set_pf_egress_mirror	= i40e_set_pf_egress_mirror,
	.get_pf_tpid		= i40e_get_pf_tpid,
	.set_pf_tpid		= i40e_set_pf_tpid,
	.get_num_queues		= i40e_get_num_queues,
	.set_num_queues		= i40e_set_num_queues,
	.get_max_tx_rate	= i40e_get_max_tx_rate,
	.set_max_tx_rate	= i40e_set_max_tx_rate,
	.get_trust_state	= i40e_get_trust_state,
	.set_trust_state	= i40e_set_trust_state,
	.get_queue_type		= i40e_get_queue_type,
	.set_queue_type		= i40e_set_queue_type,
	.get_allow_bcast	= i40e_get_allow_bcast,
	.set_allow_bcast	= i40e_set_allow_bcast,
	.set_pf_qos_tc_max_bw	= i40e_set_pf_qos_tc_max_bw,
	.get_pf_qos_tc_max_bw	= i40e_get_pf_qos_tc_max_bw,
	.set_pf_qos_tc_lsp	= i40e_set_pf_qos_tc_lsp,
	.get_pf_qos_tc_lsp	= i40e_get_pf_qos_tc_lsp,
	.set_pf_qos_tc_priority	= i40e_set_pf_qos_tc_priority,
	.get_pf_qos_tc_priority	= i40e_get_pf_qos_tc_priority,
	.set_vf_max_tc_tx_rate	= i40e_set_vf_max_tc_tx_rate,
	.get_vf_max_tc_tx_rate	= i40e_get_vf_max_tc_tx_rate,
	.set_vf_qos_tc_share	= i40e_set_vf_qos_tc_share,
	.get_vf_qos_tc_share	= i40e_get_vf_qos_tc_share,

};
#endif /* HAVE_NDO_SET_VF_LINK_STATE */
