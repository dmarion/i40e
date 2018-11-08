/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#ifndef _KCOMPAT_VFD_H_
#define _KCOMPAT_VFD_H_

#define VFD_PROMISC_OFF		0x00
#define VFD_PROMISC_UNICAST	0x01
#define VFD_PROMISC_MULTICAST	0x02

#define VFD_LINKSTATE_OFF	0x00
#define VFD_LINKSTATE_ON	0x01
#define VFD_LINKSTATE_AUTO	0x02

#define VFD_EGRESS_MIRROR_OFF	-1
#define VFD_INGRESS_MIRROR_OFF	-1

/**
 * struct vfd_objects - VF-d kobjects information struct
 * @num_vfs:	number of VFs allocated
 * @sriov_kobj:	pointer to the top sriov kobject
 * @vf_kobj:	array of pointer to each VF's kobjects
 */
struct vfd_objects {
	int num_vfs;
	struct kobject *sriov_kobj;
	struct kobject *vf_kobj[0];
};

struct vfd_macaddr {
	u8 mac[ETH_ALEN];
	struct list_head list;
};

#define VFD_LINK_SPEED_100MB_SHIFT		0x1
#define VFD_LINK_SPEED_1GB_SHIFT		0x2
#define VFD_LINK_SPEED_10GB_SHIFT		0x3
#define VFD_LINK_SPEED_40GB_SHIFT		0x4
#define VFD_LINK_SPEED_20GB_SHIFT		0x5
#define VFD_LINK_SPEED_25GB_SHIFT		0x6

enum vfd_link_speed {
	VFD_LINK_SPEED_UNKNOWN	= 0,
	VFD_LINK_SPEED_100MB	= BIT(VFD_LINK_SPEED_100MB_SHIFT),
	VFD_LINK_SPEED_1GB	= BIT(VFD_LINK_SPEED_1GB_SHIFT),
	VFD_LINK_SPEED_10GB	= BIT(VFD_LINK_SPEED_10GB_SHIFT),
	VFD_LINK_SPEED_40GB	= BIT(VFD_LINK_SPEED_40GB_SHIFT),
	VFD_LINK_SPEED_20GB	= BIT(VFD_LINK_SPEED_20GB_SHIFT),
	VFD_LINK_SPEED_25GB	= BIT(VFD_LINK_SPEED_25GB_SHIFT),
};

struct vfd_ops {
	int (*get_trunk)(struct pci_dev *pdev, int vf_id, unsigned long *buff);
	int (*set_trunk)(struct pci_dev *pdev, int vf_id,
			 const unsigned long *buff);
	int (*get_vlan_mirror)(struct pci_dev *pdev, int vf_id,
			       unsigned long *buff);
	int (*set_vlan_mirror)(struct pci_dev *pdev, int vf_id,
			       const unsigned long *buff);
	int (*get_egress_mirror)(struct pci_dev *pdev, int vf_id, int *data);
	int (*set_egress_mirror)(struct pci_dev *pdev, int vf_id,
				 const int data);
	int (*get_ingress_mirror)(struct pci_dev *pdev, int vf_id, int *data);
	int (*set_ingress_mirror)(struct pci_dev *pdev, int vf_id,
				  const int data);
	int (*get_mac_anti_spoof)(struct pci_dev *pdev, int vf_id, bool *data);
	int (*set_mac_anti_spoof)(struct pci_dev *pdev, int vf_id,
				  const bool data);
	int (*get_vlan_anti_spoof)(struct pci_dev *pdev, int vf_id, bool *data);
	int (*set_vlan_anti_spoof)(struct pci_dev *pdev, int vf_id,
				   const bool data);
	int (*get_allow_untagged)(struct pci_dev *pdev, int vf_id, bool *data);
	int (*set_allow_untagged)(struct pci_dev *pdev, int vf_id,
				  const bool data);
	int (*get_loopback)(struct pci_dev *pdev, int vf_id, bool *data);
	int (*set_loopback)(struct pci_dev *pdev, int vf_id, const bool data);
	int (*get_mac)(struct pci_dev *pdev, int vf_id, u8 *macaddr);
	int (*set_mac)(struct pci_dev *pdev, int vf_id, const u8 *macaddr);
	int (*get_mac_list)(struct pci_dev *pdev, int vf_id,
			    struct list_head *mac_list);
	int (*add_macs_to_list)(struct pci_dev *pdev, int vf_id,
				struct list_head *mac_list);
	int (*rem_macs_from_list)(struct pci_dev *pdev, int vf_id,
				  struct list_head *mac_list);
	int (*get_promisc)(struct pci_dev *pdev, int vf_id, u8 *data);
	int (*set_promisc)(struct pci_dev *pdev, int vf_id, const u8 data);
	int (*get_vlan_strip)(struct pci_dev *pdev, int vf_id, bool *data);
	int (*set_vlan_strip)(struct pci_dev *pdev, int vf_id, const bool data);
	int (*get_link_state)(struct pci_dev *pdev, int vf_id, bool *enabled,
			      enum vfd_link_speed *link_speed);
	int (*set_link_state)(struct pci_dev *pdev, int vf_id, const u8 data);
	int (*get_max_tx_rate)(struct kobject *,
			       struct kobj_attribute *, char *);
	int (*set_max_tx_rate)(struct kobject *, struct kobj_attribute *,
			       const char *, size_t);
	int (*get_min_tx_rate)(struct kobject *,
			       struct kobj_attribute *, char *);
	int (*set_min_tx_rate)(struct kobject *, struct kobj_attribute *,
			       const char *, size_t);
	int (*get_spoofcheck)(struct kobject *,
			      struct kobj_attribute *, char *);
	int (*set_spoofcheck)(struct kobject *, struct kobj_attribute *,
			      const char *, size_t);
	int (*get_trust)(struct kobject *,
			 struct kobj_attribute *, char *);
	int (*set_trust)(struct kobject *, struct kobj_attribute *,
			 const char *, size_t);
	int (*get_vlan)(struct kobject *, struct kobj_attribute *, char *);
	int (*set_vlan)(struct kobject *, struct kobj_attribute *,
			const char *, size_t);
	int (*get_rx_bytes)  (struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_rx_dropped)(struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_rx_packets)(struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_tx_bytes)  (struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_tx_dropped)(struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_tx_packets)(struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_tx_spoofed)(struct pci_dev *pdev, int vf_id, u64 *data);
	int (*get_tx_errors)(struct pci_dev *pdev, int vf_id, u64 *data);
};

extern const struct vfd_ops *vfd_ops;

#endif /* _KCOMPAT_VFD_H_ */
