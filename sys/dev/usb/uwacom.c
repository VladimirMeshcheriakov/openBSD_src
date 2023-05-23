/*	$OpenBSD: uwacom.c,v 1.7 2022/10/08 06:53:06 mglocker Exp $	*/

/*
 * Copyright (c) 2016 Frank Groeneveld <frank@frankgroeneveld.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Driver for USB Wacom tablets */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>

#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdevs.h>
#include <dev/usb/uhidev.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#include <dev/hid/hidmsvar.h>

#define	UWACOM_USE_PRESSURE	0x0001 /* button 0 is flaky, use tip pressure */
#define	UWACOM_BIG_ENDIAN	0x0002 /* XY reporting byte order */


#ifdef UWACOM_DEBUG
#define UWACOM_PACKET_PRINTF(data, len) do { \
	printf("Ox");  \
	for (int i = 0; i < (len); i++) \
		printf("%02x ",*((data)+i)); \
	printf("\n");	\
} while(0)
#define UWACOM_BUTTON_EVENT(buttons) do { \
	printf("Current button event: 0x%x\n",buttons); \
} while (0)
#endif

#define UWACOM_USE_PRESSURE 0x0001 /* button 0 is flaky, use tip pressure */
#define UWACOM_BIG_ENDIAN 0x0002   /* xy reporting byte order */

struct uwacom_softc {
	struct uhidev		sc_hdev;
	struct hidms		sc_ms;
	struct hid_location	sc_loc_tip_press;
	int			sc_flags;
	int sc_x;
	int sc_y;
	int sc_z;
	int sc_w;
	int sc_moved;
};

struct cfdriver uwacom_cd = {
	NULL, "uwacom", DV_DULL
};


const struct usb_devno uwacom_devs[] = {
	{ USB_VENDOR_WACOM, USB_PRODUCT_WACOM_INTUOS_DRAW },
	{ USB_VENDOR_WACOM, USB_PRODUCT_WACOM_ONE_S },
	{ USB_VENDOR_WACOM, USB_PRODUCT_WACOM_ONE_M },
	{ USB_VENDOR_WACOM, USB_PRODUCT_WACOM_INTUOS_S }
};

int	uwacom_match(struct device *, void *, void *);
void	uwacom_attach(struct device *, struct device *, void *);
int	uwacom_detach(struct device *, int);
void	uwacom_intr(struct uhidev *, void *, u_int);
int	uwacom_enable(void *);
void	uwacom_disable(void *);
int	uwacom_ioctl(void *, u_long, caddr_t, int, struct proc *);

const struct cfattach uwacom_ca = {
	sizeof(struct uwacom_softc), uwacom_match, uwacom_attach, uwacom_detach
};

const struct wsmouse_accessops uwacom_accessops = {
	uwacom_enable,
	uwacom_ioctl,
	uwacom_disable,
};

int
uwacom_match(struct device *parent, void *match, void *aux)
{
	struct uhidev_attach_arg *uha = aux;
	int size;
	void *desc;
	#ifdef	UWACOM_DEBUG
	printf("Wacom Vendor: 0x%x, Product: 0x%x\n",uha->uaa->vendor, uha->uaa->product);
	#endif
	if (UHIDEV_CLAIM_MULTIPLE_REPORTID(uha))
		return (UMATCH_NONE);

	if (usb_lookup(uwacom_devs, uha->uaa->vendor,
	    uha->uaa->product) == NULL)
		return (UMATCH_NONE);

	uhidev_get_report_desc(uha->parent, &desc, &size);

	if (hid_is_collection(desc, size, uha->reportid, 
		HID_USAGE2(HUP_WACOM | HUP_DIGITIZERS, HUG_POINTER)))
		return UMATCH_IFACECLASS;
	if (!hid_locate(desc, size, HID_USAGE2(HUP_WACOM, HUG_POINTER),
	    uha->reportid, hid_input, NULL, NULL))
		return (UMATCH_NONE);

	return (UMATCH_IFACECLASS);
}

void
uwacom_attach(struct device *parent, struct device *self, void *aux)
{
	struct uwacom_softc *sc = (struct uwacom_softc *)self;
	struct hidms *ms = &sc->sc_ms;
	struct uhidev_attach_arg *uha = (struct uhidev_attach_arg *)aux;
	struct usb_attach_arg *uaa = uha->uaa;
	int size, repid;
	void *desc;

	sc->sc_hdev.sc_intr = uwacom_intr;
	sc->sc_hdev.sc_parent = uha->parent;
	sc->sc_hdev.sc_udev = uaa->device;
	sc->sc_hdev.sc_report_id = uha->reportid;

	usbd_status usbd_req_stat = usbd_set_idle(uha->parent->sc_udev, uha->parent->sc_ifaceno, 0, 0);
	if (USBD_NORMAL_COMPLETION != usbd_req_stat)
		printf("0x%x\n", usbd_req_stat);

	uhidev_get_report_desc(uha->parent, &desc, &size);
	repid = uha->reportid;
	sc->sc_hdev.sc_isize = hid_report_size(desc, size, hid_input, repid);
	#ifdef UWACOM_DEBUG
	printf("Wacom packet max size: %d\n",sc->sc_hdev.sc_isize);
	#endif
	sc->sc_hdev.sc_osize = hid_report_size(desc, size, hid_output, repid);
	sc->sc_hdev.sc_fsize
		= hid_report_size(desc, size, hid_feature, repid);
	/* If a more modern tablet */
	if (uha->uaa->product == USB_PRODUCT_WACOM_ONE_S
		|| uha->uaa->product == USB_PRODUCT_WACOM_INTUOS_S) {
		static uByte report_buf[2] = { 0x02, 0x02 };
		uhidev_set_report(
			uha->parent, UHID_FEATURE_REPORT, sc->sc_hdev.sc_report_id, &report_buf, sizeof(report_buf));
		hidms_setup((struct device *)sc, ms, HIDMS_WACOM_SETUP, repid, desc, size);
	}

	if (uha->uaa->product == USB_PRODUCT_WACOM_INTUOS_DRAW) {
		sc->sc_flags = UWACOM_USE_PRESSURE | UWACOM_BIG_ENDIAN;
		sc->sc_loc_tip_press.pos = 43;
		sc->sc_loc_tip_press.size = 8;
		ms->sc_tsscale.maxx = 7600;
		ms->sc_tsscale.maxy = 4750;
	}

	hidms_attach(ms, &uwacom_accessops);
}

