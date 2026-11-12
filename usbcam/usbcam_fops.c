/*
 * USBCAM V4L2 file operations and ioctl handler
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
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>

#include "usbcam.h"
#include "usbcam_priv.h"

/*
 * ============================================================================
 * Helper: extract usbcam_dev from the V4L2 ioctl priv pointer
 *
 * In the standard V4L2 pattern, the V4L2 core sets file->private_data
 * to a struct v4l2_fh.  The driver's private data is stored via
 * video_set_drvdata() and retrieved with video_get_drvdata().
 * The 'priv' argument passed to v4l2_ioctl_ops handlers is the
 * driver-specific data set by the V4L2 core (typically the
 * video_device or a driver-private pointer).
 *
 * For usbcam, we use video_get_drvdata() to retrieve the usbcam_dev.
 * ============================================================================
 */

static struct usbcam_dev *usbcam_get_dev(struct file *file, void *priv)
{
	struct video_device *vdev;
	struct usbcam_dev *udp;

	/*
	 * The V4L2 core passes 'priv' which is typically the
	 * video_device pointer (set via video_set_drvdata or
	 * the vdev->v4l2_dev pattern).  We use video_devdata()
	 * to get the video_device from the file, then
	 * video_get_drvdata() to get our private data.
	 */
	vdev = video_devdata(file);
	if (!vdev)
		return NULL;

	udp = video_get_drvdata(vdev);
	if (udp)
		return udp;

	/* Fallback: if vdev is embedded in usbcam_dev */
	return usbcam_to_dev(vdev);
}

/*
 * ============================================================================
 * V4L2 ioctl handler implementations
 * ============================================================================
 */

/*
 * VIDIOC_QUERYCAP - Return driver capabilities
 */
static int usbcam_v4l2_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	memset(cap, 0, sizeof(*cap));

	strscpy(cap->driver, "usbcam", sizeof(cap->driver));
	strscpy(cap->card, dev_name(&udp->udev->dev), sizeof(cap->card));
	strscpy(cap->bus_info, "USB", sizeof(cap->bus_info));

	cap->version = 0x0102; /* V4L2 spec 1.2 */
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE |
			     V4L2_CAP_STREAMING |
			     V4L2_CAP_READWRITE;

	return 0;
}

/*
 * VIDIOC_ENUM_FMT - Enumerate supported pixel formats
 */
static int usbcam_v4l2_enum_fmt_vid_cap(struct file *file, void *priv,
					 struct v4l2_fmtdesc *f)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (f->index >= (unsigned int)udp->fmts_len)
		return -EINVAL;

	if (f->pixelformat != 0 &&
	    f->pixelformat != udp->fmts[f->index].fourcc)
		return -EINVAL;

	f->pixelformat = udp->fmts[f->index].fourcc;
	strscpy(f->description, udp->fmts[f->index].name,
		 sizeof(f->description));
	f->flags = 0;

	return 0;
}

/*
 * VIDIOC_G_FMT - Get current format
 */
static int usbcam_v4l2_g_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (!udp->curfmt)
		return -EINVAL;

	f->fmt.pix.width = udp->curfmt->resolutions[0].width;
	f->fmt.pix.height = udp->curfmt->resolutions[0].height;
	f->fmt.pix.pixelformat = udp->curfmt->fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = udp->curfmt->bytesperline;
	f->fmt.pix.sizeimage = udp->curfmt->sizeimage;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	f->fmt.pix.priv = 0;

	return 0;
}

/*
 * VIDIOC_S_FMT - Set current format
 */
