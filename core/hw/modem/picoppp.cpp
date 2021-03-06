/*
	Created on: Sep 15, 2018

	Copyright 2018 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#if !defined(_MSC_VER) && !defined(TARGET_NO_THREADS)

#include "stdclass.h"

#ifdef __MINGW32__
#define _POSIX_SOURCE
#endif

extern "C" {
#include <pico_stack.h>
#include <pico_dev_ppp.h>
#include <pico_socket.h>
#include <pico_socket_tcp.h>
#include <pico_ipv4.h>
#include <pico_tcp.h>
}

#include "network/net_platform.h"

#include "types.h"
#include "cfg/cfg.h"
#include "picoppp.h"

#include <map>
#include <mutex>
#include <queue>

#define RESOLVER1_OPENDNS_COM "208.67.222.222"
#define AFO_ORIG_IP 0x83f2fb3f		// 63.251.242.131 in network order
#define IGP_ORIG_IP 0xef2bd2cc		// 204.210.43.239 in network order

static struct pico_device *ppp;

static std::queue<u8> in_buffer;
static std::queue<u8> out_buffer;

static std::mutex in_buffer_lock;
static std::mutex out_buffer_lock;

struct pico_ip4 dcaddr;
struct pico_ip4 dnsaddr;
static struct pico_socket *pico_tcp_socket, *pico_udp_socket;

struct pico_ip4 public_ip;
struct pico_ip4 afo_ip;

// src socket -> socket fd
static std::map<struct pico_socket *, sock_t> tcp_sockets;
static std::map<struct pico_socket *, sock_t> tcp_connecting_sockets;
// src port -> socket fd
static std::map<uint16_t, sock_t> udp_sockets;

static const uint16_t games_udp_ports[] = {
		7980,	// Alien Front Online
		9789,	// ChuChu Rocket
		// NBA/NFL/NCAA 2K Series
		5502,
		5503,
		5656,
		3512,	// The Next Tetris
		6001,	// Ooga Booga
		// PBA Tour Bowling 2001, Starlancer
		// 2300-2400, ?
		6500,
		13139,
		// Planet Ring
		7648,
		1285,
		1028,
		// World Series Baseball 2K2
		37171,
		13713,
};
static const uint16_t games_tcp_ports[] = {
		// NBA/NFL/NCAA 2K Series
		5011,
		6666,
		3512,	// The Next Tetris
		// PBA Tour Bowling 2001, Starlancer
		// 2300-2400, ?
		47624,
		17219,	// Worms World Party
};
// listening port -> socket fd
static std::map<uint16_t, sock_t> tcp_listening_sockets;

static void read_native_sockets();
void get_host_by_name(const char *name, struct pico_ip4 dnsaddr);
int get_dns_answer(struct pico_ip4 *address, struct pico_ip4 dnsaddr);

static int modem_read(struct pico_device *dev, void *data, int len)
{
	u8 *p = (u8 *)data;

	int count = 0;
	out_buffer_lock.lock();
	while (!out_buffer.empty() && count < len)
	{
		*p++ = out_buffer.front();
		out_buffer.pop();
		count++;
	}
	out_buffer_lock.unlock();

    return count;
}

static int modem_write(struct pico_device *dev, const void *data, int len)
{
	u8 *p = (u8 *)data;

	in_buffer_lock.lock();
	while (len > 0)
	{
		in_buffer.push(*p++);
		len--;
	}
	in_buffer_lock.unlock();

    return len;
}

void write_pico(u8 b)
{
	out_buffer_lock.lock();
	out_buffer.push(b);
	out_buffer_lock.unlock();
}

int read_pico()
{
	in_buffer_lock.lock();
	if (in_buffer.empty())
	{
		in_buffer_lock.unlock();
		return -1;
	}
	else
	{
		u32 b = in_buffer.front();
		in_buffer.pop();
		in_buffer_lock.unlock();
		return b;
	}
}

static void read_from_dc_socket(pico_socket *pico_sock, sock_t nat_sock)
{
	char buf[1510];

	int r = pico_socket_read(pico_sock, buf, sizeof(buf));
	if (r > 0)
	{
		if (send(nat_sock, buf, r, 0) < r)
		{
			perror("tcp_callback send");
			closesocket(nat_sock);
			pico_socket_close(pico_sock);
			tcp_sockets.erase(pico_sock);
		}
	}
}

static void tcp_callback(uint16_t ev, struct pico_socket *s)
{
	int r = 0;

	if (ev & PICO_SOCK_EV_RD)
	{
		auto it = tcp_sockets.find(s);
		if (it == tcp_sockets.end())
		{
			if (tcp_connecting_sockets.find(s) == tcp_connecting_sockets.end())
				INFO_LOG(MODEM, "Unknown socket: remote port %d", short_be(s->remote_port));
		}
		else
		{
			read_from_dc_socket(it->first, it->second);
		}
	}

	if (ev & PICO_SOCK_EV_CONN)
	{
		uint32_t ka_val = 0;
		struct pico_ip4 orig;
		uint16_t port;
		char peer[30];
		int yes = 1;

		struct pico_socket *sock_a = pico_socket_accept(s, &orig, &port);
		if (sock_a == NULL)
		{
			// Also called for child sockets
			if (tcp_sockets.find(s) == tcp_sockets.end())
				INFO_LOG(MODEM, "pico_socket_accept: %s\n", strerror(pico_err));
		}
		else
		{
			pico_ipv4_to_string(peer, sock_a->local_addr.ip4.addr);
			//printf("Connection established from %s:%d to %08x:%d\n", peer, short_be(port), sock_a->local_addr.ip4.addr, short_be(sock_a->local_port));
			pico_socket_setoption(sock_a, PICO_TCP_NODELAY, &yes);
			/* Set keepalive options */
	//		ka_val = 5;
	//		pico_socket_setoption(sock_a, PICO_SOCKET_OPT_KEEPCNT, &ka_val);
	//		ka_val = 30000;
	//		pico_socket_setoption(sock_a, PICO_SOCKET_OPT_KEEPIDLE, &ka_val);
	//		ka_val = 5000;
	//		pico_socket_setoption(sock_a, PICO_SOCKET_OPT_KEEPINTVL, &ka_val);

			sock_t sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (!VALID(sockfd))
			{
				perror("socket");
			}
			else
			{
				struct sockaddr_in serveraddr;
				memset(&serveraddr, 0, sizeof(serveraddr));
				serveraddr.sin_family = AF_INET;
				serveraddr.sin_addr.s_addr = sock_a->local_addr.ip4.addr;
		        if (serveraddr.sin_addr.s_addr == AFO_ORIG_IP			// Alien Front Online
					|| serveraddr.sin_addr.s_addr == IGP_ORIG_IP)		// Internet Game Pack
				{
		        	serveraddr.sin_addr.s_addr = afo_ip.addr;		// same ip for both for now
				}

				serveraddr.sin_port = sock_a->local_port;
				set_non_blocking(sockfd);
				if (connect(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
				{
					if (get_last_error() != EINPROGRESS && get_last_error() != L_EWOULDBLOCK)
					{
						pico_ipv4_to_string(peer, sock_a->local_addr.ip4.addr);
						INFO_LOG(MODEM, "TCP connection to %s:%d failed: %s", peer, short_be(sock_a->local_port), strerror(get_last_error()));
						closesocket(sockfd);
					}
					else
						tcp_connecting_sockets[sock_a] = sockfd;
				}
				else
				{
					set_tcp_nodelay(sockfd);

					tcp_sockets[sock_a] = sockfd;
				}
			}
		}
	}

	if (ev & PICO_SOCK_EV_FIN) {
		auto it = tcp_sockets.find(s);
		if (it == tcp_sockets.end())
		{
			INFO_LOG(MODEM, "PICO_SOCK_EV_FIN: Unknown socket: remote port %d", short_be(s->remote_port));
		}
		else
		{
			closesocket(it->second);
			tcp_sockets.erase(it);
		}
	}

	if (ev & PICO_SOCK_EV_ERR) {
		INFO_LOG(MODEM, "Socket error received: %s", strerror(pico_err));
		auto it = tcp_sockets.find(s);
		if (it == tcp_sockets.end())
		{
			INFO_LOG(MODEM, "PICO_SOCK_EV_ERR: Unknown socket: remote port %d", short_be(s->remote_port));
		}
		else
		{
			closesocket(it->second);
			tcp_sockets.erase(it);
		}
	}

	if (ev & PICO_SOCK_EV_CLOSE)
	{
		auto it = tcp_sockets.find(s);
		if (it == tcp_sockets.end())
		{
			INFO_LOG(MODEM, "PICO_SOCK_EV_CLOSE: Unknown socket: remote port %d", short_be(s->remote_port));
		}
		else
		{
			shutdown(it->second, SHUT_WR);
			pico_socket_shutdown(s, PICO_SHUT_RD);
		}
	}

