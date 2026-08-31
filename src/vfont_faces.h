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
// -DVFONT_VALUES=<name> draws every value face from that generated font
// instead of Anton. Tried so far: JBMono_800 (monospaced, digits keep their
// column as they tick) and Geologica_700 (clean geometric).
#ifdef VFONT_VALUES
#include "fonts/vf/jbmono_800.h"
#include "fonts/vf/geologica_700.h"
#define VF_VALUES (&VFONT_VALUES)
#else
#define VF_VALUES (&Anton)
#endif
#define Impact_S     (*vf_face_digit(VF_VALUES, 20))
#define Impact_T     (*vf_face_digit(VF_VALUES, 30))
#define Impact_V     (*vf_face_digit(VF_VALUES, 46))
#define Impact_A     (*vf_face_digit(VF_VALUES, 58))
#define Impact_M     (*vf_face_digit(VF_VALUES, 69))
#define Impact_40    (*vf_face_digit(VF_VALUES, 68))
#define Impact_B     (*vf_face_digit(VF_VALUES, 81))
#define Impact_H     (*vf_face_digit(VF_VALUES, 95))
#define Impact_C     (*vf_face_digit(VF_VALUES, 120))
#define Impact_XL    (*vf_face_digit(VF_VALUES, 158))
#define Impact_128   (*vf_face_digit(VF_VALUES, 219))
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
