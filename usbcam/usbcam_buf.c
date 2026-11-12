/*
 * USBCAM abstraction library for USB webcam drivers
 * V4L2 / videobuf2-compatible buffer management + URB streaming engine
 *
 * Copyright (c) 2007 Sam Revitch <samr7@cs.washington.edu>
 * Copyright (c) 2008 Alexander Hixon <hixon.alexander@mediati.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/ktime.h>

#include "usbcam.h"
#include "usbcam_priv.h"

/* ------------------------------------------------------------------ */
/*  Workitem queue (deferred work)                                     */
/* ------------------------------------------------------------------ */

void usbcam_workitem_init(struct usbcam_workitem *work,
			   struct usbcam_dev *udp,
			   usbcam_workfunc_t func)
{
	INIT_LIST_HEAD(&work->uw_links);
	work->uw_dev = udp;
	work->uw_func = func;
}

void usbcam_workitem_queue(struct usbcam_workitem *work)
{
	/*
	 * Execute the deferred work function.
	 * In a more complex implementation this would be queued to a
	 * kernel workqueue for execution in process context.  Here we
	 * call it directly (safe because the work functions in this
	 * driver are short and non-blocking).
	 */
	if (work->uw_func)
		work->uw_func(work);
}

/* ------------------------------------------------------------------ */
/*  URB streaming engine                                               */
/* ------------------------------------------------------------------ */

/*
 * Per-URB private data.  Attached to each URB via urb->context.
 * Also linked into the stream's unused/active/complete lists.
 */
struct usbcam_urb_priv {
	struct list_head	link;		/* node in us_unused/active/complete */
	struct urb		*urb;
	struct usbcam_urbstream	*us;		/* back-pointer to stream */
	unsigned int		seq;		/* index in the URB pool */
	unsigned char		ep_num;		/* endpoint number (kernel 7.0 compat) */
};

/*
 * Track the current vb2 buffer being filled (per-device, set when a
 * buffer is dequeued for streaming).
 */
static DEFINE_SPINLOCK(usbcam_vb_lock);
static struct vb2_buffer *usbcam_cur_vb;

/* ------------------------------------------------------------------ */
/*  URB completion handler (host-side, called in IRQ context)          */
/* ------------------------------------------------------------------ */

static void usbcam_urb_complete(struct urb *urb)
{
	struct usbcam_urb_priv *priv = (struct usbcam_urb_priv *)urb->context;
	struct usbcam_urbstream *us = priv->us;
	struct usbcam_dev *udp = us->us_dev;
	int status = urb->status;
	unsigned int xfer = urb->actual_length;

	/*
	 * --- Error handling -------------------------------------------
	 */
	if (status != 0) {
		if (status == -ESHUTDOWN || status == -ECONNRESET ||
		    status == -ENODEV || status == -EPIPE) {
			/* Fatal: device gone, stream stopped, or EP stalled */
			usbcam_err(udp, "URB fatal error %d, stopping stream\n",
				   status);
			us->us_streaming = 0;
			if (us->us_ops && us->us_ops->submit_error)
				us->us_ops->submit_error(udp, us, status);
			goto move_to_complete;
		}

		/* Transient error (babble, etc.) – log and resubmit */
		usbcam_warn(udp, "URB transient error %d, resubmitting\n",
			    status);
		us->us_resubmit_err++;

		if (us->us_ops && us->us_ops->submit_error)
			us->us_ops->submit_error(udp, us, status);
	}

	/*
	 * --- Frame accumulation ----------------------------------------
	 * Accumulate URB payload into the current frame buffer.
	 * Multiple URBs may make up one frame (frame_size > pktsize).
	 */
	if (status == 0 && xfer > 0 && udp->curframe_buf) {
		unsigned int offset = (unsigned int)udp->frame_accum;
		unsigned int fsize = (unsigned int)udp->frame_size;
		unsigned int copy_len;

		if (offset < fsize) {
			copy_len = min_t(unsigned int, xfer, fsize - offset);
			memcpy(udp->curframe_buf + offset,
			       urb->transfer_buffer, copy_len);
			udp->frame_accum += (int)copy_len;
		}

		/*
		 * Frame complete?  Call the driver's callback which
		 * marks the vb2 buffer done and dequeues the next one.
		 */
		if (udp->frame_accum >= udp->frame_size) {
			udp->frame_accum = 0;
			usbcam_curframe_complete(udp);
		}
	}

	/*
	 * --- Re-submit URB if streaming is still active ----------------
	 */
	if (us->us_streaming) {
		int ret;

		urb->actual_length = 0;
		urb->status = 0;

		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret) {
			usbcam_err(udp, "URB resubmit failed (%d)\n", ret);
			us->us_streaming = 0;
			goto move_to_complete;
		}

		/* URB is active again – it stays on the active list */
		return;
	}

