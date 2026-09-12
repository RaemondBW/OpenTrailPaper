#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
production=re.sub(r'^#include[^\n]*\n','',(root/'src/crash_report.cpp').read_text(),flags=re.M)
source=(root/'tools/crash_test/service.cpp').read_text().replace('// PRODUCTION',production)
with tempfile.TemporaryDirectory(prefix='crash-tests-') as tmp:
    path=Path(tmp)/'service.cpp';path.write_text(source)
    exe=Path(tmp)/'service'
    for memfault in (0, 1):
        subprocess.run(['c++',f'-DOT_MEMFAULT={memfault}','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g','-I',str(root/'src'),str(path),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
