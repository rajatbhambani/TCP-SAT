#!/bin/bash
# register_tests.sh -- Wire bbr_sat_test.c into the picoquic test framework
# Run from: ~/tcpsatproject/picoquic-main
# 
# Makes 3 changes:
#   1. Copy test file into picoquictest/
#   2. Add declarations to picoquictest/picoquictest.h
#   3. Add registrations to picoquic_t/picoquic_t.c
#   4. Add bbr_sat_test.c to CMakeLists.txt

set -e
REPO=~/tcpsatproject/picoquic-main

# --- 1. Copy test file ---
cp "$(dirname "$0")/bbr_sat_test.c" "$REPO/picoquictest/bbr_sat_test.c"
echo "Copied bbr_sat_test.c"

# --- 2. Add declarations to picoquictest.h (after satellite_preemptive_fc_test) ---
sed -i 's/int satellite_preemptive_fc_test(void);/int satellite_preemptive_fc_test(void);\n\/* BBR-SAT sanity tests *\/\nint bbr_sat_handover_test(void);\nint bbr_sat_leo_to_geo_test(void);\nint bbr_sat_geo_to_leo_test(void);\nint bbr_sat_bw_ceiling_test(void);/' \
    "$REPO/picoquictest/picoquictest.h"
echo "Added declarations to picoquictest.h"

# --- 3. Add registrations to picoquic_t.c (after satellite_preemptive_fc entry) ---
sed -i 's/{ "satellite_preemptive_fc", satellite_preemptive_fc_test }/{ "satellite_preemptive_fc", satellite_preemptive_fc_test },\n    { "bbr_sat_handover", bbr_sat_handover_test },\n    { "bbr_sat_leo_to_geo", bbr_sat_leo_to_geo_test },\n    { "bbr_sat_geo_to_leo", bbr_sat_geo_to_leo_test },\n    { "bbr_sat_bw_ceiling", bbr_sat_bw_ceiling_test }/' \
    "$REPO/picoquic_t/picoquic_t.c"
echo "Added registrations to picoquic_t.c"

# --- 4. Add to CMakeLists.txt (after satellite_test.c) ---
sed -i 's|picoquictest/satellite_test.c|picoquictest/satellite_test.c\n    picoquictest/bbr_sat_test.c|' \
    "$REPO/CMakeLists.txt"
echo "Added bbr_sat_test.c to CMakeLists.txt"

echo ""
echo "Done. Now run:"
echo "  cd $REPO/build && make -j3"
echo "  cd $REPO && ./build/picoquic_ct bbr_sat_handover bbr_sat_leo_to_geo bbr_sat_geo_to_leo bbr_sat_bw_ceiling"

