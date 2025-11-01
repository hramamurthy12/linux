// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2025 Google LLC
 */

#include <linux/etherdevice.h>
#include "gve.h"
#include "gve_mailbox.h"

void gve_mbx_task(struct work_struct *work)
{
	struct gve_mailbox *mailbox;
	int err;

	mailbox = container_of(work, struct gve_mailbox, gve_mbx_task.work);

	queue_delayed_work(mailbox->gve_mbx_wq, &mailbox->gve_mbx_task,
			   usecs_to_jiffies(300));

	err = gve_receive_mbx_msg(mailbox);
}

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

static bool gve_mbx_reset_detected(struct gve_mailbox *mailbox)
{
	struct gve_mbx_queue *mbx_rx = mailbox->mbx_rx;

	if (!mbx_rx)
		return true;

	return !(ioread32(&mbx_rx->reg->queue_len) & GVE_MBX_RX_LEN_M);
}

static void gve_clean_send_mbx(struct gve_mailbox *mailbox)
{
	struct gve_mbx_queue *mbx_tx = mailbox->mbx_tx;
	struct gve_dma_mem *clean_msg;
	struct gve_mbx_desc *desc;
	u16 num_to_clean;
	u16 ntc;
	int i;

	/* Attempt to clean the entire queue */
	num_to_clean = mbx_tx->ring_size;
	ntc = mbx_tx->next_to_clean;

	for (i = 0; i < num_to_clean; i++) {
		/* should clean from ntc */
		desc = GVE_MBX_DESC(mbx_tx, ntc);

		/* check if desc is marked as done */
		if (!(le16_to_cpu(desc->flags) & GVE_MBX_FLAG_DD))
			break;

		clean_msg = mailbox->mbx_tx_bufs[ntc];
		if (clean_msg) {
			dma_free_coherent(&mailbox->pdev->dev, clean_msg->size,
					  clean_msg->va, clean_msg->pa);
			kfree(clean_msg);
		}

		mailbox->mbx_tx_bufs[ntc] = NULL;
		memset(desc, 0, sizeof(*desc));

		ntc++;
		if (ntc == mbx_tx->ring_size)
			ntc = 0;
	}

	mbx_tx->next_to_clean = ntc;
}

static int gve_mbx_get_free_send_idx(struct gve_mbx_msg_queue *mbx_msg_queue,
				     u16 *cookie)
{
	int idx;

	idx = find_first_zero_bit(mbx_msg_queue->msg_queue_map,
				  mbx_msg_queue->size);

	if (idx >= mbx_msg_queue->size)
		return -EBUSY;

	*cookie = idx;
	return 0;
}

int gve_send_mbx_msg_wait(struct gve_mailbox *mailbox, u32 opcode, u16 msg_size,
			  u8 *msg)
{
	struct gve_mbx_msg_queue *mbx_msg_queue = mailbox->mbx_msg_queue;
	struct gve_mbx_msg *mbx_msg;
	u16 cookie;
	int err;

	mbx_msg = kzalloc(sizeof(*mbx_msg), GFP_KERNEL);
	if (!mbx_msg)
		return -ENOMEM;

	init_completion(&mbx_msg->work);

	spin_lock(&mbx_msg_queue->mbx_msg_q_lock);

	/* get cookie */
	if (gve_mbx_get_free_send_idx(mbx_msg_queue, &cookie)) {
		err = -EBUSY;
		goto err_unlock;
	}

	mbx_msg->sw_cookie = cookie;
	set_bit(cookie, mbx_msg_queue->msg_queue_map);
	mailbox->mbx_msgs[cookie] = mbx_msg;

	err = gve_send_mbx_msg(mailbox, opcode, msg_size, msg, cookie);
	if (err)
		goto err_unmap_cookie;

	spin_unlock(&mbx_msg_queue->mbx_msg_q_lock);

	/* wait for completion with timeout */
	if (!wait_for_completion_timeout(&mbx_msg->work,
					 msecs_to_jiffies(GVE_MBX_MSG_TIMEOUT_MSEC))) {
		err = -ETIMEDOUT;
		goto err_lock_free_cookie;
	}

	if (mbx_msg->status == GVE_MBX_STATUS_PASSED)
		err = 0;
	else
		err = -EBADMSG;

err_lock_free_cookie:
	spin_lock(&mailbox->mbx_msg_queue->mbx_msg_q_lock);

err_unmap_cookie:
	clear_bit(cookie, mbx_msg_queue->msg_queue_map);
	mailbox->mbx_msgs[cookie] = NULL;

err_unlock:
	spin_unlock(&mailbox->mbx_msg_queue->mbx_msg_q_lock);
	kfree(mbx_msg);
	return err;
}