static int usbcam_v4l2_s_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int i, j;
	int found;

	if (!udp)
		return -ENODEV;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (udp->streaming)
		return -EBUSY;

	/* Find matching pixel format */
	for (i = 0; i < udp->fmts_len; i++) {
		if (udp->fmts[i].fourcc == f->fmt.pix.pixelformat)
			break;
	}
	if (i >= udp->fmts_len)
		return -EINVAL;

	/* Find matching resolution (or default to first) */
	found = 0;
	for (j = 0; j < udp->fmts[i].resolutions_len; j++) {
		if (udp->fmts[i].resolutions[j].width == f->fmt.pix.width &&
		    udp->fmts[i].resolutions[j].height == f->fmt.pix.height) {
			found = 1;
			break;
		}
	}
	if (!found && udp->fmts[i].resolutions_len > 0) {
		/* Clamp to first available resolution */
		f->fmt.pix.width = udp->fmts[i].resolutions[0].width;
		f->fmt.pix.height = udp->fmts[i].resolutions[0].height;
	}

	udp->curfmt = (struct usbcam_pix_fmt *)&udp->fmts[i];

	/* Notify minidriver */
	if (udp->ops->set_fmt)
		udp->ops->set_fmt(udp, udp->curfmt);

	/* Update format fields to reflect what was actually set */
	f->fmt.pix.width = udp->curfmt->resolutions[0].width;
	f->fmt.pix.height = udp->curfmt->resolutions[0].height;
	f->fmt.pix.pixelformat = udp->curfmt->fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = udp->curfmt->bytesperline;
	f->fmt.pix.sizeimage = udp->curfmt->sizeimage;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

	return 0;
}

/*
 * VIDIOC_ENUM_FRAMESIZES - Enumerate discrete frame sizes for current format
 */
static int usbcam_v4l2_enum_framesizes(struct file *file, void *priv,
					 struct v4l2_frmsizeenum *fse)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (!udp->curfmt)
		return -EINVAL;

	if (fse->index >= (unsigned int)udp->curfmt->resolutions_len)
		return -EINVAL;

	fse->discrete.width = udp->curfmt->resolutions[fse->index].width;
	fse->discrete.height = udp->curfmt->resolutions[fse->index].height;

	return 0;
}

/*
 * VIDIOC_ENUM_FRAMEINTERVALS - Enumerate frame intervals for current format
 */
static int usbcam_v4l2_enum_frameintervals(struct file *file, void *priv,
					     struct v4l2_frmivalenum *fiv)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (!udp->curfmt)
		return -EINVAL;

	if (fiv->index >= (unsigned int)udp->curfmt->resolutions_len)
		return -EINVAL;

	/*
	 * interval is stored as fps * 1000000, so:
	 *   timeperframe = 1 / (interval / 1000000)
	 *   = 1000000 / interval
	 */
	{
		u32 interval = udp->curfmt->resolutions[fiv->index].interval;

		if (interval > 0) {
			fiv->discrete.numerator = 1000000;
			fiv->discrete.denominator = interval;
		} else {
			fiv->discrete.numerator = 1;
			fiv->discrete.denominator = 30;
		}
	}

	return 0;
}

/*
 * VIDIOC_REQBUFS - Request buffer allocation
 */
static int usbcam_v4l2_reqbufs(struct file *file, void *priv,
			       struct v4l2_requestbuffers *rb)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (rb->count < 1 || rb->count > 32)
		return -EINVAL;

	if (rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (rb->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	return vb2_reqbufs(&udp->vq, rb);
}

/*
 * VIDIOC_QUERYBUF - Query buffer information
 */
static int usbcam_v4l2_querybuf(struct file *file, void *priv,
				 struct v4l2_buffer *buf)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (buf->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	return vb2_querybuf(&udp->vq, buf);
}

/*
 * VIDIOC_QBUF - Queue a buffer for capture
 */
static int usbcam_v4l2_qbuf(struct file *file, void *priv,
			    struct v4l2_buffer *buf)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	struct media_device *mdev;

	if (!udp)
		return -ENODEV;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (buf->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	mdev = udp->vdev.v4l2_dev ? udp->vdev.v4l2_dev->mdev : NULL;
	return vb2_qbuf(&udp->vq, mdev, buf);
}

/*
 * VIDIOC_DQBUF - Dequeue a filled buffer
 */
static int usbcam_v4l2_dqbuf(struct file *file, void *priv,
			     struct v4l2_buffer *buf)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (buf->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	return vb2_dqbuf(&udp->vq, buf, 1);
}

/*
 * VIDIOC_STREAMON - Start streaming
 */
static int usbcam_v4l2_streamon(struct file *file, void *priv,
				 unsigned int type)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int ret;

	if (!udp)
		return -ENODEV;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (udp->streaming)
		return 0;

	if (!udp->curfmt)
		return -EINVAL;

	ret = vb2_streamon(&udp->vq, type);
	if (ret)
		return ret;

	/* cap_start is already called via vb2_streamon -> start_streaming */

	udp->streaming = 1;
	wake_up_all(&udp->wq);

	return 0;
}

