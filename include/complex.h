#ifndef MCC_COMPLEX_H
#define MCC_COMPLEX_H

#define complex _Complex
#define _Complex_I (__builtin_complex(0.0f, 1.0f))
#undef I
#define I _Complex_I

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define CMPLX(real, imaginary) __builtin_complex((double)(real), (double)(imaginary))
#define CMPLXF(real, imaginary) __builtin_complex((float)(real), (float)(imaginary))
#define CMPLXL(real, imaginary) __builtin_complex((long double)(real), (long double)(imaginary))
#endif

#define __MCC_DECLARE_COMPLEX(type, suffix)                  \
    type complex cacos##suffix(type complex);                \
    type complex casin##suffix(type complex);                \
    type complex catan##suffix(type complex);                \
    type complex ccos##suffix(type complex);                 \
    type complex csin##suffix(type complex);                 \
    type complex ctan##suffix(type complex);                 \
    type complex cacosh##suffix(type complex);               \
    type complex casinh##suffix(type complex);               \
    type complex catanh##suffix(type complex);               \
    type complex ccosh##suffix(type complex);                \
    type complex csinh##suffix(type complex);                \
    type complex ctanh##suffix(type complex);                \
    type complex cexp##suffix(type complex);                 \
    type complex clog##suffix(type complex);                 \
    type complex cpow##suffix(type complex, type complex);   \
    type complex csqrt##suffix(type complex);                \
    type complex conj##suffix(type complex);                 \
    type complex cproj##suffix(type complex);                \
    type cabs##suffix(type complex);                        \
    type carg##suffix(type complex);                        \
    type cimag##suffix(type complex);                       \
    type creal##suffix(type complex)

__MCC_DECLARE_COMPLEX(double, );
__MCC_DECLARE_COMPLEX(float, f);
__MCC_DECLARE_COMPLEX(long double, l);

#ifdef __STDC_WANT_IEC_60559_TYPES_EXT__
#define CMPLXF32(real, imaginary) __builtin_complex((_Float32)(real), (_Float32)(imaginary))
#define CMPLXF64(real, imaginary) __builtin_complex((_Float64)(real), (_Float64)(imaginary))
#define CMPLXF128(real, imaginary) __builtin_complex((_Float128)(real), (_Float128)(imaginary))
#define CMPLXF32X(real, imaginary) __builtin_complex((_Float32x)(real), (_Float32x)(imaginary))
#define CMPLXF64X(real, imaginary) __builtin_complex((_Float64x)(real), (_Float64x)(imaginary))

__MCC_DECLARE_COMPLEX(_Float32, f32);
__MCC_DECLARE_COMPLEX(_Float64, f64);
__MCC_DECLARE_COMPLEX(_Float128, f128);
__MCC_DECLARE_COMPLEX(_Float32x, f32x);
__MCC_DECLARE_COMPLEX(_Float64x, f64x);
#endif

#undef __MCC_DECLARE_COMPLEX

#endif
