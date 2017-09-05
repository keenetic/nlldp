/* Copyright (c) 2017 NDM Systems, Inc. http://www.ndmsystems.com/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <net/ethernet.h>

#include <linux/if.h>
#include <linux/ip.h>
#include <linux/if_packet.h>

/* libndm headers */
#include <ndm/log.h>
#include <ndm/ip_sockaddr.h>
#include <ndm/time.h>
#include <ndm/sys.h>
#include <ndm/attr.h>
#include <ndm/mac_addr.h>

#define SEND_INTERVAL				(60 * 1000) // ms
#define SEND_TTL					120 // sec
#define READ_RETRY_MS				100 // ms
#define READ_RETRY_TIMES			5 //

#define ETH_P_LLDP					0x88cc

#define TLV_HDR(id_, len_)			htons((uint16_t)(((id_) << 9) + (len_)))
#define TLV_ADD(p_, tlv_, tlv_len_)							\
{															\
		memcpy((p_), &(tlv_), (size_t)((tlv_len_) + 2));	\
		(p_) += ((tlv_len_) + 2);							\
}

struct lldp_tlv
{
	uint16_t hdr;
	union
	{
		uint8_t data[512];
		struct
		{
			uint8_t subtype;
			uint8_t data[511];
		} sub;
	} u;
} NDM_ATTR_PACKED;

static uint8_t dst_broadcast_mac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static uint8_t dst_multicast_mac[] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };

/* external configuration */
static bool debug = false;
static const char *seclvl = "public";
static const char *mode = "router";
static struct ndm_mac_addr_t mac;
static const char *interface_id = "";
static int interface_idx = 0;
static const char *system_name = "";
static const char *port_description = "";
static const char *description = "";
static struct ndm_ip_sockaddr_t ipv4_address;
static unsigned short port = 0;
static bool is_bridge = false;
static bool is_wlan_ap = false;

/* internal state */
static int fd_send = -1;

static bool nlldpd_set_nonblock(int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL)) == -1) {
		const int err = errno;

		NDM_LOG_ERROR("unable to get socket flags: %s", strerror(err));

		return false;
	}

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		const int err = errno;

		NDM_LOG_ERROR("unable to set socket flags: %s", strerror(err));

		return false;
	}

	return true;
}

static bool nlldpd_nonblock_write(
		int fd, const void *p, size_t buf_size, size_t *bytes_written,
		struct sockaddr_ll *sa)
{
	size_t bwrite = 0;
	unsigned long retries = 0;

	*bytes_written = 0;

	while (!ndm_sys_is_interrupted() && bwrite == 0) {
		ssize_t n =
			sendto(fd, p, buf_size, 0, (struct sockaddr*)sa, sizeof(*sa)); 

		if (n < 0) {
			const int error = errno;

			if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
				if (++retries <= READ_RETRY_TIMES) {
					struct timespec ts;

					ts.tv_sec = 0;
					ts.tv_nsec = READ_RETRY_MS * 1000;

					nanosleep(&ts, NULL);
				} else {
					return false;
				}
			} else {
				NDM_LOG_ERROR("unable send packet: %s", strerror(error));

				return false;
			}
		} else {
			bwrite = (size_t)n;
		}
	}

	*bytes_written = bwrite;

	return true;
}

