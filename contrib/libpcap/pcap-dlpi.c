/*
 * Copyright (c) 1993, 1994, 1995, 1996, 1997
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * This code contributed by Atanu Ghosh (atanu@cs.ucl.ac.uk),
 * University College London.
 */

/*
 * Packet capture routine for dlpi under SunOS 5
 *
 * Notes:
 *
 *    - Apparently the DLIOCRAW ioctl() is specific to SunOS.
 *
 *    - There is a bug in bufmod(7) such that setting the snapshot
 *      length results in data being left of the front of the packet.
 *
 *    - It might be desirable to use pfmod(7) to filter packets in the
 *      kernel.
 */

#ifndef lint
static const char rcsid[] =
    "@(#) $Header: /tcpdump/master/libpcap/pcap-dlpi.c,v 1.74 2001/12/10 07:14:15 guy Exp $ (LBL)";
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/time.h>
#ifdef HAVE_SYS_BUFMOD_H
#include <sys/bufmod.h>
#endif
#include <sys/dlpi.h>
#ifdef HAVE_SYS_DLPI_EXT_H
#include <sys/dlpi_ext.h>
#endif
#ifdef HAVE_HPUX9
#include <sys/socket.h>
#endif
#ifdef DL_HP_PPA_ACK_OBS
#include <sys/stat.h>
#endif
#include <sys/stream.h>
#if defined(HAVE_SOLARIS) && defined(HAVE_SYS_BUFMOD_H)
#include <sys/systeminfo.h>
#endif

#ifdef HAVE_HPUX9
#include <net/if.h>
#endif

#include <ctype.h>
#ifdef HAVE_HPUX9
#include <nlist.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>

#include "pcap-int.h"

#ifdef HAVE_OS_PROTO_H
#include "os-proto.h"
#endif

#ifndef PCAP_DEV_PREFIX
#ifdef _AIX
#define PCAP_DEV_PREFIX "/dev/dlpi"
#else
#define PCAP_DEV_PREFIX "/dev"
#endif
#endif

#define	MAXDLBUF	8192

/* Forwards */
static char *split_dname(char *, int *, char *);
static int dlattachreq(int, bpf_u_int32, char *);
static int dlbindack(int, char *, char *);
static int dlbindreq(int, bpf_u_int32, char *);
static int dlinfoack(int, char *, char *);
static int dlinforeq(int, char *);
static int dlokack(int, const char *, char *, char *);
static int recv_ack(int, int, const char *, char *, char *);
static char *dlstrerror(bpf_u_int32);
static char *dlprim(bpf_u_int32);
static int dlpromisconreq(int, bpf_u_int32, char *);
#if defined(HAVE_SOLARIS) && defined(HAVE_SYS_BUFMOD_H)
static char *get_release(bpf_u_int32 *, bpf_u_int32 *, bpf_u_int32 *);
#endif
static int send_request(int, char *, int, char *, char *);
#ifdef HAVE_SYS_BUFMOD_H
static int strioctl(int, int, int, char *);
#endif
#ifdef HAVE_HPUX9
static int dlpi_kread(int, off_t, void *, u_int, char *);
#endif
#ifdef HAVE_DEV_DLPI
static int get_dlpi_ppa(int, const char *, int, char *);
#endif

int
pcap_stats(pcap_t *p, struct pcap_stat *ps)
{

	/*
	 * "ps_recv" counts packets handed to the filter, not packets
	 * that passed the filter.  As filtering is done in userland,
	 * this does not include packets dropped because we ran out
	 * of buffer space.
	 *
	 * "ps_drop" counts packets dropped inside the DLPI service
	 * provider device device because of flow control requirements
	 * or resource exhaustion; it doesn't count packets dropped by
	 * the interface driver, or packets dropped upstream.  As
	 * filtering is done in userland, it counts packets regardless
	 * of whether they would've passed the filter.
	 *
	 * These statistics don't include packets not yet read from
	 * the kernel by libpcap, but they may include packets not
	 * yet read from libpcap by the application.
	 */
	*ps = p->md.stat;
	return (0);
}

/* XXX Needed by HP-UX (at least) */
static bpf_u_int32 ctlbuf[MAXDLBUF];
static struct strbuf ctl = {
	MAXDLBUF,
	0,
	(char *)ctlbuf
};