int gve_send_mbx_msg(struct gve_mailbox *mailbox, u32 opcode, u16 msg_size,
		     u8 *msg, u16 cookie)
{
	struct gve_mbx_queue *mbx_tx = mailbox->mbx_tx;
	struct gve_mbx_desc *send_desc;
	struct gve_dma_mem *send_msg;
	u16 flags = 0;
	int err = 0;

	/* check for reset */
	if (gve_mbx_reset_detected(mailbox)) {
		dev_err_ratelimited(&mailbox->pdev->dev,
				    "Reset detected, cannot send mbx msg\n");
		return -EBUSY;
	}

	/* reclaim TX mbx descriptors */
	gve_clean_send_mbx(mailbox);

	send_desc = GVE_MBX_DESC(mbx_tx, mbx_tx->next_to_use);

	send_msg = kzalloc(sizeof(*send_msg), GFP_ATOMIC);
	if (!send_msg)
		return -ENOMEM;

	send_desc->destination = cpu_to_le16(GVE_MBX_CONTROL_PLANE);
	send_desc->pfid_vfid = 0;
	send_desc->buf_len = cpu_to_le16(msg_size);
	send_desc->cmd_opcode = cpu_to_le32(opcode);
	send_desc->cmd_cookie = cpu_to_le16(cookie);

	/* paylod */
	send_msg->va = dma_alloc_coherent(&mailbox->pdev->dev, GVE_MBX_BUF_SIZE,
					  &send_msg->pa, GFP_ATOMIC);

	if (!send_msg->va) {
		err = -ENOMEM;
		goto dma_alloc_error;
	}
	send_msg->size = GVE_MBX_BUF_SIZE;

	send_desc->addr_high = cpu_to_le32(upper_32_bits(send_msg->pa));
	send_desc->addr_low = cpu_to_le32(lower_32_bits(send_msg->pa));

	/* set required flags */
	flags |= GVE_MBX_FLAG_BUF;
	flags |= GVE_MBX_FLAG_RD;

	send_desc->flags = cpu_to_le16(flags);

	if (msg && msg_size)
		memcpy(send_msg->va, msg, msg_size);

	mailbox->mbx_tx_bufs[mbx_tx->next_to_use] = send_msg;

	mbx_tx->next_to_use++;
	if (mbx_tx->next_to_use == mbx_tx->ring_size)
		mbx_tx->next_to_use = 0;

	dma_wmb();
	iowrite32(mbx_tx->next_to_use, &mailbox->mbx_tx->reg->queue_tail);
	return 0;

dma_alloc_error:
	kfree(send_msg);
	return err;
}

static void gve_fill_version_info(struct gve_mbx_caps_req *gve_caps_msg)
{
	gve_caps_msg->os_type = 1; /* Linux */
	gve_caps_msg->os_version_major = cpu_to_le32(LINUX_VERSION_MAJOR);
	gve_caps_msg->os_version_minor = cpu_to_le32(LINUX_VERSION_SUBLEVEL);
	gve_caps_msg->os_version_sub = cpu_to_le32(LINUX_VERSION_PATCHLEVEL);

	strscpy(gve_caps_msg->os_version_str, utsname()->release,
		sizeof(gve_caps_msg->os_version_str));
	strscpy(gve_caps_msg->driver_version_str, utsname()->version,
		sizeof(gve_caps_msg->driver_version_str));
}