static void nlldpd_loop()
{
	while (!ndm_sys_is_interrupted()) {
		uint8_t packet[1024];
		uint8_t *p = packet;
		uint8_t *dst_mac;
		uint16_t *proto;
		uint16_t caps;
		size_t tlv_len;
		struct lldp_tlv tlv;

		if (strcmp(seclvl, "private") == 0)
			dst_mac = dst_broadcast_mac;
		else
			dst_mac = dst_multicast_mac;

		memset(p, 0, sizeof(packet));

		memcpy(p, dst_mac, ETHER_ADDR_LEN);
		p += ETHER_ADDR_LEN;
		memcpy(p, &mac.sa.sa_data, ETHER_ADDR_LEN);
		p += ETHER_ADDR_LEN;
		proto = (uint16_t*)p;
		*proto = htons(ETH_P_LLDP);
		p += sizeof(uint16_t);

		/* Chassis ID */
		tlv_len = 7;
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(1, tlv_len); /* Chassis ID */
		tlv.u.sub.subtype = 4; /* mac address */
		memcpy(&tlv.u.sub.data, &mac.sa.sa_data, ETHER_ADDR_LEN);
		TLV_ADD(p, tlv, tlv_len);

		/* Port ID */
		tlv_len = 1 + strlen(interface_id);
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(2, tlv_len); /* port id */
		tlv.u.sub.subtype = 5; /* interface name */
		memcpy(&tlv.u.sub.data, interface_id, strlen(interface_id));
		TLV_ADD(p, tlv, tlv_len);

		/* TTL */
		tlv_len = 2;
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(3, tlv_len); /* TTL */
		*((uint16_t*)tlv.u.data) = htons(SEND_TTL);
		TLV_ADD(p, tlv, tlv_len);

		/* port description */
		tlv_len = strlen(port_description);
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(4, tlv_len); /* port description */
		memcpy(&tlv.u.data, port_description, strlen(port_description));
		TLV_ADD(p, tlv, tlv_len);

		/* System name */
		tlv_len = strlen(system_name);
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(5, tlv_len); /* system name */
		memcpy(&tlv.u.data, system_name, strlen(system_name));
		TLV_ADD(p, tlv, tlv_len);

		/* System description */
		tlv_len = strlen(description);
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(6, tlv_len); /* system description */
		memcpy(&tlv.u.data, description, strlen(description));
		TLV_ADD(p, tlv, tlv_len);

		if (!ndm_ip_sockaddr_is_equal(&ipv4_address, &NDM_IP_SOCKADDR_ANY) &&
			strcmp(seclvl, "private") == 0) {
			/* management access */
			tlv_len = 12;
			memset(&tlv, 0, sizeof(tlv));
			tlv.hdr = TLV_HDR(8, tlv_len); /* management access */
			*((uint8_t *)(tlv.u.data)) = 5; /* address string length */
			*((uint8_t *)(tlv.u.data + 1)) = 1; /* address subtype: IPv4 */
			*((uint32_t *)(tlv.u.data + 2)) = ipv4_address.un.in.sin_addr.s_addr;
			*((uint8_t *)(tlv.u.data + 6)) = 2; /* interface subtype ifindex */
			*((uint32_t *)(tlv.u.data + 7)) = htonl((uint32_t)interface_idx);
			TLV_ADD(p, tlv, tlv_len);
		}

		caps = 0;

		if (is_bridge)
			caps += (1 << 2); /* Bridge */

		if (!strcmp(mode, "router"))
			caps += (1 << 4); /* Router */
		else if (!strcmp(mode, "repeater"))
			caps += (1 << 1); /* Repeater */

		/* Capabilities */
		tlv_len = 4;
		memset(&tlv, 0, sizeof(tlv));
		tlv.hdr = TLV_HDR(7, tlv_len); /* capabilities */
		*((uint16_t*)tlv.u.data) = htons(caps);
		*((uint16_t*)(tlv.u.data + 2)) = htons(caps);
		TLV_ADD(p, tlv, tlv_len);

		if (strcmp(seclvl, "private") == 0) {
			uint8_t mval = 1; // router

			if (strcmp(mode, "client") == 0)
				mval = 2;
			else if (strcmp(mode, "repeater") == 0)
				mval = 3;
			else if (strcmp(mode, "ap") == 0)
				mval = 4;

			/* NDM Specific System Mode */
			tlv_len = 5;
			memset(&tlv, 0, sizeof(tlv));
			tlv.hdr = TLV_HDR(127, tlv_len); /* NDM Specific System Mode */
			*((uint8_t*)tlv.u.data) = 0xce;
			*((uint8_t*)tlv.u.data + 1) = 0x1e;
			*((uint8_t*)tlv.u.data + 2) = 0xe7;
			*((uint8_t*)tlv.u.data + 3) = 1; /* NDM Subtype System Mode */
			*((uint8_t*)tlv.u.data + 4) = mval; /* NDM Subtype System Mode value */
			TLV_ADD(p, tlv, tlv_len);
		}

		if (strcmp(seclvl, "private") == 0 && port != 0) {
			/* NDM Specific HTTP port */
			tlv_len = 6;
			memset(&tlv, 0, sizeof(tlv));
			tlv.hdr = TLV_HDR(127, tlv_len); /* NDM Specific HTTP port */
			*((uint8_t*)tlv.u.data) = 0xce;
			*((uint8_t*)tlv.u.data + 1) = 0x1e;
			*((uint8_t*)tlv.u.data + 2) = 0xe7;
			*((uint8_t*)tlv.u.data + 3) = 2; /* NDM Subtype HTTP port */
			*((uint16_t*)(tlv.u.data + 4)) = htons(port); /* NDM Subtype HTTP port value */
			TLV_ADD(p, tlv, tlv_len);
		}

		/* End of LLDPDU */
		memset(&tlv, 0, sizeof(tlv));
		memcpy(p, &tlv, 2);
		p += 2; // End of LLDPDU

		{
			struct sockaddr_ll sa;
			size_t bytes_written = 0;
			size_t len = (size_t)(p - packet);

			sa.sll_family = AF_PACKET;
			sa.sll_ifindex = interface_idx;
			sa.sll_halen = ETHER_ADDR_LEN;
			sa.sll_protocol = htons(ETH_P_LLDP);

			memcpy(&sa.sll_addr, &mac.sa.sa_data, ETHER_ADDR_LEN);

			if (!nlldpd_nonblock_write(
					fd_send, packet, len, &bytes_written, &sa) ||
				len != bytes_written) {
				NDM_LOG_ERROR("unable to send LLDPDU");
			}
		}

		ndm_sys_sleep_msec(SEND_INTERVAL);
	}
}