int
pcap_read(pcap_t *p, int cnt, pcap_handler callback, u_char *user)
{
	register int cc, n, caplen, origlen;
	register u_char *bp, *ep, *pk;
	register struct bpf_insn *fcode;
#ifdef HAVE_SYS_BUFMOD_H
	register struct sb_hdr *sbp;
#ifdef LBL_ALIGN
	struct sb_hdr sbhdr;
#endif
#endif
	int flags;
	struct strbuf data;
	struct pcap_pkthdr pkthdr;

	flags = 0;
	cc = p->cc;
	if (cc == 0) {
		data.buf = (char *)p->buffer + p->offset;
		data.maxlen = MAXDLBUF;
		data.len = 0;
		do {
			if (getmsg(p->fd, &ctl, &data, &flags) < 0) {
				/* Don't choke when we get ptraced */
				if (errno == EINTR) {
					cc = 0;
					continue;
				}
				strlcpy(p->errbuf, pcap_strerror(errno),
				    sizeof(p->errbuf));
				return (-1);
			}
			cc = data.len;
		} while (cc == 0);
		bp = p->buffer + p->offset;
	} else
		bp = p->bp;

	/* Loop through packets */
	fcode = p->fcode.bf_insns;
	ep = bp + cc;
	n = 0;
#ifdef HAVE_SYS_BUFMOD_H
	while (bp < ep) {
#ifdef LBL_ALIGN
		if ((long)bp & 3) {
			sbp = &sbhdr;
			memcpy(sbp, bp, sizeof(*sbp));
		} else
#endif
			sbp = (struct sb_hdr *)bp;
		p->md.stat.ps_drop += sbp->sbh_drops;
		pk = bp + sizeof(*sbp);
		bp += sbp->sbh_totlen;
		origlen = sbp->sbh_origlen;
		caplen = sbp->sbh_msglen;
#else
		origlen = cc;
		caplen = min(p->snapshot, cc);
		pk = bp;
		bp += caplen;
#endif
		++p->md.stat.ps_recv;
		if (bpf_filter(fcode, pk, origlen, caplen)) {
#ifdef HAVE_SYS_BUFMOD_H
			pkthdr.ts = sbp->sbh_timestamp;
#else
			(void)gettimeofday(&pkthdr.ts, NULL);
#endif
			pkthdr.len = origlen;
			pkthdr.caplen = caplen;
			/* Insure caplen does not exceed snapshot */
			if (pkthdr.caplen > p->snapshot)
				pkthdr.caplen = p->snapshot;
			(*callback)(user, &pkthdr, pk);
			if (++n >= cnt && cnt >= 0) {
				p->cc = ep - bp;
				p->bp = bp;
				return (n);
			}
		}
#ifdef HAVE_SYS_BUFMOD_H
	}
#endif
	p->cc = 0;
	return (n);
}

pcap_t *
pcap_open_live(char *device, int snaplen, int promisc, int to_ms, char *ebuf)
{
	register char *cp;
	register pcap_t *p;
	int ppa;
	register dl_info_ack_t *infop;
#ifdef HAVE_SYS_BUFMOD_H
	bpf_u_int32 ss, flag;
#ifdef HAVE_SOLARIS
	register char *release;
	bpf_u_int32 osmajor, osminor, osmicro;
#endif
#endif
	bpf_u_int32 buf[MAXDLBUF];
	char dname[100];
#ifndef HAVE_DEV_DLPI
	char dname2[100];
#endif

	p = (pcap_t *)malloc(sizeof(*p));
	if (p == NULL) {
		strlcpy(ebuf, pcap_strerror(errno), PCAP_ERRBUF_SIZE);
		return (NULL);
	}
	memset(p, 0, sizeof(*p));
	p->fd = -1;	/* indicate that it hasn't been opened yet */

#ifdef HAVE_DEV_DLPI
	/*
	** Remove any "/dev/" on the front of the device.
	*/
	cp = strrchr(device, '/');
	if (cp == NULL)
		cp = device;
	else
		cp++;
	strlcpy(dname, cp, sizeof(dname));

	/*
	 * Split the device name into a device type name and a unit number;
	 * chop off the unit number, so "dname" is just a device type name.
	 */
	cp = split_dname(dname, &ppa, ebuf);
	if (cp == NULL)
		goto bad;
	*cp = '\0';

	/*
	 * Use "/dev/dlpi" as the device.
	 *
	 * XXX - HP's DLPI Programmer's Guide for HP-UX 11.00 says that
	 * the "dl_mjr_num" field is for the "major number of interface
	 * driver"; that's the major of "/dev/dlpi" on the system on
	 * which I tried this, but there may be DLPI devices that
	 * use a different driver, in which case we may need to
	 * search "/dev" for the appropriate device with that major
	 * device number, rather than hardwiring "/dev/dlpi".
	 */
	cp = "/dev/dlpi";
	if ((p->fd = open(cp, O_RDWR)) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "%s: %s", cp, pcap_strerror(errno));
		goto bad;
	}

	/*
	 * Get a table of all PPAs for that device, and search that
	 * table for the specified device type name and unit number.
	 */
	ppa = get_dlpi_ppa(p->fd, dname, ppa, ebuf);
	if (ppa < 0)
		goto bad;
#else
	/*
	 * Get the unit number, and a pointer to the end of the device
	 * type name.
	 */
	cp = split_dname(device, &ppa, ebuf);
	if (cp == NULL)
		goto bad;

	/*
	 * If the device name begins with "/", assume it begins with
	 * the pathname of the directory containing the device to open;
	 * otherwise, concatenate the device directory name and the
	 * device name.
	 */
	if (*device == '/')
		strlcpy(dname, device, sizeof(dname));
	else
		snprintf(dname, sizeof(dname), "%s/%s", PCAP_DEV_PREFIX,
		    device);

	/*
	 * Make a copy of the device pathname, and then remove the unit
	 * number from the device pathname.
	 */
	strlcpy(dname2, dname, sizeof(dname));
	*(dname + strlen(dname) - strlen(cp)) = '\0';

	/* Try device without unit number */
	if ((p->fd = open(dname, O_RDWR)) < 0) {
		if (errno != ENOENT) {
			snprintf(ebuf, PCAP_ERRBUF_SIZE, "%s: %s", dname,
			    pcap_strerror(errno));
			goto bad;
		}

		/* Try again with unit number */
		if ((p->fd = open(dname2, O_RDWR)) < 0) {
			snprintf(ebuf, PCAP_ERRBUF_SIZE, "%s: %s", dname2,
			    pcap_strerror(errno));
			goto bad;
		}
		/* XXX Assume unit zero */
		ppa = 0;
	}
