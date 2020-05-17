#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iterator>
#include "common.h"
#include "protocol.h"
#include "MessageQueue.h"
#include "Zone.h"
#include "MessageBuffer.h"

using namespace std;
using namespace chrono;

#define IORING_FEAT_FAST_POLL (1U << 5)
#define MAX_ENTRIES 4096

//atomic_int num_client{ 0 };

GlobalRecvBufferPool globalBufferPool;
LocalSendBufferPool sendBufferPool[NUM_WORKER_THREADS];
io_uring_params params[NUM_WORKER_THREADS];
io_uring rings[NUM_WORKER_THREADS];

const unsigned int zone_width = int(WORLD_WIDTH / ZONE_SIZE);
const unsigned int zone_heigt = int(WORLD_HEIGHT / ZONE_SIZE);
Zone zone[zone_heigt][zone_width];

const unsigned int per_max_clients = int(MAX_CLIENT / 2);
//thread_local int new_user_id = 0;
//thread_local unordered_map<int, SOCKETINFO*> my_clients;
thread_local SOCKETINFO* my_clients[per_max_clients];
thread_local int empty_cli_idx[per_max_clients];
thread_local int num_my_clients = 0;
thread_local int rem_sqe_cnt = MAX_ENTRIES;

int unsigned long my_next = 1;

int my_rand(){
    my_next = my_next * 1103515245 + 12345;
    return (unsigned long)(my_next/65536) % 32768;
}

void Disconnect(int id);

void error_display(const char* msg)
{
	cout << msg << endl;
}


bool is_near(int x1, int y1, int x2, int y2)
{
	if (VIEW_RANGE < abs(x1 - x2)) return false;
	if (VIEW_RANGE < abs(y1 - y2)) return false;
	return true;
}

void send_packet(unsigned sock_fd, ExtendedBuf* send_buf, int cid, int len)
{
	if (rem_sqe_cnt <= 0) {
		std::cout << "send ignored" << std::endl;
		sendBufferPool[tid].return_sendBuffer(send_buf);
		Disconnect(cid);
		return;
	}
	
	io_uring_sqe* sqe = io_uring_get_sqe(&rings[tid]);
	io_uring_prep_send(sqe, sock_fd, send_buf->buf_addr, len, 0);

	send_buf->cid = cid;
	io_uring_sqe_set_data(sqe, send_buf);
	
	--(rem_sqe_cnt);
}

void send_login_ok_packet(unsigned sock_fd, int gid, int x, int y, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_login_ok* packet = reinterpret_cast<sc_packet_login_ok*>(sendBuf->buf_addr);

	packet->id = gid;
	packet->size = sizeof(sc_packet_login_ok);
	packet->type = SC_LOGIN_OK;
	packet->x = x;
	packet->y = y;
	packet->hp = 100;
	packet->level = 1;
	packet->exp = 1;
	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_login_ok));
}


void send_login_fail(unsigned sock_fd, int gid, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_login_fail* packet = reinterpret_cast<sc_packet_login_fail*>(sendBuf->buf_addr);

	packet->size = sizeof(sc_packet_login_fail);
	packet->type = SC_LOGIN_FAIL;

	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_login_fail));
}

void send_put_object_packet(unsigned sock_fd, int new_gid, int nx, int ny, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_put_object* packet = reinterpret_cast<sc_packet_put_object*>(sendBuf->buf_addr);

	packet->id = new_gid;
	packet->size = sizeof(sc_packet_put_object);
	packet->type = SC_PUT_OBJECT;
	packet->x = nx;
	packet->y = ny;
	packet->o_type = 1;

	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_put_object));
}

void send_pos_packet(unsigned sock_fd, int mover, int mx, int my, unsigned mv_time, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_pos* packet = reinterpret_cast<sc_packet_pos*>(sendBuf->buf_addr);
	packet->id = mover;
	packet->size = sizeof(sc_packet_pos);
	packet->type = SC_POS;
	packet->x = mx;
	packet->y = my;
	packet->move_time = mv_time;

	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_pos));
}


