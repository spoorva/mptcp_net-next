// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <linux/err.h>
#include <linux/in.h>
#include <linux/in6.h>

#include "bpf_util.h"
#include "network_helpers.h"

#ifndef IPPROTO_MPTCP
#define IPPROTO_MPTCP 262
#endif

#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define log_err(MSG, ...) ({						\
			int __save = errno;				\
			fprintf(stderr, "(%s:%d: errno: %s) " MSG "\n", \
				__FILE__, __LINE__, clean_errno(),	\
				##__VA_ARGS__);				\
			errno = __save;					\
})

struct ipv4_packet pkt_v4 = {
	.eth.h_proto = __bpf_constant_htons(ETH_P_IP),
	.iph.ihl = 5,
	.iph.protocol = IPPROTO_TCP,
	.iph.tot_len = __bpf_constant_htons(MAGIC_BYTES),
	.tcp.urg_ptr = 123,
	.tcp.doff = 5,
};

struct ipv6_packet pkt_v6 = {
	.eth.h_proto = __bpf_constant_htons(ETH_P_IPV6),
	.iph.nexthdr = IPPROTO_TCP,
	.iph.payload_len = __bpf_constant_htons(MAGIC_BYTES),
	.tcp.urg_ptr = 123,
	.tcp.doff = 5,
};

int settimeo(int fd, int timeout_ms)
{
	struct timeval timeout = { .tv_sec = 3 };

	if (timeout_ms > 0) {
		timeout.tv_sec = timeout_ms / 1000;
		timeout.tv_usec = (timeout_ms % 1000) * 1000;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		       sizeof(timeout))) {
		log_err("Failed to set SO_RCVTIMEO");
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
		       sizeof(timeout))) {
		log_err("Failed to set SO_SNDTIMEO");
		return -1;
	}

	return 0;
}

#define save_errno_close(fd) ({ int __save = errno; close(fd); errno = __save; })

static int __start_server(int type, int protocol, const struct sockaddr *addr,
			  socklen_t addrlen, int timeout_ms, bool reuseport)
{
	int on = 1;
	int fd;

	fd = socket(addr->sa_family, type, protocol);
	if (fd < 0) {
		log_err("Failed to create server socket");
		return -1;
	}

	if (settimeo(fd, timeout_ms))
		goto error_close;

	if (reuseport &&
	    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on))) {
		log_err("Failed to set SO_REUSEPORT");
		return -1;
	}

	if (bind(fd, addr, addrlen) < 0) {
		log_err("Failed to bind socket");
		goto error_close;
	}

	if (type == SOCK_STREAM) {
		if (listen(fd, 1) < 0) {
			log_err("Failed to listed on socket");
			goto error_close;
		}
	}

	return fd;

error_close:
	save_errno_close(fd);
	return -1;
}

static int start_server_proto(int family, int type, int protocol,
			      const char *addr_str, __u16 port, int timeout_ms)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	if (make_sockaddr(family, addr_str, port, &addr, &addrlen))
		return -1;

	return __start_server(type, protocol, (struct sockaddr *)&addr,
			      addrlen, timeout_ms, false);
}

int start_server(int family, int type, const char *addr_str, __u16 port,
		 int timeout_ms)
{
	return start_server_proto(family, type, 0, addr_str, port, timeout_ms);
}

int start_mptcp_server(int family, const char *addr_str, __u16 port,
		       int timeout_ms)
{
	return start_server_proto(family, SOCK_STREAM, IPPROTO_MPTCP, addr_str,
				  port, timeout_ms);
}

int *start_reuseport_server(int family, int type, const char *addr_str,
			    __u16 port, int timeout_ms, unsigned int nr_listens)
{
	struct sockaddr_storage addr;
	unsigned int nr_fds = 0;
	socklen_t addrlen;
	int *fds;

	if (!nr_listens)
		return NULL;

	if (make_sockaddr(family, addr_str, port, &addr, &addrlen))
		return NULL;

	fds = malloc(sizeof(*fds) * nr_listens);
	if (!fds)
		return NULL;

	fds[0] = __start_server(type, 0, (struct sockaddr *)&addr, addrlen,
				timeout_ms, true);
	if (fds[0] == -1)
		goto close_fds;
	nr_fds = 1;

	if (getsockname(fds[0], (struct sockaddr *)&addr, &addrlen))
		goto close_fds;

	for (; nr_fds < nr_listens; nr_fds++) {
		fds[nr_fds] = __start_server(type, 0, (struct sockaddr *)&addr,
					     addrlen, timeout_ms, true);
		if (fds[nr_fds] == -1)
			goto close_fds;
	}

	return fds;

close_fds:
	free_fds(fds, nr_fds);
	return NULL;
}

