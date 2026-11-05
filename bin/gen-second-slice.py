#!/usr/bin/env python3
"""Second S-NSSAI (SST 2, no SD) for the core network of the single-cell profile.

Reads etc/mini_nonrf_config.yaml and etc/docker-compose-core.yaml and writes
etc/mini_nonrf_config.second.yaml and etc/docker-compose-core.second.yaml next to
them.  The two source files are never modified, so a repository update on the node
stays a fast-forward and the single-slice deployment is untouched.

Every edit is made at an anchor that must occur exactly once; a configuration that
has changed shape makes the script fail instead of producing a half-patched file.

Usage: gen-second-slice.py [etc_dir]      (default: etc/ of this repository)
"""
import sys
from pathlib import Path

SLICE_ANCHOR = "second_slice"

EDITS = [
    # the shared definition of the S-NSSAIs
    ("snssais:\n  - &embb_slice1\n    sst: 1\n",
     "snssais:\n  - &embb_slice1\n    sst: 1\n  - &second_slice\n    sst: 2\n"),
    # the AMF serves it
    ("      nssai:\n        - *embb_slice1\n",
     "      nssai:\n        - *embb_slice1\n        - *second_slice\n"),
    # the SMF selects the DNN for it
    ("      - sNssai: *embb_slice1\n        dnnSmfInfoList:\n          - dnn: \"oai\"\n",
     "      - sNssai: *embb_slice1\n        dnnSmfInfoList:\n          - dnn: \"oai\"\n"
     "      - sNssai: *second_slice\n        dnnSmfInfoList:\n          - dnn: \"oai\"\n"),
    # the SMF has local subscription data for it
    ("    - single_nssai: *embb_slice1\n      dnn: \"oai\"\n      qos_profile:\n"
     "        5qi: 9\n        session_ambr_ul: \"200Mbps\"\n        session_ambr_dl: \"400Mbps\"\n",
     "    - single_nssai: *embb_slice1\n      dnn: \"oai\"\n      qos_profile:\n"
     "        5qi: 9\n        session_ambr_ul: \"200Mbps\"\n        session_ambr_dl: \"400Mbps\"\n"
     "    - single_nssai: *second_slice\n      dnn: \"oai\"\n      qos_profile:\n"
     "        5qi: 9\n        session_ambr_ul: \"200Mbps\"\n        session_ambr_dl: \"400Mbps\"\n"),
    # the UPF serves it
    ("      - sNssai: *embb_slice1\n        dnnUpfInfoList:\n          - dnn: \"oai\"\n",
     "      - sNssai: *embb_slice1\n        dnnUpfInfoList:\n          - dnn: \"oai\"\n"
     "      - sNssai: *second_slice\n        dnnUpfInfoList:\n          - dnn: \"oai\"\n"),
]


def patch_config(text):
    for old, new in EDITS:
        if text.count(old) != 1:
            raise SystemExit("gen-second-slice: anchor found %d times (expected 1):\n%s" % (text.count(old), old))
        text = text.replace(old, new)
    return text


def patch_compose(text):
    old = "./mini_nonrf_config.yaml:"
    if text.count(old) != 6:
        raise SystemExit("gen-second-slice: expected 6 mounts of mini_nonrf_config.yaml, found %d" % text.count(old))
    return text.replace(old, "./mini_nonrf_config.second.yaml:")


def main(argv):
    etc = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent / "etc"
    config = (etc / "mini_nonrf_config.yaml").read_text()
    compose = (etc / "docker-compose-core.yaml").read_text()
    (etc / "mini_nonrf_config.second.yaml").write_text(patch_config(config))
    (etc / "docker-compose-core.second.yaml").write_text(patch_compose(compose))
    print("[SECOND-SLICE] wrote %s and %s" % (etc / "mini_nonrf_config.second.yaml", etc / "docker-compose-core.second.yaml"))


if __name__ == "__main__":
    main(sys.argv)
