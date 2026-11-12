/*
 * USBCAM abstraction library for USB webcam drivers
 *
 * Copyright (c) 2007 Sam Revitch <samr7 cs washington edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "usbcam_priv.h"
#include <linux/version.h>

/*
 * Internal macros not yet defined in headers
 */
#ifndef USBCAM_EXPORT_SYMBOL
#define USBCAM_EXPORT_SYMBOL(sym) EXPORT_SYMBOL(sym)
#endif

#ifndef ALTSETTING
#define ALTSETTING USBCAM_DBG_ALTSETTING
#endif

/*
 * usbcam_chklock: verify the device mutex is held by the caller.
 * In production this could be a lockdep assertion; here it is a no-op
 * to preserve API compatibility with minidrivers that call it.
 */
#ifndef usbcam_chklock
#define usbcam_chklock(udp) do { } while (0)
#endif

/*
 * This file contains utility routines, including:
 * - usbcam_choose_altsetting() for selecting int/isoc altsettings
 * - The usbcam_urbstream accessory module
 * - usbcam_hexdump() buffer dumping utility function
 *
 * TODO:
 * - Find a way to alert minidrivers of URB shortages
 */

/*
 * Traverse the alternate setting list and find one that provides
 * the least bandwidth that satisfies the minimum requirement.
 */
int usbcam_choose_altsetting(struct usbcam_dev *udp, int ifnum,
			     int pipe, int bytes_per_sec_min,
			     int pkt_min, int pkt_max,
			     int *altsetting_nr)
{
	struct usb_interface *intf;
	const struct usb_host_interface *aintf;
	const struct usb_endpoint_descriptor *epd = NULL;
	int i, j;

	int wmp, bw;
	int best_alt = -1, best_alt_bw = 0;

	usbcam_chklock(udp);

	if (!udp->udev) {
		usbcam_warn(udp, "%s: device is disconnected", __FUNCTION__);
		return -ENODEV;
	}

	if (ifnum < 0)
		ifnum = udp->intf->cur_altsetting->desc.bInterfaceNumber;

	intf = usb_ifnum_to_if(udp->udev, ifnum);
	if (!intf) {
		usbcam_warn(udp, "%s: interface %d does not exist",
			    __FUNCTION__, ifnum);
		return -ENODEV;
	}

	if ((bytes_per_sec_min >= 0) &&
	    !usb_pipeisoc(pipe) && !usb_pipeint(pipe)) {
		usbcam_warn(udp, "%s: minidriver specified bytes_per_sec_min "
			    "on non-iso non-int pipe", __FUNCTION__);
	}

	for (i = 0; i < intf->num_altsetting; i++) {

		aintf = &intf->altsetting[i];
		for (j = 0; j < aintf->desc.bNumEndpoints; j++) {
			epd = &aintf->endpoint[j].desc;
			if ((epd->bEndpointAddress &
			     USB_ENDPOINT_NUMBER_MASK) ==
			    usb_pipeendpoint(pipe))
				break;
		}

		if (j == aintf->desc.bNumEndpoints) {
			/* Desired endpoint not present in this descriptor */
			usbcam_dbg(udp, ALTSETTING,
				   "altsetting %d has no EP%d",
				   i, usb_pipeendpoint(pipe));
			continue;
		}

		if (((usb_pipetype(pipe) == PIPE_ISOCHRONOUS) &&
		     !usb_endpoint_xfer_isoc(epd)) ||
		    ((usb_pipetype(pipe) == PIPE_INTERRUPT) &&
		     !usb_endpoint_xfer_int(epd)) ||
		    (usb_pipein(pipe) && !usb_endpoint_dir_in(epd)) ||
		    (!usb_pipein(pipe) && usb_endpoint_dir_in(epd))) {
			/* Something is horribly wrong */
			usbcam_dbg(udp, ALTSETTING,
				   "altsetting %d has unexpected EP%d",
				   i, usb_pipeendpoint(pipe));
			continue;
		}

		bw = 0;
		wmp = le16_to_cpu(epd->wMaxPacketSize);

		/* Bandwidth only applies to iso & int pipes */
		if (usb_pipeisoc(pipe) || usb_pipeint(pipe)) {
			if (udp->udev->speed == USB_SPEED_HIGH) {
				/* 8 uframes per regular frame */
				bw = 8000;

				/* high bandwidth endpoint? */
				wmp = ((wmp & 0x7ff) *
				       (((wmp >> 11) & 0x3) + 1));
			} else {
				bw = 1000;
				wmp &= 0x7ff;
			}

			bw *= wmp;

			/* Divide by interval / frame skippage */
			bw = bw / (1 << (epd->bInterval - 1));

			usbcam_dbg(udp, ALTSETTING,
				   "altsetting %d provides %d B/s bandwidth",
				   i, bw);

			/* Check the bandwidth */
			if (bw < bytes_per_sec_min)
				continue;

		} else
			wmp &= 0x7ff;

		/* Check the packet size */
		if (((pkt_min >= 0) && (wmp < pkt_min)) ||
		    ((pkt_max >= 0) && (wmp > pkt_max)))
			continue;

		if ((best_alt < 0) || (bw < best_alt_bw)) {
			best_alt = i;
			best_alt_bw = bw;
		}
	}

	if (best_alt == -1)
		return -ENODEV;

	*altsetting_nr = best_alt;
	return 0;
}
USBCAM_EXPORT_SYMBOL(usbcam_choose_altsetting);