#endif

	p->snapshot = snaplen;

	/*
	** Attach if "style 2" provider
	*/
	if (dlinforeq(p->fd, ebuf) < 0 ||
	    dlinfoack(p->fd, (char *)buf, ebuf) < 0)
		goto bad;
	infop = &((union DL_primitives *)buf)->info_ack;
	if (infop->dl_provider_style == DL_STYLE2 &&
	    (dlattachreq(p->fd, ppa, ebuf) < 0 ||
	    dlokack(p->fd, "attach", (char *)buf, ebuf) < 0))
		goto bad;
	/*
	** Bind (defer if using HP-UX 9 or HP-UX 10.20, totally skip if
	** using SINIX)
	*/
#if !defined(HAVE_HPUX9) && !defined(HAVE_HPUX10_20) && !defined(sinix)
#ifdef _AIX
	/* According to IBM's AIX Support Line, the dl_sap value
	** should not be less than 0x600 (1536) for standard Ethernet.
	** However, we seem to get DL_BADADDR - "DLSAP addr in improper
	** format or invalid" - errors if we use 1537 on the "tr0"
	** device, which, given that its name starts with "tr" and that
	** it's IBM, probably means a Token Ring device.  (Perhaps we
	** need to use 1537 on "/dev/dlpi/en" because that device is for
	** D/I/X Ethernet, the "SAP" is actually an Ethernet type, and
	** it rejects invalid Ethernet types.)
	**
	** So if 1537 fails, we try 2, as Hyung Sik Yoon of IBM Korea
	** says that works on Token Ring (he says that 0 does *not*
	** work; perhaps that's considered an invalid LLC SAP value - I
	** assume the SAP value in a DLPI bind is an LLC SAP for network
	** types that use 802.2 LLC).
	*/
	if ((dlbindreq(p->fd, 1537, ebuf) < 0 &&
	     dlbindreq(p->fd, 2, ebuf) < 0) ||
#else
	if (dlbindreq(p->fd, 0, ebuf) < 0 ||
#endif
	    dlbindack(p->fd, (char *)buf, ebuf) < 0)
		goto bad;
#endif

	if (promisc) {
		/*
		** Enable promiscuous
		*/
		if (dlpromisconreq(p->fd, DL_PROMISC_PHYS, ebuf) < 0 ||
		    dlokack(p->fd, "promisc_phys", (char *)buf, ebuf) < 0)
			goto bad;

		/*
		** Try to enable multicast (you would have thought
		** promiscuous would be sufficient). (Skip if using
		** HP-UX or SINIX)
		*/
#if !defined(__hpux) && !defined(sinix)
		if (dlpromisconreq(p->fd, DL_PROMISC_MULTI, ebuf) < 0 ||
		    dlokack(p->fd, "promisc_multi", (char *)buf, ebuf) < 0)
			fprintf(stderr,
			    "WARNING: DL_PROMISC_MULTI failed (%s)\n", ebuf);
#endif
	}
	/*
	** Try to enable sap (when not in promiscuous mode when using
	** using HP-UX and never under SINIX)
	*/
#ifndef sinix
	if (
#ifdef __hpux
	    !promisc &&
#endif
	    (dlpromisconreq(p->fd, DL_PROMISC_SAP, ebuf) < 0 ||
	    dlokack(p->fd, "promisc_sap", (char *)buf, ebuf) < 0)) {
		/* Not fatal if promisc since the DL_PROMISC_PHYS worked */
		if (promisc)
			fprintf(stderr,
			    "WARNING: DL_PROMISC_SAP failed (%s)\n", ebuf);
		else
			goto bad;
	}
#endif

	/*
	** HP-UX 9 and HP-UX 10.20 must bind after setting promiscuous
	** options)
	*/
#if defined(HAVE_HPUX9) || defined(HAVE_HPUX10_20)
	if (dlbindreq(p->fd, 0, ebuf) < 0 ||
	    dlbindack(p->fd, (char *)buf, ebuf) < 0)
		goto bad;
#endif

	/*
	** Determine link type
	*/
	if (dlinforeq(p->fd, ebuf) < 0 ||
	    dlinfoack(p->fd, (char *)buf, ebuf) < 0)
		goto bad;

	infop = &((union DL_primitives *)buf)->info_ack;
	switch (infop->dl_mac_type) {

	case DL_CSMACD:
	case DL_ETHER:
		p->linktype = DLT_EN10MB;
		p->offset = 2;
		break;

	case DL_FDDI:
		p->linktype = DLT_FDDI;
		p->offset = 3;
		break;

	case DL_TPR:
		p->linktype = DLT_IEEE802;
		p->offset = 2;
		break;

	default:
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "unknown mac type %lu",
		    infop->dl_mac_type);
		goto bad;
	}