void free_fds(int *fds, unsigned int nr_close_fds)
{
	if (fds) {
		while (nr_close_fds)
			close(fds[--nr_close_fds]);
		free(fds);
	}
}

int fastopen_connect(int server_fd, const char *data, unsigned int data_len,
		     int timeout_ms)
{
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	struct sockaddr_in *addr_in;
	int fd, ret;

	if (getsockname(server_fd, (struct sockaddr *)&addr, &addrlen)) {
		log_err("Failed to get server addr");
		return -1;
	}

	addr_in = (struct sockaddr_in *)&addr;
	fd = socket(addr_in->sin_family, SOCK_STREAM, 0);
	if (fd < 0) {
		log_err("Failed to create client socket");
		return -1;
	}

	if (settimeo(fd, timeout_ms))
		goto error_close;

	ret = sendto(fd, data, data_len, MSG_FASTOPEN, (struct sockaddr *)&addr,
		     addrlen);
	if (ret != data_len) {
		log_err("sendto(data, %u) != %d\n", data_len, ret);
		goto error_close;
	}

	return fd;

error_close:
	save_errno_close(fd);
	return -1;
}

static int connect_fd_to_addr(int fd,
			      const struct sockaddr_storage *addr,
			      socklen_t addrlen)
{
	if (connect(fd, (const struct sockaddr *)addr, addrlen)) {
		log_err("Failed to connect to server");
		return -1;
	}

	return 0;
}

static int connect_to_fd_proto(int server_fd, int protocol, int timeout_ms)
{
	struct sockaddr_storage addr;
	struct sockaddr_in *addr_in;
	socklen_t addrlen, optlen;
	int fd, type;

	optlen = sizeof(type);
	if (getsockopt(server_fd, SOL_SOCKET, SO_TYPE, &type, &optlen)) {
		log_err("getsockopt(SOL_TYPE)");
		return -1;
	}

	addrlen = sizeof(addr);
	if (getsockname(server_fd, (struct sockaddr *)&addr, &addrlen)) {
		log_err("Failed to get server addr");
		return -1;
	}

	addr_in = (struct sockaddr_in *)&addr;
	fd = socket(addr_in->sin_family, type, protocol);
	if (fd < 0) {
		log_err("Failed to create client socket");
		return -1;
	}

	if (settimeo(fd, timeout_ms))
		goto error_close;

	if (connect_fd_to_addr(fd, &addr, addrlen))
		goto error_close;

	return fd;

error_close:
	save_errno_close(fd);
	return -1;
}

int connect_to_fd(int server_fd, int timeout_ms)
{
	return connect_to_fd_proto(server_fd, 0, timeout_ms);
}

int connect_to_mptcp_fd(int server_fd, int timeout_ms)
{
	return connect_to_fd_proto(server_fd, IPPROTO_MPTCP, timeout_ms);
}

int connect_fd_to_fd(int client_fd, int server_fd, int timeout_ms)
{
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);

	if (settimeo(client_fd, timeout_ms))
		return -1;

	if (getsockname(server_fd, (struct sockaddr *)&addr, &len)) {
		log_err("Failed to get server addr");
		return -1;
	}

	if (connect_fd_to_addr(client_fd, &addr, len))
		return -1;

	return 0;
}

int make_sockaddr(int family, const char *addr_str, __u16 port,
		  struct sockaddr_storage *addr, socklen_t *len)
{
	if (family == AF_INET) {
		struct sockaddr_in *sin = (void *)addr;

		memset(addr, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		if (addr_str &&
		    inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
			log_err("inet_pton(AF_INET, %s)", addr_str);
			return -1;
		}
		if (len)
			*len = sizeof(*sin);
		return 0;
	} else if (family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (void *)addr;

		memset(addr, 0, sizeof(*sin6));
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(port);
		if (addr_str &&
		    inet_pton(AF_INET6, addr_str, &sin6->sin6_addr) != 1) {
			log_err("inet_pton(AF_INET6, %s)", addr_str);
			return -1;
		}
		if (len)
			*len = sizeof(*sin6);
		return 0;
	}
	return -1;
}
