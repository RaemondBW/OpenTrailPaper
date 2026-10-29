#!/bin/sh
# Builds and runs the host tests for the phone link's framing, reassembly and
# CRC (src/link_frame.c) plus a nanopb round trip of the generated
# src/proto/link.pb.c. See tools/link_test/link_test.c for what is covered.
set -e
cd "$(dirname "$0")/../.."
cc -std=c11 -O2 -Wall -Wextra -I src -I src/proto -I third_party/nanopb \
    -DPB_ENABLE_MALLOC=0 \
    tools/link_test/link_test.c src/link_frame.c src/proto/link.pb.c \
    third_party/nanopb/pb_common.c third_party/nanopb/pb_encode.c \
    third_party/nanopb/pb_decode.c \
    -o tools/link_test/link_test
./tools/link_test/link_test
