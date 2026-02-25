// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2026 Google LLC
 */

#ifndef _GVE_MAILBOX_H
#define _GVE_MAILBOX_H

#include <linux/io.h>
#include <linux/kernel.h>

/* Mailbox Queue defines */
#define GVE_MBX_BASE			0x08400000

#define GVE_MBX_RESET_CTRL		(GVE_MBX_BASE + 0x700C)
#define GVE_MBX_RESET_STATUS		(GVE_MBX_BASE + 0x7008)

#define GVE_MBX_RX_BASE			GVE_MBX_BASE
#define GVE_MBX_RX_LEN_M		GENMASK(12, 0)
#define GVE_MBX_TX_BASE			(GVE_MBX_BASE + 0x14)

#define GVE_MBX_Q_ENABLE_M		BIT(31)

#define GVE_MBX_DEFAULT_RING_SIZE	64
/* Length of msg Q is < mbx Q to allow for async msgs from the device */
#define GVE_MBX_MSG_QUEUE_LEN		48
#define GVE_MBX_BUF_SIZE                4096
#define GVE_MBX_CONTROL_PLANE		0x0801

#define GVE_MBX_FLAG_DD_S		0
#define GVE_MBX_FLAG_ERR_S		2
#define GVE_MBX_FLAG_RD_S               10
#define GVE_MBX_FLAG_BUF_S              12

#define GVE_MBX_FLAG_DD			BIT(GVE_MBX_FLAG_DD_S)
#define GVE_MBX_FLAG_ERR		BIT(GVE_MBX_FLAG_ERR_S)
#define GVE_MBX_FLAG_RD                 BIT(GVE_MBX_FLAG_RD_S)
#define GVE_MBX_FLAG_BUF                BIT(GVE_MBX_FLAG_BUF_S)

#define GVE_MBX_DESC(R, i) \
	(&(((struct gve_mbx_desc *)((R)->desc_ring.va))[i]))

#define GVE_MBX_MSG_TIMEOUT_MSEC	60000

struct gve_priv;
struct gve_mailbox;
struct gve_queue_resources;

struct gve_dma_mem {
	void *va;
	dma_addr_t pa;
	size_t size;
};

struct gve_mbx_registers {
	/* Lower 6bits are 0 to meet the 64-byte alignment */
	__le32 base_addr_low;
	__le32 base_addr_high;
	/* Max size required by the hw is 1023 */
	__le32 queue_len;
	__le32 queue_head;
	__le32 queue_tail;
};

enum gve_mbx_status {
	GVE_MBX_STATUS_UNSET				= 0,
	GVE_MBX_STATUS_PASSED				= 1,
	GVE_MBX_STATUS_UNSUPPORTED_ERROR		= 0xFFEF,
	GVE_MBX_STATUS_ABORTED_ERROR			= 0xFFF0,
	GVE_MBX_STATUS_ALREADY_EXISTS_ERROR		= 0xFFF1,
	GVE_MBX_STATUS_CANCELLED_ERROR			= 0xFFF2,
	GVE_MBX_STATUS_DATA_LOSS_ERROR			= 0xFFF3,
	GVE_MBX_STATUS_DEADLINE_EXCEEDED_ERROR		= 0xFFF4,
	GVE_MBX_STATUS_FAILED_PRECONDITION_ERROR	= 0xFFF5,
	GVE_MBX_STATUS_INTERNAL_ERROR			= 0xFFF6,
	GVE_MBX_STATUS_INVALID_ARGUMENT_ERROR		= 0xFFF7,
	GVE_MBX_STATUS_NOT_FOUND_ERROR			= 0xFFF8,
	GVE_MBX_STATUS_OUT_OF_RANGE_ERROR		= 0xFFF9,
	GVE_MBX_STATUS_PERMISSION_DENIED_ERROR		= 0xFFFA,
	GVE_MBX_STATUS_UNAUTHENTICATED_ERROR		= 0xFFFB,
	GVE_MBX_STATUS_RESOURCE_EXHAUSTED_ERROR		= 0xFFFC,
	GVE_MBX_STATUS_UNAVAILABLE_ERROR		= 0xFFFD,
	GVE_MBX_STATUS_UNIMPLEMENTED_ERROR		= 0xFFFE,
	GVE_MBX_STATUS_UNKNOWN_ERROR			= 0xFFFF,
};