int
uwacom_detach(struct device *self, int flags)
{
	struct uwacom_softc *sc = (struct uwacom_softc *)self;
	struct hidms *ms = &sc->sc_ms;

	return hidms_detach(ms, flags);
}

void
uwacom_intr(struct uhidev *addr, void *buf, u_int len)
{
	struct uwacom_softc *sc = (struct uwacom_softc *)addr;
	struct hidms *ms = &sc->sc_ms;
	u_int32_t pad_buttons = 0;
	u_int32_t stylus_buttons = 0;
	uint8_t *data = (uint8_t *)buf;
	int x, y, pressure,distance;

	#ifdef UWACOM_DEBUG
		UWACOM_PACKET_PRINTF(data, len);
	#endif
	if (ms->sc_enabled == 0)
		return;

	x = hid_get_data(data, len, &ms->sc_loc_x);
	y = hid_get_data(data, len, &ms->sc_loc_y);
	pressure = hid_get_data(data, len, &ms->sc_loc_z);
	distance = hid_get_data(data, len, &ms->sc_loc_w);

	if (!sc->sc_moved)
	{
		sc->sc_x = x;
		sc->sc_y = y;
		sc->sc_z = pressure;
		sc->sc_w = distance;
		sc->sc_moved = 1;
	}

	int dx = sc->sc_x - x;
	int dy = sc->sc_y - y;
	int dz = sc->sc_z/32 - pressure/32; // Clamp the sensetivity to be in the range of -127 to 127
	int dw = sc->sc_w - distance;

	sc->sc_x = x;
	sc->sc_y = y;
	sc->sc_z = pressure;
	sc->sc_w = distance;

	if (sc->sc_flags & UWACOM_BIG_ENDIAN) {
		x = be16toh(x);
		y = be16toh(y);
	}
	
	for (int i = 0; i < ms->sc_num_stylus_buttons; i++)
		if (hid_get_data(data, len, &ms->sc_loc_stylus_btn[i]))
			stylus_buttons |= (1 << i);
			
	for (int i = 0; i < ms->sc_num_pad_buttons; i++)
		if (hid_get_data(data, len, &ms->sc_loc_pad_btn[i]))
			pad_buttons |= (1 << i);
	
	
	#ifdef UWACOM_DEBUG
		UWACOM_BUTTON_EVENT(pad_buttons);
		UWACOM_BUTTON_EVENT(stylus_buttons);
	#endif

	if (x != 0 || y != 0 || pressure != 0 || distance != 0
		       	|| pad_buttons != ms->sc_buttons || stylus_buttons != ms->sc_buttons) {
		wsmouse_buttons(ms->sc_wsmousedev, (pad_buttons | stylus_buttons));
		wsmouse_motion(ms->sc_wsmousedev, -dx, dy, dz, dw);
		wsmouse_input_sync(ms->sc_wsmousedev);
	}
}

int
uwacom_enable(void *v)
{
	struct uwacom_softc *sc = v;
	struct hidms *ms = &sc->sc_ms;
	int rv;

	if (usbd_is_dying(sc->sc_hdev.sc_udev))
		return EIO;

	if ((rv = hidms_enable(ms)) != 0)
		return rv;

	return uhidev_open(&sc->sc_hdev);
}

void
uwacom_disable(void *v)
{
	struct uwacom_softc *sc = v;
	struct hidms *ms = &sc->sc_ms;

	hidms_disable(ms);
	uhidev_close(&sc->sc_hdev);
}

int
uwacom_ioctl(void *v, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct uwacom_softc *sc = v;
	struct hidms *ms = &sc->sc_ms;
	int rc;

	switch (cmd) {
	case WSMOUSEIO_GTYPE:
		*(u_int *)data = WSMOUSE_TYPE_TPANEL;
		return 0;
	}

	rc = uhidev_ioctl(&sc->sc_hdev, cmd, data, flag, p);
	if (rc != -1)
		return rc;

	return hidms_ioctl(ms, cmd, data, flag, p);
}

