#pragma once
// The named faces the UI draws with. Default: the epdiy bitmap fonts. Built
// with -DVFONT, every name resolves to a vector face (see vfont.h) at the
// same digit / cap height as the bitmap it replaces, so layouts are
// unchanged and only the outlines differ. Values: Anton (Impact re-cut);
// labels: Arimo 700 (Arial-metric). Heights are epdc_digit_height() of the
// bitmap faces as measured on the host.
#ifdef VFONT
#include "vfont.h"
#include "fonts/vf/anton.h"
#include "fonts/vf/arimo_700.h"
#define Impact_S     (*vf_face_digit(&Anton, 20))
#define Impact_T     (*vf_face_digit(&Anton, 30))
#define Impact_V     (*vf_face_digit(&Anton, 46))
#define Impact_A     (*vf_face_digit(&Anton, 58))
#define Impact_M     (*vf_face_digit(&Anton, 69))
#define Impact_40    (*vf_face_digit(&Anton, 68))
#define Impact_B     (*vf_face_digit(&Anton, 81))
#define Impact_H     (*vf_face_digit(&Anton, 95))
#define Impact_C     (*vf_face_digit(&Anton, 120))
#define Impact_XL    (*vf_face_digit(&Anton, 158))
#define Impact_128   (*vf_face_digit(&Anton, 219))
#define Arial_L      (*vf_face_digit(&Arimo_700, 11))
#define Arial_B      (*vf_face_digit(&Arimo_700, 15))
#define ArialBold_14 (*vf_face_digit(&Arimo_700, 21))
#define ArialBold_20 (*vf_face_digit(&Arimo_700, 30))
#else
#include "fonts/arial_l.h"
#include "fonts/arial_b.h"
#include "fonts/impact_s.h"
#include "fonts/impact_t.h"
#include "fonts/impact_m.h"
#include "fonts/impact_v.h"
#include "fonts/impact_h.h"
#include "fonts/impact_a.h"
#include "fonts/impact_b.h"
#include "fonts/impact_c.h"
#include "fonts/impact_xl.h"
#include "fonts/arialbold_14.h"
#include "fonts/arialbold_20.h"
#include "fonts/impact_40.h"
#include "fonts/impact_128.h"
#endif
