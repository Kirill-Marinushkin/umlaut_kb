/*
 * umlaut_kb driver for the 4-keys keyboard with symbols:
 * a umlaut, o umlaut, u umlaut, eszett.
 * Requires English International keyboard layout.
 *
 * Copyright (C) 2017 Kirill Marinushkin (k.marinushkin@gmail.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 *
 * This driver is based on drivers/usb/usb-skeleton.c
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <linux/usb/input.h>

/* Define non-standard interface protocol to match the device */
#define UMLAUT_KB_INTERFACE_PROTOCOL	0xDE

/* table of devices that work with this driver */
static const struct usb_device_id umlaut_kb_table[] = {
	{ USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID,
			     USB_INTERFACE_SUBCLASS_BOOT,
			     UMLAUT_KB_INTERFACE_PROTOCOL) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, umlaut_kb_table);

#define EP_NUM		0x81
#define EP_BUF_SIZE	8

#define SCAN_BASE	0x1E

static const u8 keycodes[] = {
	KEY_Q, /* a umlaut */
	KEY_P, /* o umlaut */
	KEY_Y, /* u umlaut */
	KEY_S  /* eszett */
};

/* Structure to hold all of our device specific stuff */
struct umlaut_kb_data {
	struct usb_device *udev;
	struct usb_interface *interface;
	struct input_dev *input;
	char phys[64];
	struct urb *bulk_in_urb;
	__u8 bulk_in_buffer[EP_BUF_SIZE];
	struct kref kref;
	spinlock_t slock;
	struct mutex mut;
};

#define to_umlaut_kb_dev(d) container_of(d, struct umlaut_kb_data, kref)

static void umlaut_kb_delete(struct kref *kref)
{
	struct umlaut_kb_data *dev = to_umlaut_kb_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);

	if (dev->input)
		input_unregister_device(dev->input);

	kfree(dev);
}

static int umlaut_kb_open(struct input_dev *input)
{
	struct umlaut_kb_data *dev;
	int retval = 0;

	dev = input_get_drvdata(input);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(dev->interface);
	if (retval) {
		dev_err(&dev->interface->dev,
			"Could not get interface, error %d\n", retval);
		goto exit;
	}

	retval = usb_submit_urb(dev->bulk_in_urb, GFP_ATOMIC);
	if (retval) {
		dev_err(&dev->interface->dev,
			"Could not submit urb, error %d\n", retval);
		goto exit;
	}

	/* increment our usage count for the device */
	kref_get(&dev->kref);

exit:
	return retval;
}

static void umlaut_kb_close(struct input_dev *input)
{
	struct umlaut_kb_data *dev;

	dev = input_get_drvdata(input);
	if (!dev)
		return;

	usb_kill_urb(dev->bulk_in_urb);

	usb_autopm_put_interface(dev->interface);

	/* decrement the count on our device */
	kref_put(&dev->kref, umlaut_kb_delete);
}

static void umlaut_kb_ep_irq(struct urb *urb)
{
	struct umlaut_kb_data *dev = urb->context;
	struct usb_interface *intf = dev->interface;
	int error;
	unsigned long flags;

	spin_lock_irqsave(&dev->slock, flags);

	switch (urb->status) {
	case 0:
		break;
	case -EOVERFLOW:
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dev_dbg(&intf->dev, "urb ignored status: %d\n",
			urb->status);

		spin_unlock_irqrestore(&dev->slock, flags);

		return;
	default:
		dev_dbg(&intf->dev, "urb submit status: %d\n", urb->status);
		goto exit;
	}

	if (dev->bulk_in_buffer[2] >= SCAN_BASE &&
	    dev->bulk_in_buffer[2] < SCAN_BASE + ARRAY_SIZE(keycodes)) {
		input_report_key(dev->input, KEY_RIGHTALT, 1);
		input_report_key(dev->input,
				 keycodes[dev->bulk_in_buffer[2] - SCAN_BASE],
				 1);
		input_sync(dev->input);
	} else if (dev->bulk_in_buffer[2] == 0) {
		int i;

		for (i = 0; i < ARRAY_SIZE(keycodes); i++)
			input_report_key(dev->input, keycodes[i], 0);
		input_report_key(dev->input, KEY_RIGHTALT, 0);
		input_sync(dev->input);
	}

exit:
	error = usb_submit_urb(dev->bulk_in_urb, GFP_ATOMIC);
	if (error)
		dev_err(&intf->dev, "urb failed with code: %d\n", error);

	spin_unlock_irqrestore(&dev->slock, flags);
}