enum gve_mbx_queue_type {
	GVE_GVE_MBX_Q_TYPE_UNKNOWN,
	GVE_GVE_MBX_Q_TYPE_TX,
	GVE_GVE_MBX_Q_TYPE_RX,
};

struct gve_mbx_queue {
	enum gve_mbx_queue_type q_type;
	struct gve_dma_mem desc_ring;
	u16 buf_size;
	u16 ring_size;
	struct gve_mbx_registers *reg;
	u16 next_to_use;
	u16 next_to_clean;
	u16 next_to_post;
	spinlock_t q_lock; /* mbx q lock */
};

/* Queue which has a list of outstanding commands */
struct gve_mbx_msg_queue {
	unsigned long *msg_queue_map;
	struct gve_mbx_msg **mbx_msgs;
	spinlock_t mbx_msg_q_lock; /* mbx msg Q lock */
	u16 size;
};

struct gve_mbx_msg {
	struct completion work;
	u16 sw_cookie;
	int status;
};

enum gve_mbx_opcode {
	GVE_MBX_NEGOTIATE_CAPABILITIES	= 0x6001,
	GVE_MBX_GET_INTERRUPT_DBS	= 0x6005,
	GVE_MBX_GET_PTYPE_MAP		= 0x6006,
	GVE_MBX_CONFIG_TX_QUEUES	= 0x6008,
	GVE_MBX_CONFIG_RX_QUEUES        = 0x6009,
	GVE_MBX_ENABLE_TX_QUEUES        = 0x600c,
	GVE_MBX_ENABLE_RX_QUEUES        = 0x600d,
	GVE_MBX_DISABLE_TX_QUEUES	= 0x600e,
	GVE_MBX_DISABLE_RX_QUEUES       = 0x600f,
	GVE_MBX_CONFIGURE_RSS		= 0x6011,
};

enum gve_mbx_caps {
	GVE_MBX_CAP_DQO_RDA		= BIT(0),
	GVE_MBX_CAP_DQO_QPL		= BIT(1),
	GVE_MBX_CAP_INTERRUPT_SHARING	= BIT(2),
	GVE_MBX_CAP_FLOW_STEERING	= BIT(3),
	GVE_MBX_CAP_NIC_TSTAMP_REG	= BIT(4),
	GVE_MBX_CAP_NIC_TSTAMP_CMD	= BIT(5),
	GVE_MBX_CAP_HW_GRO		= BIT(6),
};

enum gve_mbx_negotiate_caps_msg_version {
	GVE_MBX_CAPS_MSG_V1 = 1,
};

struct gve_mbx_caps_req {
	__le32 msg_version;
	__le32 msg_size;
	__le64 supported_caps;
	u8 os_type; /* 0x01 = Linux */
	u8 driver_major;
	u8 driver_minor;
	u8 driver_sub;
	__le32 os_version_major;
	__le32 os_version_minor;
	__le32 os_version_sub;
	u8 os_version_str[512];
	u8 driver_version_str[64];
};

/* this structure layout cannot be modified,
 * new fields to be only added in the end
 * when bumping msg_version
 */
