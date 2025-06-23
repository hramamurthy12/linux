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
#define GVE_MBX_TX_BASE			(GVE_MBX_BASE + 0x14)

#define GVE_MBX_Q_ENABLE_M		BIT(31)

#define GVE_MBX_DEFAULT_RING_SIZE	64
#define GVE_MBX_BUF_SIZE                4096

#define GVE_MBX_FLAG_RD_S               10
#define GVE_MBX_FLAG_BUF_S              12

#define GVE_MBX_FLAG_RD                 BIT(GVE_MBX_FLAG_RD_S)
#define GVE_MBX_FLAG_BUF                BIT(GVE_MBX_FLAG_BUF_S)

#define GVE_MBX_DESC(R, i) \
	(&(((struct gve_mbx_desc *)((R)->desc_ring.va))[i]))

struct gve_mailbox;

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

void gve_free_mailbox(struct gve_mailbox *mailbox, void __iomem *reg_bar0);
int gve_initialize_mbx(struct gve_mailbox *mailbox, void __iomem *reg_bar0);
int gve_mbx_reset(struct gve_mailbox *mailbox, void __iomem *reg_bar0);
#endif /* _GVE_MAILBOX_H */
