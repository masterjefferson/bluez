/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/epoll.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/mgmt.h>

#ifndef HCI_CHANNEL_MONITOR
#define HCI_CHANNEL_MONITOR  2
#endif

struct monitor_hdr {
	uint16_t opcode;
	uint16_t index;
	uint16_t len;
} __attribute__((packed));

#define MONITOR_HDR_SIZE 6

#define MONITOR_NEW_INDEX	0
#define MONITOR_DEL_INDEX	1
#define MONITOR_COMMAND_PKT	2
#define MONITOR_EVENT_PKT	3
#define MONITOR_ACL_TX_PKT	4
#define MONITOR_ACL_RX_PKT	5
#define MONITOR_SCO_TX_PKT	6
#define MONITOR_SCO_RX_PKT	7

struct monitor_new_index {
	uint8_t  type;
	uint8_t  bus;
	bdaddr_t bdaddr;
	char     name[8];
} __attribute__((packed));

#define MONITOR_NEW_INDEX_SIZE 16

#define MONITOR_DEL_INDEX_SIZE 0

static unsigned long filter_mask = 0;

#define FILTER_SHOW_INDEX	(1 << 0)
#define FILTER_SHOW_DATE	(1 << 1)
#define FILTER_SHOW_TIME	(1 << 2)
#define FILTER_SHOW_ACL_DATA	(1 << 3)
#define FILTER_SHOW_SCO_DATA	(1 << 4)

#define MAX_INDEX 16

static struct monitor_new_index index_list[MAX_INDEX];

static const char *devtype2str(uint8_t type)
{
	switch (type) {
	case 0:
		return "BR/EDR";
	case 1:
		return "AMP";
	}

	return "UNKNOWN";
}

static const char *devbus2str(uint8_t bus)
{
	switch (bus) {
	case 0:
		return "VIRTUAL";
	case 1:
		return "USB";
	case 2:
		return "PCCARD";
	case 3:
		return "UART";
	}

	return "UNKNOWN";
}