static void nlldpd_main()
{
	fd_send = socket(AF_PACKET, SOCK_RAW, 0);

	if (fd_send == -1) {
		const int err = errno;

		NDM_LOG_ERROR("unable to open send socket: %s", strerror(err));

		goto cleanup;
	}

	if (!nlldpd_set_nonblock(fd_send)) {
		goto cleanup;
	}

	{
		struct sockaddr_ll sa;

		memset(&sa, 0, sizeof(struct sockaddr_ll));

		sa.sll_family = AF_PACKET;
		sa.sll_ifindex = interface_idx;
		sa.sll_halen = ETHER_ADDR_LEN;

		memcpy(&sa.sll_addr, &mac.sa.sa_data, ETHER_ADDR_LEN);

		if (bind(fd_send, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
			const int err = errno;

			NDM_LOG_ERROR("unable to bind send socket: %s", strerror(err));

			goto cleanup;
		}
	}

	nlldpd_loop();

cleanup:
	if (fd_send != -1)
		close(fd_send);
}

int main(int argc, char *argv[])
{
	int ret_code = EXIT_FAILURE;
	const char *const ident = ndm_log_get_ident(argv);
	int c;

	if (!ndm_log_init(ident, NULL, false, true)) {
		fprintf(stderr, "%s: failed to initialize a log\n", ident);

		return ret_code;
	}

	ndm_mac_addr_init(&mac);
	ipv4_address = NDM_IP_SOCKADDR_ANY;

	for (;;) {
		c = getopt(argc, argv, "dS:m:M:I:p:x:n:D:A:P:bw");

		if (c < 0)
			break;

		switch (c) {

		case 'd':
			debug = true;
			break;

		case 'S':
			seclvl = optarg;
			break;

		case 'm':
			mode = optarg;
			break;

		case 'M':
			if (!ndm_mac_addr_parse(optarg, &mac)) {
				NDM_LOG_ERROR("invalid mac value: \"%s\"",
							  optarg);
				return ret_code;
			}
			break;

		case 'I':
			interface_id = optarg;
			break;

		case 'p':
			port_description = optarg;
			break;

		case 'x':
			if (!ndm_int_parse_int(optarg, &(interface_idx))) {
				NDM_LOG_ERROR("invalid interface_idx value: \"%s\"",
							  optarg);
				return ret_code;
			}
			break;

		case 'n':
			system_name = optarg;
			break;

		case 'D':
			description = optarg;
			break;

		case 'P':
			if (!ndm_int_parse_ushort(optarg, &(port))) {
				NDM_LOG_ERROR("invalid port value: \"%s\"",
							  optarg);
				return ret_code;
			}
			break;

		case 'A':
			if (!ndm_ip_sockaddr_pton(optarg, &ipv4_address)) {
				NDM_LOG_ERROR("invalid IPv4 address value: \"%s\"",
							  optarg);
				return ret_code;
			}
			break;

		case 'b':
			is_bridge = true;
			break;

		case 'w':
			is_wlan_ap = true;
			break;

		default:
			NDM_LOG_ERROR("unknown option \"%c\"", (char) optopt);

			return ret_code;
		}
	}

	if (!ndm_log_init(ident, interface_id, false, true)) {
		fprintf(stderr, "%s: failed to reinitialize log\n", ident);

		return ret_code;
	}

	if (!ndm_sys_init()) {
		NDM_LOG_ERROR("unable to init libndm");

		return ret_code;
	}

	if (!ndm_sys_set_default_signals()) {
		NDM_LOG_ERROR("unable set signal handlers");

		return ret_code;
	}

	nlldpd_main();

	return EXIT_SUCCESS;
}