move_to_complete:
	/*
	 * Move URB from active list to complete list.
	 */
	spin_lock(&us->us_lock);
	list_del(&priv->link);
	list_add_tail(&priv->link, &us->us_complete_list);
	us->us_active_count--;
	spin_unlock(&us->us_lock);

	/* Wake waiters if no active URBs remain */
	if (us->us_active_count == 0)
		complete_all(&us->us_active_empty);
}

/* ------------------------------------------------------------------ */
/*  URB stream initialisation                                          */
/* ------------------------------------------------------------------ */

int usbcam_urbstream_init(struct usbcam_urbstream *us,
			   struct usbcam_dev *udp,
			   int nurb, int pktsize, int pktcount,
			   int interval, int ep, int type)
{
	struct usbcam_urb_priv *priv;
	struct urb *urb;
	void *buf;
	int i, ret;
	unsigned int len;

	/*
	 * Determine transfer buffer size per URB.
	 * Bulk: one packet per URB.
	 * Iso:  pktcount packets per URB.
	 */
	if (type & USB_ENDPOINT_XFER_ISOC)
		len = (unsigned int)(pktsize * pktcount);
	else
		len = (unsigned int)pktsize;

	/*
	 * Initialise the public stream structure.
	 */
	us->us_dev = udp;
	us->us_endpoint = (char)ep;
	us->us_streaming = 0;
	us->us_active_goal = (char)nurb;
	us->us_active_count = 0;
	us->us_timeout_ticks = 0;
	us->us_resubmit_err = 0;
	us->us_ops = NULL;

	INIT_LIST_HEAD(&us->us_unused_list);
	INIT_LIST_HEAD(&us->us_active_list);
	INIT_LIST_HEAD(&us->us_complete_list);
	init_completion(&us->us_active_empty);
	spin_lock_init(&us->us_lock);

	/* Workitem for deferred error handling */
	usbcam_workitem_init(&us->us_error_workitem, udp, NULL);

	/*
	 * Allocate the URB pool.
	 */
	for (i = 0; i < nurb; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		{
			dma_addr_t dma_handle;

			buf = usb_alloc_coherent(udp->udev, len, GFP_KERNEL,
						 &dma_handle);
			if (!buf) {
				usb_free_urb(urb);
				ret = -ENOMEM;
				goto err_cleanup;
			}

			urb->dev = udp->udev;
			urb->transfer_buffer = buf;
			urb->transfer_dma = dma_handle;
			urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
			urb->transfer_buffer_length = len;
			urb->context = NULL;	/* set after priv alloc */
			urb->complete = usbcam_urb_complete;

			priv = kzalloc(sizeof(*priv), GFP_KERNEL);
			if (!priv) {
				usb_free_coherent(udp->udev, len, buf, dma_handle);
				usb_free_urb(urb);
				ret = -ENOMEM;
				goto err_cleanup;
			}

			priv->urb = urb;
			priv->us = us;
			priv->seq = (unsigned int)i;
			priv->ep_num = (unsigned char)ep;
			INIT_LIST_HEAD(&priv->link);

			urb->context = priv;
		}

		list_add_tail(&priv->link, &us->us_unused_list);
	}

	/*
	 * Set the frame size in the device struct.
	 * For bulk: frame = pktsize (one URB per frame chunk).
	 * For iso:  frame = pktsize * pktcount.
	 */
	udp->frame_size = (int)len;
	udp->frame_accum = 0;

	usbcam_info(udp, "URB stream init: nurb=%d pktsize=%d pktcount=%d\n",
		     nurb, pktsize, pktcount);

	return 0;

err_cleanup:
	/* Free all already-allocated URBs */
	while (!list_empty(&us->us_unused_list)) {
		priv = list_first_entry(&us->us_unused_list,
					 struct usbcam_urb_priv, link);
		list_del(&priv->link);
		usb_free_coherent(udp->udev, len,
				  priv->urb->transfer_buffer,
				  priv->urb->transfer_dma);
		usb_free_urb(priv->urb);
		kfree(priv);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/*  Configuration helpers                                              */
/* ------------------------------------------------------------------ */

int usbcam_urbstream_config_iso(struct usbcam_urbstream *us,
				 int pktsize, int pktcount, int interval)
{
	struct usbcam_dev *udp = us->us_dev;

	/*
	 * Update frame parameters for isochronous mode.
	 * In a full implementation we would re-allocate URBs.
	 */
	udp->frame_size = pktsize * pktcount;
	udp->frame_accum = 0;

	usbcam_info(udp, "URB stream config iso: pktsize=%d pktcount=%d interval=%d\n",
		     pktsize, pktcount, interval);

	return 0;
}

int usbcam_urbstream_config_bulk(struct usbcam_urbstream *us,
				 int pktsize)
{
	struct usbcam_dev *udp = us->us_dev;

	udp->frame_size = pktsize;
	udp->frame_accum = 0;

	usbcam_info(udp, "URB stream config bulk: pktsize=%d\n", pktsize);

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Start / Stop / Cleanup                                             */
/* ------------------------------------------------------------------ */

int usbcam_urbstream_start(struct usbcam_urbstream *us)
{
	struct usbcam_dev *udp = us->us_dev;
	struct usbcam_urb_priv *priv, *tmp;
	int ret = 0;

	if (us->us_streaming)
		return 0;

	us->us_streaming = 1;
	udp->frame_accum = 0;

	/*
	 * Submit all URBs from the unused list.
	 */
	list_for_each_entry_safe(priv, tmp, &us->us_unused_list, link) {
		list_del(&priv->link);
		list_add_tail(&priv->link, &us->us_active_list);

		priv->urb->actual_length = 0;
		priv->urb->status = 0;

		ret = usb_submit_urb(priv->urb, GFP_KERNEL);
		if (ret) {
			usbcam_err(udp, "URB submit failed (%d)\n", ret);
			/* Put it back on the unused list */
			list_del(&priv->link);
			list_add_tail(&priv->link, &us->us_unused_list);
			us->us_streaming = 0;
			return ret;
		}

		us->us_active_count++;
	}

	usbcam_info(udp, "URB stream started (%d URBs active)\n",
		     us->us_active_count);

	return 0;
}

void usbcam_urbstream_stop(struct usbcam_urbstream *us)
{
	struct usbcam_dev *udp = us->us_dev;
	struct usbcam_urb_priv *priv, *tmp;

	if (!us->us_streaming)
		return;

	us->us_streaming = 0;

	/*
	 * Kill all active URBs.  usb_kill_urb() is synchronous –
	 * it waits for the completion handler to finish.
	 */
	list_for_each_entry_safe(priv, tmp, &us->us_active_list, link) {
		usb_kill_urb(priv->urb);
		list_del(&priv->link);
		list_add_tail(&priv->link, &us->us_unused_list);
		us->us_active_count--;
	}

	/*
	 * Move any URBs in the complete list back to unused.
	 */
	list_for_each_entry_safe(priv, tmp, &us->us_complete_list, link) {
		list_del(&priv->link);
		list_add_tail(&priv->link, &us->us_unused_list);
	}

	usbcam_info(udp, "URB stream stopped\n");
}

void usbcam_urbstream_cleanup(struct usbcam_urbstream *us)
{
	struct usbcam_dev *udp = us->us_dev;
	struct usbcam_urb_priv *priv, *tmp;

	usbcam_urbstream_stop(us);

	/*
	 * Free all URBs and their DMA buffers from every list.
	 */
	list_for_each_entry_safe(priv, tmp, &us->us_unused_list, link) {
		list_del(&priv->link);
		usb_free_coherent(udp->udev,
				  priv->urb->transfer_buffer_length,
				  priv->urb->transfer_buffer,
				  priv->urb->transfer_dma);
		usb_free_urb(priv->urb);
		kfree(priv);
	}

	list_for_each_entry_safe(priv, tmp, &us->us_active_list, link) {
		list_del(&priv->link);
		usb_free_coherent(udp->udev,
				  priv->urb->transfer_buffer_length,
				  priv->urb->transfer_buffer,
				  priv->urb->transfer_dma);
		usb_free_urb(priv->urb);
		kfree(priv);
	}

	list_for_each_entry_safe(priv, tmp, &us->us_complete_list, link) {
		list_del(&priv->link);
		usb_free_coherent(udp->udev,
				  priv->urb->transfer_buffer_length,
				  priv->urb->transfer_buffer,
				  priv->urb->transfer_dma);
		usb_free_urb(priv->urb);
		kfree(priv);
	}

	INIT_LIST_HEAD(&us->us_unused_list);
	INIT_LIST_HEAD(&us->us_active_list);
	INIT_LIST_HEAD(&us->us_complete_list);
	us->us_active_count = 0;

	usbcam_info(udp, "URB stream cleaned up\n");
}

/* ------------------------------------------------------------------ */
/*  Frame-complete callback (called from URB completion context)       */
/* ------------------------------------------------------------------ */

void usbcam_curframe_complete(struct usbcam_dev *udp)
{
	struct vb2_buffer *vb;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;

	spin_lock_irqsave(&usbcam_vb_lock, flags);
	vb = usbcam_cur_vb;
	usbcam_cur_vb = NULL;
	spin_unlock_irqrestore(&usbcam_vb_lock, flags);

	if (!vb)
		return;

	/*
	 * Fill in the V4L2 buffer metadata via the vb2_v4l2_buffer wrapper.
	 */
	vbuf = to_vb2_v4l2_buffer(vb);
	vbuf->vb2_buf.planes[0].bytesused = (unsigned int)udp->frame_size;
	vbuf->vb2_buf.timestamp = ktime_get_ns();

	/*
	 * Mark the buffer as done and hand it to the vb2 core.
	 * vb2_buffer_done() wakes up any waiting userspace reader (DQBUF).
	 */
	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);

	/*
	 * Clear the current frame pointer in the device struct.
	 */
	udp->curframe_buf = NULL;
	udp->curframe = 0;

	usbcam_info(udp, "frame complete (bytes=%u)\n",
		     (unsigned int)udp->frame_size);
}

/* ------------------------------------------------------------------ */
/*  Buffer queue / dequeue helpers (vb2 integration)                   */
/* ------------------------------------------------------------------ */

int usbcam_buf_queue(struct usbcam_dev *udp, struct vb2_buffer *vb)
{
	unsigned long flags;

	/*
	 * In modern kernels (>= 4.x), buffers are queued by the vb2 core
	 * via VIDIOC_QBUF.  This helper simply records the buffer as the
	 * current frame target so the URB completion handler knows where
	 * to accumulate data.
	 */
	if (udp->streaming) {
		spin_lock_irqsave(&usbcam_vb_lock, flags);
		usbcam_cur_vb = vb;
		spin_unlock_irqrestore(&usbcam_vb_lock, flags);

		/*
		 * Point the device's frame buffer at the vb2 plane data.
		 * Use the modern plane access (no .m.userptr union).
		 */
		udp->curframe_buf = (unsigned char *)vb2_plane_vaddr(vb, 0);
		udp->curframe = 1;
		udp->frame_accum = 0;
	}

	return 0;
}

void usbcam_buf_dequeue(struct usbcam_dev *udp)
{
	/*
	 * In modern kernels, buffer dequeue is handled by the vb2 core
	 * via VIDIOC_DQBUF. This helper is a no-op placeholder for
	 * compatibility with older driver code paths.
	 */
	usbcam_info(udp, "buf_dequeue: handled by vb2 core\n");
}

/* ------------------------------------------------------------------ */
/*  URB streaming start/stop (called from usbcam_dev.c vb2_ops)       */
/* ------------------------------------------------------------------ */

int usbcam_vb2_start_streaming_urb(struct vb2_queue *vq, unsigned int count)
{
	struct usbcam_dev *udp = container_of(vq, struct usbcam_dev, vq);
	int ret;

	if (count < 2) {
		usbcam_err(udp, "need at least 2 buffers for streaming\n");
		return -EINVAL;
	}

	udp->streaming = 1;

	/* Start the URB stream */
	ret = usbcam_urbstream_start(&udp->urbstream);
	if (ret) {
		udp->streaming = 0;
		return ret;
	}

	usbcam_info(udp, "streaming started (count=%u)\n", count);
	return 0;
}

void usbcam_vb2_stop_streaming_urb(struct vb2_queue *vq)
{
	struct usbcam_dev *udp = container_of(vq, struct usbcam_dev, vq);

	usbcam_urbstream_stop(&udp->urbstream);
	udp->streaming = 0;

	usbcam_info(udp, "streaming stopped\n");
}

/* ------------------------------------------------------------------ */
/*  NOTE: This file is linked into r5u870.ko (not a standalone module). */
/*  Module init/exit and MODULE_* are provided by the                   */
/*  DEFINE_USBCAM_MINIDRV_MODULE macro in r5u870.c.                     */
/* ------------------------------------------------------------------ */
