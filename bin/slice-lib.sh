#!/bin/bash
# =============================================================================
# slice-lib.sh -- second S-NSSAI (SST 2) of the single-cell profile.
#
# Sourced by cell-setup.sh and gen-subscribers.sh.  SECOND_SLICE=1 turns it on;
# unset or 0 leaves every generated file as it is with the single slice (SST 1,
# no SD).  The core configuration is handled by gen-second-slice.py.
# =============================================================================

second_slice_enabled() {
    [ "${SECOND_SLICE:-0}" = 1 ]
}

# ue_sst <u> <ues_per_cell>
# SST of the u-th UE (1-based) of the cell.  With the second slice on, the first
# ceil(n/2) UEs use SST 1 and the others SST 2.
ue_sst() {
    local u="$1" n="$2"
    if second_slice_enabled && [ "$u" -gt $(( (n + 1) / 2 )) ]; then
        echo 2
    else
        echo 1
    fi
}

# gnb_add_second_slice <gnb.conf>
# Adds SST 2 (no SD) to the snssaiList of the gNB; fails when the list does not
# have the expected form.
gnb_add_second_slice() {
    local f="$1"
    grep -q 'snssaiList = ({ sst = 1, sd = 0xffffff })' "$f" || return 1
    sed -i 's/snssaiList = ({ sst = 1, sd = 0xffffff })/snssaiList = ({ sst = 1, sd = 0xffffff }, { sst = 2, sd = 0xffffff })/' "$f"
}

# ue_set_sst <nr-ue.conf> <sst>
ue_set_sst() {
    local f="$1" sst="$2"
    grep -q 'nssai_sst=1;' "$f" || return 1
    sed -i "s/nssai_sst=1;/nssai_sst=${sst};/" "$f"
}
