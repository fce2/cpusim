#ifndef _SIM_H_
#define _SIM_H_

#include <stdint.h>
#include <stddef.h>

typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
#ifdef __OSCAR64C__
	typedef long i64;
	typedef unsigned long u64;
#else
	typedef int64_t i64;
	typedef uint64_t u64;
#endif

#ifndef _SIM_BOOL_DEFINED
#define _SIM_BOOL_DEFINED
	typedef u8 BOOL;
#endif
#ifndef FALSE
	enum { FALSE, TRUE };
#elif !defined(TRUE)
	#define TRUE 1
#endif

#ifdef CPU_TYPE
	typedef struct CPU_TYPE CPU_TYPE;
	#undef _CPUP
	#undef _CPUC
	#undef _CCPUP
	#undef _CCPUC
	#undef _CPUPA
	#undef _CPUCA
	#ifdef SINGLE_INST
		#define _CPUP
		#define _CPUC
		#define _CCPUP
		#define _CCPUC
		#define _CPUPA
		#define _CPUCA
		#define CPUGETSET_(C, c) \
			void c##_set(C* cpu); \
			C* c##_get(void); \
			const C* c##_ptr(void);
		#define CPUGETSET(C, c) CPUGETSET_(C, c)
		CPUGETSET(CPU_TYPE, cpu_type)
	#else
		#define _CPUP CPU_TYPE *cpu
		#define _CPUC CPU_TYPE *cpu,
		#define _CCPUP const CPU_TYPE *cpu
		#define _CCPUC const CPU_TYPE *cpu,
		#define _CPUPA cpu
		#define _CPUCA cpu,
	#endif
#endif

#ifdef COUNT_CYCLES
	#define CYCLES CPU_CYCLES
	#define CYC(n) cycles += n
	#define STEPSTART CYCLES cycles = 0;
	#define STEPRET cycles
#else
	#define CYCLES void
	#define CYC(n)
	#define STEPSTART
	#define STEPRET
#endif

#ifndef UNLIKELY
	#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
#ifndef LIKELY
	#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif

#if defined(__GNUC__) || defined(__clang__)
	#define INLINE static inline __attribute__((always_inline))
	#define ALIGNED(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
	#define INLINE static __forceinline
	#define ALIGNED(n) /* __declspec(align(n)) is prefix-only; not usable postfix */
#elif defined(__VBCC__) || defined(__OSCAR64C__)
	#define INLINE static
	#define ALIGNED(n)
#else
	#define INLINE static inline
	#define ALIGNED(n)
#endif

#ifdef __OSCAR64C__
	#define ZEROPAGE __zeropage
#elif defined(__VBCC__)
	#define ZEROPAGE /* vbcc: zero-page via __zpw/__zpb per-variable if needed */
#else
	#define ZEROPAGE
#endif

#endif /* _SIM_H_ */
