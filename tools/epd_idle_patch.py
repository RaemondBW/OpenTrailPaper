"""Replace the pinned display worker's 1-tick idle poll with a work semaphore.

PlatformIO post scripts run after dependency installation and before compilation.
Only the current project's private libdeps copy is changed; edits are repeatable.
"""
from pathlib import Path
import re

MARKER = '// OpenTrailPaper: block idle display worker until submitted work.'

def patched(header, source):
    if MARKER in source:
        if '_paint_work_sem' not in header or source.count('xSemaphoreGive(_paint_work_sem);') != 6:
            raise ValueError('Incomplete display idle patch; restore the pinned dependency')
        return header, source
    field = '  TaskHandle_t      _paint_task_h    = nullptr;'
    idle = '      while(paintStage==0){\n         vTaskDelay(1);\n      }'
    if header.count(field) != 1 or source.count(idle) != 1:
        raise ValueError('Display dependency changed: idle worker patch needs review')
    header = header.replace(field, '  SemaphoreHandle_t _paint_work_sem = nullptr;\n' + field)
    source = source.replace(idle, '      ' + MARKER + '\n      while (paintStage == 0) {\n'
                            '        xSemaphoreTake(_paint_work_sem, portMAX_DELAY);\n      }')
    init = re.compile(r'  _paint_active_sem = xSemaphoreCreateBinary\(\);\s*'
                      r'xSemaphoreGive\(_paint_active_sem\);\s*'
                      r'_paint_buffer_sem = xSemaphoreCreateBinary\(\);\s*'
                      r'xSemaphoreGive\(_paint_buffer_sem\);')
    source, count = init.subn('''  _paint_active_sem = xSemaphoreCreateBinary();
  _paint_buffer_sem = xSemaphoreCreateBinary();
  _paint_work_sem = xSemaphoreCreateBinary();
  if (!_paint_active_sem || !_paint_buffer_sem || !_paint_work_sem) {
    if (_paint_active_sem) vSemaphoreDelete(_paint_active_sem);
    if (_paint_buffer_sem) vSemaphoreDelete(_paint_buffer_sem);
    if (_paint_work_sem) vSemaphoreDelete(_paint_work_sem);
    _paint_active_sem = _paint_buffer_sem = _paint_work_sem = nullptr;
    return false;
  }
  xSemaphoreGive(_paint_active_sem);
  xSemaphoreGive(_paint_buffer_sem);''', source)
    if count != 1:
        raise ValueError('Display semaphore initialization changed')
    cleanup = '  vSemaphoreDelete(_paint_active_sem);\n  vSemaphoreDelete(_paint_buffer_sem);'
    if source.count(cleanup) != 1:
        raise ValueError('Display semaphore teardown changed')
    source = source.replace(cleanup, '''  if (_paint_active_sem) vSemaphoreDelete(_paint_active_sem);
  if (_paint_buffer_sem) vSemaphoreDelete(_paint_buffer_sem);
  if (_paint_work_sem) vSemaphoreDelete(_paint_work_sem);
  _paint_active_sem = _paint_buffer_sem = _paint_work_sem = nullptr;''')
    submit = re.compile(r'(paintStage\s*=\s*[12];[^\n]*\n([ \t]*)'
                        r'xSemaphoreGive\(_paint_buffer_sem\);[^\n]*\n)')
    source, count = submit.subn(lambda m: m[1] + m[2] + 'xSemaphoreGive(_paint_work_sem);\n', source)
    if count != 6:
        raise ValueError(f'Expected six display submissions, found {count}')
    return header, source

def apply(env):
    from platformio.package.lockfile import LockFile
    root = Path(env.subst('$PROJECT_DIR')).resolve()
    directory = Path(env.subst('$PROJECT_LIBDEPS_DIR')) / env.subst('$PIOENV') / 'EPD Painter' / 'src'
    if root not in directory.resolve().parents:
        raise ValueError('Display patch requires libdeps inside this project')
    h, cpp = directory / 'EPD_Painter.h', directory / 'EPD_Painter.cpp'
    # Different framework/build directories may share this env's libdeps.
    # Serialize the two-file edit so another build never reads half a patch.
    with LockFile(str(directory / '.otp-idle-patch')):
        old_h, old_cpp = h.read_text(), cpp.read_text()
        new_h, new_cpp = patched(old_h, old_cpp)
        if new_h != old_h: h.write_text(new_h)
        if new_cpp != old_cpp: cpp.write_text(new_cpp)
    print('EPD idle wait verified: dedicated work semaphore; six submission paths')

try:
    Import('env')
except NameError:
    pass  # host tests import patched() without SCons
else:
    apply(env)
