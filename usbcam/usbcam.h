/*
 * USBCAM abstraction library for USB webcam drivers
 * V4L2-compatible rewrite
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

#ifndef __USBCAM_H__
#define __USBCAM_H__

#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * Debug subsystem bits
 */
enum {
	USBCAM_DBG_VIDEOBUF = 0,
	USBCAM_DBG_CAPTURE,
	USBCAM_DBG_IOCTL_BUF,
	USBCAM_DBG_IOCTL_FMT,
	USBCAM_DBG_IOCTL_MISC,
	USBCAM_DBG_DEV_STATE,
	USBCAM_DBG_ALTSETTING,
	USBCAM_DBG_URBSTREAM,

	/* First bit available to minidrivers */
	USBCAM_DBGBIT_MD_START = 8,
};

/*
 * Pixel format descriptor
 */
struct usbcam_pix_fmt_resolution {
	u32	width;
	u32	height;
	u32	interval;   /* numerator (frames per second * 1000000) */
	u32	reqbw;      /* required bandwidth in bytes/sec */
};

struct usbcam_pix_fmt {
	const char		*name;
	u32			fourcc;
	u32			bpp;
	u32			bytesperline;
	u32			sizeimage;
	struct usbcam_pix_fmt_resolution	resolutions[16];
	int			resolutions_len;
};

/*
 * Forward declaration
 */
struct usbcam_dev;

/*
 * V4L2 control descriptor
 */
struct usbcam_ctrl {
	struct v4l2_ctrl	base;

	/* Retrieves the current value of the control */
	int (*get_fn)(struct usbcam_dev *udp, const struct usbcam_ctrl *ctrl,
		      struct v4l2_ext_control *ext);

	/* Sets the value of the control; NULL means read-only */
	int (*set_fn)(struct usbcam_dev *udp, const struct usbcam_ctrl *ctrl,
		      const struct v4l2_ext_control *ext);

	/* Optional: dynamic query override */
	int (*query_fn)(struct usbcam_dev *udp, const struct usbcam_ctrl *ctrl,
		       struct v4l2_query_ext_ctrl *qc);

	/* Optional: release callback */
	void (*release_fn)(struct usbcam_dev *udp, struct usbcam_ctrl *ctrl);

	/* Optional: menu names for MENU/INTEGER_MENU controls */
	const char		**menu_names;
};

/*
 * Work item for deferred processing
 */
struct usbcam_workitem;
typedef void (*usbcam_workfunc_t)(struct usbcam_workitem *work);

struct usbcam_workitem {
	struct list_head	uw_links;
	struct usbcam_dev	*uw_dev;
	usbcam_workfunc_t	uw_func;
};

/*
 * URB streaming infrastructure
 */
struct usbcam_urbstream;

struct usbcam_urbstream_ops {
	void (*packet_done)(struct usbcam_dev *udp,
			    struct usbcam_urbstream *usp,
			    const u8 *pktdata, int pktlen, int pktstatus);
	void (*submit_error)(struct usbcam_dev *udp,
			     struct usbcam_urbstream *usp,
			     int status);
};

struct usbcam_urbstream {
	struct usbcam_dev		*us_dev;
	char				us_endpoint;
	char				us_streaming;
	char				us_active_goal;
	char				us_active_count;
	const struct usbcam_urbstream_ops	*us_ops;
	spinlock_t			us_lock;
	int				us_timeout_ticks;
	int				us_resubmit_err;
	struct list_head		us_unused_list;
	struct list_head		us_active_list;
	struct list_head		us_complete_list;
	struct completion		us_active_empty;
	struct usbcam_workitem		us_error_workitem;
};

struct usbcam_delayedwork {
	struct usbcam_workitem	dw_work;
	struct timer_list	dw_timer;
};

/*
 * Minidriver callback operations
 */
struct usbcam_dev_ops {
	/* REQUIRED: called on device probe */
	int (*init)(struct usbcam_dev *udp);

	/* REQUIRED: called on device disconnect */
	void (*exit)(struct usbcam_dev *udp);

	/* Optional: called on first open */
	int (*open)(struct usbcam_dev *udp);

	/* Optional: called on last close */
	void (*close)(struct usbcam_dev *udp);

	/* Optional: intercept V4L2 ioctls (return 0 if handled) */
	int (*ioctl)(struct usbcam_dev *udp, unsigned int cmd,
		     void *arg);

	/* REQUIRED: start capture */
	int (*cap_start)(struct usbcam_dev *udp);

	/* REQUIRED: stop capture */
	void (*cap_stop)(struct usbcam_dev *udp);

	/* Optional: decide how to handle a received packet */
	int (*decide_pkt)(struct usbcam_dev *udp, const u8 *pktdata,
			  int pktlen);

	/* Optional: set format (called after S_FMT) */
	int (*set_fmt)(struct usbcam_dev *udp, const struct usbcam_pix_fmt *fmt);

	/* Optional: generate test pattern */
	int (*testpattern)(struct usbcam_dev *udp);
};

