/*
 * xApp that sets the downlink scheduler policy, the per-UE weights and caps and the
 * slices of the gNB through the MAC service model's control message.
 *
 *   xapp_sched_ctrl policy <pf|rr|wrr|wshare|slice>
 *   xapp_sched_ctrl weight <rnti|*> <1..65535>     rnti in hexadecimal, * = default weight
 *   xapp_sched_ctrl cap <rnti> <0|5..275>          resource blocks per slot, 0 = no cap
 *   xapp_sched_ctrl slice-weight <0..7> <1..65535>
 *   xapp_sched_ctrl slice-cap <0..7> <0|5..275>
 *   xapp_sched_ctrl slice-del <0..7>               slice back to its defaults, members fall back to slice 0
 *   xapp_sched_ctrl assoc <rnti> <0..7|none>       slice of a UE, none removes the association
 *   xapp_sched_ctrl nssai-map <sst> <sd|none> <0..7|none>   slice for UEs with that S-NSSAI (sd in hex)
 *   xapp_sched_ctrl reset
 *
 * The command is sent to every connected gNB that registered the MAC service
 * model.  The E2 control acknowledgement says that the gNB received the message,
 * not that the scheduler accepted it: check the result on the cell node with
 * bin/sched-ctl.py status, or in the gNB log ("[E2 Agent]: MAC control ...").
 * Run it after the gNB has registered with the RIC.
 *
 * Built inside the FlexRIC source tree by bin/patch-flexric-sched-ctrl.py, which
 * copies this file and nr_dl_ctrl_e2.h next to the stock MAC control example.
 */
#include "../../../../src/xApp/e42_xapp_api.h"
#include "../../../../src/util/alg_ds/alg/defer.h"

#include "nr_dl_ctrl_e2.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAC_SM_ID 142

static void usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s policy <pf|rr|wrr|wshare|slice>\n"
          "       %s weight <rnti|*> <1..65535>\n"
          "       %s cap <rnti> <0|5..275>\n"
          "       %s slice-weight <0..7> <1..65535>\n"
          "       %s slice-cap <0..7> <0|5..275>\n"
          "       %s slice-del <0..7>\n"
          "       %s assoc <rnti> <0..7|none>\n"
          "       %s nssai-map <sst> <sd|none> <0..7|none>\n"
          "       %s reset\n",
          prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
  uint32_t hdr = 0, act = 0;
  const char *err = NULL;
  if (argc < 2 || argc > 5
      || nr_ctrl_e2_pack(argv[1], argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL, &hdr, &act, &err) != 0) {
    if (argc >= 2 && argc <= 5)
      fprintf(stderr, "error: %s\n", err);
    usage(argv[0]);
    return 2;
  }

  /* the FlexRIC options (-c, -p) are not used here: the default configuration file is read */
  char *fr_argv[] = {argv[0], NULL};
  fr_args_t args = init_fr_args(1, fr_argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  defer({ free_e2_node_arr_xapp(&nodes); });

  printf("Connected E2 nodes = %d\n", nodes.len);
  int sent = 0, failed = 0;
  for (int i = 0; i < nodes.len; i++) {
    e2_node_connected_xapp_t *n = &nodes.n[i];
    bool has_mac = false;
    for (size_t j = 0; j < n->len_rf; j++)
      if (n->rf[j].id == MAC_SM_ID)
        has_mac = true;
    if (!has_mac || (n->id.type != ngran_gNB && n->id.type != ngran_gNB_DU)) {
      printf("Node %d: no MAC service model on a gNB, skipped\n", i);
      continue;
    }
    mac_ctrl_req_data_t wr = {.hdr.dummy = hdr, .msg.action = act};
    sm_ans_xapp_t const a = control_sm_xapp_api(&nodes.n[i].id, MAC_SM_ID, &wr);
    printf("Node %d: control message (header 0x%08x, message 0x%08x) %s\n", i, (unsigned)hdr, (unsigned)act, a.success ? "acknowledged" : "not acknowledged");
    if (a.success)
      sent++;
    else
      failed++;
  }

  while (try_stop_xapp_api() == false)
    usleep(1000);

  if (sent == 0 || failed > 0) {
    printf("Scheduler command not delivered\n");
    return 1;
  }
  printf("Scheduler command delivered; check the cell with sched-ctl.py status\n");
  return 0;
}