int gve_mbx_negotiate_caps(struct gve_mailbox *mailbox)
{
	struct gve_mbx_caps_req *gve_caps_msg;
	int err;

	gve_caps_msg = kzalloc(sizeof(*gve_caps_msg), GFP_KERNEL);
	if (!gve_caps_msg)
		return -ENOMEM;

	gve_caps_msg->msg_version =  cpu_to_le32(GVE_MBX_CAPS_MSG_V1);
	gve_caps_msg->supported_caps |= cpu_to_le64(GVE_MBX_CAP_DQO_RDA);
	gve_fill_version_info(gve_caps_msg);
	gve_caps_msg->msg_size = cpu_to_le32(sizeof(*gve_caps_msg));

	err = gve_send_mbx_msg_wait(mailbox, GVE_MBX_NEGOTIATE_CAPABILITIES,
				    sizeof(*gve_caps_msg), (u8 *)gve_caps_msg);
	if (err)
		dev_err(&mailbox->pdev->dev, "Failed to send negotiate caps mailbox msg\n");
	else
		dev_info(&mailbox->pdev->dev, "hramamurthy: Successfully sent negotiate caps mailbox msg\n");

	kfree(gve_caps_msg);
	return 0;
}

static int gve_mbx_get_interrupt_dbs(struct gve_mailbox *mailbox,
				     struct gve_mbx_get_interrupt_dbs_req *request)
{
	int err;

	err = gve_send_mbx_msg_wait(mailbox, GVE_MBX_GET_INTERRUPT_DBS,
				    sizeof(*request), (u8 *)request);
	if (err)
		dev_err(&mailbox->pdev->dev,
			"Failed to send get interrupt doorbells message.");

	return err;

}

int gve_mbx_request_db_info(struct gve_priv *priv)
{
	int db_infos_per_response, remain_db_infos;
	struct gve_mailbox *mailbox = priv->mailbox;
	// TODO: validate that his actually needs to be num_tfy_blks
	int start_msix_idx = 1;

	/* Compute the maximum number of response items */
	db_infos_per_response = GVE_MBX_BUF_SIZE;
	db_infos_per_response -= sizeof(struct gve_mbx_get_interrupt_dbs_resp);
	db_infos_per_response /= sizeof(struct gve_mbx_interrupt_db_info);

	remain_db_infos = priv->num_ntfy_blks;
	priv->next_msix_vec = start_msix_idx;
	do {
		struct gve_mbx_get_interrupt_dbs_req req;
		int err;

		req.num_vecs = min_t(int, remain_db_infos,
				     db_infos_per_response);
		req.start_msix_index = priv->next_msix_vec;
		err = gve_mbx_get_interrupt_dbs(mailbox, &req);
		if (err)
			return err;

		/* mailbox->last_received_msix_vec is updated synchronously as
		 * part of mailbox RX processing
		 */
		remain_db_infos = (priv->num_ntfy_blks + start_msix_idx) -
			priv->next_msix_vec;
	} while (remain_db_infos);

	return 0;
}

int gve_mbx_get_ptype_map(struct gve_priv *priv)
{
	int err = gve_send_mbx_msg_wait(priv->mailbox, GVE_MBX_GET_PTYPE_MAP, 0,
					NULL);
	if (err)
		dev_err(&priv->pdev->dev,
			"Failed to send get ptype map message.");

	return err;
}