static const struct {
	uint16_t opcode;
	const char *str;
} opcode2str_table[] = {
	/* OGF 1 - Link Control */
	{ 0x0401, "Inquiry"				},
	{ 0x0402, "Inquiry Cancel"			},
	{ 0x0403, "Periodic Inquiry Mode"		},
	{ 0x0404, "Exit Periodic Inquiry Mode"		},
	{ 0x0405, "Create Connection"			},
	{ 0x0406, "Disconnect"				},
	{ 0x0407, "Add SCO Connection"			},
	{ 0x0408, "Create Connection Cancel"		},
	{ 0x0409, "Accept Connection Request"		},
	{ 0x040a, "Reject Connection Request"		},
	{ 0x040b, "Link Key Request Reply"		},
	{ 0x040c, "Link Key Request Negative Reply"	},
	{ 0x040d, "PIN Code Request Reply"		},
	{ 0x040e, "PIN Code Request Negative Reply"	},
	{ 0x040f, "Change Connection Packet Type"	},
	/* reserved command */
	{ 0x0411, "Authentication Requested"		},
	/* reserved command */
	{ 0x0413, "Set Connection Encryption"		},
	/* reserved command */
	{ 0x0415, "Change Connection Link Key"		},
	/* reserved command */
	{ 0x0417, "Master Link Key"			},
	/* reserved command */
	{ 0x0419, "Remote Name Request"			},
	{ 0x041a, "Remote Name Request Cancel"		},
	{ 0x041b, "Read Remote Supported Features"	},
	{ 0x041c, "Read Remote Extended Features"	},
	{ 0x041d, "Read Remote Version Information"	},
	/* reserved command */
	{ 0x041f, "Read Clock Offset"			},
	{ 0x0420, "Read LMP Handle"			},
	/* reserved commands */
	{ 0x0428, "Setup Synchronous Connection"	},
	{ 0x0429, "Accept Synchronous Connection"	},
	{ 0x042a, "Reject Synchronous Connection"	},
	{ 0x042b, "IO Capability Request Reply"		},
	{ 0x042c, "User Confirmation Request Reply"	},
	{ 0x042d, "User Confirmation Request Neg Reply"	},
	{ 0x042e, "User Passkey Request Reply"		},
	{ 0x042f, "User Passkey Request Negative Reply"	},
	{ 0x0430, "Remote OOB Data Request Reply"	},
	/* reserved commands */
	{ 0x0433, "Remote OOB Data Request Neg Reply"	},
	{ 0x0434, "IO Capability Request Negative Reply"},
	{ 0x0435, "Create Physical Link"		},
	{ 0x0436, "Accept Physical Link"		},
	{ 0x0437, "Disconnect Physical Link"		},
	{ 0x0438, "Create Logical Link"			},
	{ 0x0439, "Accept Logical Link"			},
	{ 0x043a, "Disconnect Logical Link"		},
	{ 0x043b, "Logical Link Cancel"			},
	{ 0x043c, "Flow Specifcation Modify"		},

	/* OGF 2 - Link Policy */
	{ 0x0801, "Holde Mode"				},
	/* reserved command */
	{ 0x0803, "Sniff Mode"				},
	{ 0x0804, "Exit Sniff Mode"			},
	{ 0x0805, "Park State"				},
	{ 0x0806, "Exit Park State"			},
	{ 0x0807, "QoS Setup"				},
	/* reserved command */
	{ 0x0809, "Role Discovery"			},
	/* reserved command */
	{ 0x080b, "Switch Role"				},
	{ 0x080c, "Read Link Policy Settings"		},
	{ 0x080d, "Write Link Policy Settings"		},
	{ 0x080e, "Read Default Link Policy Settings"	},
	{ 0x080f, "Write Default Link Policy Settings"	},
	{ 0x0810, "Flow Specification"			},
	{ 0x0811, "Sniff Subrating"			},

	/* OGF 3 - Host Control */
	{ 0x0c01, "Set Event Mask"			},
	/* reserved command */
	{ 0x0c03, "Reset"				},
	/* reserved command */
	{ 0x0c05, "Set Event Filter"			},
	/* reserved commands */
	{ 0x0c08, "Flush"				},
	{ 0x0c09, "Read PIN Type"			},
	{ 0x0c0a, "Write PIN Type"			},
	{ 0x0c0b, "Create New Unit Key"			},
	/* reserved command */
	{ 0x0c0d, "Read Stored Link Key"		},
	/* reserved commands */
	{ 0x0c11, "Write Stored Link Key"		},
	{ 0x0c12, "Delete Stored Link Key"		},
	{ 0x0c13, "Write Local Name"			},
	{ 0x0c14, "Read Local Name"			},
	{ 0x0c15, "Read Connection Accept Timeout"	},
	{ 0x0c16, "Write Connection Accept Timeout"	},
	{ 0x0c17, "Read Page Timeout"			},
	{ 0x0c18, "Write Page Timeout"			},
	{ 0x0c19, "Read Scan Enable"			},
	{ 0x0c1a, "Write Scan Enable"			},
	{ 0x0c1b, "Read Page Scan Activity"		},
	{ 0x0c1c, "Write Page Scan Activity"		},
	{ 0x0c1d, "Read Inquiry Scan Activity"		},
	{ 0x0c1e, "Write Inquiry Scan Activity"		},
	{ 0x0c1f, "Read Authentication Enable"		},
	{ 0x0c20, "Write Authentication Enable"		},
	{ 0x0c21, "Read Encryption Mode"		},
	{ 0x0c22, "Write Encryption Mode"		},
	{ 0x0c23, "Read Class of Device"		},
	{ 0x0c24, "Write Class of Device"		},
	{ 0x0c25, "Read Voice Setting"			},
	{ 0x0c26, "Write Voice Setting"			},
	{ 0x0c27, "Read Automatic Flush Timeout"	},
	{ 0x0c28, "Write Automatic Flush Timeout"	},
	{ 0x0c29, "Read Num Broadcast Retransmissions"	},
	{ 0x0c2a, "Write Num Broadcast Retransmissions"	},
	{ 0x0c2b, "Read Hold Mode Activity"		},
	{ 0x0c2c, "Write Hold Mode Activity"		},
	{ 0x0c2d, "Read Transmit Power Level"		},
	{ 0x0c2e, "Read Sync Flow Control Enable"	},
	{ 0x0c2f, "Write Sync Flow Control Enable"	},
	/* reserved command */
	{ 0x0c31, "Set Host Controller To Host Flow"	},
	/* reserved command */
	{ 0x0c33, "Host Buffer Size"			},
	/* reserved command */
	{ 0x0c35, "Host Number of Completed Packets"	},
	{ 0x0c36, "Read Link Supervision Timeout"	},
	{ 0x0c37, "Write Link Supervision Timeout"	},
	{ 0x0c38, "Read Number of Supported IAC"	},
	{ 0x0c39, "Read Current IAC LAP"		},
	{ 0x0c3a, "Write Current IAC LAP"		},
	{ 0x0c3b, "Read Page Scan Period Mode"		},
	{ 0x0c3c, "Write Page Scan Period Mode"		},
	{ 0x0c3d, "Read Page Scan Mode"			},
	{ 0x0c3e, "Write Page Scan Mode"		},
	{ 0x0c3f, "Set AFH Host Channel Classification"	},
	/* reserved commands */
	{ 0x0c42, "Read Inquiry Scan Type"		},
	{ 0x0c43, "Write Inquiry Scan Type"		},
	{ 0x0c44, "Read Inquiry Mode"			},
	{ 0x0c45, "Write Inquiry Mode"			},
	{ 0x0c46, "Read Page Scan Type"			},
	{ 0x0c47, "Write Page Scan Type"		},
	{ 0x0c48, "Read AFH Channel Assessment Mode"	},
	{ 0x0c49, "Write AFH Channel Assessment Mode"	},
	/* reserved commands */
	{ 0x0c51, "Read Extended Inquiry Response"	},
	{ 0x0c52, "Write Extended Inquiry Response"	},
	{ 0x0c53, "Refresh Encryption Key"		},
	/* reserved command */
	{ 0x0c55, "Read Simple Pairing Mode"		},
	{ 0x0c56, "Write Simple Pairing Mode"		},
	{ 0x0c57, "Read Local OOB Data"			},
	{ 0x0c58, "Read Inquiry Response TX Power Level"},
	{ 0x0c59, "Write Inquiry Transmit Power Level"	},
	{ 0x0c5a, "Read Default Erroneous Reporting"	},
	{ 0x0c5b, "Write Default Erroneous Reporting"	},
	/* reserved commands */
	{ 0x0c5f, "Enhanced Flush"			},
	/* reserved command */
	{ 0x0c61, "Read Logical Link Accept Timeout"	},
	{ 0x0c62, "Write Logical Link Accept Timeout"	},
	{ 0x0c63, "Set Event Mask Page 2"		},
	{ 0x0c64, "Read Location Data"			},
	{ 0x0c65, "Write Location Data"			},
	{ 0x0c66, "Read Flow Control Mode"		},
	{ 0x0c67, "Write Flow Control Mode"		},
	{ 0x0c68, "Read Enhanced Transmit Power Level"	},
	{ 0x0c69, "Read Best Effort Flush Timeout"	},
	{ 0x0c6a, "Write Best Effort Flush Timeout"	},
	{ 0x0c6b, "Short Range Mode"			},
	{ 0x0c6c, "Read LE Host Supported"		},
	{ 0x0c6d, "Write LE Host Supported"		},

	/* OGF 4 - Information Parameter */
	{ 0x1001, "Read Local Version Information"	},
	{ 0x1002, "Read Local Supported Commands"	},
	{ 0x1003, "Read Local Supported Features"	},
	{ 0x1004, "Read Local Extended Features"	},
	{ 0x1005, "Read Buffer Size"			},
	/* reserved command */
	{ 0x1007, "Read Country Code"			},
	/* reserved command */
	{ 0x1009, "Read BD ADDR"			},
	{ 0x100a, "Read Data Block Size"		},

	/* OGF 5 - Status Parameter */
	{ 0x1401, "Read Failed Contact Counter"		},
	{ 0x1402, "Reset Failed Contact Counter"	},
	{ 0x1403, "Read Link Quality"			},
	/* reserved command */
	{ 0x1405, "Read RSSI"				},
	{ 0x1406, "Read AFH Channel Map"		},
	{ 0x1407, "Read Clock"				},
	{ 0x1408, "Read Encryption Key Size"		},
	{ 0x1409, "Read Local AMP Info"			},
	{ 0x140a, "Read Local AMP ASSOC"		},
	{ 0x140b, "Write Remote AMP ASSOC"		},

	/* OGF 8 - LE Control */
	{ }
};

