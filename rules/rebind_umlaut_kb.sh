#!/bin/bash

DEVN=$1

echo -n "$DEVN" > /sys/bus/usb/drivers/usbhid/unbind
echo "$DEVN" > /sys/bus/usb/drivers/umlaut_kb/bind

