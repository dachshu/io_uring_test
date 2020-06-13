#pragma once
#include <iostream>
#include <atomic>
#include <liburing.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/poll.h>

#define BACKLOG 512
#define MAX_CLIENT			10000
#define MAX_BUFFER			128
#define NUM_WORKER_THREADS	4
#define LOCAL_SEND_BUF_CNT	int((MAX_CLIENT * 128))
#define INIT_NUM_ZONE_NODE		16

#define RECV_RQ_SIZE		50
#define SEND_RQ_SIZE		400
#define MAX_RIO_REUSLTS		int( MAX_CLIENT / NUM_WORKER_THREADS)
#define ZONE_SIZE			20
constexpr auto VIEW_RANGE = 7;

thread_local int tid;
//std::atomic_uint mem_exceed_cnt{ 0 };
//thread_local int per_mem_exceed_cnt{ 0 };
