#include "socket.h"

/**
 * 创建套接字
 */
int wmSocket_create(int type) {
	int _domain;
	int _type;
	//判断类型
	if (type == WM_SOCK_TCP) {
		_domain = AF_INET;
		_type = SOCK_STREAM;
	} else if (type == WM_SOCK_UDP) {
		_domain = AF_INET;
		_type = SOCK_DGRAM;
	} else {
		return -1;
	}
	return socket(_domain, _type, 0);
}

/**
 * 对bind()函数进行了封装
 */
int wmSocket_bind(int sock, int type, char *host, int port) {
	int ret;
	struct sockaddr_in servaddr;

	//如果是TCP
	if (type == WM_SOCK_TCP) {
		//初始化servaddr
		bzero(&servaddr, sizeof(servaddr));
		//将host转换为网络结构体sockaddr_in
		inet_aton(host, &(servaddr.sin_addr));
		servaddr.sin_family = AF_INET;
		servaddr.sin_port = htons(port);
		//把socket和地址，端口绑定
		ret = bind(sock, (struct sockaddr *) &servaddr, sizeof(servaddr));
		if (ret < 0) {
			return -1;
		}
	} else {
		return -1;
	}

	return ret;
}

int wmSocket_listen(int sock) {
	int ret;

	ret = listen(sock, 512);
	if (ret < 0) {
		wmWarn("Error has occurred: (errno %d) %s", errno, strerror(errno));
	}
	return ret;
}

int wmSocket_accept(int sock) {
	int connfd;
	struct sockaddr_in sa;
	socklen_t len;

	len = sizeof(sa);
	connfd = accept(sock, (struct sockaddr *) &sa, &len);
	return connfd;
}

ssize_t wmSocket_recv(int sock, void *buf, size_t len, int flag) {
	ssize_t ret;

	/**
	 * 成功返回发送的字节数；失败返回-1，同时errno被设置
	 *
	 * flags 一般设置为0，此时send为阻塞式发送
	 * 即发送不成功会一直阻塞，直到被某个信号终端终止，或者直到发送成功为止。
	 * 指定MSG_NOSIGNAL，表示当连接被关闭时不会产生SIGPIPE信号
	 * 指定MSG_DONTWAIT 表示非阻塞发送
	 * 指定MSG_OOB 表示带外数据
	 */
	ret = recv(sock, buf, len, flag);
	if (ret < 0) {
		wmWarn("Error has occurred: (errno %d) %s", errno, strerror(errno));
	}
	return ret;
}

ssize_t wmSocket_send(int sock, void *buf, size_t len, int flag) {
	ssize_t ret;

	ret = send(sock, buf, len, flag);
	if (ret < 0) {
		wmWarn("Error has occurred: (errno %d) %s", errno, strerror(errno));
	}
	return ret;
}