void send_remove_object_packet(unsigned sock_fd, int leaver, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_remove_object* packet = reinterpret_cast<sc_packet_remove_object*>(sendBuf->buf_addr);
	packet->id = leaver;
	packet->size = sizeof(sc_packet_remove_object);
	packet->type = SC_REMOVE_OBJECT;
	
	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_remove_object));
}

void send_chat_packet(unsigned sock_fd, int teller, char* mess, int cid)
{
	ExtendedBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	sc_packet_chat* packet = reinterpret_cast<sc_packet_chat*>(sendBuf->buf_addr);
	packet->id = teller;
	packet->size = sizeof(sc_packet_chat);
	packet->type = SC_CHAT;

	send_packet(sock_fd, sendBuf, cid, sizeof(sc_packet_chat));
}



void Disconnect(int id)
{
	if (my_clients[id] == nullptr) return;
	
	zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id);

	//2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::BYE, -1, -1, my_clients[id]->gid);
	}


	my_clients[id]->is_connected = false;
	shutdown(my_clients[id]->sock_fd, SHUT_RDWR);
	
	globalBufferPool.return_recvBuffer(my_clients[id]->recv_buf);

	delete my_clients[id];
	my_clients[id] = nullptr;
	
	empty_cli_idx[per_max_clients - num_my_clients] = id;
	--num_my_clients;
	//my_clients.erase(id);
}

void get_new_zone(set<int>& new_zone, int x, int y) {
	int x1 = int((x - VIEW_RANGE) / ZONE_SIZE);
	if (x1 < 0) x1 = 0;
	int x2 = int((x + VIEW_RANGE) / ZONE_SIZE);
	if (x2 >= zone_width) x2 = zone_width - 1;
	int y1 = int((y - VIEW_RANGE) / ZONE_SIZE);
	if (y1 < 0) y1 = 0;
	int y2 = int((y + VIEW_RANGE) / ZONE_SIZE);
	if (y2 >= zone_heigt) y2 = zone_heigt - 1;

	new_zone.insert(x1 * zone_width + y1);
	new_zone.insert(x1 * zone_width + y2);
	new_zone.insert(x2 * zone_width + y1);
	new_zone.insert(x2 * zone_width + y2);
}


void ProcessMove(int id, unsigned char dir)
{
	short x = my_clients[id]->x;
	short y = my_clients[id]->y;
	
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = my_rand() % WORLD_WIDTH;
		y = my_rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		//while (true);
		break;
	}

	my_clients[id]->x = x;
	my_clients[id]->y = y;

	send_pos_packet(my_clients[id]->sock_fd, my_clients[id]->gid, x, y, my_clients[id]->move_time, id);

	// 1. zone in/out
	int zc = int(x / ZONE_SIZE);
	int zr = int(y / ZONE_SIZE);
	if (zc != my_clients[id]->my_zone_col || zr != my_clients[id]->my_zone_row) {
		int idx = -1;
		while (idx == -1) {
			idx = get_empty_zone_nodeIdx(my_clients[id]);
		}
		zone[zr][zc].Add(&(my_clients[id]->zone_nodes[idx]));
		zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id);
		my_clients[id]->my_zone_col = zc;
		my_clients[id]->my_zone_row = zr;
	}

	// 2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->gid);
	}
	set<int> new_zone;
	get_new_zone(new_zone, x, y);
	
	// ���� ��� �� zone
	for (auto i : new_zone) {
		if (my_clients[id]->broadcast_zone.count(i) == 0) {
			zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->gid);
		}
	}
	my_clients[id]->broadcast_zone.swap(new_zone);

}