static const char *opcode2str(uint16_t opcode)
{
	int i;

	for (i = 0; opcode2str_table[i].str; i++) {
		if (opcode2str_table[i].opcode == opcode)
			return opcode2str_table[i].str;
	}

	return "Unknown";
}

static const struct {
	uint8_t event;
	const char *str;
} event2str_table[] = {
	{ 0x01, "Inquiry Complete"			},
	{ 0x02, "Inquiry Result"			},
	{ 0x03, "Connect Complete"			},
	{ 0x04, "Connect Request"			},
	{ 0x05, "Disconn Complete"			},
	{ 0x06, "Auth Complete"				},
	{ 0x07, "Remote Name Req Complete"		},
	{ 0x08, "Encrypt Change"			},
	{ 0x09, "Change Connection Link Key Complete"	},
	{ 0x0a, "Master Link Key Complete"		},
	{ 0x0b, "Read Remote Supported Features"	},
	{ 0x0c, "Read Remote Version Complete"		},
	{ 0x0d, "QoS Setup Complete"			},
	{ 0x0e, "Command Complete"			},
	{ 0x0f, "Command Status"			},
	{ 0x10, "Hardware Error"			},
	{ 0x11, "Flush Occurred"			},
	{ 0x12, "Role Change"				},
	{ 0x13, "Number of Completed Packets"		},
	{ 0x14, "Mode Change"				},
	{ 0x15, "Return Link Keys"			},
	{ 0x16, "PIN Code Request"			},
	{ 0x17, "Link Key Request"			},
	{ 0x18, "Link Key Notification"			},
	{ 0x19, "Loopback Command"			},
	{ 0x1a, "Data Buffer Overflow"			},
	{ 0x1b, "Max Slots Change"			},
	{ 0x1c, "Read Clock Offset Complete"		},
	{ 0x1d, "Connection Packet Type Changed"	},
	{ 0x1e, "QoS Violation"				},
	{ 0x1f, "Page Scan Mode Change"			},
	{ 0x20, "Page Scan Repetition Mode Change"	},
	{ 0x21, "Flow Specification Complete"		},
	{ 0x22, "Inquiry Result with RSSI"		},
	{ 0x23, "Read Remote Extended Features"		},
	/* reserved events */
	{ 0x2c, "Synchronous Connect Complete"		},
	{ 0x2d, "Synchronous Connect Changed"		},
	{ 0x2e, "Sniff Subrate"				},
	{ 0x2f, "Extended Inquiry Result"		},
	{ 0x30, "Encryption Key Refresh Complete"	},
	{ 0x31, "IO Capability Request"			},
	{ 0x32, "IO Capability Response"		},
	{ 0x33, "User Confirmation Request"		},
	{ 0x34, "User Passkey Request"			},
	{ 0x35, "Remote OOB Data Request"		},
	{ 0x36, "Simple Pairing Complete"		},
	/* reserved event */
	{ 0x38, "Link Supervision Timeout Change"	},
	{ 0x39, "Enhanced Flush Complete"		},
	/* reserved event */
	{ 0x3b, "User Passkey Notification"		},
	{ 0x3c, "Keypress Notification"			},
	{ 0x3d, "Remote Host Supported Features"	},
	{ 0x3e, "LE Meta Event"				},
	/* reserved event */
	{ 0x40, "Physical Link Complete"		},
	{ 0x41, "Channel Selected"			},
	{ 0x42, "Disconn Physical Link Complete"	},
	{ 0x43, "Physical Link Loss Early Warning"	},
	{ 0x44, "Physical Link Recovery"		},
	{ 0x45, "Logical Link Complete"			},
	{ 0x46, "Disconn Logical Link Complete"		},
	{ 0x47, "Flow Spec Modify Complete"		},
	{ 0x48, "Number Of Completed Data Blocks"	},
	{ 0x49, "AMP Start Test"			},
	{ 0x4a, "AMP Test End"				},
	{ 0x4b, "AMP Receiver Report"			},
	{ 0x4c, "Short Range Mode Change Complete"	},
	{ 0x4d, "AMP Status Change"			},
	{ 0xfe, "Testing"				},
	{ 0xff, "Vendor"				},
	{ }
};

