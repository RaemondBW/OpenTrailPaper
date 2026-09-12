#!/usr/bin/env python3
"""Exercise production transport with the real pinned SDK packetizer/exporter."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
import base64
root=Path(__file__).resolve().parents[2]
sdk=root/'third_party/memfault-firmware-sdk'
production=re.sub(r'^#include[^\n]*\n','',(root/'src/memfault_service.cpp').read_text(),flags=re.M)
source=(root/'tools/memfault_test/transport.cpp').read_text().replace('// PRODUCTION',production)
ble=(root/'src/ble_server.cpp').read_text()
start=ble.index('void sendLogFile(const char* name) {')
end=ble.index('\n}',start)+2
source=source.replace('// BLE LOG DOWNLOAD',ble[start:end])
with tempfile.TemporaryDirectory(prefix='memfault-tests-') as tmp:
    tmp=Path(tmp)
    (tmp/'memfault_platform_config.h').write_text('#define MEMFAULT_DATA_SOURCE_RLE_ENABLED 0\n#define MEMFAULT_SDK_LOG_SAVE_DISABLE 1\n')
    flags=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-DOT_MEMFAULT=1','-I',str(tmp),'-I',str(sdk/'components/include')]
    sources=['core/src/memfault_data_packetizer.c','core/src/memfault_data_export.c','util/src/memfault_base64.c','util/src/memfault_chunk_transport.c','util/src/memfault_crc16_ccitt.c','util/src/memfault_varint.c']
    objects=[]
    for i,p in enumerate(sources):
        obj=tmp/f'{i}.o';objects.append(str(obj))
        subprocess.run(['cc','-std=c11',*flags,'-c',str(sdk/'components'/p),'-o',str(obj)],check=True)
    path=tmp/'transport.cpp';path.write_text(source);exe=tmp/'transport'
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'src'),str(path),*objects,'-o',str(exe)],check=True)
    capture=tmp/'capture.log'
    subprocess.run([str(exe),str(capture)],check=True)
    sys.path.insert(0,str(root/'tools'))
    from memfault_export import decode
    text=capture.read_text()
    chunks,core,manifest=decode(text)
    assert manifest['chunk_crc_verified'] and len(core)==1064
    lines=chunks.splitlines()
    changed=bytearray(base64.b64decode(lines[-1][3:-1]));changed[-1]^=1
    bad_crc='\n'.join(lines[:-1]+['MC:'+base64.b64encode(changed).decode()+':'])
    for bad in ('\n'.join(lines[:-1]),'\n'.join(lines[:1]+lines[2:]),chunks+chunks,bad_crc):
        try:decode(bad)
        except ValueError:pass
        else:raise AssertionError('Invalid export accepted')
    print('Offline decoder accepts real SDK output and rejects missing, duplicate, truncated and corrupt chunks')
