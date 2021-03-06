#include "connection.h"
#include "coroutine.h"
#include "loop.h"

static long wm_coroutine_socket_last_id = 0;
static swHashMap *wm_connections = NULL; //记录着正在连接状态的conn

//检查是否发送缓存区慢
void checkBufferWillFull(wmConnection *connection);
bool bufferIsFull(wmConnection *connection, size_t len);
int _close(wmConnection *connection);

wmConnection * wmConnection_create(int fd) {
	if (!wm_connections) {
		wm_connections = swHashMap_new(NULL);
	}
	swHashMap_del_int(wm_connections, fd);
	wmConnection *connection = (wmConnection *) wm_malloc(sizeof(wmConnection));
	connection->fd = fd;
	connection->read_buffer = NULL;
	connection->write_buffer = NULL;
	connection->_This = NULL;
	connection->id = ++wm_coroutine_socket_last_id;
	connection->events = WM_EVENT_NULL;
	connection->_status = WM_CONNECTION_STATUS_ESTABLISHED;

	connection->maxSendBufferSize = WM_MAX_SEND_BUFFER_SIZE;
	connection->maxPackageSize = WM_MAX_PACKAGE_SIZE;

	connection->onMessage = NULL;
	connection->onClose = NULL;
	connection->onBufferFull = NULL;
	connection->onBufferDrain = NULL;
	connection->onError = NULL;
	if (connection->fd < 0) {
		wm_coroutine_socket_last_id = 0;
		connection->id = ++wm_coroutine_socket_last_id;
	}

	wmSocket_set_nonblock(connection->fd);

	//添加到map中
	swHashMap_add_int(wm_connections, connection->fd, connection);
	return connection;
}

wmConnection* wmConnection_find_by_fd(int fd) {
	wmConnection* connection = (wmConnection*) swHashMap_find_int(wm_connections, fd);
	return connection;
}

/**
 * onMessage协程调用结束一次，就触发一次
 * 这里得研究一下，引用计数这块不太懂
 */
void onMessage_callback(void* _mess_data) {
	zval* md = (zval*) _mess_data;
	zval* md2 = (zval*) ((char *) _mess_data + sizeof(zval));
	////php_printf("aaa %d \n",md->value.counted->gc.refcount);
	zval_ptr_dtor(md2);
	efree(md);
}

void onError_callback(void* _mess_data) {
	zval* md = (zval*) _mess_data;
	zval* md2 = (zval*) ((char *) _mess_data + (sizeof(zval) * 2));
	zval_ptr_dtor(md2);
	efree(md);
}

//已经可以读消息了
void _wmConnection_read_callback(int fd) {
	wmConnection* connection = wmConnection_find_by_fd(fd);
	if (connection == NULL) {
		wmWarn("Error has occurred: _wmConnection_read_callback fd=%d wmConnection is NULL", fd);
		return;
	}

	if (!connection->read_buffer) {
		connection->read_buffer = wmString_new(WM_BUFFER_SIZE_BIG);
	}
	int ret = wmSocket_recv(connection->fd, connection->read_buffer->str + connection->read_buffer->length,
	WM_BUFFER_SIZE_BIG, 0);

	//连接关闭
	if (ret == 0) {
		zend_update_property_long(workerman_connection_ce_ptr, connection->_This, ZEND_STRL("errCode"), WM_ERROR_SESSION_CLOSED_BY_CLIENT);

		zend_update_property_string(workerman_connection_ce_ptr, connection->_This, ZEND_STRL("errMsg"), wmCode_str(WM_ERROR_SESSION_CLOSED_BY_CLIENT));
		_close(connection);
		return;
	}

	if (ret < 0) {
		wmWarn("Error has occurred: (fd=%d,errno %d) %s", connection->fd, errno, strerror(errno));
		php_error_docref(NULL, E_WARNING, "recv error");
		return;
	}

	connection->read_buffer->length += ret;

	if (connection->read_buffer->length == 0) {
		return;
	}

	//创建一个单独协程处理
	if (connection->onMessage) {
		//构建zval，默认的引用计数是1，在php方法调用完毕释放
		zval* _mess_data = (zval*) emalloc(sizeof(zval) * 2);
		_mess_data[0] = *connection->_This;
		zend_string* _zs = zend_string_init(connection->read_buffer->str, connection->read_buffer->length, 0);
		ZVAL_STR(&_mess_data[1], _zs);
		//现在就可以清空read）bufferle
		connection->read_buffer->length = 0;
		long _cid = wmCoroutine_create(&(connection->onMessage->fcc), 2, _mess_data); //创建新协程
		wmCoroutine_set_callback(_cid, onMessage_callback, _mess_data);
	}
}

/**
 * 发送数据
 */