static const char *event2str(uint8_t event)
{
	int i;

	for (i = 0; event2str_table[i].str; i++) {
		if (event2str_table[i].event == event)
			return event2str_table[i].str;
	}

	return "Unknown";
}

static void hexdump(const unsigned char *buf, uint16_t len)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	uint16_t i;

	if (!len)
		return;

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 0] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 1] = hexdigits[buf[i] & 0xf];
		str[((i % 16) * 3) + 2] = ' ';
		str[(i % 16) + 49] = isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[47] = ' ';
			str[48] = ' ';
			str[65] = '\0';
			printf("%-12c%s\n", ' ', str);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		uint16_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 0] = ' ';
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[j + 49] = ' ';
		}
		str[47] = ' ';
		str[48] = ' ';
		str[65] = '\0';
		printf("%-12c%s\n", ' ', str);
	}
}

static void process_new_index(uint16_t index, uint16_t len, void *buf)
{
	struct monitor_new_index *ni = buf;
	char str[18];

	if (len != MONITOR_NEW_INDEX_SIZE) {
		printf("* Malformed New Index packet\n");
		return;
	}

	ba2str(&ni->bdaddr, str);

	printf("= New Index: %s (%s,%s,%s)\n", str,
					devtype2str(ni->type),
					devbus2str(ni->bus), ni->name);

	if (index < MAX_INDEX)
		memcpy(&index_list[index], ni, MONITOR_NEW_INDEX_SIZE);
}