static void gve_post_rx_buffs(struct gve_mailbox *mailbox)
{
	struct gve_mbx_queue *mbx_rx = mailbox->mbx_rx;
	u16 ntp = mbx_rx->next_to_post;
	struct gve_mbx_desc *desc;

	/* no buffers to clean */
	if (((ntp + 1) % mbx_rx->ring_size) == mbx_rx->next_to_clean)
		return;

	while (((ntp + 1) % mbx_rx->ring_size) != mbx_rx->next_to_clean) {
		desc = GVE_MBX_DESC(mbx_rx, ntp);

		if (mailbox->mbx_rx_bufs[ntp])
			goto prep_desc;

		mailbox->mbx_rx_bufs[ntp] = &mailbox->mbx_dma_mem[ntp];

prep_desc:
		desc->flags = cpu_to_le16(GVE_MBX_FLAG_BUF | GVE_MBX_FLAG_RD);
		desc->buf_len = cpu_to_le16(GVE_MBX_BUF_SIZE);
		desc->addr_high =
			cpu_to_le32(upper_32_bits(mailbox->mbx_rx_bufs[ntp]->pa));
		desc->addr_low =
			cpu_to_le32(lower_32_bits(mailbox->mbx_rx_bufs[ntp]->pa));

		ntp++;
		if (ntp == mbx_rx->ring_size)
			ntp = 0;
	}

	/* update tail if buffers were posted */
	if (mbx_rx->next_to_post != ntp) {
		if (ntp)
			mbx_rx->next_to_post = ntp - 1;
		else
			mbx_rx->next_to_post = mbx_rx->ring_size - 1;

		dma_wmb();
		iowrite32(mbx_rx->next_to_post, &mbx_rx->reg->queue_tail);
	}
}

int gve_mbx_set_num_ntfy_blks(struct gve_priv *priv)
{
	/* The first vector is reserved for mailbox interrupt */
	priv->num_ntfy_blks = priv->device_info.num_msix_vectors - 1;
	priv->mgmt_msix_idx = 0;
	return 0;
}

void gve_mbx_set_num_queues(struct gve_priv *priv)
{
	struct gve_device_info *device_info = &priv->device_info;

	priv->tx_cfg.max_queues = device_info->max_tx_queues;
	priv->rx_cfg.max_queues = device_info->max_rx_queues;

	priv->tx_cfg.num_queues = device_info->default_tx_queues;
	priv->rx_cfg.num_queues = device_info->default_rx_queues;
}

void gve_mbx_get_max_queues(struct gve_mailbox *mailbox, int *max_tx_queues,
			    int *max_rx_queues)
{
	struct gve_device_info *device_info = mailbox->device_info;

	*max_tx_queues = device_info->max_tx_queues;
	*max_rx_queues = device_info->max_rx_queues;
}

static void gve_mbx_process_caps(struct gve_mailbox *mailbox,
				 struct gve_mbx_caps_resp *caps_resp)
{
	struct device *dev = &mailbox->pdev->dev;
	u64 caps = le64_to_cpu(caps_resp->negotiated_caps);

	/* Decode and print the negotiated capability flags */
	if (caps & GVE_MBX_CAP_DQO_RDA) {
		mailbox->device_info->queue_format = GVE_DQO_RDA_FORMAT;
		dev_info(dev, "GVE_MBX_CAP_DQO_RDA supported\n");
	}

	if (caps & GVE_MBX_CAP_DQO_QPL) {
		mailbox->device_info->queue_format = GVE_DQO_QPL_FORMAT;
		dev_info(dev, "GVE_MBX_CAP_DQO_QPL supported\n");
	}

	if (caps & GVE_MBX_CAP_INTERRUPT_SHARING)
		dev_info(dev, "GVE_MBX_CAP_INTERRUPT_SHARING supported\n");

	if (caps & GVE_MBX_CAP_FLOW_STEERING)
		dev_info(dev, "GVE_MBX_CAP_FLOW_STEERING supported\n");

	if (caps & GVE_MBX_CAP_NIC_TSTAMP_REG)
		dev_info(dev, "GVE_MBX_CAP_NIC_TSTAMP_REG supported\n");

	if (caps & GVE_MBX_CAP_NIC_TSTAMP_CMD)
		dev_info(dev, "GVE_MBX_CAP_NIC_TSTAMP_CMD supported\n");

	if (caps & GVE_MBX_CAP_HW_GRO)
		dev_info(dev, "GVE_MBX_CAP_HW_GRO supported\n");
}

static int gve_mbx_fill_device_properties(struct gve_mailbox *mailbox,
					  struct gve_dma_mem *recv_msg)
{
	struct gve_device_info *device_info = mailbox->device_info;
	struct gve_mbx_caps_resp *caps = recv_msg->va;