bool wmConnection_send(wmConnection *connection, const void *buf, size_t len) {
	if (connection->_status == WM_CONNECTION_STATUS_CLOSED || connection->_status == WM_CONNECTION_STATUS_CLOSING) {
		return false;
	}

	if (!connection->write_buffer) {
		connection->write_buffer = wmString_new(WM_BUFFER_SIZE_BIG);
	}
	int ret;

	if (bufferIsFull(connection, len)) {
		return false;
	}

	wmString_append_ptr(connection->write_buffer, buf, len);
	//检查是否缓冲区满
	checkBufferWillFull(connection);
	bool _add_Loop = false;
	while (1) {
		ret = wmSocket_send(connection->fd, connection->write_buffer->str + connection->write_buffer->offset,
			connection->write_buffer->length - connection->write_buffer->offset, 0);
		//如果发生错误
		if (ret < 0) {
			if (errno != EAGAIN) { // 不是缓冲区错误，就返回报错
				return false;
			}
			ret = 0;
		}
		connection->write_buffer->offset += ret;

		//发送完了，就直接返回
		if (connection->write_buffer->offset == connection->write_buffer->length) {

			//在这里取消事件注册，使用修改的方式
			if (_add_Loop) {
				connection->events = connection->events - WM_EVENT_WRITE;
				wmWorkerLoop_update(connection->fd, connection->events);
			}
			connection->write_buffer->offset = 0;
			connection->write_buffer->length = 0;
			return true;
		}
		if (!_add_Loop) {
			connection->events |= WM_EVENT_WRITE;
			if (connection->events & WM_EVENT_READ) {
				wmWorkerLoop_update(connection->fd, connection->events);
			} else {
				wmWorkerLoop_add(connection->fd, connection->events);
			}
			_add_Loop = true;
		}

		wmCoroutine_yield();
	}
	return true;
}

void _wmConnection_write_callback(int fd, int coro_id) {
	wmCoroutine* co = wmCoroutine_get_by_cid(coro_id);
	if (co == NULL) {
		wmWarn("Error has occurred: _wmConnection_write_callback wmCoroutine is NULL");
		return;
	}
	wmCoroutine_resume(co);
}

//检查应用层发送缓冲区是否这次添加之后，已经满了
void checkBufferWillFull(wmConnection *connection) {
	if (connection->maxSendBufferSize <= (connection->write_buffer->length - connection->write_buffer->offset)) {
		if (connection->onBufferFull) {
			wmCoroutine_create(&(connection->onBufferFull->fcc), 1, connection->_This); //创建新协程
		}
	}
}

//检查是否已经满了,并回调
bool bufferIsFull(wmConnection *connection, size_t len) {
	//如果是空的，那就不检查
	if ((connection->write_buffer->length - connection->write_buffer->offset) == 0) {
		return false;
	}

	if (connection->maxSendBufferSize <= (connection->write_buffer->length - connection->write_buffer->offset + len)) {
		if (connection->onError) {
			//构建zval，默认的引用计数是1，在php方法调用完毕释放
			zval* _mess_data = (zval*) emalloc(sizeof(zval) * 3);
			_mess_data[0] = *connection->_This;
			ZVAL_LONG(&_mess_data[1], WM_ERROR_SEND_FAIL);
			zend_string* _zs = zend_string_init(wmCode_str(WM_ERROR_SEND_FAIL), strlen(wmCode_str(WM_ERROR_SEND_FAIL)), 0);
			ZVAL_STR(&_mess_data[2], _zs);
			long _cid = wmCoroutine_create(&(connection->onError->fcc), 3, _mess_data); //创建新协程
			wmCoroutine_set_callback(_cid, onError_callback, _mess_data);
		}
		return true;
	}
	return false;
}

//这是一个用户调用的方法
int wmConnection_close(wmConnection *connection) {
	return _close(connection);
}

/**
 * 关闭所有的连接
 */
void wmConnection_close_connections() {
	swHashMap_rewind(wm_connections);
	uint64_t key;
	//循环_workers
	wmConnection* need_close_conn = NULL;
	while (1) {
		wmConnection* conn = (wmConnection *) swHashMap_each_int(wm_connections, &key);
		if (need_close_conn != NULL) {
			_close(need_close_conn);
			need_close_conn = NULL;
		}
		if (conn == NULL) {
			break;
		}
		need_close_conn = conn;
	}
}

/**
 * 关闭这个连接
 */
int _close(wmConnection *connection) {
	if (connection->_status == WM_CONNECTION_STATUS_CLOSED) {
		return 0;
	}

	wmWorkerLoop_del(connection->fd); //释放事件
	int ret = wmSocket_close(connection->fd);

	connection->_status = WM_CONNECTION_STATUS_CLOSED;
	//触发onClose
	if (connection->onClose) {
		wmCoroutine_create(&(connection->onClose->fcc), 1, connection->_This); //创建新协程
	}

	swHashMap_del_int(wm_connections, connection->fd); //从hash表删除

	//释放connection,摧毁这个类，如果顺利的话会触发wmConnection_free
	zval_ptr_dtor(connection->_This);

	return ret;
}

//释放obj和connection的内存
void wmConnection_free(wmConnection *connection) {
	if (!connection) {
		return;
	}
	//如果还在连接，那么调用close
	if (connection->_status == WM_CONNECTION_STATUS_CONNECTING) {
		_close(connection);
	}

	wmString_free(connection->read_buffer);
	wmString_free(connection->write_buffer);

	//释放暂时申请指向自身php对象的zval指针
	if (connection->_This) {
		efree(connection->_This);
	}
	wm_free(connection);	//释放connection
	connection = NULL;
}

