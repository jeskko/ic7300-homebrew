/* hb_sinf/hb_cosf: range-reduce to [-pi/4, pi/4], then minimax-style Taylor polynomials. */
#include "hb/math.h"

static float sin_poly(float x)
{
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040))));
}

static float cos_poly(float x)
{
    float x2 = x * x;
    return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}

float hb_sinf(float x)
{
    /* x = q * pi/2 + r, |r| <= pi/4 */
    float qf = x * (2.0f / HB_PI);
    int q = (int)(qf + (qf >= 0 ? 0.5f : -0.5f));
    float r = x - (float)q * (HB_PI / 2);
    switch (q & 3) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
    }
}

float hb_cosf(float x)
{
    return hb_sinf(x + HB_PI / 2);
}