/*
 * DMA buffer helper routines
 */
static int usbcam_urb_allocbuf(struct urb *urbp, size_t nbytes)
{
/* 3pei */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35)		
	urbp->transfer_buffer = usb_alloc_coherent(urbp->dev,
						 nbytes,
						 GFP_KERNEL,
						 &urbp->transfer_dma);
#else
	urbp->transfer_buffer = usb_buffer_alloc(urbp->dev,
						 nbytes,
						 GFP_KERNEL,
						 &urbp->transfer_dma);
#endif
	if (!urbp->transfer_buffer)
		return -ENOMEM;

	urbp->transfer_buffer_length = nbytes;
	urbp->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	return 0;
}

static inline void usbcam_urb_freebuf(struct urb *urbp)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35)		
	usb_free_coherent(urbp->dev,
			urbp->transfer_buffer_length,
			urbp->transfer_buffer,
			urbp->transfer_dma);
#else
	usb_buffer_free(urbp->dev,
			urbp->transfer_buffer_length,
			urbp->transfer_buffer,
			urbp->transfer_dma);
#endif
}

struct urb *usbcam_urb_alloc(struct usbcam_dev *udp, int pipe,
			     int pktsize, int npkts, int alloc_buf)
{
	struct urb *urbp;
	int nbytes, i;

	if (npkts) {
		if (!usb_pipeisoc(pipe)) {
			usbcam_err(udp, "npkts=%d but !pipeisoc", npkts);
			return NULL;
		}
	} else if (usb_pipeisoc(pipe)) {
		usbcam_err(udp, "npkts=0 but pipeisoc");
		return NULL;
	}


	urbp = usb_alloc_urb(npkts, GFP_KERNEL);
	if (!urbp)
		return NULL;

	urbp->dev = udp->udev;
	urbp->pipe = pipe;
	urbp->number_of_packets = npkts;
	urbp->transfer_buffer = NULL;
	urbp->transfer_buffer_length = 0;

	if (alloc_buf) {
		nbytes = pktsize;
		if (npkts)
			nbytes *= npkts;

		if (usbcam_urb_allocbuf(urbp, nbytes)) {
			usb_free_urb(urbp);
			return NULL;
		}
	}

	if (npkts)
		urbp->transfer_flags |= URB_ISO_ASAP;

	for (i = 0; i < npkts; i++) {
		urbp->iso_frame_desc[i].offset = (i * pktsize);
		urbp->iso_frame_desc[i].length = pktsize;
		urbp->iso_frame_desc[i].actual_length = 0;
		urbp->iso_frame_desc[i].status = 0;
	}

	return urbp;
}
USBCAM_EXPORT_SYMBOL(usbcam_urb_alloc);

void usbcam_urb_free(struct urb *urbp, int free_buf)
{
	if (free_buf)
		usbcam_urb_freebuf(urbp);
	usb_free_urb(urbp);
}
USBCAM_EXPORT_SYMBOL(usbcam_urb_free);


/*
 * Streaming request helper functions
 * This depends on the other usbcam stuff, but they don't depend on it,
 * and it should be considered an extension sub-library.
 */

/* URB streaming functions are now in usbcam_buf.c */



#ifdef usbcam_hexdump
#undef usbcam_hexdump
#endif
#define dumpable_char(X) (((X) >= ' ') && ((X) <= '~'))
void usbcam_hexdump(struct usbcam_dev *udp, const u8 *buf, size_t len)
{
	const int bpl = 16;
	const int cend_max = ((bpl * 4) + 1);
	static const char n2x[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
				      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
	char x, outbuf[cend_max + 1];
	unsigned int cstart, cend, xpos, cpos, offset;

	offset = 0;
	cstart = (3 * bpl) + 1;
	cend = cstart + bpl;
	outbuf[cend] = '\0';
	goto beginning;

	while (len) {
		x = *buf++;
		outbuf[xpos++] = n2x[(x >> 4) & 0xf];
		outbuf[xpos++] = n2x[x & 0xf];
		outbuf[cpos+cstart] = dumpable_char(x) ? x : '.';
		cpos++;
		xpos++;
		len--;

		if (!len || (cpos == bpl)) {
			printk(KERN_DEBUG "%s: %08x %s\n",
			       dev_name(udp->dev), offset, outbuf);
			offset += bpl;

		beginning:
			memset(outbuf, ' ', cend);
			cpos = 0;
			xpos = 0;
		}
	}
}
USBCAM_EXPORT_SYMBOL(usbcam_hexdump);
