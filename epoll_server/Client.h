#pragma once
#include "common.h"
#include "Zone.h"
#include "protocol.h"
#include <set>

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

struct ExtendedBuf{
    char* buf_addr;
    
    EVENT_TYPE event_type;
    int cid;
};


struct SOCKETINFO
{
    ExtendedBuf* recv_buf;
	
	int		prev_packet_size;
	int		recv_buf_start_idx;

	unsigned sock_fd;

	int		cid;
	int		gid;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	unsigned	move_time;
	std::set <int> near_id;

	int my_woker_id;
	ZoneNode zone_nodes[MAX_ZONE_NODE];
	std::set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;
};

int get_empty_zone_nodeIdx(SOCKETINFO* cli) {
	unsigned long long max_safe_epoch = get_min_reservation();

	for (int i = 0; i < MAX_ZONE_NODE; ++i) {
		if (cli->zone_nodes[i].used == false
			&& cli->zone_nodes[i].retired_epoch < max_safe_epoch) {
			cli->zone_nodes[i].used = true;
			return i;
		}
	}

	std::cout << "zone node empty!!" << std::endl;
	return -1;
}