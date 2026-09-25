/* Small float math for apps (there is no libm). Accurate to ~1e-6 over any range. */
#ifndef HB_MATH_H
#define HB_MATH_H

#define HB_PI 3.14159265358979f

float hb_sinf(float x);
float hb_cosf(float x);

#endif
