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

	u32 get_registry_bytes[] = { 1, ((u32)12 << 16) | 1, id++ };
	struct iovec get_registry_iov = {.iov_base = get_registry_bytes, .iov_len = sizeof(get_registry_bytes)};
	struct msghdr get_registry_msg = {.msg_iov = &get_registry_iov, .msg_iovlen = 1};
	if (sendmsg(fd, &get_registry_msg, 0) == -1) {
	  perror("get_registry sendmsg");
	  exit(1);
	}

	u32 sync_callback_id = id++;
	u32 sync_bytes[] = {1, ((u32)12 << 16) | 0, sync_callback_id++};
	struct iovec sync_iov = {.iov_base = sync_bytes, .iov_len = sizeof(sync_bytes)};
	struct msghdr sync_msg = {.msg_iov = &sync_iov, .msg_iovlen = 1};
	if (sendmsg(fd, &sync_msg, 0) == -1) {
	  perror("sync sendmsg");
	  exit(1);
	}

	sleep(1);
	printf("done sleeping\n");

	// TODO: Turn this into a ring buffer.
	u32 read_index = 0;
	u32 write_index = 0;
	u8 recv_ring[1<<16] = {0};

	// TODO: Create an outgoing ring buffer.

	for (int x = 0; x < 1; ++x) {
		assert(write_index < sizeof(recv_ring));
		struct iovec recv_iov = {.iov_base = &recv_ring[write_index], .iov_len = sizeof(recv_ring) - write_index};
		struct msghdr recv_msg = {.msg_iov = &recv_iov, .msg_iovlen = 1};

		ssize_t bytes_read = recvmsg(fd, &recv_msg, 0);
		if (bytes_read < 0) {
			perror("recvmsg peek");
			exit(1);
		}
		write_index += bytes_read;
		if (write_index - read_index < 8) {
			continue;
		}

		u16 msg_size = ( *(u32 *)(&recv_ring[read_index+4]) ) >> 16;
		assert(msg_size%4 == 0 && msg_size >= 8);
		if (write_index - read_index < msg_size) {
			continue;
		}

		do {
			for (u32 end = read_index + msg_size; read_index < end; read_index += 4) {
				printf("0x%08X ", *(u32 *)&recv_ring[read_index]);
			}
			printf("\n");
			if (write_index - read_index < 8) {
				break;
			}
			msg_size = ( *(u32 *)(&recv_ring[read_index+4]) ) >> 16;
		} while (write_index - read_index >= msg_size);
	}

	if (close(fd) == -1) {
	  perror("close");
	  exit(1);
	}

	return 0;
}
