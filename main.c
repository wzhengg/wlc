#include "base.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 0 is reserved for null
// 1 is implicitly assigned to wl_display
static u32 id = 2;

#define RINGSIZE (1 << 16) // 65536

static u32 send_tail = 0;
static u32 send_head = 0;
static u8  send_ring[RINGSIZE];

static u32 recv_tail = 0;
static u32 recv_head = 0;
static u8  recv_ring[RINGSIZE];

// TODO: codegen
static void wl_display_sync(u32 new_id)
{
	u32 msg[] = {1, ((u32)12 << 16) | 0, new_id};

	for (u32 i = 0; i < arraycount(msg); ++i) {
		// TODO: Check if head crosses tail.
		u32 write_idx = send_head & (sizeof(send_ring) - 1);
		u32 *p = (u32 *)&send_ring[write_idx];
		*p = msg[i];
		send_head += 4;
	}
}

static void wl_display_get_registry(u32 new_id)
{
	u32 msg[] = { 1, ((u32)12 << 16) | 1, new_id};

	for (u32 i = 0; i < arraycount(msg); ++i) {
		// TODO: Check if head crosses tail.
		u32 write_idx = send_head & (sizeof(send_ring) - 1);
		u32 *p = (u32 *)&send_ring[write_idx];
		*p = msg[i];
		send_head += 4;
	}
}

static void flush_outgoing(int sockfd)
{
	while (1) {
		u32 len = send_head - send_tail;
		assert(len <= sizeof(send_ring));

		u32 read_idx = send_tail & (sizeof(send_ring) - 1);
		u32 write_idx = send_head & (sizeof(send_ring) - 1);

		assert(read_idx%4 == 0 && write_idx%4 == 0);
		if (len < 8) {
			break;
		}

		u32 *word2 = (read_idx+4 < sizeof(send_ring)) ?
			(u32 *)&send_ring[read_idx+4] :
			(u32 *)&send_ring[0];
		u32 msg_len = *word2 >> 16;

		assert(msg_len%4 == 0);
		if (len < msg_len) {
			break;
		}

		u32 first_len = min(msg_len, sizeof(recv_ring) - read_idx);
		struct iovec iovs[] = {
			{.iov_base = &send_ring[read_idx], .iov_len = first_len},
			{.iov_base = &send_ring[0],        .iov_len = msg_len - first_len}
		};
		struct msghdr req = {.msg_iov = iovs, .msg_iovlen = 2};

		if (sendmsg(sockfd, &req, 0) == -1) {
			perror("sendmsg");
			exit(1);
		}

		send_tail += msg_len;
	}
}

int main(void)
{
	char *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
	char *wayland_display = getenv("WAYLAND_DISPLAY");
	if (!xdg_runtime_dir || !wayland_display) {
		exit(1);
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;

	if (strlen(xdg_runtime_dir) + 1 + strlen(wayland_display) + 1 > sizeof(addr.sun_path)) {
		exit(1);
	}

	memcpy(addr.sun_path, xdg_runtime_dir, strlen(xdg_runtime_dir));
	addr.sun_path[strlen(xdg_runtime_dir)] = '/';
	memcpy(addr.sun_path + strlen(xdg_runtime_dir) + 1, wayland_display, strlen(wayland_display));

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		exit(1);
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("connect");
		exit(1);
	}

	wl_display_get_registry(id++);
	wl_display_sync(id++);
	flush_outgoing(fd);

	// TODO: No more sleeping
	sleep(1);
	printf("done sleeping\n");

	for (int x = 0; x < 1; ++x) {
		// TODO: Check math (wrap-around).

		u32 used = recv_head - recv_tail;
		u32 left = sizeof(recv_ring) - used;
		assert(used <= sizeof(recv_ring));

		u32 read_idx = recv_tail & (sizeof(recv_ring) - 1);
		u32 write_idx = recv_head & (sizeof(recv_ring) - 1);

		u32 first_len = min(left, sizeof(recv_ring) - recv_head);
		struct iovec iovs[] = {
			{.iov_base = &recv_ring[write_idx], .iov_len = first_len},
			{.iov_base = &recv_ring[0], .iov_len = left - first_len}
		};
		struct msghdr recv_msg = {.msg_iov = iovs, .msg_iovlen = 2};

		ssize_t bytes_read = recvmsg(fd, &recv_msg, 0);
		if (bytes_read < 0) {
			perror("recvmsg peek");
			exit(1);
		}
		recv_head += bytes_read;
		if (recv_head - recv_tail < 8) {
			continue;
		}

		// TODO: Copy message to tmp buf if it wraps the ring.

		u16 msg_size = ( *(u32 *)(&recv_ring[read_idx+4]) ) >> 16;
		assert(msg_size%4 == 0 && msg_size >= 8);
		if (recv_head - recv_tail < msg_size) {
			continue;
		}

		do {
			for (u32 end = recv_tail + msg_size; recv_tail < end; recv_tail += 4) {
				printf("0x%08X ", *(u32 *)&recv_ring[recv_tail]);
			}
			printf("\n");
			if (recv_head - recv_tail < 8) {
				break;
			}
			msg_size = ( *(u32 *)(&recv_ring[recv_tail+4]) ) >> 16;
		} while (recv_head - recv_tail >= msg_size);
	}

	if (close(fd) == -1) {
	  perror("close");
	  exit(1);
	}

	return 0;
}