static void process_del_index(uint16_t index, uint16_t len)
{
	char str[18];

	if (len != MONITOR_DEL_INDEX_SIZE) {
		printf("* Malformed Delete Index packet\n");
		return;
	}

	if (index < MAX_INDEX)
		ba2str(&index_list[index].bdaddr, str);
	else
		ba2str(BDADDR_ANY, str);

	printf("= Delete Index: %s\n", str);
}

static void process_command_pkt(uint16_t len, void *buf)
{
	hci_command_hdr *hdr = buf;
	uint16_t opcode = btohs(hdr->opcode);
	uint16_t ogf = cmd_opcode_ogf(opcode);
	uint16_t ocf = cmd_opcode_ocf(opcode);

	if (len < HCI_COMMAND_HDR_SIZE) {
		printf("* Malformed HCI Command packet\n");
		return;
	}

	printf("< HCI Command: %s (0x%2.2x|0x%4.4x) plen %d\n",
				opcode2str(opcode), ogf, ocf, hdr->plen);

	buf += HCI_COMMAND_HDR_SIZE;
	len -= HCI_COMMAND_HDR_SIZE;

	hexdump(buf, len);
}

static void process_event_pkt(uint16_t len, void *buf)
{
	hci_event_hdr *hdr = buf;

	if (len < HCI_EVENT_HDR_SIZE) {
		printf("* Malformed HCI Event packet\n");
		return;
	}

	printf("> HCI Event: %s (0x%2.2x) plen %d\n",
				event2str(hdr->evt), hdr->evt, hdr->plen);

	buf += HCI_EVENT_HDR_SIZE;
	len -= HCI_EVENT_HDR_SIZE;

	hexdump(buf, len);
}

static void process_acldata_pkt(bool in, uint16_t len, void *buf)
{
	hci_acl_hdr *hdr = buf;
	uint16_t handle = btohs(hdr->handle);
	uint16_t dlen = btohs(hdr->dlen);
	uint8_t flags = acl_flags(handle);

	if (len < HCI_ACL_HDR_SIZE) {
		printf("* Malformed ACL Data %s packet\n", in ? "RX" : "TX");
		return;
	}

	printf("%c ACL Data: handle %d flags 0x%2.2x dlen %d\n",
			in ? '>' : '<', acl_handle(handle), flags, dlen);

	buf += HCI_ACL_HDR_SIZE;
	len -= HCI_ACL_HDR_SIZE;

	if (filter_mask & FILTER_SHOW_ACL_DATA)
		hexdump(buf, len);
}

static void process_scodata_pkt(bool in, uint16_t len, void *buf)
{
	hci_sco_hdr *hdr = buf;
	uint16_t handle = btohs(hdr->handle);
	uint8_t flags = acl_flags(handle);

	if (len < HCI_SCO_HDR_SIZE) {
		printf("* Malformed SCO Data %s packet\n", in ? "RX" : "TX");
		return;
	}

	printf("%c SCO Data: handle %d flags 0x%2.2x dlen %d\n",
			in ? '>' : '<',	acl_handle(handle), flags, hdr->dlen);

	buf += HCI_SCO_HDR_SIZE;
	len -= HCI_SCO_HDR_SIZE;

	if (filter_mask & FILTER_SHOW_SCO_DATA)
		hexdump(buf, len);
}

