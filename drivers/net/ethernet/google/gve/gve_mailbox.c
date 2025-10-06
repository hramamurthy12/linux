// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2025 Google LLC
 */

#include <linux/etherdevice.h>
#include "gve.h"
#include "gve_mailbox.h"

static void gve_mbx_reg_init(struct gve_mbx_queue *mbx_q, void __iomem *bar0)
{
	if (mbx_q->q_type == GVE_GVE_MBX_Q_TYPE_RX) {
		mbx_q->reg = (struct gve_mbx_registers __iomem *)(bar0 + GVE_MBX_RX_BASE);

		iowrite32(mbx_q->ring_size - 1, &mbx_q->reg->queue_tail);
	} else {
		mbx_q->reg = (struct gve_mbx_registers __iomem *)(bar0 + GVE_MBX_TX_BASE);

		iowrite32(0, &mbx_q->reg->queue_tail);
	}

	iowrite32(0, &mbx_q->reg->queue_head);
	iowrite32(lower_32_bits(mbx_q->desc_ring.pa),
		  &mbx_q->reg->base_addr_low);
	iowrite32(upper_32_bits(mbx_q->desc_ring.pa),
		  &mbx_q->reg->base_addr_high);
	iowrite32((mbx_q->ring_size | GVE_MBX_Q_ENABLE_M),
		  &mbx_q->reg->queue_len);
}

static int gve_alloc_mbx_desc_ring(struct gve_mailbox *mailbox,
				   struct gve_dma_mem *desc_ring,
				   u16 ring_size)
{
	int size = ring_size * sizeof(struct gve_mbx_desc);

	desc_ring->va = dma_alloc_coherent(&mailbox->pdev->dev, size,
					   &desc_ring->pa, GFP_KERNEL);
	if (!desc_ring->va)
		return -ENOMEM;

	desc_ring->size = size;
	return 0;
}

static void gve_free_mbx_desc_ring(struct gve_mailbox *mailbox,
				   struct gve_mbx_queue *mbx_q)
{
	struct gve_dma_mem *desc_ring = &mbx_q->desc_ring;

	if (!mbx_q)
		return;

	dma_free_coherent(&mailbox->pdev->dev, desc_ring->size, desc_ring->va,
			  desc_ring->pa);
	desc_ring->size = 0;
	desc_ring->va = NULL;
	desc_ring->pa  = 0;
}

void gve_free_mailbox(struct gve_mailbox *mailbox, void __iomem *reg_bar0)
{
	int err;

	err = gve_mbx_reset(mailbox, reg_bar0);
	if (err)
		dev_err(&mailbox->pdev->dev, "Failed to reset in mailbox mode\n");

	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_rx);
	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_tx);

	kfree(mailbox->mbx_rx);
	mailbox->mbx_rx = NULL;
	kfree(mailbox->mbx_tx);
	mailbox->mbx_tx = NULL;
}

static int gve_alloc_mailbox(struct gve_mailbox *mailbox)
{
	int err = 0;

	mailbox->mbx_tx = kzalloc(sizeof(*mailbox->mbx_tx), GFP_KERNEL);
	if (!mailbox->mbx_tx)
		return -ENOMEM;

	mailbox->mbx_rx = kzalloc(sizeof(*mailbox->mbx_rx), GFP_KERNEL);
	if (!mailbox->mbx_rx) {
		err = -ENOMEM;
		goto free_mbx_tx;
	}

	mailbox->mbx_rx->q_type = GVE_GVE_MBX_Q_TYPE_RX;
	mailbox->mbx_tx->q_type = GVE_GVE_MBX_Q_TYPE_TX;
	mailbox->mbx_tx->ring_size = GVE_MBX_DEFAULT_RING_SIZE;
	mailbox->mbx_rx->ring_size = GVE_MBX_DEFAULT_RING_SIZE;

	err = gve_alloc_mbx_desc_ring(mailbox, &mailbox->mbx_tx->desc_ring,
				      GVE_MBX_DEFAULT_RING_SIZE);
	if (err)
		goto free_mbx_rx;

	err = gve_alloc_mbx_desc_ring(mailbox, &mailbox->mbx_rx->desc_ring,
				      GVE_MBX_DEFAULT_RING_SIZE);
	if (err)
		goto free_mbx_tx_desc_ring;

	return 0;

free_mbx_tx_desc_ring:
	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_tx);
free_mbx_rx:
	kfree(mailbox->mbx_rx);
	mailbox->mbx_rx = NULL;
free_mbx_tx:
	kfree(mailbox->mbx_tx);
	mailbox->mbx_tx = NULL;
	return err;
}

static int gve_mbx_check_reset_complete(struct gve_mailbox *mailbox,
					void __iomem *reg_bar0)
{
	void __iomem *reset_status_reg = reg_bar0 + GVE_MBX_RESET_STATUS;
	u32 reg_val;
	int i;

	for (i = 0; i < 2000; i++) {
		reg_val = ioread32(reset_status_reg);

		if (reg_val != 0xFFFFFFFF && (reg_val & GENMASK(1, 0)))
			return 0;

		usleep_range(5000, 10000);
	}

	dev_warn_ratelimited(&mailbox->pdev->dev, "Mailbox device reset timeout!");
	return -EBUSY;
}

int gve_mbx_reset(struct gve_mailbox *mailbox, void __iomem *reg_bar0)
{
	void __iomem *reset_reg = reg_bar0 + GVE_MBX_RESET_CTRL;
	u32 reg_val;

	reg_val = ioread32(reset_reg);

	iowrite32(reg_val | BIT(0), reset_reg);
	return gve_mbx_check_reset_complete(mailbox, reg_bar0);
}

int gve_initialize_mbx(struct gve_mailbox *mailbox, void __iomem *reg_bar0)
{
	int err;

	err = gve_mbx_reset(mailbox, reg_bar0);
	if (err) {
		dev_err(&mailbox->pdev->dev, "Failed to reset in mailbox mode\n");
		return err;
	}

	if (gve_alloc_mailbox(mailbox)) {
		dev_err(&mailbox->pdev->dev,
			"Failed to alloc mailbox queues\n");
		return -ENOMEM;
	}

	/* Init the mailbox queues */
	gve_mbx_reg_init(mailbox->mbx_tx, reg_bar0);
	gve_mbx_reg_init(mailbox->mbx_rx, reg_bar0);
	return 0;
}