	if (!recv_msg)
		return -EBADMSG;

	gve_mbx_process_caps(mailbox, caps);

	/* queue properties */
	device_info->default_tx_queues = le16_to_cpu(caps->default_tx_queues);
	device_info->default_rx_queues = le16_to_cpu(caps->default_rx_queues);
	device_info->max_tx_queues = le16_to_cpu(caps->max_tx_queues);
	device_info->max_rx_queues = le16_to_cpu(caps->max_rx_queues);

	/* ring size properties */
	device_info->default_tx_ring_size =
					le16_to_cpu(caps->default_tx_ring_size);
	device_info->default_rx_ring_size =
					le16_to_cpu(caps->default_rx_ring_size);
	device_info->max_tx_ring_size = le16_to_cpu(caps->max_tx_ring_size);
	device_info->max_rx_ring_size = le16_to_cpu(caps->max_rx_ring_size);
	device_info->min_tx_ring_size = le16_to_cpu(caps->min_tx_ring_size);
	device_info->min_rx_ring_size = le16_to_cpu(caps->min_rx_ring_size);

	device_info->num_msix_vectors = le16_to_cpu(caps->num_msix_vectors);
	device_info->mbx_irq_db_offset = le32_to_cpu(caps->mbx_irq_db_offset);
	device_info->max_mtu = le16_to_cpu(caps->max_mtu);
	ether_addr_copy(device_info->mac, caps->mac);

	device_info->max_rx_buffer_size =
				le16_to_cpu(caps->max_packet_buffer_size);
	device_info->header_buf_size =
				le16_to_cpu(caps->max_header_buffer_size);

	device_info->rss_key_size = le16_to_cpu(caps->hash_key_size);
	device_info->rss_lut_size = le16_to_cpu(caps->hash_lut_size);
	device_info->cache_rss_config = false;
	return 0;
}

static void gve_mbx_process_interrupt_dbs(struct gve_mailbox *mailbox,
					  struct gve_dma_mem *recv_msg)
{
	struct gve_mbx_get_interrupt_dbs_resp *db_resp = recv_msg->va;
	struct gve_priv *priv = mailbox->priv;
	int i;

	/* Populate notify blocks */
	for (i = 0; i < db_resp->num_vecs; i++) {
		int msix_index = db_resp->start_msix_index + i;
		struct gve_notify_block *block;
		int notify_index;

		if (msix_index == 0 || msix_index > priv->num_ntfy_blks)
			continue;

		notify_index = gve_msix_idx_to_ntfy(priv, msix_index);
		block = &priv->ntfy_blocks[notify_index];
		memcpy(&block->mbx_db_info, &db_resp->info[i],
		       sizeof(struct gve_mbx_interrupt_db_info));
	}

	priv->next_msix_vec =
		db_resp->start_msix_index + db_resp->num_vecs;
}

static int gve_mbx_process_ptype_map(struct gve_mailbox *mailbox,
				     struct gve_dma_mem *recv_msg)
{
	struct gve_ptype_lut *ptype_map = recv_msg->va;

	memcpy(mailbox->priv->ptype_lut_dqo, ptype_map, sizeof(*ptype_map));
	return 0;
}

static int gve_process_mbx_msg(struct gve_mailbox *mailbox, u32 opcode,
			       struct gve_dma_mem *recv_msg)
{
	int err = 0;

	switch (opcode) {
	case GVE_MBX_NEGOTIATE_CAPABILITIES:
		err = gve_mbx_fill_device_properties(mailbox, recv_msg);
		break;
	case GVE_MBX_GET_INTERRUPT_DBS:
		gve_mbx_process_interrupt_dbs(mailbox, recv_msg);
		break;
	case GVE_MBX_GET_PTYPE_MAP:
		err = gve_mbx_process_ptype_map(mailbox, recv_msg);
		break;
	default:
		err = -EBADMSG;
		dev_err_ratelimited(&mailbox->pdev->dev,
				    "Received unrecognized opcode = %d\n",
				    opcode);
	}

