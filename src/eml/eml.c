// bine/eml/eml.c — EML: Exact Math Language Implementation
// Precision-preserving mathematical operations

#include "eml.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// Constants
const EMLRational EML_ZERO = {0, 1, true};
const EMLRational EML_ONE = {1, 1, true};
const EMLRational EML_NEG_ONE = {-1, 1, true};
const EMLRational EML_HALF = {1, 2, true};
const EMLRational EML_PI_APPROX = {355, 113, true};  // ~3.1415929
const EMLRational EML_E_APPROX = {193, 71, true};    // ~2.71831

// Greatest common divisor using Euclidean algorithm
int64_t eml_gcd(int64_t a, int64_t b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        int64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

// Least common multiple
int64_t eml_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    return (a / eml_gcd(a, b)) * b;
}

// Create rational from integers
EMLRational eml_rational(int64_t num, int64_t den) {
    EMLRational r;
    if (den == 0) {
        r.valid = false;
        return r;
    }
    r.numerator = num;
    r.denominator = den < 0 ? -den : den;  // Keep denominator positive
    if (den < 0) r.numerator = -r.numerator;
    r.valid = true;
    return eml_normalize(r);
}

// Create rational from double using continued fractions
EMLRational eml_rational_from_double(double val) {
    if (val == 0.0) return EML_ZERO;
    if (isnan(val) || isinf(val)) {
        EMLRational r = {0, 0, false};
        return r;
    }

    int sign = val < 0.0 ? -1 : 1;
    double target = fabs(val);
    double x = target;

    int64_t h_m2 = 0, h_m1 = 1;
    int64_t k_m2 = 1, k_m1 = 0;
    int64_t best_num = 0, best_den = 1;
    const int64_t max_den = 1000000000LL;

    for (int i = 0; i < 32; i++) {
        double floor_part = floor(x);
        if (floor_part > (double)(INT64_MAX / 4)) break;
        int64_t a = (int64_t)floor_part;

        if (h_m1 != 0 && a > (INT64_MAX - h_m2) / h_m1) break;
        if (k_m1 != 0 && a > (INT64_MAX - k_m2) / k_m1) break;

        int64_t h = a * h_m1 + h_m2;
        int64_t k = a * k_m1 + k_m2;
        if (k <= 0 || k > max_den) break;

        best_num = h;
        best_den = k;

        double approx = (double)best_num / (double)best_den;
        if (fabs(approx - target) < 1e-12) break;

        double frac = x - floor_part;
        if (fabs(frac) < 1e-15) break;
        x = 1.0 / frac;

        h_m2 = h_m1;
        h_m1 = h;
        k_m2 = k_m1;
        k_m1 = k;
    }

    return eml_rational(sign * best_num, best_den);
}

// Convert rational to double
double eml_rational_to_double(EMLRational r) {
    if (!r.valid || r.denominator == 0) return NAN;
    return (double)r.numerator / (double)r.denominator;
}

// Normalize rational (reduce to lowest terms)
EMLRational eml_normalize(EMLRational r) {
    if (!r.valid) return r;
    if (r.numerator == 0) {
        r.denominator = 1;
        return r;
    }
    
    int64_t g = eml_gcd(r.numerator, r.denominator);
    r.numerator /= g;
    r.denominator /= g;
    
    // Ensure denominator is positive
    if (r.denominator < 0) {
        r.denominator = -r.denominator;
        r.numerator = -r.numerator;
    }
    
    return r;
}

// Exact addition: a/b + c/d = (ad + bc) / bd
EMLRational eml_add(EMLRational a, EMLRational b) {
    if (!a.valid || !b.valid) {
        EMLRational r = {0, 0, false};
        return r;
    }
    
    int64_t num = a.numerator * b.denominator + b.numerator * a.denominator;
    int64_t den = a.denominator * b.denominator;
    return eml_rational(num, den);
}

// Exact subtraction
EMLRational eml_sub(EMLRational a, EMLRational b) {
    if (!a.valid || !b.valid) {
        EMLRational r = {0, 0, false};
        return r;
    }
    
    int64_t num = a.numerator * b.denominator - b.numerator * a.denominator;
    int64_t den = a.denominator * b.denominator;
    return eml_rational(num, den);
}

// Exact multiplication: a/b * c/d = ac / bd
EMLRational eml_mul(EMLRational a, EMLRational b) {
    if (!a.valid || !b.valid) {
        EMLRational r = {0, 0, false};
        return r;
    }
    
    int64_t num = a.numerator * b.numerator;
    int64_t den = a.denominator * b.denominator;
    return eml_rational(num, den);
}

// Exact division: (a/b) / (c/d) = ad / bc
EMLRational eml_div(EMLRational a, EMLRational b) {
    if (!a.valid || !b.valid || b.numerator == 0) {
        EMLRational r = {0, 0, false};
        return r;
    }
    
    int64_t num = a.numerator * b.denominator;
    int64_t den = a.denominator * b.numerator;
    return eml_rational(num, den);
}

