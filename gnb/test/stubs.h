#ifndef STUBS_H
#define STUBS_H
#include <stdint.h>
extern int stub_pf_calls;
extern uint16_t stub_commit_fail_rnti;
extern int stub_fail_is_cce;
extern int stub_ack_budget;
extern int stub_ack_period;
void stub_ack_reset(void);
extern int stub_bytes_per_rb;
#endif
