/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test double for the gNB control endpoint: serves the real command parser
 * with two fixed UEs.  Usage: ctrl_server <port>  (runs until killed).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nr_dl_ctrl_state.h"

int main(int argc, char **argv)
{
  if (argc != 2)
    return 2;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1]))};
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    return 1;
  nr_ctrl_state_t st;
  memset(&st, 0, sizeof(st));
  nr_ctrl_init(&st);
  nr_ctrl_ue_view_t ues[2] = {{.rnti = 0x1001}, {.rnti = 0x1002}};
  printf("ready\n");
  fflush(stdout);
  for (;;) {
    char cmd[256], reply[4096];
    struct sockaddr_in peer;
    socklen_t pl = sizeof(peer);
    ssize_t r = recvfrom(fd, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *)&peer, &pl);
    if (r < 0)
      continue;
    cmd[r] = 0;
    /* grow the counters so `measure` has something to report */
    ues[0].mac_dl_rbs += 300; ues[0].mac_dl_sdu_bytes += 3000;
    ues[1].mac_dl_rbs += 100; ues[1].mac_dl_sdu_bytes += 1000;
    int n = nr_ctrl_handle(&st, cmd, ues, 2, reply, sizeof(reply));
    sendto(fd, reply, (size_t)n, 0, (struct sockaddr *)&peer, pl);
  }
}
