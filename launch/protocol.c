#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t
send_fd(int socket, int fd, struct iovec *iov, int iovlen)
{
	char control[CMSG_SPACE(sizeof(fd))];
	struct msghdr message = {
	    .msg_name = NULL,
	    .msg_namelen = 0,
	    .msg_iov = iov,
	    .msg_iovlen = iovlen,
	};
	struct cmsghdr *cmsg;

	if (fd != -1) {
		message.msg_control = control, message.msg_controllen = sizeof(control);

		cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;

		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	} else {
		message.msg_control = NULL;
		message.msg_controllen = 0;
	}

	return sendmsg(socket, &message, 0);
}

ssize_t
receive_fd(int socket, int *fd, struct iovec *iov, int iovlen)
{
	ssize_t size;
	char control[CMSG_SPACE(sizeof(*fd))];
	struct msghdr message = {
	    .msg_name = NULL,
	    .msg_namelen = 0,
	    .msg_iov = iov,
	    .msg_iovlen = iovlen,
	};
	struct cmsghdr *cmsg;

	if (fd) {
		*fd = -1;
		message.msg_control = &control;
		message.msg_controllen = sizeof(control);
	}

	size = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	if (size < 0) {
		return -1;
	}

	/*
	 * Exactly one descriptor is expected. A sender can attach more -- two
	 * fit in the same control space one does -- and the kernel installs
	 * every one of them here before we look. Anything not claimed has to be
	 * closed, or a peer can fill this process's descriptor table.
	 */
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
	     cmsg = CMSG_NXTHDR(&message, cmsg)) {
		size_t payload;
		unsigned i, count;
		int received;

		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			continue;
		}
		payload = cmsg->cmsg_len - CMSG_LEN(0);
		count = payload / sizeof(int);
		for (i = 0; i < count; ++i) {
			memcpy(&received, CMSG_DATA(cmsg) + i * sizeof(int),
			       sizeof(received));
			if (fd && *fd == -1 && count == 1) {
				*fd = received;
			} else {
				close(received);
			}
		}
	}

	/* A truncated control message means fds may have been dropped by the
	 * kernel rather than delivered; treat the message as unusable. */
	if (message.msg_flags & MSG_CTRUNC) {
		if (fd && *fd != -1) {
			close(*fd);
			*fd = -1;
		}
		errno = EMSGSIZE;
		return -1;
	}

	return size;
}