#ifdef	DLIOCRAW
	/*
	** This is a non standard SunOS hack to get the ethernet header.
	*/
	if (strioctl(p->fd, DLIOCRAW, 0, NULL) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "DLIOCRAW: %s",
		    pcap_strerror(errno));
		goto bad;
	}
#endif

#ifdef HAVE_SYS_BUFMOD_H
	/*
	** Another non standard call to get the data nicely buffered
	*/
	if (ioctl(p->fd, I_PUSH, "bufmod") != 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "I_PUSH bufmod: %s",
		    pcap_strerror(errno));
		goto bad;
	}

	/*
	** Now that the bufmod is pushed lets configure it.
	**
	** There is a bug in bufmod(7). When dealing with messages of
	** less than snaplen size it strips data from the beginning not
	** the end.
	**
	** This bug is supposed to be fixed in 5.3.2. Also, there is a
	** patch available. Ask for bugid 1149065.
	*/
	ss = snaplen;
#ifdef HAVE_SOLARIS
	release = get_release(&osmajor, &osminor, &osmicro);
	if (osmajor == 5 && (osminor <= 2 || (osminor == 3 && osmicro < 2)) &&
	    getenv("BUFMOD_FIXED") == NULL) {
		fprintf(stderr,
		"WARNING: bufmod is broken in SunOS %s; ignoring snaplen.\n",
		    release);
		ss = 0;
	}
#endif
	if (ss > 0 &&
	    strioctl(p->fd, SBIOCSSNAP, sizeof(ss), (char *)&ss) != 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "SBIOCSSNAP: %s",
		    pcap_strerror(errno));
		goto bad;
	}

	/*
	** Set up the bufmod flags
	*/
	if (strioctl(p->fd, SBIOCGFLAGS, sizeof(flag), (char *)&flag) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "SBIOCGFLAGS: %s",
		    pcap_strerror(errno));
		goto bad;
	}
	flag |= SB_NO_DROPS;
	if (strioctl(p->fd, SBIOCSFLAGS, sizeof(flag), (char *)&flag) != 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "SBIOCSFLAGS: %s",
		    pcap_strerror(errno));
		goto bad;
	}
	/*
	** Set up the bufmod timeout
	*/
	if (to_ms != 0) {
		struct timeval to;

		to.tv_sec = to_ms / 1000;
		to.tv_usec = (to_ms * 1000) % 1000000;
		if (strioctl(p->fd, SBIOCSTIME, sizeof(to), (char *)&to) != 0) {
			snprintf(ebuf, PCAP_ERRBUF_SIZE, "SBIOCSTIME: %s",
			    pcap_strerror(errno));
			goto bad;
		}
	}
#endif

	/*
	** As the last operation flush the read side.
	*/
	if (ioctl(p->fd, I_FLUSH, FLUSHR) != 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "FLUSHR: %s",
		    pcap_strerror(errno));
		goto bad;
	}
	/* Allocate data buffer */
	p->bufsize = MAXDLBUF * sizeof(bpf_u_int32);
	p->buffer = (u_char *)malloc(p->bufsize + p->offset);

	return (p);
bad:
	if (p->fd >= 0)
		close(p->fd);
	free(p);
	return (NULL);
}

/*
 * Split a device name into a device type name and a unit number;
 * return the a pointer to the beginning of the unit number, which
 * is the end of the device type name, and set "*unitp" to the unit
 * number.
 *
 * Returns NULL on error, and fills "ebuf" with an error message.
 */
static char *
split_dname(char *device, int *unitp, char *ebuf)
{
	char *cp;
	char *eos;
	int unit;

	/*
	 * Look for a number at the end of the device name string.
	 */
	cp = device + strlen(device) - 1;
	if (*cp < '0' || *cp > '9') {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "%s missing unit number",
		    device);
		return (NULL);
	}

	/* Digits at end of string are unit number */
	while (cp-1 >= device && *(cp-1) >= '0' && *(cp-1) <= '9')
		cp--;

	unit = strtol(cp, &eos, 10);
	if (*eos != '\0') {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "%s bad unit number", device);
		return (NULL);
	}
	*unitp = unit;
	return (cp);
}

int
pcap_setfilter(pcap_t *p, struct bpf_program *fp)
{

	if (install_bpf_program(p, fp) < 0)
		return (-1);
	return (0);
}

static int
send_request(int fd, char *ptr, int len, char *what, char *ebuf)
{
	struct	strbuf	ctl;
	int	flags;

	ctl.maxlen = 0;
	ctl.len = len;
	ctl.buf = ptr;

	flags = 0;
	if (putmsg(fd, &ctl, (struct strbuf *) NULL, flags) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "send_request: putmsg \"%s\": %s",
		    what, pcap_strerror(errno));
		return (-1);
	}
	return (0);
}