void ProcessLogin(int user_id, char* id_str)
{
	strcpy(my_clients[user_id]->name, id_str);
	my_clients[user_id]->is_connected = true;
	send_login_ok_packet(my_clients[user_id]->sock_fd, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);
	send_put_object_packet(my_clients[user_id]->sock_fd, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y, user_id);

	// 1. zone in11
	int zc = int(my_clients[user_id]->x / ZONE_SIZE);
	int zr = int(my_clients[user_id]->y / ZONE_SIZE);
	//int idx = get_empty_zone_nodeIdx(my_clients[user_id]);
	int idx = -1;
	
	while (idx == -1) {
		idx = get_empty_zone_nodeIdx(my_clients[user_id]);
		//if(retry) mem_exceed_cnt.fetch_add(1);
	}
	zone[zr][zc].Add(&(my_clients[user_id]->zone_nodes[idx]));
	my_clients[user_id]->my_zone_col = zc;
	my_clients[user_id]->my_zone_row = zr;
	

	// 2. broadcast
	set<int> new_zone;
	get_new_zone(new_zone, my_clients[user_id]->x, my_clients[user_id]->y);

	// ���� ��� �� zone
	for (auto i : new_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, user_id, Msg::MOVE
			, my_clients[user_id]->x, my_clients[user_id]->y, my_clients[user_id]->gid);
	}
	my_clients[user_id]->broadcast_zone.swap(new_zone);
}


bool ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		my_clients[id]->move_time = move_packet->move_time;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT: {
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		//ProcessChat(id, chat_packet->chat_str);
	}
				break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: 
		cout << "Invalid Packet Type Error\n";
		//while (true);
		return false;
		break;
	}
	return true;
}

void handle_recv(unsigned int uid, ExtendedBuf* buf, unsigned bytes) {
	int recv_buf_start_idx = my_clients[uid]->recv_buf_start_idx;
	char* p = &(buf->buf_addr[recv_buf_start_idx]);
	int remain = bytes;
	int packet_size;
	int prev_packet_size = my_clients[uid]->prev_packet_size;

	if (0 == prev_packet_size)
		packet_size = 0;
	else packet_size = my_clients[uid]->recv_buf->buf_addr[recv_buf_start_idx - prev_packet_size];
	
	int rem = 0;
	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		int required = packet_size - prev_packet_size;
		if (required <= remain) {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, required);
			bool su = ProcessPacket(uid, &(buf->buf_addr[recv_buf_start_idx - prev_packet_size]));
			if (su == false) {
				Disconnect(uid);
				return;
			}
			remain -= required;
			p += required;
			recv_buf_start_idx += required;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else {
			//memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, remain);4
			memcpy(buf->buf_addr, &buf->buf_addr[recv_buf_start_idx - prev_packet_size], prev_packet_size);
			prev_packet_size += remain;
			recv_buf_start_idx = prev_packet_size;
			remain = 0;
			rem = 1;
		}
	}
	recv_buf_start_idx = rem * recv_buf_start_idx;
	
	my_clients[uid]->recv_buf_start_idx = recv_buf_start_idx;
	my_clients[uid]->prev_packet_size = prev_packet_size;
	
	
	if (rem_sqe_cnt <= 0) {
		std::cout << "can't recv sqe full" << std::endl;
		Disconnect(uid);
		return;
	}

	io_uring_sqe* sqe = io_uring_get_sqe(&rings[tid]);
	io_uring_prep_recv(sqe, my_clients[uid]->sock_fd, &(my_clients[uid]->recv_buf->buf_addr[recv_buf_start_idx]), MAX_BUFFER - recv_buf_start_idx, 0);

	io_uring_sqe_set_data(sqe, my_clients[uid]->recv_buf);


}

void handle_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;
	
	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list�� �ְ� ������ send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->sock_fd, msg->gid, msg->x, msg->y, my_id);
		// ������� HI �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
	
	// 2. �˴� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// �׳� ������ send_pos_packet
		send_pos_packet(my_clients[my_id]->sock_fd, msg->gid, msg->x, msg->y
			, my_clients[my_id]->move_time, my_id);
		return;
	}

	// 3. ������� ��
	if (!in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// list���� ���� send_remove_packet
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->sock_fd, msg->gid, my_id);
		// ������׵� BYE �����ֱ�
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
}