/*
 * Main device structure (full definition in usbcam_priv.h)
 */
struct usbcam_dev;

/*
 * Helper macros (full definitions in usbcam_priv.h)
 */

#define usbcam_dbg(UDP, BIT, fmt, ...) \
	if ((UDP)->debug & (1 << (BIT))) \
		dev_info((UDP)->dev, fmt, ##__VA_ARGS__)

#define usbcam_err(UDP, fmt, ...) \
	dev_err((UDP)->dev, fmt, ##__VA_ARGS__)

#define usbcam_warn(UDP, fmt, ...) \
	dev_warn((UDP)->dev, fmt, ##__VA_ARGS__)

/*
 * Public API: Registration
 */
extern int usbcam_register(struct usbcam_dev *udp,
			     const struct usbcam_dev_ops *ops,
			     int video_nr);
extern void usbcam_unregister(struct usbcam_dev *udp);

/*
 * Public API: Frame completion
 */
extern void usbcam_curframe_complete(struct usbcam_dev *udp);
extern void usbcam_curframe_abortall(struct usbcam_dev *udp);
extern int usbcam_buf_queue(struct usbcam_dev *udp, struct vb2_buffer *vb);
extern void usbcam_buf_dequeue(struct usbcam_dev *udp);
extern void usbcam_curframe_fill(struct usbcam_dev *udp, size_t offset,
				  const void *pattern, int patlen, int nrecs);
extern int usbcam_curframe_testpattern(struct usbcam_dev *udp);

/*
 * Public API: URB streaming
 */
extern int usbcam_urbstream_init(struct usbcam_urbstream *usp,
				  struct usbcam_dev *udp, int nurb,
				  int pktsize, int pktcount, int interval,
				  int ep, int type);
extern int usbcam_urbstream_config_iso(struct usbcam_urbstream *usp,
					 int pktsize, int pktcount, int interval);
extern int usbcam_urbstream_config_bulk(struct usbcam_urbstream *usp,
					 int pktsize);
extern int usbcam_urbstream_start(struct usbcam_urbstream *usp);
extern void usbcam_urbstream_stop(struct usbcam_urbstream *usp);
extern void usbcam_urbstream_cleanup(struct usbcam_urbstream *usp);
extern int usbcam_urbstream_submit_one(struct usbcam_urbstream *usp);

/*
 * Public API: Work items
 */
extern void usbcam_workitem_init(struct usbcam_workitem *wip,
				  struct usbcam_dev *udp,
				  usbcam_workfunc_t func);
extern void usbcam_workitem_queue(struct usbcam_workitem *wip);
extern int usbcam_workitem_cancel(struct usbcam_workitem *wip);

/*
 * Public API: V4L2 standard ioctls (static in usbcam_fops.c)
 */

/*
 * Public API: V4L2 buffer ioctls (static in usbcam_fops.c)
 */

/*
 * Public API: V4L2 control ioctls (static in usbcam_fops.c)
 */

/*
 * Public API: V4L2 file operations (static in usbcam_fops.c)
 */

/*
 * Public API: Utility
 */
extern int usbcam_claim_interface(struct usbcam_dev *udp, int ifnum);
extern int usbcam_choose_altsetting(struct usbcam_dev *udp, int ifnum,
				    int pipe, int bytes_per_sec_min,
				    int pkt_min, int pkt_max,
				    int *altsetting_nr);
extern struct usbcam_ctrl *usbcam_ctrl_find(struct usbcam_dev *udp, u32 ctlid);
extern struct usbcam_ctrl *usbcam_ctrl_add_tmpl(struct usbcam_dev *udp,
						 const struct usbcam_ctrl *tplp,
						 size_t real_size);
extern struct usbcam_ctrl *usbcam_ctrl_alloc(size_t real_size);
#define usbcam_ctrl_free(CTRLP)	kfree(CTRLP)
extern int usbcam_ctrl_add(struct usbcam_dev *udp, struct usbcam_ctrl *ctrlp);

/*
 * Public API: URB allocation helpers
 */
extern struct urb *usbcam_urb_alloc(struct usbcam_dev *udp, int pipe,
				    int pktsize, int npkts, int alloc_buf);
extern void usbcam_urb_free(struct urb *urbp, int free_buf);

/*
 * Public API: Work queue management
 */
extern int usbcam_work_ref(struct usbcam_dev *udp);
extern void usbcam_work_unref(struct usbcam_dev *udp);
extern void usbcam_work_runqueue(struct usbcam_dev *udp);

/*
 * Public API: Delayed work
 */
extern void usbcam_delayedwork_init(struct usbcam_dev *udp,
				    struct usbcam_delayedwork *dwp,
				    usbcam_workfunc_t func);
extern void usbcam_delayedwork_queue(struct usbcam_delayedwork *dwp,
				     unsigned int timeout_ms);
extern int usbcam_delayedwork_cancel(struct usbcam_delayedwork *dwp);

#endif /* __USBCAM_H__ */