static int
recv_ack(int fd, int size, const char *what, char *bufp, char *ebuf)
{
	union	DL_primitives	*dlp;
	struct	strbuf	ctl;
	int	flags;

	ctl.maxlen = MAXDLBUF;
	ctl.len = 0;
	ctl.buf = bufp;

	flags = 0;
	if (getmsg(fd, &ctl, (struct strbuf*)NULL, &flags) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "recv_ack: %s getmsg: %s",
		    what, pcap_strerror(errno));
		return (-1);
	}

	dlp = (union DL_primitives *) ctl.buf;
	switch (dlp->dl_primitive) {

	case DL_INFO_ACK:
	case DL_BIND_ACK:
	case DL_OK_ACK:
#ifdef DL_HP_PPA_ACK
	case DL_HP_PPA_ACK:
#endif
		/* These are OK */
		break;

	case DL_ERROR_ACK:
		switch (dlp->error_ack.dl_errno) {

		case DL_SYSERR:
			snprintf(ebuf, PCAP_ERRBUF_SIZE,
			    "recv_ack: %s: UNIX error - %s",
			    what, pcap_strerror(dlp->error_ack.dl_unix_errno));
			break;

		default:
			snprintf(ebuf, PCAP_ERRBUF_SIZE, "recv_ack: %s: %s",
			    what, dlstrerror(dlp->error_ack.dl_errno));
			break;
		}
		return (-1);

	default:
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "recv_ack: %s: Unexpected primitive ack %s",
		    what, dlprim(dlp->dl_primitive));
		return (-1);
	}

	if (ctl.len < size) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "recv_ack: %s: Ack too small (%d < %d)",
		    what, ctl.len, size);
		return (-1);
	}
	return (ctl.len);
}

static char *
dlstrerror(bpf_u_int32 dl_errno)
{
	static char errstring[6+2+8+1];

	switch (dl_errno) {

	case DL_ACCESS:
		return ("Improper permissions for request");

	case DL_BADADDR:
		return ("DLSAP addr in improper format or invalid");

	case DL_BADCORR:
		return ("Seq number not from outstand DL_CONN_IND");

	case DL_BADDATA:
		return ("User data exceeded provider limit");

	case DL_BADPPA:
#ifdef HAVE_DEV_DLPI
		/*
		 * With a single "/dev/dlpi" device used for all
		 * DLPI providers, PPAs have nothing to do with
		 * unit numbers.
		 */
		return ("Specified PPA was invalid");
#else
		/*
		 * We have separate devices for separate devices;
		 * the PPA is just the unit number.
		 */
		return ("Specified PPA (device unit) was invalid");
#endif

	case DL_BADPRIM:
		return ("Primitive received not known by provider");

	case DL_BADQOSPARAM:
		return ("QOS parameters contained invalid values");

	case DL_BADQOSTYPE:
		return ("QOS structure type is unknown/unsupported");

	case DL_BADSAP:
		return ("Bad LSAP selector");

	case DL_BADTOKEN:
		return ("Token used not an active stream");

	case DL_BOUND:
		return ("Attempted second bind with dl_max_conind");

	case DL_INITFAILED:
		return ("Physical link initialization failed");

	case DL_NOADDR:
		return ("Provider couldn't allocate alternate address");

	case DL_NOTINIT:
		return ("Physical link not initialized");

	case DL_OUTSTATE:
		return ("Primitive issued in improper state");

	case DL_SYSERR:
		return ("UNIX system error occurred");

	case DL_UNSUPPORTED:
		return ("Requested service not supplied by provider");

	case DL_UNDELIVERABLE:
		return ("Previous data unit could not be delivered");

	case DL_NOTSUPPORTED:
		return ("Primitive is known but not supported");

	case DL_TOOMANY:
		return ("Limit exceeded");

	case DL_NOTENAB:
		return ("Promiscuous mode not enabled");

	case DL_BUSY:
		return ("Other streams for PPA in post-attached");

	case DL_NOAUTO:
		return ("Automatic handling XID&TEST not supported");

	case DL_NOXIDAUTO:
		return ("Automatic handling of XID not supported");

	case DL_NOTESTAUTO:
		return ("Automatic handling of TEST not supported");

	case DL_XIDAUTO:
		return ("Automatic handling of XID response");

	case DL_TESTAUTO:
		return ("Automatic handling of TEST response");

	case DL_PENDING:
		return ("Pending outstanding connect indications");

	default:
		sprintf(errstring, "Error %02x", dl_errno);
		return (errstring);
	}
}