//	if (ev & PICO_SOCK_EV_WR)
//	{
//	}
}

static sock_t find_udp_socket(uint16_t src_port)
{
	auto it = udp_sockets.find(src_port);
	if (it != udp_sockets.end())
		return it->second;

	sock_t sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (!VALID(sockfd))
	{
		perror("socket");
		return -1;
	}
#ifndef _WIN32
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
#else
	u_long optl = 1;
	ioctlsocket(sockfd, FIONBIO, &optl);
#endif

	// FIXME Need to clean up at some point?
	udp_sockets[src_port] = sockfd;

	return sockfd;
}

static void udp_callback(uint16_t ev, struct pico_socket *s)
{
	if (ev & PICO_SOCK_EV_RD)
	{
		char buf[1510];
		struct pico_ip4 src_addr;
		uint16_t src_port;
		pico_msginfo msginfo;
		int r = 0;
		while (true)
		{
			r = pico_socket_recvfrom_extended(s, buf, sizeof(buf), &src_addr.addr, &src_port, &msginfo);

			if (r <= 0)
			{
				if (r < 0)
					INFO_LOG(MODEM, "error UDP recv: %s", strerror(pico_err));
				break;
			}

			sock_t sockfd = find_udp_socket(src_port);
			if (VALID(sockfd))
			{
				struct sockaddr_in dst_addr;
				socklen_t addr_len = sizeof(dst_addr);
				memset(&dst_addr, 0, sizeof(dst_addr));
				dst_addr.sin_family = AF_INET;
				dst_addr.sin_addr.s_addr = msginfo.local_addr.ip4.addr;
				dst_addr.sin_port = msginfo.local_port;
				if (sendto(sockfd, buf, r, 0, (const struct sockaddr *)&dst_addr, addr_len) < 0)
					perror("sendto udp socket");
			}
		}
	}

	if (ev & PICO_SOCK_EV_ERR) {
		INFO_LOG(MODEM, "UDP Callback error received");
	}
}

