"""The link's protobuf messages, built at runtime from proto/link.proto.

protoc writes a descriptor set (cached under sim/build); the classes come from
the protobuf runtime's message factory, so no generated Python is checked in
and protoc's version needn't match the runtime's."""
import os
import subprocess

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PROTO = os.path.join(ROOT, "proto", "link.proto")
CACHE = os.path.join(ROOT, "sim", "build", "link.desc")


def _descriptor_set():
    if not os.path.exists(CACHE) or os.path.getmtime(CACHE) < os.path.getmtime(PROTO):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        subprocess.check_call(["protoc", "-I", os.path.dirname(PROTO), "--include_imports",
                               "--descriptor_set_out=" + CACHE, PROTO])
    fds = descriptor_pb2.FileDescriptorSet()
    with open(CACHE, "rb") as f:
        fds.ParseFromString(f.read())
    return fds


class _Messages:
    """pb.PhoneMsg, pb.Hello, ... and the enums' values as pb.FEATURE_LIVE etc."""

    def __init__(self):
        pool = descriptor_pool.DescriptorPool()
        fds = _descriptor_set()
        for fd in fds.file:
            pool.Add(fd)
        file = pool.FindFileByName("link.proto")
        for name, desc in file.message_types_by_name.items():
            setattr(self, name, message_factory.GetMessageClass(desc))
        for enum in file.enum_types_by_name.values():
            setattr(self, enum.name, enum)
            for v in enum.values:
                setattr(self, v.name, v.number)


pb = _Messages()