static char *
dlprim(bpf_u_int32 prim)
{
	static char primbuf[80];

	switch (prim) {

	case DL_INFO_REQ:
		return ("DL_INFO_REQ");

	case DL_INFO_ACK:
		return ("DL_INFO_ACK");

	case DL_ATTACH_REQ:
		return ("DL_ATTACH_REQ");

	case DL_DETACH_REQ:
		return ("DL_DETACH_REQ");

	case DL_BIND_REQ:
		return ("DL_BIND_REQ");

	case DL_BIND_ACK:
		return ("DL_BIND_ACK");

	case DL_UNBIND_REQ:
		return ("DL_UNBIND_REQ");

	case DL_OK_ACK:
		return ("DL_OK_ACK");

	case DL_ERROR_ACK:
		return ("DL_ERROR_ACK");

	case DL_SUBS_BIND_REQ:
		return ("DL_SUBS_BIND_REQ");

	case DL_SUBS_BIND_ACK:
		return ("DL_SUBS_BIND_ACK");

	case DL_UNITDATA_REQ:
		return ("DL_UNITDATA_REQ");

	case DL_UNITDATA_IND:
		return ("DL_UNITDATA_IND");

	case DL_UDERROR_IND:
		return ("DL_UDERROR_IND");

	case DL_UDQOS_REQ:
		return ("DL_UDQOS_REQ");

	case DL_CONNECT_REQ:
		return ("DL_CONNECT_REQ");

	case DL_CONNECT_IND:
		return ("DL_CONNECT_IND");

	case DL_CONNECT_RES:
		return ("DL_CONNECT_RES");

	case DL_CONNECT_CON:
		return ("DL_CONNECT_CON");

	case DL_TOKEN_REQ:
		return ("DL_TOKEN_REQ");

	case DL_TOKEN_ACK:
		return ("DL_TOKEN_ACK");

	case DL_DISCONNECT_REQ:
		return ("DL_DISCONNECT_REQ");

	case DL_DISCONNECT_IND:
		return ("DL_DISCONNECT_IND");

	case DL_RESET_REQ:
		return ("DL_RESET_REQ");

	case DL_RESET_IND:
		return ("DL_RESET_IND");

	case DL_RESET_RES:
		return ("DL_RESET_RES");

	case DL_RESET_CON:
		return ("DL_RESET_CON");

	default:
		(void) sprintf(primbuf, "unknown primitive 0x%x", prim);
		return (primbuf);
	}
}

static int
dlattachreq(int fd, bpf_u_int32 ppa, char *ebuf)
{
	dl_attach_req_t	req;

	req.dl_primitive = DL_ATTACH_REQ;
	req.dl_ppa = ppa;

	return (send_request(fd, (char *)&req, sizeof(req), "attach", ebuf));
}

static int
dlbindreq(int fd, bpf_u_int32 sap, char *ebuf)
{

	dl_bind_req_t	req;

	memset((char *)&req, 0, sizeof(req));
	req.dl_primitive = DL_BIND_REQ;
#ifdef DL_HP_RAWDLS
	req.dl_max_conind = 1;			/* XXX magic number */
	/* 22 is INSAP as per the HP-UX DLPI Programmer's Guide */
	req.dl_sap = 22;
	req.dl_service_mode = DL_HP_RAWDLS;
#else
	req.dl_sap = sap;
#ifdef DL_CLDLS
	req.dl_service_mode = DL_CLDLS;
#endif
#endif

	return (send_request(fd, (char *)&req, sizeof(req), "bind", ebuf));
}

static int
dlbindack(int fd, char *bufp, char *ebuf)
{

	return (recv_ack(fd, DL_BIND_ACK_SIZE, "bind", bufp, ebuf));
}

static int
dlpromisconreq(int fd, bpf_u_int32 level, char *ebuf)
{
	dl_promiscon_req_t req;

	req.dl_primitive = DL_PROMISCON_REQ;
	req.dl_level = level;

	return (send_request(fd, (char *)&req, sizeof(req), "promiscon", ebuf));
}

static int
dlokack(int fd, const char *what, char *bufp, char *ebuf)
{

	return (recv_ack(fd, DL_OK_ACK_SIZE, what, bufp, ebuf));
}


static int
dlinforeq(int fd, char *ebuf)
{
	dl_info_req_t req;

	req.dl_primitive = DL_INFO_REQ;

	return (send_request(fd, (char *)&req, sizeof(req), "info", ebuf));
}

static int
dlinfoack(int fd, char *bufp, char *ebuf)
{

	return (recv_ack(fd, DL_INFO_ACK_SIZE, "info", bufp, ebuf));
}

#ifdef HAVE_SYS_BUFMOD_H
static int
strioctl(int fd, int cmd, int len, char *dp)
{
	struct strioctl str;
	int rc;

	str.ic_cmd = cmd;
	str.ic_timout = -1;
	str.ic_len = len;
	str.ic_dp = dp;
	rc = ioctl(fd, I_STR, &str);

	if (rc < 0)
		return (rc);
	else
		return (str.ic_len);
}
#endif

#if defined(HAVE_SOLARIS) && defined(HAVE_SYS_BUFMOD_H)
static char *
get_release(bpf_u_int32 *majorp, bpf_u_int32 *minorp, bpf_u_int32 *microp)
{
	char *cp;
	static char buf[32];

	*majorp = 0;
	*minorp = 0;
	*microp = 0;
	if (sysinfo(SI_RELEASE, buf, sizeof(buf)) < 0)
		return ("?");
	cp = buf;
	if (!isdigit((unsigned char)*cp))
		return (buf);
	*majorp = strtol(cp, &cp, 10);
	if (*cp++ != '.')
		return (buf);
	*minorp =  strtol(cp, &cp, 10);
	if (*cp++ != '.')
		return (buf);
	*microp =  strtol(cp, &cp, 10);
	return (buf);
}
#endif

#ifdef DL_HP_PPA_ACK_OBS
/*
 * Under HP-UX 10 and HP-UX 11, we can ask for the ppa
 */