struct gve_mbx_caps_resp {
	__le32 msg_version;
	__le32 msg_size;
	__le64 negotiated_caps;
	u8 db_bar; /* doorbell BAR no */
	u8 pad[3];
	/* Offset in bytes into db_bar for mbx IRQ doorbell register */
	__le32 mbx_irq_db_offset;
	__le16 mbx_response_timeout_ms;
	__le16 tx_queue_watchdog_timeout_ms;
	__le16 num_msix_vectors;
	__le16 default_tx_queues;
	__le16 default_rx_queues;
	__le16 max_tx_queues;
	__le16 max_rx_queues;
	__le16 max_mtu;
	u8 mac[ETH_ALEN];
	__le16 default_tx_ring_size;
	__le16 default_rx_ring_size;
	__le16 max_tx_ring_size;
	__le16 max_rx_ring_size;
	__le16 min_tx_ring_size;
	__le16 min_rx_ring_size;
	__le16 max_packet_buffer_size;
	__le16 max_header_buffer_size;
	__le16 hash_key_size;
	__le16 hash_lut_size;
};

/**
 * struct gve_mbx_get_interrupt_dbs_req - request for GET_INTERRUPT_DBS
 *
 * Request for getting MSI-X vectors and doorbell registers from device.
 *
 * @start_msix_index: first MSI-X index to retrieve info for.
 * @num_vecs: number of interrupt vectors to retrieve info for.
 */
struct gve_mbx_get_interrupt_dbs_req {
	__le16 start_msix_index;
	__le16 num_vecs;
};
static_assert(sizeof(struct gve_mbx_get_interrupt_dbs_req) == 4);

/**
 * struct gve_mbx_interrupt_db_info - irq and coalescing doorbell information.
 *
 * @irq_db_offset: Offset in bytes from db_bar for this vector's IRQ doorbell
 *	register
 * @irq_coalesc_db_offset: Offset in bytes from db_bar for this vector's
 *	coalescing doorbell register
 */
struct gve_mbx_interrupt_db_info {
	__le32 irq_db_offset;
	__le32 irq_coalesce_db_offset;
};
static_assert(sizeof(struct gve_mbx_interrupt_db_info) == 8);

/**
 * struct gve_mbx_get_interrupt_dbs_resp - response to GET_INTERRUPT_DBS
 * request.
 *
 * There is a 1:1:1 mapping between MSI-X vector, IRQ doorbell, and IRQ
 * coalescing doorbell.
 *
 * @start_msix_index: First MSI-X vector for which to get the doorbell
 *	registers for.
 * @num_vecs: Number of vectors in following payload.
 * @info: Array of interrupt doorbell info, starting at @start_msix_index, with
 *	@num_vecs elements.
 */
struct gve_mbx_get_interrupt_dbs_resp {
	__le16 start_msix_index;
	__le16 num_vecs;
	struct gve_mbx_interrupt_db_info info[];
};

enum gve_mbx_hash_alg {
	GVE_MBX_HASH_ALG_TOEPLITZ = 1,
};

struct gve_mbx_configure_rss_req {
	__le16 hash_types;
	u8 hash_alg;
	u8 reserved;
	__le16 hash_key_size;
	__le16 hash_lut_size;
	u8 hash_key[256];
	__le32 hash_lut[];
};

struct gve_mbx_tx_q_info {
	__le32 queue_id;
#define GVE_MBX_NO_INTERRUPT 0xffff
	__le16 msix_index;
	u8 pad1[2];
#define GVE_RAW_ADDRESSING_QPL_ID 0xFFFFFFFF
	__le32 queue_page_list_id;
	u8 pad2[4];
	__le64 tx_ring_addr;
	__le64 tx_comp_ring_addr;
	__le16 tx_ring_size;
	__le16 tx_comp_ring_size;
	u8 pad3[4];
};

struct gve_mbx_config_tx_q_req {
	__le16 num_queues;
	u8 pad[6];
	struct gve_mbx_tx_q_info tx_queues[] __counted_by_le(num_queues);
};

struct gve_mbx_configured_tx_q_info {
	__le32 queue_id;
	__le32 tail_db_offset;
};

struct gve_mbx_config_tx_qs_resp {
	__le16 num_queues;
	u8 pad[6];
	struct gve_mbx_configured_tx_q_info queues[] __counted_by_le(num_queues);
};

enum gve_mbx_rx_queue_flags {
	GVE_MBX_RX_QUEUE_ENABLE_RSC = BIT(0),
};