static void read_native_sockets()
{
	int r;
	struct sockaddr_in src_addr;
	socklen_t addr_len;

	// Accept incoming TCP connections
	for (auto it = tcp_listening_sockets.begin(); it != tcp_listening_sockets.end(); it++)
	{
		addr_len = sizeof(src_addr);
		memset(&src_addr, 0, addr_len);
		sock_t sockfd = accept(it->second, (struct sockaddr *)&src_addr, &addr_len);
		if (!VALID(sockfd))
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				perror("accept");
			continue;
		}
    	//printf("Incoming TCP connection from %08x to port %d\n", src_addr.sin_addr.s_addr, short_be(it->first));
    	struct pico_socket *ps = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &tcp_callback);
    	if (ps == NULL)
    	{
    		INFO_LOG(MODEM, "pico_socket_open failed: error %d", pico_err);
    		closesocket(sockfd);
    		continue;
    	}
    	ps->local_addr.ip4.addr = src_addr.sin_addr.s_addr;
    	ps->local_port = src_addr.sin_port;
    	if (pico_socket_connect(ps, &dcaddr.addr, it->first) != 0)
    	{
    		INFO_LOG(MODEM, "pico_socket_connect failed: error %d", pico_err);
    		closesocket(sockfd);
    		pico_socket_close(ps);
    		continue;
    	}
    	set_non_blocking(sockfd);
    	set_tcp_nodelay(sockfd);
    	tcp_sockets[ps] = sockfd;
	}

	// Check connecting outbound TCP sockets
	fd_set write_fds;
	FD_ZERO(&write_fds);
	int max_fd = -1;
	for (auto it = tcp_connecting_sockets.begin(); it != tcp_connecting_sockets.end(); it++)
	{
		FD_SET(it->second, &write_fds);
		max_fd = std::max(max_fd, (int)it->second);
	}
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(max_fd + 1, NULL, &write_fds, NULL, &tv) > 0)
	{
		for (auto it = tcp_connecting_sockets.begin(); it != tcp_connecting_sockets.end(); )
		{
			if (!FD_ISSET(it->second, &write_fds))
			{
				it++;
				continue;
			}
#ifdef _WIN32
			char value;
#else
			int value;
#endif
			socklen_t l = sizeof(int);
			if (getsockopt(it->second, SOL_SOCKET, SO_ERROR, &value, &l) < 0 || value)
			{
				char peer[30];
				pico_ipv4_to_string(peer, it->first->local_addr.ip4.addr);
				INFO_LOG(MODEM, "TCP connection to %s:%d failed: %s", peer, short_be(it->first->local_port), strerror(get_last_error()));
				pico_socket_close(it->first);
				closesocket(it->second);
			}
			else
			{
				set_tcp_nodelay(it->second);
				tcp_sockets[it->first] = it->second;

				read_from_dc_socket(it->first, it->second);
			}
			it = tcp_connecting_sockets.erase(it);
		}
	}

	char buf[1500];		// FIXME MTU ?
	struct pico_msginfo msginfo;

	// If modem buffer is full, wait
	in_buffer_lock.lock();
	size_t in_buffer_size = in_buffer.size();
	in_buffer_lock.unlock();
	if (in_buffer_size >= 256)
		return;

	// Read UDP sockets
	for (auto it = udp_sockets.begin(); it != udp_sockets.end(); it++)
	{
		if (!VALID(it->second))
			continue;

		addr_len = sizeof(src_addr);
		memset(&src_addr, 0, addr_len);
		r = recvfrom(it->second, buf, sizeof(buf), 0, (struct sockaddr *)&src_addr, &addr_len);
		if (r > 0)
		{
			msginfo.dev = ppp;
			msginfo.tos = 0;
			msginfo.ttl = 0;
			msginfo.local_addr.ip4.addr = src_addr.sin_addr.s_addr;
			msginfo.local_port = src_addr.sin_port;
			//printf("read_native_sockets UDP received %d bytes from %08x:%d\n", r, long_be(msginfo.local_addr.ip4.addr), short_be(msginfo.local_port));
			int r2 = pico_socket_sendto_extended(pico_udp_socket, buf, r, &dcaddr, it->first, &msginfo);
			if (r2 < r)
				INFO_LOG(MODEM, "error UDP sending to %d: %s", short_be(it->first), strerror(pico_err));
		}
		else if (r < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
		{
			perror("recvfrom udp socket");
			continue;
		}
	}

	// Read TCP sockets
	for (auto it = tcp_sockets.begin(); it != tcp_sockets.end(); )
	{
		uint32_t space;
		pico_tcp_get_bufspace_out(it->first, &space);
		if (space < sizeof(buf))
		{
			// Wait for the out buffer to empty a bit
			it++;
			continue;
		}
		r = recv(it->second, buf, sizeof(buf), 0);
		if (r > 0)
		{
			if (it->first->remote_port == short_be(5011) && r >= 5)
			{
				// Visual Concepts sport games
				if (buf[0] == 1)
					memcpy(&buf[1], &it->first->local_addr.ip4.addr, 4);
			}

			int r2 = pico_socket_send(it->first, buf, r);
			if (r2 < 0)
				INFO_LOG(MODEM, "error TCP sending: %s", strerror(pico_err));
			else if (r2 < r)
				// FIXME EAGAIN errors. Need to buffer data or wait for call back.
				INFO_LOG(MODEM, "truncated send: %d -> %d", r, r2);
		}
		else if (r == 0)
		{
			pico_socket_shutdown(it->first, PICO_SHUT_WR);
			shutdown(it->second, SHUT_RD);
		}
		else if (r < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
		{
			perror("recv tcp socket");
			closesocket(it->second);
			pico_socket_close(it->first);
			it = tcp_sockets.erase(it);
			continue;
		}
		it++;
	}
}

void close_native_sockets()
{
	for (auto it = udp_sockets.begin(); it != udp_sockets.end(); it++)
		closesocket(it->second);
	udp_sockets.clear();
	for (auto it = tcp_sockets.begin(); it != tcp_sockets.end(); it++)
	{
		pico_socket_close(it->first);
		closesocket(it->second);
	}
	tcp_sockets.clear();
	for (auto it = tcp_connecting_sockets.begin(); it != tcp_connecting_sockets.end(); it++)
	{
		pico_socket_close(it->first);
		closesocket(it->second);
	}
	tcp_connecting_sockets.clear();
}

static int modem_set_speed(struct pico_device *dev, uint32_t speed)
{
    return 0;
}

#if 0 // _WIN32
static void usleep(unsigned int usec)
{
	HANDLE timer;
	LARGE_INTEGER ft;

	ft.QuadPart = -(10 * (__int64)usec);

	timer = CreateWaitableTimer(NULL, TRUE, NULL);
	SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
	WaitForSingleObject(timer, INFINITE);
	CloseHandle(timer);
}
#endif

static void check_dns_entries()
{
    static uint32_t dns_query_start = 0;
    static uint32_t dns_query_attempts = 0;


	if (public_ip.addr == 0)
	{
		if (!dns_query_start)
		{
			dns_query_start = PICO_TIME_MS();
			struct pico_ip4 tmpdns;
			pico_string_to_ipv4(RESOLVER1_OPENDNS_COM, &tmpdns.addr);
			get_host_by_name("myip.opendns.com", tmpdns);
		}
		else
		{
			struct pico_ip4 tmpdns;
			pico_string_to_ipv4(RESOLVER1_OPENDNS_COM, &tmpdns.addr);
			if (get_dns_answer(&public_ip, tmpdns) == 0)
			{
				dns_query_attempts = 0;
				dns_query_start = 0;
				char myip[16];
				pico_ipv4_to_string(myip, public_ip.addr);
				INFO_LOG(MODEM, "My IP is %s", myip);
			}
			else
			{
				if (PICO_TIME_MS() - dns_query_start > 1000)
				{
					if (++dns_query_attempts >= 5)
					{
						public_ip.addr = 0xffffffff;	// Bogus but not null
						dns_query_attempts = 0;
					}
					else
						// Retry
						dns_query_start = 0;
				}
			}
		}
	}
	else if (afo_ip.addr == 0)
	{
		if (!dns_query_start)
		{
			dns_query_start = PICO_TIME_MS();
			get_host_by_name("auriga.segasoft.com", dnsaddr);	// Alien Front Online server
		}
		else
		{
			if (get_dns_answer(&afo_ip, dnsaddr) == 0)
			{
				dns_query_attempts = 0;
				dns_query_start = 0;
				char afoip[16];
				pico_ipv4_to_string(afoip, afo_ip.addr);
				INFO_LOG(MODEM, "AFO server IP is %s", afoip);
			}
			else
			{
				if (PICO_TIME_MS() - dns_query_start > 1000)
				{
					if (++dns_query_attempts >= 5)
					{
						pico_string_to_ipv4("146.185.135.179", &afo_ip.addr);	// Default address
						dns_query_attempts = 0;
					}
					else
						// Retry
						dns_query_start = 0;
				}
			}
		}
	}
}

static bool pico_stack_inited;
static bool pico_thread_running = false;

static void *pico_thread_func(void *)
{
    struct pico_ip4 ipaddr, netmask, zero = {
    	    0
    	};

    if (!pico_stack_inited)
    {
    	pico_stack_init();
    	pico_stack_inited = true;
#if _WIN32
		static WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
			WARN_LOG(MODEM, "WSAStartup failed");
#endif
    }

    // PPP
    ppp = pico_ppp_create();
    if (!ppp)
        return NULL;
    pico_string_to_ipv4("192.168.167.2", &dcaddr.addr);
    pico_ppp_set_peer_ip(ppp, dcaddr);
    pico_string_to_ipv4("192.168.167.1", &ipaddr.addr);
    pico_ppp_set_ip(ppp, ipaddr);

    pico_string_to_ipv4(settings.network.dns.c_str(), &dnsaddr.addr);
    pico_ppp_set_dns1(ppp, dnsaddr);

    pico_udp_socket = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_UDP, &udp_callback);
    if (pico_udp_socket == NULL) {
    	INFO_LOG(MODEM, "error opening UDP socket: %s", strerror(pico_err));
    }
    int yes = 1;
    struct pico_ip4 inaddr_any = {0};
    uint16_t listen_port = 0;
    int ret = pico_socket_bind(pico_udp_socket, &inaddr_any, &listen_port);
    if (ret < 0)
    	INFO_LOG(MODEM, "error binding UDP socket to port %u: %s", short_be(listen_port), strerror(pico_err));

    pico_tcp_socket = pico_socket_open(PICO_PROTO_IPV4, PICO_PROTO_TCP, &tcp_callback);
    if (pico_tcp_socket == NULL) {
    	INFO_LOG(MODEM, "error opening TCP socket: %s", strerror(pico_err));
    }
    pico_socket_setoption(pico_tcp_socket, PICO_TCP_NODELAY, &yes);
    ret = pico_socket_bind(pico_tcp_socket, &inaddr_any, &listen_port);
    if (ret < 0) {
    	INFO_LOG(MODEM, "error binding TCP socket to port %u: %s", short_be(listen_port), strerror(pico_err));
    }
    else
    {
        if (pico_socket_listen(pico_tcp_socket, 10) != 0)
        	INFO_LOG(MODEM, "error listening on port %u", short_be(listen_port));
    }
    ppp->proxied = 1;

	struct sockaddr_in saddr;
	socklen_t saddr_len = sizeof(saddr);
	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
    for (int i = 0; i < sizeof(games_udp_ports) / sizeof(uint16_t); i++)
    {
    	uint16_t port = short_be(games_udp_ports[i]);
		sock_t sockfd = find_udp_socket(port);
		saddr.sin_port = port;

		if (::bind(sockfd, (struct sockaddr *)&saddr, saddr_len) < 0)
		{
			perror("bind");
			closesocket(sockfd);
			auto it = udp_sockets.find(port);
			if (it != udp_sockets.end())
				it->second = INVALID_SOCKET;
			continue;
		}
    }

    for (int i = 0; i < sizeof(games_tcp_ports) / sizeof(uint16_t); i++)
    {
    	uint16_t port = short_be(games_tcp_ports[i]);
    	saddr.sin_port = port;
    	sock_t sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (::bind(sockfd, (struct sockaddr *)&saddr, saddr_len) < 0)
    	{
    		perror("bind");
    		closesocket(sockfd);
    		continue;
    	}
		if (listen(sockfd, 5) < 0)
		{
			perror("listen");
    		closesocket(sockfd);
    		continue;
    	}
		set_non_blocking(sockfd);
		tcp_listening_sockets[port] = sockfd;
    }

    pico_ppp_set_serial_read(ppp, modem_read);
    pico_ppp_set_serial_write(ppp, modem_write);
    pico_ppp_set_serial_set_speed(ppp, modem_set_speed);

    pico_ppp_connect(ppp);

    while (pico_thread_running)
    {
    	read_native_sockets();
    	pico_stack_tick();
    	check_dns_entries();
    	usleep(1000);
    }

    for (auto it = tcp_listening_sockets.begin(); it != tcp_listening_sockets.end(); it++)
    	closesocket(it->second);
	close_native_sockets();
	pico_socket_close(pico_tcp_socket);
	pico_socket_close(pico_udp_socket);

	if (ppp)
	{
		pico_ppp_destroy(ppp);
		ppp = NULL;
	}
	pico_stack_tick();

	return NULL;
}

static cThread pico_thread(pico_thread_func, NULL);

bool start_pico()
{
	pico_thread_running = true;
	pico_thread.Start();

    return true;
}

void stop_pico()
{
	pico_thread_running = false;
	pico_thread.WaitToEnd();
}

#else

#include "types.h"

bool start_pico() { return false; }
void stop_pico() { }
void write_pico(u8 b) { }
int read_pico() { return -1; }

#endif