	return err;
}

static void gve_process_mbx_resp_status(struct gve_mailbox *mailbox,
					struct gve_mbx_desc *recv_desc)
{
	dev_err_ratelimited(&mailbox->pdev->dev,
		"Error 0x%04X received in mailbox response for opcode 0x%04X\n",
		recv_desc->cmd_retval, recv_desc->cmd_opcode);

	/* GVE_MBX_STATUS_UNAVAILABLE_ERROR means that the device is in
	 * an unrecoverable state. Trigger reset.
	 */
	if (recv_desc->cmd_retval == GVE_MBX_STATUS_UNAVAILABLE_ERROR &&
	    mailbox->priv)
		gve_schedule_reset(mailbox->priv);
}

static void gve_process_mbx_resp_completion(struct gve_mailbox *mailbox,
					    struct gve_mbx_desc *recv_desc)
{
	struct gve_mbx_msg_queue *mbx_msg_q = mailbox->mbx_msg_queue;
	struct gve_mbx_msg *mbx_msg;
	u16 cookie;
	int err;

	err = le16_to_cpu(recv_desc->cmd_retval);
	cookie = le16_to_cpu(recv_desc->cmd_cookie);

	if (cookie >= mbx_msg_q->size) {
		dev_err_ratelimited(&mailbox->pdev->dev,
				    "Received invalid cookie = %d\n", cookie);
		return;
	}

	spin_lock(&mbx_msg_q->mbx_msg_q_lock);
	if (mailbox->mbx_msgs[cookie]) {
		mbx_msg = mailbox->mbx_msgs[cookie];
		mbx_msg->status = err;
		complete(&mbx_msg->work);
	}
	spin_unlock(&mbx_msg_q->mbx_msg_q_lock);
}

int gve_receive_mbx_msg(struct gve_mailbox *mailbox)
{
	struct gve_mbx_queue *mbx_rx = mailbox->mbx_rx;
	struct gve_mbx_desc *recv_desc;
	struct gve_dma_mem *recv_msg;
	u16 ntc, flags;
	int err = 0;
	u32 opcode;

	spin_lock(&mbx_rx->q_lock);

	ntc = mbx_rx->next_to_clean;
	recv_desc = GVE_MBX_DESC(mbx_rx, ntc);
	flags = le16_to_cpu(recv_desc->flags);

	/* check if desc is marked as done */
	if (!(flags & GVE_MBX_FLAG_DD)) {
		err = -EAGAIN;
		goto err_unlock;
	}
	dma_rmb();

	recv_msg = mailbox->mbx_rx_bufs[ntc];

	/* check for error status code in response */
	if (le16_to_cpu(recv_desc->cmd_retval) != GVE_MBX_STATUS_PASSED) {
		gve_process_mbx_resp_status(mailbox, recv_desc);
		gve_process_mbx_resp_completion(mailbox, recv_desc);
		err = -EBADMSG;
		goto update_tail;
	}

	if (!recv_desc->buf_len) {
		/* buffer not used, should not reach this condition */
		err = -EBADMSG;
		dev_err_ratelimited(&mailbox->pdev->dev, "Direct message received, buf_len in descriptor set to 0\n");
		goto update_tail;
	}

	opcode = le32_to_cpu(recv_desc->cmd_opcode);
	err = gve_process_mbx_msg(mailbox, opcode, recv_msg);
	if (err)
		dev_err_ratelimited(&mailbox->pdev->dev,
				    "Error received for opcode 0x%04x\n",
				    opcode);
	gve_process_mbx_resp_completion(mailbox, recv_desc);

update_tail:
	memset(recv_desc, 0, sizeof(*recv_desc));

	/* set to NULL so this can be posted back in post_buffers */
	mailbox->mbx_rx_bufs[ntc] = NULL;

	/* update next_to_clean */
	ntc++;
	if (ntc == mbx_rx->ring_size)
		ntc = 0;

	mailbox->mbx_rx->next_to_clean = ntc;

	spin_unlock(&mbx_rx->q_lock);

	/* post the buffers back */
	gve_post_rx_buffs(mailbox);

	return err;

err_unlock:
	spin_unlock(&mbx_rx->q_lock);
	return err;
}