struct gve_mbx_rx_q_info {
	__le32 queue_id;
	__le16 msix_index;
	u8 pad[2];
	__le32 queue_page_list_id;
	__le32 flags;
	__le64 rx_desc_ring_addr;
	__le64 rx_data_ring_addr;
	__le16 rx_desc_ring_size;
	__le16 rx_data_ring_size;
	__le16 packet_buffer_size;
	__le16 header_buffer_size;
};

struct gve_mbx_config_rx_qs_req {
	__le16 num_queues;
	u8 pad[6];
	struct gve_mbx_rx_q_info rx_queues[] __counted_by_le(num_queues);
};

struct gve_mbx_configured_rx_q_info {
	__le32 queue_id;
	__le32 tail_db_offset;
};

struct gve_mbx_config_rx_qs_resp {
	__le16 num_queues;
	u8 pad[6];
	struct gve_mbx_configured_rx_q_info queues[] __counted_by_le(num_queues);
};

struct gve_mbx_disable_qs_req {
	__le16 num_queues;
	__le16 queue_id[] __counted_by_le(num_queues);
};

struct gve_mbx_enable_qs_req {
	__le16 num_queues;
	__le16 queue_id[] __counted_by_le(num_queues);
};

struct gve_mbx_desc {
	__le16 flags;			/* DD bit, extra payload etc */
	__le16 destination;		/* send to CP/HMA 0x0801 */
	__le16 buf_len;			/* 0 when no extra payload, max is 4k */
	union {
		__le16 retval;		/* MBX RX: status of message */
		__le16 pfid_vfid;	/* MBX TX: func_id, 0 for PF */
	};
	__le32 cmd_opcode;
	__le16 cmd_retval;		/* size of the message */
	__le16 reserved1;
	__le32 function_id;
	__le16 reserved2;
	__le16 cmd_cookie;		/* for SW use */
	__le32 addr_high;		/* of the allocated buffer */
	__le32 addr_low;		/* of the allocated buffer */
};

int gve_mbx_set_num_ntfy_blks(struct gve_priv *priv);
void gve_mbx_set_num_queues(struct gve_priv *priv);
void gve_mbx_get_max_queues(struct gve_mailbox *mailbox, int *max_tx_queues,
			    int *max_rx_queues);
int gve_mbx_request_db_info(struct gve_priv *priv);
int gve_mbx_setup_mgmt_irq(struct gve_priv *priv);
void gve_mbx_teardown_mgmt_irq(struct gve_priv *priv);
int gve_mbx_get_ptype_map(struct gve_priv *priv);
int gve_mbx_configure_rss(struct gve_priv *priv,
			  struct ethtool_rxfh_param *rxfh);
int gve_mbx_negotiate_caps(struct gve_mailbox *mailbox);
void gve_mbx_task(struct work_struct *work);
int gve_send_mbx_msg_wait(struct gve_mailbox *mailbox, u32 opcode, u16 msg_size,
			  u8 *msg);
int gve_send_mbx_msg(struct gve_mailbox *mailbox, u32 opcode, u16 msg_size,
		     u8 *msg, u16 cookie);
int gve_receive_mbx_msg(struct gve_mailbox *mailbox);
void gve_free_mailbox(struct gve_mailbox *mailbox, void __iomem *reg_bar0);
int gve_initialize_mbx(struct gve_mailbox *mailbox, void __iomem *reg_bar0);
int gve_mbx_reset(struct gve_mailbox *mailbox, void __iomem *reg_bar0);

static inline int gve_mbx_map_db_bar(struct gve_priv *priv)
{
	return 0;
}

static inline void gve_mbx_unmap_db_bar(struct gve_priv *priv)
{
}

static inline int gve_mbx_setup_stats_report(struct gve_priv *priv,
					     u64 stats_report_len,
					     dma_addr_t stats_report_addr,
					     u64 interval)
{
	return 0;
}

static inline void gve_mbx_free_db_resources(struct gve_priv *priv)
{
}
#endif /* _GVE_MAILBOX_H */
