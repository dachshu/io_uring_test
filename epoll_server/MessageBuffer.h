#pragma once
#include <vector>
#include <iostream>
#include <mutex>
#include "Client.h"


void echo(const char* buf, ExtendedBuf* sendBuf, unsigned bytes) {
	char* dest = sendBuf->buf_addr;

	for (int i = 0; i < bytes; ++i) {
		dest[i] = buf[i];
	}
	dest[bytes] = 0;
}

class GlobalRecvBufferPool {
    std::vector<ExtendedBuf*> emptyRecvBuf;

	std::mutex recvBufLock;

public:
	GlobalRecvBufferPool() {

	}
	void init() {
		for(int i = 0; i < MAX_CLIENT; ++i){
            ExtendedBuf* buf = new ExtendedBuf;
            buf->buf_addr = new char[MAX_BUFFER];
            buf->event_type = EV_RECV;
            emptyRecvBuf.push_back(buf);
        }
	}

	ExtendedBuf* get_recvBuffer() {
		std::lock_guard<std::mutex> lg(recvBufLock);
		if (emptyRecvBuf.empty())
			return nullptr;
		ExtendedBuf* ret = emptyRecvBuf.back();
		emptyRecvBuf.pop_back();
		return ret;
	}
	void return_recvBuffer(ExtendedBuf* buf) {
		std::lock_guard<std::mutex> lg(recvBufLock);
		emptyRecvBuf.push_back(buf);
	}


};


class LocalSendBufferPool {
	std::vector<ExtendedBuf*> sendBuffers;
public:
	LocalSendBufferPool() {
	}
	void init() {
        sendBuffers.reserve(int(LOCAL_SEND_BUF_CNT * 1.5));

        for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
            ExtendedBuf* buf = new ExtendedBuf;
            buf->buf_addr = new char[MAX_BUFFER];
            buf->event_type = EV_SEND;
            sendBuffers.push_back(buf);
        }
	}


	ExtendedBuf* get_sendBuffer() {
		if (sendBuffers.empty()) {
			std::cout << "no more send buf" << std::endl;
			ExtendedBuf* buf = new ExtendedBuf;
            buf->buf_addr = new char[MAX_BUFFER];
            buf->event_type = EV_SEND;
            return buf;
		}

		ExtendedBuf* ret = sendBuffers.back();
		sendBuffers.pop_back();
		return ret;
	}
	void return_sendBuffer(ExtendedBuf* buf) {
		sendBuffers.push_back(buf);
	}

};