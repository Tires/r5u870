/*
 * USBCAM abstraction library for USB webcam drivers
 * Private/internal header - V4L2 + videobuf2 compatible
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __USBCAM_PRIV_H__
#define __USBCAM_PRIV_H__

/*
 * Kernel headers
 */
#include <linux/videodev2.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>

/*
 * Public types (pixel formats, controls, urbstream, dev_ops, etc.)
 */
#include "usbcam.h"

/*
 * Internal frame descriptor (used by usbcam_buf.c for frame accumulation)
 */
struct usbcam_frame {
	struct list_head		fl_links;
	unsigned char			*fl_buf;
	int				fl_size;
	int				fl_filled;
	int				fl_done;
	struct vb2_buffer		*fl_vb;
};

/*
 * Main device structure (internal definition with all driver fields)
 *
 * NOTE: This is the authoritative definition used by the library .c files.
 * The public header (usbcam.h) forward-declares struct usbcam_dev for
 * minidriver callback signatures.
 */
struct usbcam_dev {
	/* V4L2 video device (must be first for container_of patterns) */
	struct video_device		vdev;

	/* V4L2 device (required by kernel 7.0 video_register_device) */
	struct v4l2_device		v4l2_dev;

	/* USB interface and device */
	struct usb_interface		*intf;
	struct usb_device		*udev;
	const struct usb_device_id	*devid;

	/* Videobuf2 queue for frame buffers */
	struct vb2_queue		vq;

	/* URB streaming infrastructure */
	struct usbcam_urbstream		urbstream;

	/* V4L2 controls */
	struct usbcam_ctrl		*ctrls;
	int				ctrls_len;

	/* Pixel formats */
	const struct usbcam_pix_fmt	*fmts;
	int				fmts_len;
	int				fmts_elem_size;
	struct usbcam_pix_fmt		*curfmt;

	/* State flags */
	int				streaming;
	int				opened;
	int				disconnected;

	/* Synchronization */
	struct mutex			lock;
	wait_queue_head_t		wq;

	/* Frame accumulation */
	int				frame_accum;
	int				frame_size;

	/* Minidriver operations */
	const struct usbcam_dev_ops	*ops;
	void				*minidrv_data;
	int				minidrv_id;

	/* Video device minor number */
	int				video_nr;

	/* DMA device pointer (for dma_map/dma_unmap) */
	struct device			*dev;

	/* Debug level (bitmask of USBCAM_DBG_* bits) */
	int				debug;

	/* URB timeout in jiffies */
	int				timeout;

	/* Deferred work items */
	struct usbcam_workitem		*workitem;
	struct usbcam_delayedwork	*delayedwork;

	/* Current frame tracking */
	int				curframe;
	unsigned char			*curframe_buf;
	int				curframe_size;

	/* Frame list (linked list of struct usbcam_frame) */
	struct list_head		frame_list;
	spinlock_t			frame_lock;
};

/*
 * Helper macros
 */

/* Convert a video_device pointer back to the containing usbcam_dev */
#define usbcam_to_dev(vdev)	container_of(vdev, struct usbcam_dev, vdev)

/* Convert a vb2_queue pointer back to the containing usbcam_dev */
#define usbcam_to_dev_vq(vq)	container_of(vq, struct usbcam_dev, vq)