void handle_hi_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;
	// �þ� �˻�
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. ó�� ���� ��
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list�� �ְ� ������ send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->sock_fd, msg->gid, msg->x, msg->y, my_id);
	}
}

void handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;

	if(my_clients[my_id]->near_id.count(msg->gid) != 0) {
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->sock_fd, msg->gid, my_id);
	}
}


void handle_new_client(MsgNode* msg) {
	int user_id = empty_cli_idx[per_max_clients - 1 - num_my_clients];
	++num_my_clients;

	SOCKETINFO* new_player = reinterpret_cast<SOCKETINFO*>(msg->info);
	new_player->cid = user_id;
	new_player->my_woker_id = tid;
	new_player->recv_buf->cid = user_id;

	for (int i = 0; i < MAX_ZONE_NODE; ++i) {
		new_player->zone_nodes[i].worker_id = tid;
		new_player->zone_nodes[i].cid = user_id;
	}

	if (rem_sqe_cnt <= 0) {
		std::cout << "can't recv sqe full" << std::endl;
		Disconnect(user_id);
		return;
	}

	io_uring_sqe* sqe = io_uring_get_sqe(&rings[tid]);

	my_clients[user_id] = new_player;
	io_uring_prep_recv(sqe, my_clients[user_id]->sock_fd, my_clients[user_id]->recv_buf->buf_addr, MAX_BUFFER, 0);

	io_uring_sqe_set_data(sqe, my_clients[user_id]->recv_buf);

	
	//my_clients.insert(make_pair(user_id, new_player));

}

thread_local io_uring_cqe* cqes[MAX_RIO_REUSLTS];

void do_worker(int t)
{
	tid = t;
	for (int i = 0; i < per_max_clients; ++i) {
		empty_cli_idx[i] = per_max_clients - 1 - i;
	}
	//high_resolution_clock::time_point last_checked_time = high_resolution_clock::now();
	//unsigned long long processed_io = 0;

	while (true) {
		//auto duration = high_resolution_clock::now() - last_checked_time;
		//if (1000 * 10 < duration_cast<milliseconds>(duration).count()) {
		//	cout << "thread[" << tid << "] : " << processed_io << endl;
		//	processed_io = 0;
		//	last_checked_time = high_resolution_clock::now();
		//}

		int checked_queue = 0;
		while (checked_queue++ < MAX_RIO_REUSLTS * 16)
		{
			MsgNode* msg = msgQueue[tid].Deq();
			if (msg == nullptr) break;
			
			if (msg->msg == Msg::NEW_CLI) {
				handle_new_client(msg);
				//msgNodeBuffer.retire(msg);
				continue;
			}

			if (my_clients[msg->to] == nullptr) {
				// ***gid�� ������ Ȯ�� �ʿ�
				//|| my_clients[msg->to]->gid)
				continue;
			}

			switch (msg->msg)
			{
			case Msg::MOVE: 
				handle_move_msg(msg);
				break;
			case Msg::HI:
				handle_hi_msg(msg);
				break;
			case Msg::BYE:
				handle_bye_msg(msg);
				break;
			default:
				cout << "UNKOWN MSG" << endl;
				break;
			}

			//msgNodeBuffer.retire(msg);
		}

		
		int cqe_count = io_uring_peek_batch_cqe(&rings[tid], cqes, sizeof(cqes)/sizeof(cqes[0]));

		for(int i = 0; i < cqe_count; ++i)
		{
			io_uring_cqe* cqe = cqes[i];
			ExtendedBuf* ex_buf = reinterpret_cast<ExtendedBuf*>(io_uring_cqe_get_data(cqe));
			
			unsigned int uid = ex_buf->cid;
			int bytes = cqe->res;
			io_uring_cqe_seen(&rings[tid], cqe);

			switch (ex_buf->event_type)
			{
			case EV_RECV:
				if (bytes == 0) {
					Disconnect(uid);
					break;
				}
				handle_recv(uid, ex_buf, bytes);
				break;
			case EV_SEND:
				sendBufferPool[tid].return_sendBuffer(ex_buf);
				break;
			default:
				std::cout << "unknown Event error" << std::endl;
				break;
			
			}

			++rem_sqe_cnt;
		}

	
		//processed_io += numResults;

		io_uring_submit(&rings[tid]);
	}
}