// Exact comparison
int eml_compare(EMLRational a, EMLRational b) {
    if (!a.valid || !b.valid) return 0;
    
    // Compare a/b vs c/d by cross-multiplying: compare ad vs bc
    int64_t lhs = a.numerator * b.denominator;
    int64_t rhs = b.numerator * a.denominator;
    
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

// Check if rational represents an integer
bool eml_is_integer(EMLRational r) {
    if (!r.valid) return false;
    return r.denominator == 1 || (r.numerator % r.denominator == 0);
}

// Exact square root for perfect squares
EMLRational eml_sqrt_exact(EMLRational r) {
    if (!r.valid || r.numerator < 0) {
        EMLRational result = {0, 0, false};
        return result;
    }
    
    // Check if numerator and denominator are perfect squares
    int64_t num_sqrt = (int64_t)sqrt((double)r.numerator);
    int64_t den_sqrt = (int64_t)sqrt((double)r.denominator);
    
    // Verify they are perfect squares
    if (num_sqrt * num_sqrt == r.numerator && den_sqrt * den_sqrt == r.denominator) {
        return eml_rational(num_sqrt, den_sqrt);
    }
    
    // Not a perfect square - return invalid (would need to be symbolic)
    EMLRational result = {0, 0, false};
    return result;
}

// Interval arithmetic: addition
EMLInterval eml_interval_add(EMLInterval a, EMLInterval b) {
    EMLInterval result;
    if (!a.valid || !b.valid) {
        result.valid = false;
        return result;
    }
    
    result.lower = eml_add(a.lower, b.lower);
    result.upper = eml_add(a.upper, b.upper);
    result.valid = result.lower.valid && result.upper.valid;
    return result;
}

// Interval arithmetic: multiplication
EMLInterval eml_interval_mul(EMLInterval a, EMLInterval b) {
    EMLInterval result;
    if (!a.valid || !b.valid) {
        result.valid = false;
        return result;
    }
    
    // Compute all four products
    EMLRational p1 = eml_mul(a.lower, b.lower);
    EMLRational p2 = eml_mul(a.lower, b.upper);
    EMLRational p3 = eml_mul(a.upper, b.lower);
    EMLRational p4 = eml_mul(a.upper, b.upper);
    
    // Find min and max
    result.lower = p1;
    result.upper = p1;
    
    EMLRational products[4] = {p1, p2, p3, p4};
    for (int i = 1; i < 4; i++) {
        if (eml_compare(products[i], result.lower) < 0) {
            result.lower = products[i];
        }
        if (eml_compare(products[i], result.upper) > 0) {
            result.upper = products[i];
        }
    }
    
    result.valid = true;
    return result;
}

// Interval division
EMLInterval eml_interval_div(EMLInterval a, EMLInterval b) {
    EMLInterval result;
    if (!a.valid || !b.valid) {
        result.valid = false;
        return result;
    }
    
    // Check if divisor interval contains zero
    if (eml_interval_contains_zero(b)) {
        result.valid = false;  // Division by zero possible
        return result;
    }
    
    // Similar to multiplication but with reciprocals
    EMLRational recip_lower = eml_div(EML_ONE, b.upper);
    EMLRational recip_upper = eml_div(EML_ONE, b.lower);
    
    EMLInterval recip = {recip_lower, recip_upper, true};
    return eml_interval_mul(a, recip);
}

// Check if interval contains zero
bool eml_interval_contains_zero(EMLInterval i) {
    if (!i.valid) return false;
    return eml_compare(i.lower, EML_ZERO) <= 0 && 
           eml_compare(i.upper, EML_ZERO) >= 0;
}

// Check if intervals overlap
bool eml_intervals_overlap(EMLInterval a, EMLInterval b) {
    if (!a.valid || !b.valid) return false;
    return eml_compare(a.lower, b.upper) <= 0 && 
           eml_compare(b.lower, a.upper) <= 0;
}

// Get number of bits of precision
uint32_t eml_precision_bits(EMLRational r) {
    if (!r.valid) return 0;
    
    // Count bits needed to represent numerator and denominator
    uint64_t num_abs = r.numerator < 0 ? -r.numerator : r.numerator;
    uint64_t den = r.denominator;
    
    uint32_t bits = 0;
    while (num_abs > 0 || den > 0) {
        num_abs >>= 1;
        den >>= 1;
        bits++;
    }
    
    return bits;
}

// Check representability in IEEE 754
EMLPrecision eml_representability(EMLRational r) {
    if (!r.valid) return EML_PREC_INEXACT;
    
    uint32_t bits = eml_precision_bits(r);
    
    if (bits <= 24) return EML_PREC_EXACT_FLOAT32;  // Fits in float mantissa
    if (bits <= 53) return EML_PREC_EXACT_FLOAT64;  // Fits in double mantissa
    return EML_PREC_INEXACT;
}

// Convert EML operation to ICOR agent representation
uint64_t eml_to_icor_agent(EMLOperation op, EMLRational a, EMLRational b) {
    // Pack operation type and operands into 64-bit agent
    // Format: [2 bits op category][4 bits op type][29 bits operand A][29 bits operand B]
    
    uint64_t op_code = (uint64_t)op & 0xF;
    uint64_t packed_a = ((uint64_t)(a.numerator & 0x1FFFFFFF)) << 4;
    uint64_t packed_b = ((uint64_t)(b.numerator & 0x1FFFFFFF)) << 33;
    
    return 0x8000000000000000ULL | (op_code << 60) | packed_a | packed_b;
}

// Evaluate ICOR agent as EML operation
EMLRational eml_eval_icor_agent(uint64_t agent, EMLRational input) {
    if ((agent & 0x8000000000000000ULL) == 0) {
        // Not an EML agent
        return EML_ZERO;
    }
    
    EMLOperation op = (EMLOperation)((agent >> 60) & 0xF);
    EMLRational operand_a = eml_rational((int64_t)((agent >> 4) & 0x1FFFFFFF), 1);
    
    switch (op) {
        case EML_OP_ADD: return eml_add(input, operand_a);
        case EML_OP_SUB: return eml_sub(input, operand_a);
        case EML_OP_MUL: return eml_mul(input, operand_a);
        case EML_OP_DIV: return eml_div(input, operand_a);
        case EML_OP_ABS: {
            EMLRational r = input;
            if (r.numerator < 0) r.numerator = -r.numerator;
            return r;
        }
        default: return input;
    }
}