static void process_monitor(int fd)
{
	unsigned char buf[4096];
	unsigned char control[32];
	struct monitor_hdr hdr;
	struct msghdr msg;
	struct iovec iov[2];
	struct cmsghdr *cmsg;
	struct timeval *tv = NULL;
	uint16_t opcode, index, pktlen;
	ssize_t len;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = MONITOR_HDR_SIZE;
	iov[1].iov_base = buf;
	iov[1].iov_len = sizeof(buf);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	len = recvmsg(fd, &msg, MSG_DONTWAIT);
	if (len < 0)
		return;

	if (len < MONITOR_HDR_SIZE)
		return;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET)
			continue;

		if (cmsg->cmsg_type == SCM_TIMESTAMP)
			tv = (void *) CMSG_DATA(cmsg);
	}

	opcode = btohs(hdr.opcode);
	index  = btohs(hdr.index);
	pktlen = btohs(hdr.len);

	if (filter_mask & FILTER_SHOW_INDEX)
		printf("[hci%d] ", index);

	if (tv) {
		time_t t = tv->tv_sec;
		struct tm tm;

		localtime_r(&t, &tm);

		if (filter_mask & FILTER_SHOW_DATE)
			printf("%04d-%02d-%02d ",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

		if (filter_mask & FILTER_SHOW_TIME)
			printf("%02d:%02d:%02d.%06lu ",
				tm.tm_hour, tm.tm_min, tm.tm_sec, tv->tv_usec);
	}

	switch (opcode) {
	case MONITOR_NEW_INDEX:
		process_new_index(index, pktlen, buf);
		break;
	case MONITOR_DEL_INDEX:
		process_del_index(index, pktlen);
		break;
	case MONITOR_COMMAND_PKT:
		process_command_pkt(pktlen, buf);
		break;
	case MONITOR_EVENT_PKT:
		process_event_pkt(pktlen, buf);
		break;
	case MONITOR_ACL_TX_PKT:
		process_acldata_pkt(false, pktlen, buf);
		break;
	case MONITOR_ACL_RX_PKT:
		process_acldata_pkt(true, pktlen, buf);
		break;
	case MONITOR_SCO_TX_PKT:
		process_scodata_pkt(false, pktlen, buf);
		break;
	case MONITOR_SCO_RX_PKT:
		process_scodata_pkt(true, pktlen, buf);
		break;
	default:
		printf("* Unknown packet (code %d len %d)\n", opcode, pktlen);
		hexdump(buf, pktlen);
		break;
	}
}

static void mgmt_index_added(uint16_t len, void *buf)
{
	printf("@ Index Added\n");

	hexdump(buf, len);
}

static void mgmt_index_removed(uint16_t len, void *buf)
{
	printf("@ Index Removed\n");

	hexdump(buf, len);
}

static void mgmt_new_settings(uint16_t len, void *buf)
{
	uint32_t settings;

        if (len < 4) {
                printf("* Malformed New Settings control\n");
                return;
        }

	settings = bt_get_le32(buf);

	printf("@ New Settings: 0x%4.4x\n", settings);

	buf += 4;
	len -= 4;

	hexdump(buf, len);
}

static void mgmt_class_of_dev_changed(uint16_t len, void *buf)
{
	struct mgmt_ev_class_of_dev_changed *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Class of Device Changed control\n");
		return;
	}

	printf("@ Class of Device Changed: 0x%2.2x%2.2x%2.2x\n",
						ev->class_of_dev[2],
						ev->class_of_dev[1],
						ev->class_of_dev[0]);

        buf += sizeof(*ev);
        len -= sizeof(*ev);

        hexdump(buf, len);
}

static void mgmt_local_name_changed(uint16_t len, void *buf)
{
	struct mgmt_ev_local_name_changed *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Local Name Changed control\n");
		return;
	}

	printf("@ Local Name Changed: %s (%s)\n", ev->name, ev->short_name);

        buf += sizeof(*ev);
        len -= sizeof(*ev);

        hexdump(buf, len);
}