/* Debug logging (gated by debug bitmask) */
#define usbcam_dbg(UDP, BIT, fmt, ...) \
	if ((UDP)->debug & (1 << (BIT))) \
		dev_info((UDP)->dev, fmt, ##__VA_ARGS__)

/* Unconditional error/warning logging */
#define usbcam_err(UDP, fmt, ...) \
	dev_err((UDP)->dev, fmt, ##__VA_ARGS__)

#define usbcam_warn(UDP, fmt, ...) \
	dev_warn((UDP)->dev, fmt, ##__VA_ARGS__)

#define usbcam_info(UDP, fmt, ...) \
	dev_info((UDP)->dev, fmt, ##__VA_ARGS__)

/*
 * Internal (non-public) function declarations
 * These are shared between usbcam_buf.c, usbcam_dev.c, usbcam_fops.c,
 * and usbcam_util.c but are NOT part of the minidriver API.
 */

/* --- usbcam_buf.c internals --- */
extern int usbcam_vb2_start_streaming_urb(struct vb2_queue *vq, unsigned int count);
extern void usbcam_vb2_stop_streaming_urb(struct vb2_queue *vq);

/* --- usbcam_dev.c internals --- */
extern void usbcam_ctrl_releaseall(struct usbcam_dev *udp);
extern int usbcam_dev_probe(struct usbcam_dev *udp,
			     const struct usbcam_dev_ops *ops,
			     int video_nr);
extern void usbcam_dev_remove(struct usbcam_dev *udp);
extern int usbcam_videodev_register(struct usbcam_dev *udp);
extern void usbcam_videodev_unregister(struct usbcam_dev *udp);

/* --- usbcam_fops.c internals (static, no extern needed) --- */

/* --- usbcam_util.c internals --- */
extern int usbcam_claim_interface(struct usbcam_dev *udp, int ifnum);
extern int usbcam_choose_altsetting(struct usbcam_dev *udp, int ifnum,
				    int pipe, int bytes_per_sec_min,
				    int pkt_min, int pkt_max,
				    int *altsetting_nr);
extern struct urb *usbcam_urb_alloc(struct usbcam_dev *udp, int pipe,
				    int pktsize, int npkts, int alloc_buf);
extern void usbcam_urb_free(struct urb *urbp, int free_buf);
extern void usbcam_hexdump(struct usbcam_dev *udp, const u8 *buf, size_t len);

/*
 * Internal template structures (defined in usbcam_fops.c / usbcam_dev.c)
 */
extern const struct v4l2_file_operations usbcam_v4l2_fops_template;
extern const struct video_device usbcam_videodev_template;

/*
 * Minidriver module registration macro
 *
 * Expands to a complete USB driver with probe/disconnect that allocates
 * the usbcam_dev + minidrv_data, registers the V4L2 device, and calls
 * the minidriver ops.  Usage:
 *
 *   DEFINE_USBCAM_MINIDRV_MODULE(VERSION, VERSION_EXTRA,
 *                                &my_dev_ops, sizeof(struct my_ctx), id_table)
 */
#define __usbcam_md_str(x) #x

#define DEFINE_USBCAM_MINIDRV_MODULE(VERSION, VERSION_EXTRA, OPS, DATA_SIZE, ID_TABLE) \
static int __usbcam_md_probe(struct usb_interface *intf, \
			     const struct usb_device_id *devid) \
{ \
	struct usbcam_dev *udp; \
	int ret; \
	udp = kzalloc(sizeof(*udp) + DATA_SIZE, GFP_KERNEL); \
	if (!udp) \
		return -ENOMEM; \
	udp->intf = intf; \
	usb_set_intfdata(intf, udp); \
	udp->udev = interface_to_usbdev(intf); \
	udp->devid = devid; \
	udp->dev = &intf->dev; \
	udp->minidrv_data = (char *)udp + sizeof(*udp); \
	udp->ops = OPS; \
	ret = usbcam_register(udp, OPS, -1); \
	if (ret) { \
		kfree(udp); \
		return ret; \
	} \
	if ((OPS)->init) { \
		ret = (OPS)->init(udp); \
		if (ret) { \
			usbcam_unregister(udp); \
			kfree(udp); \
			return ret; \
		} \
	} \
	return 0; \
} \
static void __usbcam_md_disconnect(struct usb_interface *intf) \
{ \
	struct usbcam_dev *udp = usb_get_intfdata(intf); \
	if (!udp) \
		return; \
	if ((OPS)->exit) \
		(OPS)->exit(udp); \
	usbcam_unregister(udp); \
	kfree(udp); \
} \
static struct usb_driver __usbcam_md_driver = { \
	.name		= KBUILD_MODNAME, \
	.probe		= __usbcam_md_probe, \
	.disconnect	= __usbcam_md_disconnect, \
	.id_table	= ID_TABLE, \
}; \
module_usb_driver(__usbcam_md_driver); \
MODULE_VERSION(__usbcam_md_str(VERSION) VERSION_EXTRA)

#endif /* __USBCAM_PRIV_H__ */
