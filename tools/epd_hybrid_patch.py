"""Small changes to EPD_Painter for src/epd_scan.cpp and the hybrid update bench.

Both have to use the driver's own sendRow(), DMA buffers, paint lock and panel power
handling, all of which are private, so they get a friend declaration. And the bench's
`paced` mode needs FAST at a constant pass period, and with extra charging time per
row, neither of which upstream has a setting for: FAST runs with no gap at all, and
row_extra_us is 16-grey only. Same mechanism as tools/epd_idle_patch.py: only this
env's private libdeps copy is changed, and applying it twice is harmless.
"""
from pathlib import Path

MARKER = ('  friend class EpdHybrid;  // OpenTrailPaper: tools/epd_hybrid_bench\n'
          '  friend class EpdScan;  // OpenTrailPaper: src/epd_scan.cpp\n')
ANCHOR = '  // ---- LCD_CAM / DMA ----\n'

FIELD_MARKER = ('      int pass_us_fast = 0;  // OpenTrailPaper: constant FAST period, 0 = none\n'
                '      int row_extra_us_fast = 0;  // OpenTrailPaper: FAST charge time per row\n'
                '      int rowloop_us = 0;  // OpenTrailPaper: last row loop, measured\n')
FIELD_ANCHOR = '      int pass_gap_us_high   = 8000;\n'

WAIT_MARKER = '      if (_config.quality == Quality::QUALITY_FAST) {  // OpenTrailPaper\n'
WAIT_ANCHOR = '      // QUALITY_FAST = No delay.\n'
WAIT = (WAIT_MARKER +
        '        _config.rowloop_us = (int)(esp_timer_get_time() - pass_t0);\n'
        '        if (_config.pass_us_fast > 0) epd_wait_until_us(pass_t0 + _config.pass_us_fast);\n'
        '      }\n')

ROW_MARKER = '  if (_config.quality == Quality::QUALITY_FAST && _config.row_extra_us_fast > 0)  // OpenTrailPaper\n'
ROW_ANCHOR = '  if (_grey16) {\n    const int extra = _config.rowExtraUs();\n'
ROW = ROW_MARKER + '    EPD_DELAY_US(_config.row_extra_us_fast);\n'


def patched(header):
    if MARKER in header:
        return header
    if header.count(ANCHOR) != 1:
        raise ValueError('Display dependency changed: hybrid patch needs review')
    return header.replace(ANCHOR, MARKER + ANCHOR)


def patched_field(header):
    if FIELD_MARKER in header:
        return header
    if header.count(FIELD_ANCHOR) != 1:
        raise ValueError('Display dependency changed: hybrid patch needs review')
    return header.replace(FIELD_ANCHOR, FIELD_ANCHOR + FIELD_MARKER)


def patched_wait(source):
    if WAIT_MARKER in source:
        return source
    if source.count(WAIT_ANCHOR) != 1:
        raise ValueError('Display dependency changed: hybrid patch needs review')
    return source.replace(WAIT_ANCHOR, WAIT)


def patched_row(source):
    if ROW_MARKER in source:
        return source
    if source.count(ROW_ANCHOR) != 1:
        raise ValueError('Display dependency changed: hybrid patch needs review')
    return source.replace(ROW_ANCHOR, ROW + ROW_ANCHOR)


def write_if_changed(path, new):
    if new != path.read_text():
        path.write_text(new)


def apply(env):
    src = Path(env.subst('$PROJECT_LIBDEPS_DIR')) / env.subst('$PIOENV') / 'EPD Painter' / 'src'
    write_if_changed(src / 'EPD_Painter.h', patched_field(patched((src / 'EPD_Painter.h').read_text())))
    write_if_changed(src / 'EPD_Painter.cpp', patched_row(patched_wait((src / 'EPD_Painter.cpp').read_text())))


try:
    Import('env')
except NameError:
    pass
else:
    apply(env)  # noqa: F821