static void process_control(int fd)
{
	unsigned char buf[4096];
	unsigned char control[32];
	struct mgmt_hdr hdr;
	struct msghdr msg;
	struct iovec iov[2];
	struct cmsghdr *cmsg;
	struct timeval *tv = NULL;
	uint16_t opcode, index, pktlen;
	ssize_t len;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = MGMT_HDR_SIZE;
	iov[1].iov_base = buf;
	iov[1].iov_len = sizeof(buf);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	len = recvmsg(fd, &msg, MSG_DONTWAIT);
	if (len < 0)
		return;

	if (len < MGMT_HDR_SIZE)
		return;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET)
			continue;

		if (cmsg->cmsg_type == SCM_TIMESTAMP)
			tv = (void *) CMSG_DATA(cmsg);
	}

	opcode = btohs(hdr.opcode);
	index  = btohs(hdr.index);
	pktlen = btohs(hdr.len);

	if (filter_mask & FILTER_SHOW_INDEX)
		printf("{hci%d} ", index);

	if (tv) {
		time_t t = tv->tv_sec;
		struct tm tm;

		localtime_r(&t, &tm);

		if (filter_mask & FILTER_SHOW_DATE)
			printf("%04d-%02d-%02d ",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

		if (filter_mask & FILTER_SHOW_TIME)
			printf("%02d:%02d:%02d.%06lu ",
				tm.tm_hour, tm.tm_min, tm.tm_sec, tv->tv_usec);
	}

	switch (opcode) {
	case MGMT_EV_INDEX_ADDED:
		mgmt_index_added(pktlen, buf);
		break;
	case MGMT_EV_INDEX_REMOVED:
		mgmt_index_removed(pktlen, buf);
		break;
	case MGMT_EV_NEW_SETTINGS:
		mgmt_new_settings(pktlen, buf);
		break;
	case MGMT_EV_CLASS_OF_DEV_CHANGED:
		mgmt_class_of_dev_changed(pktlen, buf);
		break;
	case MGMT_EV_LOCAL_NAME_CHANGED:
		mgmt_local_name_changed(pktlen, buf);
		break;
	default:
		printf("* Unknown control (code %d len %d)\n", opcode, pktlen);
		hexdump(buf, pktlen);
		break;
	}
}

static int open_monitor(void)
{
	struct sockaddr_hci addr;
	int fd, opt = 1;

	fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (fd < 0) {
		perror("Failed to open monitor channel");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_MONITOR;

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind monitor channel");
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt)) < 0) {
		perror("Failed to enable monitor timestamps");
		close(fd);
		return -1;
	}

	return fd;
}

static int open_control(void)
{
	struct sockaddr_hci addr;
	int fd, opt = 1;

	fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (fd < 0) {
		perror("Failed to open control channel");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = HCI_CHANNEL_CONTROL;

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind control channel");
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt)) < 0) {
		perror("Failed to enable control timestamps");
		close(fd);
		return -1;
	}

	return fd;
}

#define MAX_EPOLL_EVENTS 10

int main(int argc, char *argv[])
{
	int exitcode = EXIT_FAILURE;
	struct epoll_event epoll_event;
	int mon_fd, ctl_fd, epoll_fd;

	filter_mask |= FILTER_SHOW_INDEX;
	filter_mask |= FILTER_SHOW_TIME;
	filter_mask |= FILTER_SHOW_ACL_DATA;

	mon_fd = open_monitor();
	if (mon_fd < 0)
		return exitcode;

	ctl_fd = open_control();
	if (ctl_fd < 0)
		goto close_monitor;

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		perror("Failed to create epoll descriptor");
		goto close_control;
	}

	memset(&epoll_event, 0, sizeof(epoll_event));
	epoll_event.events = EPOLLIN;
	epoll_event.data.fd = mon_fd;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, mon_fd, &epoll_event) < 0) {
		perror("Failed to setup monitor event watch");
                goto close_epoll;
        }

	memset(&epoll_event, 0, sizeof(epoll_event));
	epoll_event.events = EPOLLIN;
	epoll_event.data.fd = ctl_fd;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctl_fd, &epoll_event) < 0) {
		perror("Failed to setup control event watch");
		goto close_epoll;
	}

	for (;;) {
		struct epoll_event events[MAX_EPOLL_EVENTS];
		int n, nfds;

		nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);
		if (nfds < 0)
			continue;

		for (n = 0; n < nfds; n++) {
			if (events[n].data.fd == mon_fd)
				process_monitor(mon_fd);
			else if (events[n].data.fd == ctl_fd)
				process_control(ctl_fd);
		}
	}

	exitcode = EXIT_SUCCESS;

close_epoll:
	close(epoll_fd);

close_control:
	close(ctl_fd);

close_monitor:
	close(mon_fd);

	return exitcode;
}