int main()
{
	tid = NUM_WORKER_THREADS;
	epoch.store(1);
	for (int r = 0; r < NUM_WORKER_THREADS; ++r)
		reservations[r] = 0xffffffffffffffff;

	msg_node_epoch.store(1);
	for (int r = 0; r < NUM_WORKER_THREADS  + 1; ++r)
		msg_node_reservations[r] = 0xffffffffffffffff;
	
	sockaddr_in serv_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);

	int sock_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	const int val = 1;
	setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(SERVER_PORT);
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if(bind(sock_listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0){
		cout << "Error binding socket.." << endl;
		exit(1);
	}
	if(listen(sock_listen_fd, BACKLOG) < 0){
		cout << "Error listening.." << endl;
		exit(1);
	}
	
	io_uring_params main_params;
	io_uring main_ring;
	memset(&main_params, 0, sizeof(main_params));

	if(io_uring_queue_init_params(4096, &main_ring, &main_params)< 0){
		cout << "io_uring_init failed.." << endl;
		exit(1);
	}
	if(!(main_params.features & IORING_FEAT_FAST_POLL)){
		cout << "IORING_FEAT_FAST_POLL not avaiable .." << endl;
		exit(1);
	}

	globalBufferPool.init();
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		sendBufferPool[i].init();
	}


	// 3. CompletionQueue �ۼ�

	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		memset(&params[i], 0, sizeof(params[i]));

		if(io_uring_queue_init_params(4096, &rings[i], &params[i]) < 0){
			cout << "io_uring_init failed.." << endl;
			exit(1);
		}
	}

	vector <thread> worker_threads;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) worker_threads.emplace_back(do_worker, i);

	cout << "server start" << endl;
	int cq_idx = 0;
	int global_id = 0;

	const int EV_ACCEPT = 100;

	//4. Accept
	while (true) {
		//if (num_client.load(memory_order_acquire) >= 10)
		//	continue;
	
		//num_client.fetch_add(1, memory_order_release);

		io_uring_sqe* sqe = io_uring_get_sqe(&main_ring);
		io_uring_prep_accept(sqe, sock_listen_fd, (struct sockaddr*)&client_addr, &client_len, 0);
		
		
		io_uring_sqe_set_data(sqe, (void*)EV_ACCEPT);

		io_uring_submit(&main_ring);

		io_uring_cqe* cqe;
		int ret = io_uring_wait_cqe(&main_ring, &cqe);
		
		if(ret != 0){
			cout << "error io_uring_wait_cqe" << endl;
			exit(1);
		}

		int ev_type = (long)io_uring_cqe_get_data(cqe);
		if(ev_type == EV_ACCEPT){
			int sock_conn_fd = cqe->res;
			io_uring_cqe_seen(&main_ring, cqe);
			cq_idx = ++cq_idx % NUM_WORKER_THREADS;
			SOCKETINFO* new_player = new SOCKETINFO;
			ExtendedBuf* recvBuf = globalBufferPool.get_recvBuffer();
			new_player->recv_buf = recvBuf;
			new_player->x = my_rand() % WORLD_WIDTH;
			new_player->y = my_rand() % WORLD_HEIGHT;

			new_player->sock_fd = sock_conn_fd;
			new_player->prev_packet_size = 0;
			new_player->recv_buf_start_idx = 0;

			new_player->is_connected = false;
			new_player->gid = global_id++;

			msgQueue[cq_idx].Enq(-1, -1, Msg::NEW_CLI, -1, -1, -1, -1, new_player);

		}
		else{
			cout << "what?" << endl;
		}
	}

	for (auto& th : worker_threads) th.join();
	shutdown(sock_listen_fd, SHUT_RDWR);
}