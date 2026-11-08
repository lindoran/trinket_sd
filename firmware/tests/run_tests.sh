#!/usr/bin/env bash
# Host-side unit tests for the pure-software parts of the firmware.
set -eu
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -Werror -O1 -DDSS_HOST_TEST -o /tmp/test_dss_model test_dss_model.c ../src/dss_model.c
/tmp/test_dss_model
gcc -std=c11 -Wall -Wextra -Werror -O1 -DDSS_HOST_TEST -o /tmp/test_dss_cmd test_dss_cmd.c ../src/dss_cmd.c ../src/dss_model.c
/tmp/test_dss_cmd