static int umlaut_kb_probe(struct usb_interface *interface,
			   const struct usb_device_id *id)
{
	struct umlaut_kb_data *dev;
	int i;
	int retval = -ENOMEM;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	spin_lock_init(&dev->slock);
	mutex_init(&dev->mut);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	/* set up the endpoint urb */
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		dev_err(&interface->dev,
			"Could not allocate bulk_in_urb\n");
		goto error;
	}

	usb_fill_int_urb(dev->bulk_in_urb, dev->udev,
			 usb_rcvintpipe(dev->udev, EP_NUM),
			 dev->bulk_in_buffer, sizeof(dev->bulk_in_buffer),
			 umlaut_kb_ep_irq, dev, 1);

	/* prepare input device */
	dev->input = input_allocate_device();
	if (!dev->input) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error;
	}

	dev->input->name = "umlaut_kb";
	dev->input->phys = dev->phys;
	usb_to_input_id(dev->udev, &dev->input->id);
	dev->input->dev.parent = &interface->dev;

	input_set_drvdata(dev->input, dev);

	dev->input->open = umlaut_kb_open;
	dev->input->close = umlaut_kb_close;

	set_bit(EV_KEY, dev->input->evbit);
	set_bit(KEY_RIGHTALT, dev->input->keybit);

	for (i = 0; i < ARRAY_SIZE(keycodes); i++)
		set_bit(keycodes[i], dev->input->keybit);

	usb_make_path(dev->udev, dev->phys, sizeof(dev->phys));
	strlcat(dev->phys, "/input0", sizeof(dev->phys));

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	retval = input_register_device(dev->input);
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev,
			"Could not register the input device.\n");
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev, "Device attached.\n");

	return 0;

error:
	/* dev->input is not registered, so free instead of unregister */
	input_free_device(dev->input);
	dev->input = NULL;

	/* this frees allocated memory */
	kref_put(&dev->kref, umlaut_kb_delete);

	return retval;
}

static void umlaut_kb_disconnect(struct usb_interface *interface)
{
	struct umlaut_kb_data *dev;

	dev = usb_get_intfdata(interface);

	mutex_lock(&dev->mut);

	usb_set_intfdata(interface, NULL);

	input_unregister_device(dev->input);

	usb_free_urb(dev->bulk_in_urb);

	mutex_unlock(&dev->mut);

	dev_info(&interface->dev, "Device disconnected.\n");
}

static int umlaut_kb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct umlaut_kb_data *dev = usb_get_intfdata(intf);

	if (!dev)
		return -ENODEV;

	if (dev->input->users)
		usb_kill_urb(dev->bulk_in_urb);

	return 0;
}

static int umlaut_kb_resume(struct usb_interface *intf)
{
	struct umlaut_kb_data *dev = usb_get_intfdata(intf);
	int retval = 0;

	if (!dev)
		return -ENODEV;

	if (dev->input->users)
		retval = usb_submit_urb(dev->bulk_in_urb, GFP_ATOMIC);

	return retval;
}

static struct usb_driver umlaut_kb_driver = {
	.name =		"umlaut_kb",
	.probe =	umlaut_kb_probe,
	.disconnect =	umlaut_kb_disconnect,
	.suspend =	umlaut_kb_suspend,
	.resume =	umlaut_kb_resume,
	.id_table =	umlaut_kb_table,
	.supports_autosuspend = 1,
};

module_usb_driver(umlaut_kb_driver);

MODULE_AUTHOR("Kirill Marinushkin <k.marinushkin@gmail.com>");
MODULE_DESCRIPTION("umlaut_kb driver for the 4-keys keyboard");
MODULE_LICENSE("GPL");