static void gve_free_mbx_rcv_buffers(struct gve_mailbox *mailbox)
{
	u16 ring_size = mailbox->mbx_rx->ring_size;
	int i;

	if (!mailbox->mbx_rx || !mailbox->mbx_dma_mem)
		return;

	for (i = 0; i < ring_size; i++) {
		struct gve_dma_mem *rx_buf;

		rx_buf = &mailbox->mbx_dma_mem[i];
		if (rx_buf->va)
			dma_free_coherent(&mailbox->pdev->dev, rx_buf->size,
					  rx_buf->va, rx_buf->pa);
	}

	kfree(mailbox->mbx_dma_mem);
	mailbox->mbx_dma_mem = NULL;

	kfree(mailbox->mbx_rx_bufs);
	mailbox->mbx_rx_bufs = NULL;
}

static int gve_alloc_mbx_rcv_buffers(struct gve_mailbox *mailbox)
{
	u16 ring_size = mailbox->mbx_rx->ring_size;
	int i;

	mailbox->mbx_rx_bufs = kcalloc(ring_size, sizeof(struct gve_dma_mem *),
				       GFP_KERNEL);

	if (!mailbox->mbx_rx_bufs)
		return -ENOMEM;

	mailbox->mbx_dma_mem = kcalloc(ring_size, sizeof(struct gve_dma_mem),
				       GFP_KERNEL);

	if (!mailbox->mbx_dma_mem) {
		kfree(mailbox->mbx_rx_bufs);
		mailbox->mbx_rx_bufs = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < ring_size - 1; i++) {
		struct gve_dma_mem *rx_buf;

		mailbox->mbx_rx_bufs[i] = &mailbox->mbx_dma_mem[i];
		rx_buf = mailbox->mbx_rx_bufs[i];
		rx_buf->va = dma_alloc_coherent(&mailbox->pdev->dev,
						GVE_MBX_BUF_SIZE, &rx_buf->pa,
						GFP_KERNEL);
		if (!rx_buf->va) {
			goto err_free_bufs;
		}
		rx_buf->size = GVE_MBX_BUF_SIZE;
	}
	mailbox->mbx_rx->next_to_post = ring_size - 1;

	return 0;

err_free_bufs:
	gve_free_mbx_rcv_buffers(mailbox);
	return -ENOMEM;
}

static void gve_post_initial_rx_bufs(struct gve_mailbox *mailbox)
{
	struct gve_mbx_queue *mbx_rx = mailbox->mbx_rx;
	int i;

	for (i = 0; i < mbx_rx->ring_size - 1; i++) {
		struct gve_mbx_desc *desc = GVE_MBX_DESC(mbx_rx, i);
		struct gve_dma_mem *gve_rx_buf = mailbox->mbx_rx_bufs[i];

		desc->flags = cpu_to_le16(GVE_MBX_FLAG_BUF | GVE_MBX_FLAG_RD);
		desc->addr_high = cpu_to_le32(upper_32_bits(gve_rx_buf->pa));
		desc->addr_low = cpu_to_le32(lower_32_bits(gve_rx_buf->pa));
		desc->buf_len = GVE_MBX_BUF_SIZE;
	}
	dma_wmb();
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

static void gve_free_mbx_msg_queue(struct gve_mailbox *mailbox)
{
	int i;

	if (!mailbox->mbx_msg_queue)
		return;

	for (i = 0; i < mailbox->mbx_msg_queue->size; i++)
		kfree(mailbox->mbx_msgs[i]);

	kfree(mailbox->mbx_msgs);
	mailbox->mbx_msgs = NULL;
	bitmap_free(mailbox->mbx_msg_queue->msg_queue_map);
	kfree(mailbox->mbx_msg_queue);
	mailbox->mbx_msg_queue = NULL;
}

void gve_free_mailbox(struct gve_mailbox *mailbox, void __iomem *reg_bar0)
{
	int err;

	err = gve_mbx_reset(mailbox, reg_bar0);
	if (err)
		dev_err(&mailbox->pdev->dev, "Failed to reset in mailbox mode\n");

	cancel_delayed_work_sync(&mailbox->gve_mbx_task);
	if (mailbox->gve_mbx_wq) {
		destroy_workqueue(mailbox->gve_mbx_wq);
		mailbox->gve_mbx_wq = NULL;
	}

	gve_free_mbx_msg_queue(mailbox);
	gve_free_mbx_rcv_buffers(mailbox);
	kfree(mailbox->mbx_tx_bufs);

	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_rx);
	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_tx);

	kfree(mailbox->mbx_rx);
	mailbox->mbx_rx = NULL;
	kfree(mailbox->mbx_tx);
	mailbox->mbx_tx = NULL;
}

