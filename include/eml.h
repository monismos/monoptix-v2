// bine/eml/eml.h — EML: Exact Math Language
// Precision-preserving mathematical operations for ICOR reduction

#ifndef BINE_EML_H
#define BINE_EML_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Exact rational number representation
// Stores numbers as numerator/denominator to avoid floating-point errors
typedef struct {
    int64_t numerator;      // Top of fraction
    int64_t denominator;    // Bottom of fraction (always positive)
    bool valid;             // True if properly initialized
} EMLRational;

// Extended precision float (80-bit extended precision)
typedef struct {
    uint64_t mantissa;      // 64-bit mantissa
    uint16_t exponent;      // 15-bit exponent + 1 sign bit
    bool valid;
} EMLEFloat;

// Exact complex number
typedef struct {
    EMLRational real;
    EMLRational imag;
} EMLComplex;

// Interval arithmetic bounds
typedef struct {
    EMLRational lower;      // Lower bound (inclusive)
    EMLRational upper;      // Upper bound (inclusive)
    bool valid;             // True if bounds are valid
} EMLInterval;

// Operation types for ICOR integration
typedef enum {
    EML_OP_ADD,             // Addition: a + b
    EML_OP_SUB,             // Subtraction: a - b
    EML_OP_MUL,             // Multiplication: a * b
    EML_OP_DIV,             // Division: a / b (exact when possible)
    EML_OP_SQRT,            // Square root (exact for perfect squares)
    EML_OP_POW,             // Power: a^b
    EML_OP_LOG,             // Logarithm
    EML_OP_SIN,             // Sine (symbolic when exact)
    EML_OP_COS,             // Cosine (symbolic when exact)
    EML_OP_EXP,             // Exponential
    EML_OP_ABS,             // Absolute value
    EML_OP_MIN,             // Minimum of two
    EML_OP_MAX,             // Maximum of two
    EML_OP_CMP,             // Comparison (returns -1, 0, 1)
} EMLOperation;

// Create rational from integers
EMLRational eml_rational(int64_t num, int64_t den);

// Create rational from double (best approximation)
EMLRational eml_rational_from_double(double val);

// Convert rational to double (may lose precision)
double eml_rational_to_double(EMLRational r);

// Exact arithmetic operations
EMLRational eml_add(EMLRational a, EMLRational b);
EMLRational eml_sub(EMLRational a, EMLRational b);
EMLRational eml_mul(EMLRational a, EMLRational b);
EMLRational eml_div(EMLRational a, EMLRational b);

// Normalize rational (reduce to lowest terms)
EMLRational eml_normalize(EMLRational r);

// Greatest common divisor
int64_t eml_gcd(int64_t a, int64_t b);

// Least common multiple
int64_t eml_lcm(int64_t a, int64_t b);

// Exact comparison: -1 if a < b, 0 if equal, 1 if a > b
int eml_compare(EMLRational a, EMLRational b);

// Check if rational represents an integer
bool eml_is_integer(EMLRational r);

// Exact square root (returns rational if perfect square, marks invalid otherwise)
EMLRational eml_sqrt_exact(EMLRational r);

// Interval arithmetic operations
EMLInterval eml_interval_add(EMLInterval a, EMLInterval b);
EMLInterval eml_interval_mul(EMLInterval a, EMLInterval b);
EMLInterval eml_interval_div(EMLInterval a, EMLInterval b);

// Check if interval contains zero
bool eml_interval_contains_zero(EMLInterval i);

// Check if two intervals overlap
bool eml_intervals_overlap(EMLInterval a, EMLInterval b);

// ICOR integration: convert EML operation to ICOR agent representation
uint64_t eml_to_icor_agent(EMLOperation op, EMLRational a, EMLRational b);

// ICOR integration: evaluate ICOR agent as EML operation
EMLRational eml_eval_icor_agent(uint64_t agent, EMLRational input);

// Precision tracking: get number of bits of precision in rational
uint32_t eml_precision_bits(EMLRational r);

// Check if rational can be exactly represented in IEEE 754
typedef enum {
    EML_PREC_EXACT_FLOAT32,   // Exact in 32-bit float
    EML_PREC_EXACT_FLOAT64,   // Exact in 64-bit double
    EML_PREC_INEXACT,         // Requires more precision
    EML_PREC_INFINITE,        // Infinite precision needed (irrational)
} EMLPrecision;

EMLPrecision eml_representability(EMLRational r);

// Constants
extern const EMLRational EML_ZERO;
extern const EMLRational EML_ONE;
extern const EMLRational EML_NEG_ONE;
extern const EMLRational EML_HALF;
extern const EMLRational EML_PI_APPROX;  // 355/113 - good approximation
extern const EMLRational EML_E_APPROX;   // 193/71 - good approximation

#ifdef __cplusplus
}
#endif

#endif // BINE_EML_H