/*
 * VIDIOC_STREAMOFF - Stop streaming
 */
static int usbcam_v4l2_streamoff(struct file *file, void *priv,
				 unsigned int type)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (!udp->streaming)
		return 0;

	/* cap_stop is already called via vb2_streamoff -> stop_streaming */
	vb2_streamoff(&udp->vq, type);

	udp->streaming = 0;
	wake_up_all(&udp->wq);

	return 0;
}

/*
 * VIDIOC_QUERYCTRL - Query a specific control
 */
static int usbcam_v4l2_query_ext_ctrl(struct file *file, void *priv,
				      struct v4l2_query_ext_ctrl *qc)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int i;

	if (!udp)
		return -ENODEV;

	for (i = 0; i < udp->ctrls_len; i++) {
		if (udp->ctrls[i].base.id == qc->id) {
			qc->id = udp->ctrls[i].base.id;
			qc->type = udp->ctrls[i].base.type;
			strscpy(qc->name, udp->ctrls[i].base.name, sizeof(qc->name));
			qc->minimum = udp->ctrls[i].base.minimum;
			qc->maximum = udp->ctrls[i].base.maximum;
			qc->step = udp->ctrls[i].base.step;
			qc->default_value = udp->ctrls[i].base.default_value;
			qc->flags = udp->ctrls[i].base.flags;
			qc->elem_size = udp->ctrls[i].base.elem_size;
			qc->elems = udp->ctrls[i].base.elems;
			qc->nr_of_dims = udp->ctrls[i].base.nr_of_dims;
			memcpy(qc->dims, udp->ctrls[i].base.dims, sizeof(qc->dims));

			if (udp->ctrls[i].query_fn)
				udp->ctrls[i].query_fn(udp, &udp->ctrls[i], qc);

			return 0;
		}
	}

	return -EINVAL;
}

/*
 * VIDIOC_G_EXT_CTRLS - Get control value(s)
 */
static int usbcam_v4l2_g_ext_ctrls(struct file *file, void *priv,
				   struct v4l2_ext_controls *ext)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int i;

	if (!udp)
		return -ENODEV;

	for (i = 0; i < ext->count; i++) {
		struct v4l2_ext_control *ctrl = &ext->controls[i];
		int j;

		for (j = 0; j < udp->ctrls_len; j++) {
			if (udp->ctrls[j].base.id == ctrl->id) {
				if (!udp->ctrls[j].get_fn)
					return -EINVAL;

				return udp->ctrls[j].get_fn(udp, &udp->ctrls[j], ctrl);
			}
		}
		return -EINVAL;
	}

	return 0;
}

/*
 * VIDIOC_S_EXT_CTRLS - Set control value(s)
 */
static int usbcam_v4l2_s_ext_ctrls(struct file *file, void *priv,
				   struct v4l2_ext_controls *ext)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int i;

	if (!udp)
		return -ENODEV;

	for (i = 0; i < ext->count; i++) {
		struct v4l2_ext_control *ctrl = &ext->controls[i];
		int j;

		for (j = 0; j < udp->ctrls_len; j++) {
			if (udp->ctrls[j].base.id == ctrl->id) {
				if (!udp->ctrls[j].set_fn)
					return -EINVAL;

				return udp->ctrls[j].set_fn(udp, &udp->ctrls[j], ctrl);
			}
		}
		return -EINVAL;
	}

	return 0;
}