static int gve_alloc_mbx_msg_queue(struct gve_mailbox *mailbox)
{
	struct gve_mbx_msg_queue *mbx_msg_queue;
	int err;

	mailbox->mbx_msg_queue = kzalloc(sizeof(*mailbox->mbx_msg_queue),
					 GFP_KERNEL);
	if (!mailbox->mbx_msg_queue)
		return -ENOMEM;

	mbx_msg_queue = mailbox->mbx_msg_queue;
	mbx_msg_queue->size = GVE_MBX_MSG_QUEUE_LEN;
	mbx_msg_queue->msg_queue_map = bitmap_zalloc(mbx_msg_queue->size,
						     GFP_KERNEL);

	if (!mbx_msg_queue->msg_queue_map) {
		err = -ENOMEM;
		goto err_free_msg_queue;
	}

	mailbox->mbx_msgs = kcalloc(mbx_msg_queue->size,
				    sizeof(struct gve_mbx_msg *), GFP_KERNEL);
	if (!mailbox->mbx_msgs) {
		err = -ENOMEM;
		goto err_free_msg_queue_map;
	}

	spin_lock_init(&mbx_msg_queue->mbx_msg_q_lock);
	return 0;

err_free_msg_queue_map:
	bitmap_free(mailbox->mbx_msg_queue->msg_queue_map);
err_free_msg_queue:
	kfree(mailbox->mbx_msg_queue);
	mailbox->mbx_msg_queue = NULL;
	return err;
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

	mailbox->mbx_tx_bufs = kcalloc(mailbox->mbx_tx->ring_size,
				       sizeof(struct gve_dma_mem *),
				       GFP_KERNEL);
	if (!mailbox->mbx_tx_bufs)
		goto free_mbx_rx_desc_ring;

	err = gve_alloc_mbx_rcv_buffers(mailbox);
	if (err)
		goto free_mbx_tx_bufs;

	err = gve_alloc_mbx_msg_queue(mailbox);
	if (err)
		goto free_mbx_rcv_bufs;

	mailbox->gve_mbx_wq = alloc_workqueue("gve-mbx", 0, 0);
	if (!mailbox->gve_mbx_wq) {
		err = -ENOMEM;
		dev_err(&mailbox->pdev->dev,
			"Failed to allocate mailbox workqueue\n");
		goto free_mbx_msg_queue;
	}

	gve_post_initial_rx_bufs(mailbox);
	return 0;

free_mbx_msg_queue:
	gve_free_mbx_msg_queue(mailbox);
free_mbx_rcv_bufs:
	gve_free_mbx_rcv_buffers(mailbox);
free_mbx_tx_bufs:
	kfree(mailbox->mbx_tx_bufs);
free_mbx_rx_desc_ring:
	gve_free_mbx_desc_ring(mailbox, mailbox->mbx_rx);
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

	INIT_DELAYED_WORK(&mailbox->gve_mbx_task, gve_mbx_task);
	queue_delayed_work(mailbox->gve_mbx_wq, &mailbox->gve_mbx_task, 0);
	return 0;
}
