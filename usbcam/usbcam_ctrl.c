/*
 * USBCAM control management and utility functions
 *
 * Implements the control allocation, registration, and frame abort
 * functions declared in usbcam.h but not provided by other library files.
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

#include "usbcam_priv.h"

/*
 * usbcam_claim_interface - Claim a USB interface for exclusive use
 * @udp: usbcam device
 * @ifnum: interface number to claim
 *
 * Returns 0 on success, negative errno on failure.
 */
int usbcam_claim_interface(struct usbcam_dev *udp, int ifnum)
{
	struct usb_driver *driver;
	struct usb_interface *intf;
	int i, ret;

	if (!udp->udev || !udp->udev->actconfig) {
		usbcam_err(udp, "no USB device/config available\n");
		return -ENODEV;
	}

	if (!udp->intf || !udp->intf->dev.driver) {
		usbcam_err(udp, "no driver bound to current interface\n");
		return -ENODEV;
	}

	driver = to_usb_driver(udp->intf->dev.driver);

	for (i = 0; i < USB_MAXINTERFACES; i++) {
		intf = udp->udev->actconfig->interface[i];
		if (!intf)
			break;
		if (intf && intf->cur_altsetting->desc.bInterfaceNumber == ifnum) {
			if (usb_interface_claimed(intf)) {
				usbcam_info(udp, "interface %d already claimed\n", ifnum);
				return 0;
			}
			ret = usb_driver_claim_interface(driver, intf, NULL);
			if (ret) {
				usbcam_err(udp, "failed to claim interface %d: %d\n", ifnum, ret);
				return ret;
			}
			usbcam_info(udp, "claimed interface %d\n", ifnum);
			return 0;
		}
	}

	usbcam_err(udp, "interface %d not found\n", ifnum);
	return -ENODEV;
}

/*
 * usbcam_ctrl_alloc - Allocate a zeroed control structure
 * @real_size: total size to allocate (may be larger than struct usbcam_ctrl
 *             for minidriver-specific extensions)
 *
 * Returns pointer to zeroed memory, or NULL on allocation failure.
 */
struct usbcam_ctrl *usbcam_ctrl_alloc(size_t real_size)
{
	return kzalloc(real_size, GFP_KERNEL);
}

/*
 * usbcam_ctrl_add - Register a control with the device
 * @udp: usbcam device
 * @ctrlp: pointer to the control to add (must be pre-allocated)
 *
 * Grows the internal control array as needed.
 * Returns 0 on success, negative errno on failure.
 */
int usbcam_ctrl_add(struct usbcam_dev *udp, struct usbcam_ctrl *ctrlp)
{
	struct usbcam_ctrl *new_ctrls;
	int new_len;

	new_len = udp->ctrls_len + 1;
	new_ctrls = krealloc(udp->ctrls, new_len * sizeof(*new_ctrls),
			      GFP_KERNEL);
	if (!new_ctrls)
		return -ENOMEM;

	new_ctrls[udp->ctrls_len] = *ctrlp;
	udp->ctrls = new_ctrls;
	udp->ctrls_len = new_len;

	usbcam_dbg(udp, USBCAM_DBG_IOCTL_MISC,
		   "ctrl_add: %s (id=0x%08x) -> total %d\n",
		   ctrlp->base.name, ctrlp->base.id, new_len);

	return 0;
}

/*
 * usbcam_ctrl_add_tmpl - Allocate a control from a template and register it
 * @udp: usbcam device
 * @tplp: template control to copy from
 * @real_size: total allocation size (for minidriver extensions)
 *
 * Allocates a new control, copies the template into it, and registers it.
 * Returns pointer to the new control, or NULL on failure.
 */
struct usbcam_ctrl *usbcam_ctrl_add_tmpl(struct usbcam_dev *udp,
					  const struct usbcam_ctrl *tplp,
					  size_t real_size)
{
	struct usbcam_ctrl *ncp;
	int ret;

	ncp = usbcam_ctrl_alloc(real_size);
	if (!ncp)
		return NULL;

	/* Copy the template (only the base usbcam_ctrl portion) */
	memcpy(ncp, tplp, sizeof(*ncp));

	ret = usbcam_ctrl_add(udp, ncp);
	if (ret) {
		usbcam_ctrl_free(ncp);
		return NULL;
	}

	return ncp;
}

/*
 * usbcam_curframe_abortall - Abort all pending frames and complete buffers
 * @udp: usbcam device
 *
 * Walks the frame list, completes any associated vb2 buffers with an error
 * state, and frees the frame descriptors. Also clears the current frame.
 */
void usbcam_curframe_abortall(struct usbcam_dev *udp)
{
	struct usbcam_frame *frame, *tmp;
	unsigned long flags;

	usbcam_info(udp, "aborting all pending frames\n");

	spin_lock_irqsave(&udp->frame_lock, flags);
	list_for_each_entry_safe(frame, tmp, &udp->frame_list, fl_links) {
		list_del(&frame->fl_links);
		spin_unlock_irqrestore(&udp->frame_lock, flags);

		/* Complete the associated vb2 buffer with error */
		if (frame->fl_vb)
			vb2_buffer_done(frame->fl_vb, VB2_BUF_STATE_ERROR);

		if (frame->fl_buf)
			kfree(frame->fl_buf);
		kfree(frame);

		spin_lock_irqsave(&udp->frame_lock, flags);
	}
	spin_unlock_irqrestore(&udp->frame_lock, flags);

	/* Clear current frame state */
	if (udp->curframe_buf) {
		kfree(udp->curframe_buf);
		udp->curframe_buf = NULL;
	}
	udp->curframe = 0;
	udp->frame_accum = 0;
}