/*
 * VIDIOC_QUERYMENU - Query menu item for MENU/INTEGER_MENU controls
 */
static int usbcam_v4l2_querymenu(struct file *file, void *priv,
				 struct v4l2_querymenu *qm)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);
	int i;

	if (!udp)
		return -ENODEV;

	for (i = 0; i < udp->ctrls_len; i++) {
		if (udp->ctrls[i].base.id == qm->id) {
			if (udp->ctrls[i].base.type != V4L2_CTRL_TYPE_MENU &&
			    udp->ctrls[i].base.type != V4L2_CTRL_TYPE_INTEGER_MENU)
				return -EINVAL;

			if (!udp->ctrls[i].menu_names)
				return -EINVAL;

			qm->value = qm->index;
			strscpy(qm->name, udp->ctrls[i].menu_names[qm->index],
				 sizeof(qm->name));
			return 0;
		}
	}

	return -EINVAL;
}

/*
 * VIDIOC_G_PARM - Get stream parameters (frame rate)
 */
static int usbcam_v4l2_g_parm(struct file *file, void *priv,
			      struct v4l2_streamparm *p)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&p->parm.capture, 0, sizeof(p->parm.capture));

	if (udp->curfmt && udp->curfmt->resolutions_len > 0) {
		/* interval is fps * 1000000, so fps = interval / 1000000 */
		u32 interval = udp->curfmt->resolutions[0].interval;
		if (interval > 0) {
			p->parm.capture.timeperframe.numerator = 1000000;
			p->parm.capture.timeperframe.denominator = interval;
		} else {
			p->parm.capture.timeperframe.numerator = 1;
			p->parm.capture.timeperframe.denominator = 30;
		}
	} else {
		p->parm.capture.timeperframe.numerator = 1;
		p->parm.capture.timeperframe.denominator = 30;
	}

	p->parm.capture.capturemode = 0;

	return 0;
}

/*
 * VIDIOC_S_PARM - Set stream parameters (frame rate)
 */
static int usbcam_v4l2_s_parm(struct file *file, void *priv,
			      struct v4l2_streamparm *p)
{
	struct usbcam_dev *udp = usbcam_get_dev(file, priv);

	if (!udp)
		return -ENODEV;

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/*
	 * Basic frame rate handling: validate the requested rate.
	 * The actual rate is determined by the USB endpoint configuration,
	 * so we only validate and store it.
	 */
	if (p->parm.capture.timeperframe.denominator == 0)
		return -EINVAL;

	/*
	 * If streaming, reject changes. Otherwise, the rate will be
	 * applied on next STREAMON via the minidriver's cap_start.
	 */
	if (udp->streaming)
		return -EBUSY;

	return 0;
}

/*
 * ============================================================================
 * v4l2_ioctl_ops struct - standard V4L2 ioctl dispatch table
 * ============================================================================
 */

const struct v4l2_ioctl_ops usbcam_v4l2_ioctl_ops = {
	.vidioc_querycap           = usbcam_v4l2_querycap,
	.vidioc_enum_fmt_vid_cap   = usbcam_v4l2_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap      = usbcam_v4l2_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap      = usbcam_v4l2_s_fmt_vid_cap,
	.vidioc_enum_framesizes    = usbcam_v4l2_enum_framesizes,
	.vidioc_enum_frameintervals = usbcam_v4l2_enum_frameintervals,
	.vidioc_reqbufs            = usbcam_v4l2_reqbufs,
	.vidioc_querybuf           = usbcam_v4l2_querybuf,
	.vidioc_qbuf               = usbcam_v4l2_qbuf,
	.vidioc_dqbuf              = usbcam_v4l2_dqbuf,
	.vidioc_streamon           = usbcam_v4l2_streamon,
	.vidioc_streamoff          = usbcam_v4l2_streamoff,
	.vidioc_query_ext_ctrl     = usbcam_v4l2_query_ext_ctrl,
	.vidioc_g_ext_ctrls        = usbcam_v4l2_g_ext_ctrls,
	.vidioc_s_ext_ctrls        = usbcam_v4l2_s_ext_ctrls,
	.vidioc_querymenu          = usbcam_v4l2_querymenu,
	.vidioc_g_parm             = usbcam_v4l2_g_parm,
	.vidioc_s_parm             = usbcam_v4l2_s_parm,
};

