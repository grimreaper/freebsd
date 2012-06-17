/* $FreeBSD$ */
/*-
 * Copyright (c) 1998 The NetBSD Foundation, Inc. All rights reserved.
 * Copyright (c) 1998 Lennart Augustsson. All rights reserved.
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include "usbdevs.h"

#define	USB_DEBUG_VAR usb_debug
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_dynamic.h>

#include <dev/usb/quirk/usb_quirk.h>

MODULE_DEPEND(usb_quirk, usb, 1, 1, 1);
MODULE_VERSION(usb_quirk, 1);

#define	USB_DEV_QUIRKS_MAX 256
#define	USB_SUB_QUIRKS_MAX 8

struct usb_quirk_entry {
	uint16_t vid;
	uint16_t pid;
	uint16_t lo_rev;
	uint16_t hi_rev;
	uint16_t quirks[USB_SUB_QUIRKS_MAX];
};

static struct mtx usb_quirk_mtx;

#define	USB_QUIRK_VP(v,p,l,h,...) \
  { .vid = (v), .pid = (p), .lo_rev = (l), .hi_rev = (h), \
    .quirks = { __VA_ARGS__ } }
#define	USB_QUIRK(v,p,l,h,...) \
  USB_QUIRK_VP(USB_VENDOR_##v, USB_PRODUCT_##v##_##p, l, h, __VA_ARGS__)

static struct usb_quirk_entry usb_quirks[USB_DEV_QUIRKS_MAX] = {
	USB_QUIRK(ASUS, LCM, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(INSIDEOUT, EDGEPORT4, 0x094, 0x094, UQ_SWAP_UNICODE),
	USB_QUIRK(DALLAS, J6502, 0x0a2, 0x0a2, UQ_BAD_ADC),
	USB_QUIRK(DALLAS, J6502, 0x0a2, 0x0a2, UQ_AU_NO_XU),
	USB_QUIRK(ALTEC, ADA70, 0x103, 0x103, UQ_BAD_ADC),
	USB_QUIRK(ALTEC, ASC495, 0x000, 0x000, UQ_BAD_AUDIO),
	USB_QUIRK(QTRONIX, 980N, 0x110, 0x110, UQ_SPUR_BUT_UP),
	USB_QUIRK(ALCOR2, KBD_HUB, 0x001, 0x001, UQ_SPUR_BUT_UP),
	USB_QUIRK(MCT, HUB0100, 0x102, 0x102, UQ_BUS_POWERED),
	USB_QUIRK(MCT, USB232, 0x102, 0x102, UQ_BUS_POWERED),
	USB_QUIRK(TI, UTUSB41, 0x110, 0x110, UQ_POWER_CLAIM),
	USB_QUIRK(TELEX, MIC1, 0x009, 0x009, UQ_AU_NO_FRAC),
	USB_QUIRK(SILICONPORTALS, YAPPHONE, 0x100, 0x100, UQ_AU_INP_ASYNC),
	USB_QUIRK(LOGITECH, UN53B, 0x0000, 0xffff, UQ_NO_STRINGS),
	USB_QUIRK(ELSA, MODEM1, 0x0000, 0xffff, UQ_CFG_INDEX_1),
	/* Quirks for printer devices */
	USB_QUIRK(HP, 895C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(HP, 880C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(HP, 815C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(HP, 810C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(HP, 830C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(HP, 1220C, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	USB_QUIRK(XEROX, WCM15, 0x0000, 0xffff, UQ_BROKEN_BIDIR),
	/* Devices which should be ignored by uhid */
	USB_QUIRK(APC, UPS, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(BELKIN, F6C550AVR, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(CYBERPOWER, 1500CAVRLCD, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(CYPRESS, SILVERSHIELD, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(DELORME, EARTHMATE, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(ITUNERNET, USBLCD2X20, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(ITUNERNET, USBLCD4X20, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(LIEBERT, POWERSURE_PXT, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(MGE, UPS1, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(MGE, UPS2, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(APPLE, IPHONE, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(APPLE, IPHONE_3G, 0x0000, 0xffff, UQ_HID_IGNORE),
	USB_QUIRK(MEGATEC, UPS, 0x0000, 0xffff, UQ_HID_IGNORE),
	/* Devices which should be ignored by both ukbd and uhid */
	USB_QUIRK(CYPRESS, WISPY1A, 0x0000, 0xffff, UQ_KBD_IGNORE, UQ_HID_IGNORE),
	USB_QUIRK(METAGEEK, WISPY1B, 0x0000, 0xffff, UQ_KBD_IGNORE, UQ_HID_IGNORE),
	USB_QUIRK(METAGEEK, WISPY24X, 0x0000, 0xffff, UQ_KBD_IGNORE, UQ_HID_IGNORE),
	USB_QUIRK(METAGEEK2, WISPYDBX, 0x0000, 0xffff, UQ_KBD_IGNORE, UQ_HID_IGNORE),
	USB_QUIRK(TENX, UAUDIO0, 0x0101, 0x0101, UQ_AUDIO_SWAP_LR),
	/* MS keyboards do weird things */
	USB_QUIRK(MICROSOFT, WLINTELLIMOUSE, 0x0000, 0xffff, UQ_MS_LEADING_BYTE),
	/* umodem(4) device quirks */
	USB_QUIRK(METRICOM, RICOCHET_GS, 0x100, 0x100, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(SANYO, SCP4900, 0x000, 0x000, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(MOTOROLA2, T720C, 0x001, 0x001, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(EICON, DIVA852, 0x100, 0x100, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(SIEMENS2, ES75, 0x000, 0x000, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(QUALCOMM, CDMA_MSM, 0x0000, 0xffff, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(QUALCOMM2, CDMA_MSM, 0x0000, 0xffff, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(CURITEL, UM150, 0x0000, 0xffff, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(CURITEL, UM175, 0x0000, 0xffff, UQ_ASSUME_CM_OVER_DATA),
	USB_QUIRK(VERTEX, VW110L, 0x0000, 0xffff, UQ_ASSUME_CM_OVER_DATA),

	/* USB Mass Storage Class Quirks */
	USB_QUIRK_VP(USB_VENDOR_ASAHIOPTICAL, 0, UQ_MSC_NO_RS_CLEAR_UA,
	    UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(ADDON, ATTACHE, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(ADDON, A256MB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(ADDON, DISKPRO512, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(ADDONICS2, CABLE_205, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(AIPTEK, POCKETCAM3M, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ALCOR, UMCR_9361, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(ALCOR, TRANSCEND, 0x0000, 0xffff, UQ_MSC_NO_GETMAXLUN,
	    UQ_MSC_NO_SYNC_CACHE, UQ_MSC_NO_TEST_UNIT_READY),
	USB_QUIRK(APACER, HT202, 0x0000, 0xffff, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(ASAHIOPTICAL, OPTIO230, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(ASAHIOPTICAL, OPTIO330, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(BELKIN, USB2SCSI, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(CASIO, QV_DIGICAM, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(CCYU, ED1064, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(CENTURY, EX35QUAT, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(CYPRESS, XX6830XX, 0x0000, 0xffff, UQ_MSC_NO_GETMAXLUN,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(DESKNOTE, UCR_61S2B, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(DMI, CFSM_RW, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(EPSON, STYLUS_875DC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(EPSON, STYLUS_895, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(FEIYA, 5IN1, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(FREECOM, DVD, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(FUJIPHOTO, MASS0100, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_RS_CLEAR_UA, UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(GENESYS, GL641USB2IDE, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE, UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(GENESYS, GL641USB2IDE_2, 0x0000, 0xffff,
	    UQ_MSC_FORCE_WIRE_BBB, UQ_MSC_FORCE_PROTO_ATAPI,
	    UQ_MSC_FORCE_SHORT_INQ, UQ_MSC_NO_START_STOP,
	    UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(GENESYS, GL641USB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(GENESYS, GL641USB_2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_WRONG_CSWSIG),
	USB_QUIRK(HAGIWARA, FG, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(HAGIWARA, FGSM, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(HITACHI, DVDCAM_DZ_MV100A, 0x0000, 0xffff,
	    UQ_MSC_FORCE_WIRE_CBI, UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(HITACHI, DVDCAM_USB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(HP, CDW4E, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(HP, CDW8200, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_START_STOP),
	USB_QUIRK(IMAGINATION, DBX1, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_WRONG_CSWSIG),
	USB_QUIRK(INSYSTEM, USBCABLE, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_START_STOP, UQ_MSC_ALT_IFACE_1),
	USB_QUIRK(INSYSTEM, ATAPI, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(INSYSTEM, STORAGE_V2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(IODATA, IU_CD2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(IODATA, DVR_UEH8, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(IOMEGA, ZIP100, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_NO_TEST_UNIT_READY), /* XXX ZIP drives can also use ATAPI */
	USB_QUIRK(JMICRON, JM20337, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(KYOCERA, FINECAM_L3, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(KYOCERA, FINECAM_S3X, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(KYOCERA, FINECAM_S4, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(KYOCERA, FINECAM_S5, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(LACIE, HD, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(LEXAR, CF_READER, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(LEXAR, JUMPSHOT, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(LOGITEC, LDR_H443SU2, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(LOGITEC, LDR_H443U2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI,),
	USB_QUIRK(MELCO, DUBPXXG, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(MICROTECH, DPCM, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_START_STOP),
	USB_QUIRK(MICRON, REALSSD, 0x0000, 0xffff, UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(MICROTECH, SCSIDB25, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(MICROTECH, SCSIHD50, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(MINOLTA, E223, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(MINOLTA, F300, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(MITSUMI, CDRRW, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI |
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(MOTOROLA2, E398, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_INQUIRY_EVPD, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK_VP(USB_VENDOR_MPMAN, 0, UQ_MSC_NO_SYNC_CACHE,
	    UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(MSYSTEMS, DISKONKEY, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE, UQ_MSC_NO_GETMAXLUN,
	    UQ_MSC_NO_RS_CLEAR_UA),
	USB_QUIRK(MSYSTEMS, DISKONKEY2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(MYSON, HEDEN, 0x0000, 0xffff, UQ_MSC_IGNORE_RESIDUE,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(NEODIO, ND3260, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ),
	USB_QUIRK(NETAC, CF_CARD, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(NETAC, ONLYDISK, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(NETCHIP, CLIK_40, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_ATAPI,
	    UQ_MSC_NO_INQUIRY),
	USB_QUIRK(NIKON, D300, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(OLYMPUS, C1, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_WRONG_CSWSIG),
	USB_QUIRK(OLYMPUS, C700, 0x0000, 0xffff, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(ONSPEC, SDS_HOTFIND_D, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN, UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(ONSPEC, CFMS_RW, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, CFSM_COMBO, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, CFSM_READER, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, CFSM_READER2, 0x0000, 0xffff,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, MDCFE_B_CF_READER, 0x0000, 0xffff,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, MDSM_B_READER, 0x0000, 0xffff,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(ONSPEC, READER, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(ONSPEC, UCF100, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_INQUIRY | UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(ONSPEC2, IMAGEMATE_SDDR55, 0x0000, 0xffff,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(PANASONIC, KXL840AN, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(PANASONIC, KXLCB20AN, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(PANASONIC, KXLCB35AN, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(PANASONIC, LS120CAM, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_UFI),
	USB_QUIRK(PLEXTOR, 40_12_40U, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_TEST_UNIT_READY),
	USB_QUIRK(PNY, ATTACHE2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE,
	    UQ_MSC_NO_START_STOP),
	USB_QUIRK(PROLIFIC, PL2506, 0x0000, 0xffff,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK_VP(USB_VENDOR_SAMSUNG_TECHWIN,
	    USB_PRODUCT_SAMSUNG_TECHWIN_DIGIMAX_410, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SANDISK, SDDR05A, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_READ_CAP_OFFBY1,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SANDISK, SDDR09, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_READ_CAP_OFFBY1, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SANDISK, SDDR12, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_READ_CAP_OFFBY1,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SANDISK, SDCZ2_256, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(SANDISK, SDCZ4_128, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(SANDISK, SDCZ4_256, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(SANDISK, SDDR31, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_READ_CAP_OFFBY1),
	USB_QUIRK(SCANLOGIC, SL11R, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SHUTTLE, EUSB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_START_STOP, UQ_MSC_SHUTTLE_INIT),
	USB_QUIRK(SHUTTLE, CDRW, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(SHUTTLE, CF, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(SHUTTLE, EUSBATAPI, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(SHUTTLE, EUSBCFSM, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(SHUTTLE, EUSCSI, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(SHUTTLE, HIFD, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SHUTTLE, SDDR09, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SHUTTLE, ZIOMMC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SIGMATEL, I_BEAD100, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_SHUTTLE_INIT),
	USB_QUIRK(SIIG, WINTERREADER, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(SKANHEX, MD_7425, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SKANHEX, SX_520Z, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SONY, HANDYCAM, 0x0500, 0x0500, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC, UQ_MSC_RBC_PAD_TO_12),
	USB_QUIRK(SONY, CLIE_40_MS, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SONY, DSC, 0x0500, 0x0500, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC, UQ_MSC_RBC_PAD_TO_12),
	USB_QUIRK(SONY, DSC, 0x0600, 0x0600, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC, UQ_MSC_RBC_PAD_TO_12),
	USB_QUIRK(SONY, DSC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(SONY, HANDYCAM, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(SONY, MSC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(SONY, MS_MSC_U03, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_UFI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SONY, MS_NW_MS7, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SONY, MS_PEG_N760C, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(SONY, MSACUS1, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(SONY, PORTABLE_HDD_V2, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(SUPERTOP, IDE, 0x0000, 0xffff, UQ_MSC_IGNORE_RESIDUE,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(TAUGA, CAMERAMATE, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(TEAC, FD05PUB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_UFI),
	USB_QUIRK(TECLAST, TLC300, 0x0000, 0xffff, UQ_MSC_NO_TEST_UNIT_READY,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(TREK, MEMKEY, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(TREK, THUMBDRIVE_8MB, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(TRUMPION, C3310, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_UFI),
	USB_QUIRK(TRUMPION, MP3, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_RBC),
	USB_QUIRK(TRUMPION, T33520, 0x0000, 0xffff, UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(TWINMOS, MDIV, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI),
	USB_QUIRK(VIA, USB2IDEBRIDGE, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(VIVITAR, 35XX, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(WESTERN, COMBO, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(WESTERN, EXTHDD, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(WESTERN, MYBOOK, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY_EVPD,
	    UQ_MSC_NO_SYNC_CACHE),
	USB_QUIRK(WESTERN, MYPASSWORD, 0x0000, 0xffff, UQ_MSC_FORCE_SHORT_INQ),
	USB_QUIRK(WINMAXGROUP, FLASH64MC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY),
	USB_QUIRK(YANO, FW800HD, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_FORCE_SHORT_INQ,
	    UQ_MSC_NO_START_STOP, UQ_MSC_IGNORE_RESIDUE),
	USB_QUIRK(YANO, U640MO, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_ATAPI, UQ_MSC_FORCE_SHORT_INQ),
	USB_QUIRK(YEDATA, FLASHBUSTERU, 0x0000, 0x007F, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_UFI, UQ_MSC_NO_RS_CLEAR_UA, UQ_MSC_FLOPPY_SPEED,
	    UQ_MSC_NO_TEST_UNIT_READY, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(YEDATA, FLASHBUSTERU, 0x0080, 0x0080, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_UFI, UQ_MSC_NO_RS_CLEAR_UA, UQ_MSC_FLOPPY_SPEED,
	    UQ_MSC_NO_TEST_UNIT_READY, UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(YEDATA, FLASHBUSTERU, 0x0081, 0xFFFF, UQ_MSC_FORCE_WIRE_CBI_I,
	    UQ_MSC_FORCE_PROTO_UFI, UQ_MSC_NO_RS_CLEAR_UA, UQ_MSC_FLOPPY_SPEED,
	    UQ_MSC_NO_GETMAXLUN),
	USB_QUIRK(ZORAN, EX20DSC, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_CBI,
	    UQ_MSC_FORCE_PROTO_ATAPI),
	USB_QUIRK(MEIZU, M6_SL, 0x0000, 0xffff, UQ_MSC_FORCE_WIRE_BBB,
	    UQ_MSC_FORCE_PROTO_SCSI, UQ_MSC_NO_INQUIRY, UQ_MSC_NO_SYNC_CACHE),

	/* Non-standard USB MIDI devices */
	USB_QUIRK(ROLAND, UM1, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SC8850, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SD90, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UM880N, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UA100, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UM4, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, U8, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UM2, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SC8820, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, PC300, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SK500, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SCD70, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UM550, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SD20, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, SD80, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(ROLAND, UA700, 0x0000, 0xffff, UQ_AU_VENDOR_CLASS),
	USB_QUIRK(EGO, M4U, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),
	USB_QUIRK(LOGILINK, U2M, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),
	USB_QUIRK(MEDELI, DD305, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI, UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(REDOCTANE, GHMIDI, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),
	USB_QUIRK(TEXTECH, U2M_1, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),
	USB_QUIRK(TEXTECH, U2M_2, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),
	USB_QUIRK(WCH2, U2M, 0x0000, 0xffff, UQ_SINGLE_CMD_MIDI),

	/*
	 * Quirks for manufacturers which USB devices does not respond
	 * after issuing non-supported commands:
	 */
	USB_QUIRK(ALCOR, DUMMY, 0x0000, 0xffff, UQ_MSC_NO_SYNC_CACHE, UQ_MSC_NO_TEST_UNIT_READY, UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(FEIYA, DUMMY, 0x0000, 0xffff, UQ_MSC_NO_SYNC_CACHE, UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(REALTEK, DUMMY, 0x0000, 0xffff, UQ_MSC_NO_SYNC_CACHE, UQ_MATCH_VENDOR_ONLY),
	USB_QUIRK(INITIO, DUMMY, 0x0000, 0xffff, UQ_MSC_NO_SYNC_CACHE, UQ_MATCH_VENDOR_ONLY),
};
#undef USB_QUIRK_VP
#undef USB_QUIRK

static const char *usb_quirk_str[USB_QUIRK_MAX] = {
	[UQ_NONE]		= "UQ_NONE",
	[UQ_MATCH_VENDOR_ONLY]	= "UQ_MATCH_VENDOR_ONLY",
	[UQ_AUDIO_SWAP_LR]	= "UQ_AUDIO_SWAP_LR",
	[UQ_AU_INP_ASYNC]	= "UQ_AU_INP_ASYNC",
	[UQ_AU_NO_FRAC]		= "UQ_AU_NO_FRAC",
	[UQ_AU_NO_XU]		= "UQ_AU_NO_XU",
	[UQ_BAD_ADC]		= "UQ_BAD_ADC",
	[UQ_BAD_AUDIO]		= "UQ_BAD_AUDIO",
	[UQ_BROKEN_BIDIR]	= "UQ_BROKEN_BIDIR",
	[UQ_BUS_POWERED]	= "UQ_BUS_POWERED",
	[UQ_HID_IGNORE]		= "UQ_HID_IGNORE",
	[UQ_KBD_IGNORE]		= "UQ_KBD_IGNORE",
	[UQ_KBD_BOOTPROTO]	= "UQ_KBD_BOOTPROTO",
	[UQ_MS_BAD_CLASS]	= "UQ_MS_BAD_CLASS",
	[UQ_MS_LEADING_BYTE]	= "UQ_MS_LEADING_BYTE",
	[UQ_MS_REVZ]		= "UQ_MS_REVZ",
	[UQ_NO_STRINGS]		= "UQ_NO_STRINGS",
	[UQ_OPEN_CLEARSTALL]	= "UQ_OPEN_CLEARSTALL",
	[UQ_POWER_CLAIM]	= "UQ_POWER_CLAIM",
	[UQ_SPUR_BUT_UP]	= "UQ_SPUR_BUT_UP",
	[UQ_SWAP_UNICODE]	= "UQ_SWAP_UNICODE",
	[UQ_CFG_INDEX_1]	= "UQ_CFG_INDEX_1",
	[UQ_CFG_INDEX_2]	= "UQ_CFG_INDEX_2",
	[UQ_CFG_INDEX_3]	= "UQ_CFG_INDEX_3",
	[UQ_CFG_INDEX_4]	= "UQ_CFG_INDEX_4",
	[UQ_CFG_INDEX_0]	= "UQ_CFG_INDEX_0",
	[UQ_ASSUME_CM_OVER_DATA]	= "UQ_ASSUME_CM_OVER_DATA",
	[UQ_MSC_NO_TEST_UNIT_READY]	= "UQ_MSC_NO_TEST_UNIT_READY",
	[UQ_MSC_NO_RS_CLEAR_UA]		= "UQ_MSC_NO_RS_CLEAR_UA",
	[UQ_MSC_NO_START_STOP]		= "UQ_MSC_NO_START_STOP",
	[UQ_MSC_NO_GETMAXLUN]		= "UQ_MSC_NO_GETMAXLUN",
	[UQ_MSC_NO_INQUIRY]		= "UQ_MSC_NO_INQUIRY",
	[UQ_MSC_NO_INQUIRY_EVPD]	= "UQ_MSC_NO_INQUIRY_EVPD",
	[UQ_MSC_NO_SYNC_CACHE]		= "UQ_MSC_NO_SYNC_CACHE",
	[UQ_MSC_SHUTTLE_INIT]		= "UQ_MSC_SHUTTLE_INIT",
	[UQ_MSC_ALT_IFACE_1]		= "UQ_MSC_ALT_IFACE_1",
	[UQ_MSC_FLOPPY_SPEED]		= "UQ_MSC_FLOPPY_SPEED",
	[UQ_MSC_IGNORE_RESIDUE]		= "UQ_MSC_IGNORE_RESIDUE",
	[UQ_MSC_WRONG_CSWSIG]		= "UQ_MSC_WRONG_CSWSIG",
	[UQ_MSC_RBC_PAD_TO_12]		= "UQ_MSC_RBC_PAD_TO_12",
	[UQ_MSC_READ_CAP_OFFBY1]	= "UQ_MSC_READ_CAP_OFFBY1",
	[UQ_MSC_FORCE_SHORT_INQ]	= "UQ_MSC_FORCE_SHORT_INQ",
	[UQ_MSC_FORCE_WIRE_BBB]		= "UQ_MSC_FORCE_WIRE_BBB",
	[UQ_MSC_FORCE_WIRE_CBI]		= "UQ_MSC_FORCE_WIRE_CBI",
	[UQ_MSC_FORCE_WIRE_CBI_I]	= "UQ_MSC_FORCE_WIRE_CBI_I",
	[UQ_MSC_FORCE_PROTO_SCSI]	= "UQ_MSC_FORCE_PROTO_SCSI",
	[UQ_MSC_FORCE_PROTO_ATAPI]	= "UQ_MSC_FORCE_PROTO_ATAPI",
	[UQ_MSC_FORCE_PROTO_UFI]	= "UQ_MSC_FORCE_PROTO_UFI",
	[UQ_MSC_FORCE_PROTO_RBC]	= "UQ_MSC_FORCE_PROTO_RBC",
	[UQ_MSC_EJECT_HUAWEI]		= "UQ_MSC_EJECT_HUAWEI",
	[UQ_MSC_EJECT_SIERRA]		= "UQ_MSC_EJECT_SIERRA",
	[UQ_MSC_EJECT_SCSIEJECT]	= "UQ_MSC_EJECT_SCSIEJECT",
	[UQ_MSC_EJECT_REZERO]		= "UQ_MSC_EJECT_REZERO",
	[UQ_MSC_EJECT_ZTESTOR]		= "UQ_MSC_EJECT_ZTESTOR",
	[UQ_MSC_EJECT_CMOTECH]		= "UQ_MSC_EJECT_CMOTECH",
	[UQ_MSC_EJECT_WAIT]		= "UQ_MSC_EJECT_WAIT",
	[UQ_MSC_EJECT_SAEL_M460]	= "UQ_MSC_EJECT_SAEL_M460",
	[UQ_MSC_EJECT_HUAWEISCSI]	= "UQ_MSC_EJECT_HUAWEISCSI",
	[UQ_MSC_EJECT_TCT]		= "UQ_MSC_EJECT_TCT",
	[UQ_BAD_MIDI]			= "UQ_BAD_MIDI",
	[UQ_AU_VENDOR_CLASS]		= "UQ_AU_VENDOR_CLASS",
	[UQ_SINGLE_CMD_MIDI]		= "UQ_SINGLE_CMD_MIDI",
};

/*------------------------------------------------------------------------*
 *	usb_quirkstr
 *
 * This function converts an USB quirk code into a string.
 *------------------------------------------------------------------------*/
static const char *
usb_quirkstr(uint16_t quirk)
{
	return ((quirk < USB_QUIRK_MAX) ?
	    usb_quirk_str[quirk] : "USB_QUIRK_UNKNOWN");
}

/*------------------------------------------------------------------------*
 *	usb_test_quirk_by_info
 *
 * Returns:
 * 0: Quirk not found
 * Else: Quirk found
 *------------------------------------------------------------------------*/
static uint8_t
usb_test_quirk_by_info(const struct usbd_lookup_info *info, uint16_t quirk)
{
	uint16_t x;
	uint16_t y;

	if (quirk == UQ_NONE)
		goto done;

	mtx_lock(&usb_quirk_mtx);

	for (x = 0; x != USB_DEV_QUIRKS_MAX; x++) {
		/* see if quirk information does not match */
		if ((usb_quirks[x].vid != info->idVendor) ||
		    (usb_quirks[x].lo_rev > info->bcdDevice) ||
		    (usb_quirks[x].hi_rev < info->bcdDevice)) {
			continue;
		}
		/* see if quirk only should match vendor ID */
		if (usb_quirks[x].pid != info->idProduct) {
			if (usb_quirks[x].pid != 0)
				continue;

			for (y = 0; y != USB_SUB_QUIRKS_MAX; y++) {
				if (usb_quirks[x].quirks[y] == UQ_MATCH_VENDOR_ONLY)
					break;
			}
			if (y == USB_SUB_QUIRKS_MAX)
				continue;
		}
		/* lookup quirk */
		for (y = 0; y != USB_SUB_QUIRKS_MAX; y++) {
			if (usb_quirks[x].quirks[y] == quirk) {
				mtx_unlock(&usb_quirk_mtx);
				DPRINTF("Found quirk '%s'.\n", usb_quirkstr(quirk));
				return (1);
			}
		}
		/* no quirk found */
		break;
	}
	mtx_unlock(&usb_quirk_mtx);
done:
	return (0);			/* no quirk match */
}

static struct usb_quirk_entry *
usb_quirk_get_entry(uint16_t vid, uint16_t pid,
    uint16_t lo_rev, uint16_t hi_rev, uint8_t do_alloc)
{
	uint16_t x;

	mtx_assert(&usb_quirk_mtx, MA_OWNED);

	if ((vid | pid | lo_rev | hi_rev) == 0) {
		/* all zero - special case */
		return (usb_quirks + USB_DEV_QUIRKS_MAX - 1);
	}
	/* search for an existing entry */
	for (x = 0; x != USB_DEV_QUIRKS_MAX; x++) {
		/* see if quirk information does not match */
		if ((usb_quirks[x].vid != vid) ||
		    (usb_quirks[x].pid != pid) ||
		    (usb_quirks[x].lo_rev != lo_rev) ||
		    (usb_quirks[x].hi_rev != hi_rev)) {
			continue;
		}
		return (usb_quirks + x);
	}

	if (do_alloc == 0) {
		/* no match */
		return (NULL);
	}
	/* search for a free entry */
	for (x = 0; x != USB_DEV_QUIRKS_MAX; x++) {
		/* see if quirk information does not match */
		if ((usb_quirks[x].vid |
		    usb_quirks[x].pid |
		    usb_quirks[x].lo_rev |
		    usb_quirks[x].hi_rev) != 0) {
			continue;
		}
		usb_quirks[x].vid = vid;
		usb_quirks[x].pid = pid;
		usb_quirks[x].lo_rev = lo_rev;
		usb_quirks[x].hi_rev = hi_rev;

		return (usb_quirks + x);
	}

	/* no entry found */
	return (NULL);
}

/*------------------------------------------------------------------------*
 *	usb_quirk_ioctl - handle quirk IOCTLs
 *
 * Returns:
 * 0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
static int
usb_quirk_ioctl(unsigned long cmd, caddr_t data,
    int fflag, struct thread *td)
{
	struct usb_gen_quirk *pgq;
	struct usb_quirk_entry *pqe;
	uint32_t x;
	uint32_t y;
	int err;

	switch (cmd) {
	case USB_DEV_QUIRK_GET:
		pgq = (void *)data;
		x = pgq->index % USB_SUB_QUIRKS_MAX;
		y = pgq->index / USB_SUB_QUIRKS_MAX;
		if (y >= USB_DEV_QUIRKS_MAX) {
			return (EINVAL);
		}
		mtx_lock(&usb_quirk_mtx);
		/* copy out data */
		pgq->vid = usb_quirks[y].vid;
		pgq->pid = usb_quirks[y].pid;
		pgq->bcdDeviceLow = usb_quirks[y].lo_rev;
		pgq->bcdDeviceHigh = usb_quirks[y].hi_rev;
		strlcpy(pgq->quirkname,
		    usb_quirkstr(usb_quirks[y].quirks[x]),
		    sizeof(pgq->quirkname));
		mtx_unlock(&usb_quirk_mtx);
		return (0);		/* success */

	case USB_QUIRK_NAME_GET:
		pgq = (void *)data;
		x = pgq->index;
		if (x >= USB_QUIRK_MAX) {
			return (EINVAL);
		}
		strlcpy(pgq->quirkname,
		    usb_quirkstr(x), sizeof(pgq->quirkname));
		return (0);		/* success */

	case USB_DEV_QUIRK_ADD:
		pgq = (void *)data;

		/* check privileges */
		err = priv_check(curthread, PRIV_DRIVER);
		if (err) {
			return (err);
		}
		/* convert quirk string into numerical */
		for (y = 0; y != USB_DEV_QUIRKS_MAX; y++) {
			if (strcmp(pgq->quirkname, usb_quirkstr(y)) == 0) {
				break;
			}
		}
		if (y == USB_DEV_QUIRKS_MAX) {
			return (EINVAL);
		}
		if (y == UQ_NONE) {
			return (EINVAL);
		}
		mtx_lock(&usb_quirk_mtx);
		pqe = usb_quirk_get_entry(pgq->vid, pgq->pid,
		    pgq->bcdDeviceLow, pgq->bcdDeviceHigh, 1);
		if (pqe == NULL) {
			mtx_unlock(&usb_quirk_mtx);
			return (EINVAL);
		}
		for (x = 0; x != USB_SUB_QUIRKS_MAX; x++) {
			if (pqe->quirks[x] == UQ_NONE) {
				pqe->quirks[x] = y;
				break;
			}
		}
		mtx_unlock(&usb_quirk_mtx);
		if (x == USB_SUB_QUIRKS_MAX) {
			return (ENOMEM);
		}
		return (0);		/* success */

	case USB_DEV_QUIRK_REMOVE:
		pgq = (void *)data;
		/* check privileges */
		err = priv_check(curthread, PRIV_DRIVER);
		if (err) {
			return (err);
		}
		/* convert quirk string into numerical */
		for (y = 0; y != USB_DEV_QUIRKS_MAX; y++) {
			if (strcmp(pgq->quirkname, usb_quirkstr(y)) == 0) {
				break;
			}
		}
		if (y == USB_DEV_QUIRKS_MAX) {
			return (EINVAL);
		}
		if (y == UQ_NONE) {
			return (EINVAL);
		}
		mtx_lock(&usb_quirk_mtx);
		pqe = usb_quirk_get_entry(pgq->vid, pgq->pid,
		    pgq->bcdDeviceLow, pgq->bcdDeviceHigh, 0);
		if (pqe == NULL) {
			mtx_unlock(&usb_quirk_mtx);
			return (EINVAL);
		}
		for (x = 0; x != USB_SUB_QUIRKS_MAX; x++) {
			if (pqe->quirks[x] == y) {
				pqe->quirks[x] = UQ_NONE;
				break;
			}
		}
		if (x == USB_SUB_QUIRKS_MAX) {
			mtx_unlock(&usb_quirk_mtx);
			return (ENOMEM);
		}
		for (x = 0; x != USB_SUB_QUIRKS_MAX; x++) {
			if (pqe->quirks[x] != UQ_NONE) {
				break;
			}
		}
		if (x == USB_SUB_QUIRKS_MAX) {
			/* all quirk entries are unused - release */
			memset(pqe, 0, sizeof(pqe));
		}
		mtx_unlock(&usb_quirk_mtx);
		return (0);		/* success */

	default:
		break;
	}
	return (ENOIOCTL);
}

static void
usb_quirk_init(void *arg)
{
	/* initialize mutex */
	mtx_init(&usb_quirk_mtx, "USB quirk", NULL, MTX_DEF);

	/* register our function */
	usb_test_quirk_p = &usb_test_quirk_by_info;
	usb_quirk_ioctl_p = &usb_quirk_ioctl;
}

static void
usb_quirk_uninit(void *arg)
{
	usb_quirk_unload(arg);

	/* destroy mutex */
	mtx_destroy(&usb_quirk_mtx);
}

SYSINIT(usb_quirk_init, SI_SUB_LOCK, SI_ORDER_FIRST, usb_quirk_init, NULL);
SYSUNINIT(usb_quirk_uninit, SI_SUB_LOCK, SI_ORDER_ANY, usb_quirk_uninit, NULL);