/*
 * Determine ppa number that specifies ifname.
 *
 * If the "dl_hp_ppa_info_t" doesn't have a "dl_module_id_1" member,
 * the code that's used here is the old code for HP-UX 10.x.
 *
 * However, HP-UX 10.20, at least, appears to have such a member
 * in its "dl_hp_ppa_info_t" structure, so the new code is used.
 * The new code didn't work on an old 10.20 system on which Rick
 * Jones of HP tried it, but with later patches installed, it
 * worked - it appears that the older system had those members but
 * didn't put anything in them, so, if the search by name fails, we
 * do the old search.
 *
 * Rick suggests that making sure your system is "up on the latest
 * lancommon/DLPI/driver patches" is probably a good idea; it'd fix
 * that problem, as well as allowing libpcap to see packets sent
 * from the system on which the libpcap application is being run.
 * (On 10.20, in addition to getting the latest patches, you need
 * to turn the kernel "lanc_outbound_promisc_flag" flag on with ADB;
 * a posting to "comp.sys.hp.hpux" at
 *
 *	http://www.deja.com/[ST_rn=ps]/getdoc.xp?AN=558092266
 *
 * says that, to see the machine's outgoing traffic, you'd need to
 * apply the right patches to your system, and also set that variable
 * with:
 
echo 'lanc_outbound_promisc_flag/W1' | /usr/bin/adb -w /stand/vmunix /dev/kmem

 * which could be put in, for example, "/sbin/init.d/lan".
 *
 * Setting the variable is not necessary on HP-UX 11.x.
 */
static int
get_dlpi_ppa(register int fd, register const char *device, register int unit,
    register char *ebuf)
{
	register dl_hp_ppa_ack_t *ap;
	register dl_hp_ppa_info_t *ipstart, *ip;
	register int i;
	char dname[100];
	register u_long majdev;
	struct stat statbuf;
	dl_hp_ppa_req_t	req;
	char buf[MAXDLBUF];
	char *ppa_data_buf;
	dl_hp_ppa_ack_t	*dlp;
	struct strbuf ctl;
	int flags;
	int ppa;

	memset((char *)&req, 0, sizeof(req));
	req.dl_primitive = DL_HP_PPA_REQ;

	memset((char *)buf, 0, sizeof(buf));
	if (send_request(fd, (char *)&req, sizeof(req), "hpppa", ebuf) < 0)
		return (-1);

	ctl.maxlen = DL_HP_PPA_ACK_SIZE;
	ctl.len = 0;
	ctl.buf = (char *)buf;

	flags = 0;
	/*
	 * DLPI may return a big chunk of data for a DL_HP_PPA_REQ. The normal
	 * recv_ack will fail because it set the maxlen to MAXDLBUF (8192)
	 * which is NOT big enough for a DL_HP_PPA_REQ.
	 *
	 * This causes libpcap applications to fail on a system with HP-APA
	 * installed.
	 *
	 * To figure out how big the returned data is, we first call getmsg
	 * to get the small head and peek at the head to get the actual data
	 * length, and  then issue another getmsg to get the actual PPA data.
	 */
	/* get the head first */
	if (getmsg(fd, &ctl, (struct strbuf *)NULL, &flags) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa getmsg: %s", pcap_strerror(errno));
		return (-1);
	}

	dlp = (dl_hp_ppa_ack_t *)ctl.buf;
	if (dlp->dl_primitive != DL_HP_PPA_ACK) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa unexpected primitive ack 0x%x",
		    (bpf_u_int32)dlp->dl_primitive);
		return (-1);
	}
	    
	if (ctl.len < DL_HP_PPA_ACK_SIZE) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa ack too small (%d < %d)",
		     ctl.len, DL_HP_PPA_ACK_SIZE);
		return (-1);
	}
	    
	/* allocate buffer */
	if ((ppa_data_buf = (char *)malloc(dlp->dl_length)) == NULL) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa malloc: %s", pcap_strerror(errno));
		return (-1);
	}
	ctl.maxlen = dlp->dl_length;
	ctl.len = 0;
	ctl.buf = (char *)ppa_data_buf;
	/* get the data */
	if (getmsg(fd, &ctl, (struct strbuf *)NULL, &flags) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa getmsg: %s", pcap_strerror(errno));
		free(ppa_data_buf);
		return (-1);
	}
	if (ctl.len < dlp->dl_length) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "get_dlpi_ppa: hpppa ack too small (%d < %d)",
		    ctl.len, dlp->dl_length);
		free(ppa_data_buf);
		return (-1);
	}

	ap = (dl_hp_ppa_ack_t *)buf;
	ipstart = (dl_hp_ppa_info_t *)ppa_data_buf;
	ip = ipstart;

