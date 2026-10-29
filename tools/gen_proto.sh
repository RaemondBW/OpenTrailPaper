#!/usr/bin/env bash
# Regenerates the committed protobuf code for proto/link.proto:
#   src/proto/link.pb.{c,h}              nanopb 0.4.9.1 (pip install nanopb==0.4.9.1)
#   companion-ios/Sources/Link.pb.swift  protoc + protoc-gen-swift (brew install protobuf swift-protobuf)
# Android generates its own at build time (companion-android/app/build.gradle.kts).
# NANOPB_GEN overrides the generator path (default: nanopb_generator on PATH).
set -euo pipefail
cd "$(dirname "$0")/.."
GEN="${NANOPB_GEN:-nanopb_generator}"
mkdir -p src/proto
"$GEN" -I proto -D src/proto -f proto/link.options proto/link.proto
protoc -I proto --swift_out=companion-ios/Sources --swift_opt=Visibility=Internal proto/link.proto
mv companion-ios/Sources/link.pb.swift companion-ios/Sources/Link.pb.swift
echo "generated src/proto/link.pb.{c,h} and companion-ios/Sources/Link.pb.swift"
