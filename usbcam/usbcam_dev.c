/*
 * USBCAM abstraction library for USB webcam drivers
 * V4L2-compatible device registration
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

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "usbcam.h"
#include "usbcam_priv.h"

/* Extern fops / ioctl_ops defined in usbcam_fops.c */
extern const struct v4l2_file_operations usbcam_v4l2_fops_template;
extern const struct v4l2_ioctl_ops usbcam_v4l2_ioctl_ops;

/*
 * Private per-buffer data for videobuf2.
 * struct vb2_v4l2_buffer MUST be the first member so that
 * container_of() / vb2_to_vb2_v4l2_buffer() work correctly.
 */
struct usbcam_vb2_buf {
	struct vb2_v4l2_buffer vb;
};

/*
 * videobuf2 queue operations
 */
static int usbcam_vb2_queue_setup(struct vb2_queue *vq,
				   unsigned int *num_buffers,
				   unsigned int *num_planes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct usbcam_dev *udp = vb2_get_drv_priv(vq);

	if (!udp->curfmt)
		return -EINVAL;

	*num_planes = 1;
	sizes[0] = udp->curfmt->sizeimage;
	return 0;
}

static int usbcam_vb2_buf_init(struct vb2_buffer *vb)
{
	/* Buffer is already allocated by vb2; nothing extra to init */
	return 0;
}

static void usbcam_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	/* Nothing to free; vb2 handles buffer deallocation */
}

static void usbcam_vb2_buf_queue(struct vb2_buffer *vb)
{
	/* Buffer is queued by the URB streaming layer; nothing extra to do */
}

static int usbcam_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct usbcam_dev *udp = vb2_get_drv_priv(vb->vb2_queue);

	/* Validate that the buffer is large enough for one frame */
	if (vb->planes[0].length < (unsigned int)udp->frame_size)
		return -EINVAL;

	return 0;
}

static int usbcam_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct usbcam_dev *udp = usbcam_to_dev_vq(vq);
	int ret;

	if (udp->ops && udp->ops->cap_start) {
		ret = udp->ops->cap_start(udp);
		if (ret)
			return ret;
	}

	return usbcam_vb2_start_streaming_urb(vq, count);
}

static void usbcam_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct usbcam_dev *udp = usbcam_to_dev_vq(vq);

	usbcam_vb2_stop_streaming_urb(vq);

	if (udp->ops && udp->ops->cap_stop)
		udp->ops->cap_stop(udp);
}

static const struct vb2_ops usbcam_vb2_ops = {
	.queue_setup		= usbcam_vb2_queue_setup,
	.buf_init		= usbcam_vb2_buf_init,
	.buf_cleanup		= usbcam_vb2_buf_cleanup,
	.buf_prepare		= usbcam_vb2_buf_prepare,
	.buf_queue		= usbcam_vb2_buf_queue,
	.start_streaming	= usbcam_vb2_start_streaming,
	.stop_streaming		= usbcam_vb2_stop_streaming,
};

/*
 * usbcam_register - register a V4L2 video device and its vb2 queue
 *
 * @udp:      device instance (caller-allocated)
 * @ops:      minidriver operations table
 * @video_nr: minor number (or -1 for auto-assign)
 *
 * Returns 0 on success, negative errno on failure.
 */
int usbcam_register(struct usbcam_dev *udp,
		     const struct usbcam_dev_ops *ops,
		     int video_nr)
{
	int ret;

	mutex_init(&udp->lock);
	init_waitqueue_head(&udp->wq);

	/* --- videobuf2 queue setup --- */
	udp->vq.io_modes = VB2_MMAP;
	udp->vq.ops = &usbcam_vb2_ops;
	udp->vq.mem_ops = &vb2_vmalloc_memops;
	udp->vq.lock = &udp->lock;
	udp->vq.drv_priv = udp;
	udp->vq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	udp->vq.dev = udp->dev;

	/* --- v4l2_device setup (required by kernel 7.0) --- */
	strscpy(udp->v4l2_dev.name, "usbcam", sizeof(udp->v4l2_dev.name));
	ret = v4l2_device_register(udp->dev, &udp->v4l2_dev);
	if (ret < 0) {
		dev_err(udp->dev, "usbcam: v4l2_device_register failed: %d\n", ret);
		return ret;
	}

	/* --- video_device setup --- */
	strscpy(udp->vdev.name, "usbcam", sizeof(udp->vdev.name));
	udp->vdev.vfl_type = VFL_TYPE_VIDEO;
	udp->vdev.vfl_dir = VFL_DIR_RX;
	udp->vdev.fops = &usbcam_v4l2_fops_template;
	udp->vdev.ioctl_ops = &usbcam_v4l2_ioctl_ops;
	udp->vdev.release = video_device_release_empty;
	udp->vdev.v4l2_dev = &udp->v4l2_dev;
	udp->vdev.dev_parent = udp->dev;
	udp->vdev.dev.parent = udp->dev;
	udp->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;

	ret = video_register_device(&udp->vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_err(udp->dev, "usbcam: video_register_device failed: %d\n", ret);
		v4l2_device_unregister(&udp->v4l2_dev);
		return ret;
	}

	ret = vb2_queue_init(&udp->vq);
	if (ret < 0) {
		dev_err(udp->dev, "usbcam: vb2_queue_init failed: %d\n", ret);
		video_unregister_device(&udp->vdev);
		return ret;
	}

	udp->ops = ops;
	udp->video_nr = video_nr;

	return 0;
}

/*
 * usbcam_unregister - tear down the V4L2 device and vb2 queue
 */
void usbcam_unregister(struct usbcam_dev *udp)
{
	vb2_queue_error(&udp->vq);
	video_unregister_device(&udp->vdev);
	v4l2_device_unregister(&udp->v4l2_dev);

	mutex_destroy(&udp->lock);
	init_waitqueue_head(&udp->wq);
}

MODULE_LICENSE("GPL");
MODULE_VERSION("0.11.3");