#ifdef HAVE_HP_PPA_INFO_T_DL_MODULE_ID_1
	/*
	 * The "dl_hp_ppa_info_t" structure has a "dl_module_id_1"
	 * member that should, in theory, contain the part of the
	 * name for the device that comes before the unit number,
	 * and should also have a "dl_module_id_2" member that may
	 * contain an alternate name (e.g., I think Ethernet devices
	 * have both "lan", for "lanN", and "snap", for "snapN", with
	 * the former being for Ethernet packets and the latter being
	 * for 802.3/802.2 packets).
	 *
	 * Search for the device that has the specified name and
	 * instance number.
	 */
	for (i = 0; i < ap->dl_count; i++) {
		if ((strcmp((const char *)ip->dl_module_id_1, device) == 0 ||
		     strcmp((const char *)ip->dl_module_id_2, device) == 0) &&
		    ip->dl_instance_num == unit)
			break;

		ip = (dl_hp_ppa_info_t *)((u_char *)ipstart + ip->dl_next_offset);
	}
#else
	/*
	 * We don't have that member, so the search is impossible; make it
	 * look as if the search failed.
	 */
	i = ap->dl_count;
#endif

	if (i == ap->dl_count) {
		/*
		 * Well, we didn't, or can't, find the device by name.
		 *
		 * HP-UX 10.20, whilst it has "dl_module_id_1" and
		 * "dl_module_id_2" fields in the "dl_hp_ppa_info_t",
		 * doesn't seem to fill them in unless the system is
		 * at a reasonably up-to-date patch level.
		 *
		 * Older HP-UX 10.x systems might not have those fields
		 * at all.
		 *
		 * Therefore, we'll search for the entry with the major
		 * device number of a device with the name "/dev/<dev><unit>",
		 * if such a device exists, as the old code did.
		 */
		snprintf(dname, sizeof(dname), "/dev/%s%d", device, unit);
		if (stat(dname, &statbuf) < 0) {
			snprintf(ebuf, PCAP_ERRBUF_SIZE, "stat: %s: %s",
			    dname, pcap_strerror(errno));
			return (-1);
		}
		majdev = major(statbuf.st_rdev);

		ip = ipstart;

		for (i = 0; i < ap->dl_count; i++) {
			if (ip->dl_mjr_num == majdev &&
			    ip->dl_instance_num == unit)
				break;

			ip = (dl_hp_ppa_info_t *)((u_char *)ipstart + ip->dl_next_offset);
		}
	}
        if (i == ap->dl_count) {
                snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "can't find /dev/dlpi PPA for %s%d", device, unit);
		return (-1);
        }
        if (ip->dl_hdw_state == HDW_DEAD) {
                snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "%s%d: hardware state: DOWN\n", device, unit);
		free(ppa_data_buf);
		return (-1);
        }
        ppa = ip->dl_ppa;
        free(ppa_data_buf);
        return (ppa);
}
#endif

#ifdef HAVE_HPUX9
/*
 * Under HP-UX 9, there is no good way to determine the ppa.
 * So punt and read it from /dev/kmem.
 */
static struct nlist nl[] = {
#define NL_IFNET 0
	{ "ifnet" },
	{ "" }
};

static char path_vmunix[] = "/hp-ux";

/* Determine ppa number that specifies ifname */
static int
get_dlpi_ppa(register int fd, register const char *ifname, register int unit,
    register char *ebuf)
{
	register const char *cp;
	register int kd;
	void *addr;
	struct ifnet ifnet;
	char if_name[sizeof(ifnet.if_name) + 1];

	cp = strrchr(ifname, '/');
	if (cp != NULL)
		ifname = cp + 1;
	if (nlist(path_vmunix, &nl) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "nlist %s failed",
		    path_vmunix);
		return (-1);
	}
	if (nl[NL_IFNET].n_value == 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE,
		    "could't find %s kernel symbol",
		    nl[NL_IFNET].n_name);
		return (-1);
	}
	kd = open("/dev/kmem", O_RDONLY);
	if (kd < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "kmem open: %s",
		    pcap_strerror(errno));
		return (-1);
	}
	if (dlpi_kread(kd, nl[NL_IFNET].n_value,
	    &addr, sizeof(addr), ebuf) < 0) {
		close(kd);
		return (-1);
	}
	for (; addr != NULL; addr = ifnet.if_next) {
		if (dlpi_kread(kd, (off_t)addr,
		    &ifnet, sizeof(ifnet), ebuf) < 0 ||
		    dlpi_kread(kd, (off_t)ifnet.if_name,
		    if_name, sizeof(ifnet.if_name), ebuf) < 0) {
			(void)close(kd);
			return (-1);
		}
		if_name[sizeof(ifnet.if_name)] = '\0';
		if (strcmp(if_name, ifname) == 0 && ifnet.if_unit == unit)
			return (ifnet.if_index);
	}

	snprintf(ebuf, PCAP_ERRBUF_SIZE, "Can't find %s", ifname);
	return (-1);
}

static int
dlpi_kread(register int fd, register off_t addr,
    register void *buf, register u_int len, register char *ebuf)
{
	register int cc;

	if (lseek(fd, addr, SEEK_SET) < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "lseek: %s",
		    pcap_strerror(errno));
		return (-1);
	}
	cc = read(fd, buf, len);
	if (cc < 0) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "read: %s",
		    pcap_strerror(errno));
		return (-1);
	} else if (cc != len) {
		snprintf(ebuf, PCAP_ERRBUF_SIZE, "short read (%d != %d)", cc,
		    len);
		return (-1);
	}
	return (cc);
}
#endif