/*
 * ============================================================================
 * V4L2 file operations (struct v4l2_file_operations)
 * ============================================================================
 */

/*
 * .open - Called when the video device is opened
 */
static int usbcam_v4l2_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct usbcam_dev *udp;
	int ret = 0;

	if (!vdev)
		return -ENODEV;

	udp = video_get_drvdata(vdev);
	if (!udp)
		udp = usbcam_to_dev(vdev);

	mutex_lock(&udp->lock);
	udp->opened++;
	mutex_unlock(&udp->lock);

	if (udp->opened == 1 && udp->ops->open) {
		ret = udp->ops->open(udp);
		if (ret) {
			mutex_lock(&udp->lock);
			udp->opened--;
			mutex_unlock(&udp->lock);
			return ret;
		}
	}

	usbcam_dbg(udp, USBCAM_DBG_DEV_STATE, "open: opened=%d\n",
		   udp->opened);

	return v4l2_fh_open(file);
}

/*
 * .release - Called when the video device is closed
 */
static int usbcam_v4l2_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct usbcam_dev *udp;

	if (!vdev)
		return -ENODEV;

	udp = video_get_drvdata(vdev);
	if (!udp)
		udp = usbcam_to_dev(vdev);

	mutex_lock(&udp->lock);
	udp->opened--;
	mutex_unlock(&udp->lock);

	usbcam_dbg(udp, USBCAM_DBG_DEV_STATE, "release: opened=%d\n",
		   udp->opened);

	if (udp->opened == 0) {
		/* Stop streaming if active */
		if (udp->streaming) {
			if (udp->ops->cap_stop)
				udp->ops->cap_stop(udp);
			vb2_streamoff(&udp->vq, V4L2_BUF_TYPE_VIDEO_CAPTURE);
			udp->streaming = 0;
			wake_up_all(&udp->wq);
		}

		if (udp->ops->close)
			udp->ops->close(udp);
	}

	v4l2_fh_release(file);

	return 0;
}

/*
 * .mmap - Map a capture buffer into userspace
 */
static int usbcam_v4l2_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct video_device *vdev = video_devdata(file);
	struct usbcam_dev *udp;

	if (!vdev)
		return -ENODEV;

	udp = video_get_drvdata(vdev);
	if (!udp)
		udp = usbcam_to_dev(vdev);

	return vb2_mmap(&udp->vq, vma);
}

/*
 * .poll - Wait for a frame to be available
 */
static __poll_t usbcam_v4l2_poll(struct file *file,
				 struct poll_table_struct *wait)
{
	struct video_device *vdev = video_devdata(file);
	struct usbcam_dev *udp;
	__poll_t fl = 0;

	if (!vdev)
		return fl;

	udp = video_get_drvdata(vdev);
	if (!udp)
		udp = usbcam_to_dev(vdev);

	poll_wait(file, &udp->wq, wait);

	if (udp->vq.streaming && !list_empty(&udp->vq.done_list))
		fl |= EPOLLIN | EPOLLRDNORM;

	return fl;
}

/*
 * ============================================================================
 * v4l2_file_operations struct
 * ============================================================================
 */

const struct v4l2_file_operations usbcam_v4l2_fops_template = {
	.owner          = THIS_MODULE,
	.open           = usbcam_v4l2_open,
	.release        = usbcam_v4l2_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = usbcam_v4l2_mmap,
	.poll           = usbcam_v4l2_poll,
};

/*
 * ============================================================================
 * Video device template
 * ============================================================================
 */

const struct video_device usbcam_videodev_template = {
	.name           = "usbcam",
	.fops           = &usbcam_v4l2_fops_template,
	.ioctl_ops      = &usbcam_v4l2_ioctl_ops,
};
