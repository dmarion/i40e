/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2021 Intel Corporation. */

#ifndef _I40E_TXRX_COMMON_H_
#define _I40E_TXRX_COMMON_H_

void i40e_fd_handle_status(struct i40e_ring *rx_ring,
			   union i40e_rx_desc *rx_desc, u8 prog_id);
int i40e_xmit_xdp_tx_ring(struct xdp_buff *xdp, struct i40e_ring *xdp_ring);
struct i40e_rx_buffer *i40e_clean_programming_status
	(struct i40e_ring *rx_ring,
	 union i40e_rx_desc *rx_desc,
	 u64 qw);
void i40e_process_skb_fields(struct i40e_ring *rx_ring,
			     union i40e_rx_desc *rx_desc,
			     struct sk_buff *skb,
			     u8 rx_ptype);
void i40e_receive_skb(struct i40e_ring *rx_ring,
		      struct sk_buff *skb, u16 vlan_tag, u16 vlan_tpid);

void i40e_xdp_ring_update_tail(struct i40e_ring *xdp_ring);
void i40e_update_rx_stats(struct i40e_ring *rx_ring,
			  unsigned int total_rx_bytes,
			  unsigned int total_rx_packets);
void i40e_finalize_xdp_rx(struct i40e_ring *rx_ring, unsigned int xdp_res);
void i40e_release_rx_desc(struct i40e_ring *rx_ring, u32 val);

/**
 * i40e_rx_is_programming_status - check for programming status descriptor
 * @qw: qword representing status_error_len in CPU ordering
 *
 * The value of in the descriptor length field indicate if this
 * is a programming status descriptor for flow director or FCoE
 * by the value of I40E_RX_PROG_STATUS_DESC_LENGTH, otherwise
 * it is a packet descriptor.
 **/
static inline bool i40e_rx_is_programming_status(u64 qw)
{
/* The Rx filter programming status and SPH bit occupy the same
 * spot in the descriptor. Since we don't support packet split we
 * can just reuse the bit as an indication that this is a
 * programming status descriptor.
 */
	return qw & I40E_RXD_QW1_LENGTH_SPH_MASK;
}

#define I40E_XDP_PASS          0
#define I40E_XDP_CONSUMED      BIT(0)
#define I40E_XDP_TX            BIT(1)
#define I40E_XDP_REDIR         BIT(2)

#endif /* _I40E_TXRX_COMMON_H_ */
