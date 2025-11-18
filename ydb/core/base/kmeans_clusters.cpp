#include "kmeans_clusters.h"

#include <library/cpp/dot_product/dot_product.h>
#include <library/cpp/l1_distance/l1_distance.h>
#include <library/cpp/l2_distance/l2_distance.h>
#include <ydb/library/yql/udfs/common/knn/knn-defines.h>
#include <ydb/library/yql/udfs/common/knn/knn-distance.h>
#include <ydb/library/yql/udfs/common/knn/knn-serializer-shared.h>

#include <span>

#include <immintrin.h>

namespace NKikimr::NKMeans {

namespace {

    Y_FORCE_INLINE
    static void TwoWayDotProductIteration(__m128& sumLR, __m128& sumRR, const __m128 a, const __m128 b) {
        sumLR = _mm_add_ps(sumLR, _mm_mul_ps(a, b));
        sumRR = _mm_add_ps(sumRR, _mm_mul_ps(b, b));
    }

    static TTriWayDotProduct<float> TwoWayDotProductImpl(const float* lhs, const float* rhs, size_t length, float ll) noexcept {
        __m128 sumLR1 = _mm_setzero_ps();
        __m128 sumRR1 = _mm_setzero_ps();
        __m128 sumLR2 = _mm_setzero_ps();
        __m128 sumRR2 = _mm_setzero_ps();

        while (length >= 8) {
            TwoWayDotProductIteration(sumLR1, sumRR1, _mm_loadu_ps(lhs + 0), _mm_loadu_ps(rhs + 0));
            TwoWayDotProductIteration(sumLR2, sumRR2, _mm_loadu_ps(lhs + 4), _mm_loadu_ps(rhs + 4));
            length -= 8;
            lhs += 8;
            rhs += 8;
        }

        if (length >= 4) {
            TwoWayDotProductIteration(sumLR1, sumRR1, _mm_loadu_ps(lhs + 0), _mm_loadu_ps(rhs + 0));
            length -= 4;
            lhs += 4;
            rhs += 4;
        }

        sumLR1 = _mm_add_ps(sumLR1, sumLR2);
        sumRR1 = _mm_add_ps(sumRR1, sumRR2);

        if (length) {
            __m128 a, b;
            switch (length) {
                case 3:
                    a = _mm_set_ps(0.0f, lhs[2], lhs[1], lhs[0]);
                    b = _mm_set_ps(0.0f, rhs[2], rhs[1], rhs[0]);
                    break;
                case 2:
                    a = _mm_set_ps(0.0f, 0.0f, lhs[1], lhs[0]);
                    b = _mm_set_ps(0.0f, 0.0f, rhs[1], rhs[0]);
                    break;
                case 1:
                    a = _mm_set_ps(0.0f, 0.0f, 0.0f, lhs[0]);
                    b = _mm_set_ps(0.0f, 0.0f, 0.0f, rhs[0]);
                    break;
                default:
                    Y_UNREACHABLE();
            }
            TwoWayDotProductIteration(sumLR1, sumRR1, a, b);
        }

        // TODO: faster trick?

        __m128 sumLL128 = _mm_set_ps(0.0f, 0.0f, 0.0f, ll);

        __m128 t0 = sumLL128;
        __m128 t1 = sumLR1;
        __m128 t2 = sumRR1;
        __m128 t3 = _mm_setzero_ps();
        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        t0 = _mm_add_ps(t0, t1);
        t0 = _mm_add_ps(t0, t2);
        t0 = _mm_add_ps(t0, t3);

        alignas(16) float res[4];
        _mm_store_ps(res, t0);
        TTriWayDotProduct<float> result{res[0], res[1], res[2]};
        return result;
    }

    Y_FORCE_INLINE
    static void TriWayDotProductIterationAvx2(__m256& sumLL, __m256& sumLR, __m256& sumRR, const __m256 a, const __m256 b) {
        sumLL = _mm256_fmadd_ps(a, a, sumLL);
        sumLR = _mm256_fmadd_ps(a, b, sumLR);
        sumRR = _mm256_fmadd_ps(b, b, sumRR);
    }

    Y_FORCE_INLINE
    static void TwoWayDotProductIterationAvx2(__m256& sumLR, __m256& sumRR, const __m256 a, const __m256 b) {
        sumLR = _mm256_fmadd_ps(a, b, sumLR);
        sumRR = _mm256_fmadd_ps(b, b, sumRR);
    }

    static TTriWayDotProduct<float>
    TriWayDotProductImplAvx2(const float* lhs, const float* rhs, size_t length) noexcept
    {
        __m256 sumLL1 = _mm256_setzero_ps();
        __m256 sumLR1 = _mm256_setzero_ps();
        __m256 sumRR1 = _mm256_setzero_ps();
        __m256 sumLL2 = _mm256_setzero_ps();
        __m256 sumLR2 = _mm256_setzero_ps();
        __m256 sumRR2 = _mm256_setzero_ps();

        // Main AVX2 loop: process 16 floats per iteration (2 * 8)
        while (length >= 16) {
            TriWayDotProductIterationAvx2(sumLL1, sumLR1, sumRR1, _mm256_loadu_ps(lhs + 0), _mm256_loadu_ps(rhs + 0));
            TriWayDotProductIterationAvx2(sumLL2, sumLR2, sumRR2, _mm256_loadu_ps(lhs + 8), _mm256_loadu_ps(rhs + 8));
            length -= 16;
            lhs += 16;
            rhs += 16;
        }

        // Handle one more 8-float block if present
        if (length >= 8) {
            TriWayDotProductIterationAvx2(sumLL1, sumLR1, sumRR1, _mm256_loadu_ps(lhs + 0), _mm256_loadu_ps(rhs + 0));
            length -= 8;
            lhs += 8;
            rhs += 8;
        }

        // Merge the two accumulators
        sumLL1 = _mm256_add_ps(sumLL1, sumLL2);
        sumLR1 = _mm256_add_ps(sumLR1, sumLR2);
        sumRR1 = _mm256_add_ps(sumRR1, sumRR2);

        // Tail < 8 floats
        if (length) {
            __m256 a, b;
            switch (length) {
                case 7:
                    a = _mm256_set_ps(0.0f,
                                    lhs[6], lhs[5], lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f,
                                    rhs[6], rhs[5], rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 6:
                    a = _mm256_set_ps(0.0f, 0.0f,
                                    lhs[5], lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f,
                                    rhs[5], rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 5:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f,
                                    lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f,
                                    rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 4:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 3:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f,
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f,
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 2:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f,
                                    lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f,
                                    rhs[1], rhs[0]);
                    break;
                case 1:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f,
                                    lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f,
                                    rhs[0]);
                    break;
                default:
                    Y_UNREACHABLE();
            }
            TriWayDotProductIterationAvx2(sumLL1, sumLR1, sumRR1, a, b);
        }

        // Collapse 256-bit sums into 128-bit sums (4-lane partials)
        __m128 sumLL128 = _mm_add_ps(_mm256_castps256_ps128(sumLL1),
                                    _mm256_extractf128_ps(sumLL1, 1));
        __m128 sumLR128 = _mm_add_ps(_mm256_castps256_ps128(sumLR1),
                                    _mm256_extractf128_ps(sumLR1, 1));
        __m128 sumRR128 = _mm_add_ps(_mm256_castps256_ps128(sumRR1),
                                    _mm256_extractf128_ps(sumRR1, 1));

        // Transpose+add trick
        __m128 t0 = sumLL128;
        __m128 t1 = sumLR128;
        __m128 t2 = sumRR128;
        __m128 t3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        t0 = _mm_add_ps(t0, t1);
        t0 = _mm_add_ps(t0, t2);
        t0 = _mm_add_ps(t0, t3);

        alignas(16) float res[4];
        _mm_store_ps(res, t0);

        TTriWayDotProduct<float> result{res[0], res[1], res[2]};
        return result;
    }

    static TTriWayDotProduct<float>
    TwoWayDotProductImplAvx2(const float* lhs, const float* rhs, size_t length, float ll) noexcept
    {
        __m256 sumLR1 = _mm256_setzero_ps();
        __m256 sumRR1 = _mm256_setzero_ps();
        __m256 sumLR2 = _mm256_setzero_ps();
        __m256 sumRR2 = _mm256_setzero_ps();

        // Main AVX2 loop: process 16 floats per iteration (2 * 8)
        while (length >= 16) {
            TwoWayDotProductIterationAvx2(sumLR1, sumRR1, _mm256_loadu_ps(lhs + 0), _mm256_loadu_ps(rhs + 0));
            TwoWayDotProductIterationAvx2(sumLR2, sumRR2, _mm256_loadu_ps(lhs + 8), _mm256_loadu_ps(rhs + 8));
            length -= 16;
            lhs += 16;
            rhs += 16;
        }

        // Handle one more 8-float block if present
        if (length >= 8) {
            TwoWayDotProductIterationAvx2(sumLR1, sumRR1, _mm256_loadu_ps(lhs + 0), _mm256_loadu_ps(rhs + 0));
            length -= 8;
            lhs += 8;
            rhs += 8;
        }

        // Merge the two accumulators
        sumLR1 = _mm256_add_ps(sumLR1, sumLR2);
        sumRR1 = _mm256_add_ps(sumRR1, sumRR2);

        // Tail < 8 floats
        if (length) {
            __m256 a, b;
            switch (length) {
                case 7:
                    a = _mm256_set_ps(0.0f,
                                    lhs[6], lhs[5], lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f,
                                    rhs[6], rhs[5], rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 6:
                    a = _mm256_set_ps(0.0f, 0.0f,
                                    lhs[5], lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f,
                                    rhs[5], rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 5:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f,
                                    lhs[4],
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f,
                                    rhs[4],
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 4:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[3], lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[3], rhs[2], rhs[1], rhs[0]);
                    break;
                case 3:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f,
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f,
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 2:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f,
                                    lhs[1], lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f,
                                    rhs[1], rhs[0]);
                    break;
                case 1:
                    a = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f,
                                    lhs[0]);
                    b = _mm256_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f,
                                    rhs[0]);
                    break;
                default:
                    Y_UNREACHABLE();
            }
            TwoWayDotProductIterationAvx2(sumLR1, sumRR1, a, b);
        }

        // Collapse 256-bit sums into 128-bit sums (4-lane partials),
        // preserving the "4-lane layout" logic used by the transpose trick.
        __m128 sumLR128 = _mm_add_ps(_mm256_castps256_ps128(sumLR1),
                                    _mm256_extractf128_ps(sumLR1, 1));
        __m128 sumRR128 = _mm_add_ps(_mm256_castps256_ps128(sumRR1),
                                    _mm256_extractf128_ps(sumRR1, 1));

        // LL: put ll into lane 0, others zero
        __m128 sumLL128 = _mm_set_ps(0.0f, 0.0f, 0.0f, ll);

        // Same transpose+add trick as before
        __m128 t0 = sumLL128;
        __m128 t1 = sumLR128;
        __m128 t2 = sumRR128;
        __m128 t3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        t0 = _mm_add_ps(t0, t1);
        t0 = _mm_add_ps(t0, t2);
        t0 = _mm_add_ps(t0, t3);

        alignas(16) float res[4];
        _mm_store_ps(res, t0);

        TTriWayDotProduct<float> result{res[0], res[1], res[2]};
        return result;
    }

    Y_FORCE_INLINE
    static void TriWayDotProductIterationAvx512(__m512& sumLL, __m512& sumLR, __m512& sumRR, const __m512 a, const __m512 b) {
        sumLL = _mm512_fmadd_ps(a, a, sumLL);
        sumLR = _mm512_fmadd_ps(a, b, sumLR);
        sumRR = _mm512_fmadd_ps(b, b, sumRR);
    }

    Y_FORCE_INLINE
    static void TwoWayDotProductIterationAvx512(__m512& sumLR, __m512& sumRR, const __m512 a, const __m512 b) {
        sumLR = _mm512_fmadd_ps(a, b, sumLR);
        sumRR = _mm512_fmadd_ps(b, b, sumRR);
    }

    static TTriWayDotProduct<float>
    TriWayDotProductImplAvx512(const float* lhs, const float* rhs, size_t length) noexcept
    {
        __m512 sumLL1 = _mm512_setzero_ps();
        __m512 sumLR1 = _mm512_setzero_ps();
        __m512 sumRR1 = _mm512_setzero_ps();
        __m512 sumLL2 = _mm512_setzero_ps();
        __m512 sumLR2 = _mm512_setzero_ps();
        __m512 sumRR2 = _mm512_setzero_ps();

        // Main AVX-512 loop: process 32 floats per iteration (2 * 16)
        while (length >= 32) {
            TriWayDotProductIterationAvx512(sumLL1, sumLR1, sumRR1, _mm512_loadu_ps(lhs + 0), _mm512_loadu_ps(rhs + 0));
            TriWayDotProductIterationAvx512(sumLL2, sumLR2, sumRR2, _mm512_loadu_ps(lhs + 16), _mm512_loadu_ps(rhs + 16));
            length -= 32;
            lhs += 32;
            rhs += 32;
        }

        // Handle one more 16-float block if present
        if (length >= 16) {
            TriWayDotProductIterationAvx512(sumLL1, sumLR1, sumRR1, _mm512_loadu_ps(lhs + 0), _mm512_loadu_ps(rhs + 0));
            length -= 16;
            lhs += 16;
            rhs += 16;
        }

        // Merge the two accumulators
        sumLL1 = _mm512_add_ps(sumLL1, sumLL2);
        sumLR1 = _mm512_add_ps(sumLR1, sumLR2);
        sumRR1 = _mm512_add_ps(sumRR1, sumRR2);

        // Tail < 16 floats
        if (length) {
            __m512 a, b;
            switch (length) {
                case 15:
                    a = _mm512_set_ps(0.0f,
                                    lhs[14], lhs[13], lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f,
                                    rhs[14], rhs[13], rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 14:
                    a = _mm512_set_ps(0.0f, 0.0f,
                                    lhs[13], lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f,
                                    rhs[13], rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 13:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f,
                                    lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f,
                                    rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 12:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 11:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 10:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 9:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 8:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 7:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 6:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 5:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 4:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 3:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 2:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[1], rhs[0]);
                    break;
                case 1:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[0]);
                    break;
                default:
                    Y_UNREACHABLE();
            }
            TriWayDotProductIterationAvx512(sumLL1, sumLR1, sumRR1, a, b);
        }

        // Collapse 512-bit sums into 128-bit sums (4-lane partials)
        // Extract 4 chunks of 128 bits and sum them
        __m128 sumLL128_0 = _mm512_castps512_ps128(sumLL1);
        __m128 sumLL128_1 = _mm512_extractf32x4_ps(sumLL1, 1);
        __m128 sumLL128_2 = _mm512_extractf32x4_ps(sumLL1, 2);
        __m128 sumLL128_3 = _mm512_extractf32x4_ps(sumLL1, 3);

        __m128 sumLR128_0 = _mm512_castps512_ps128(sumLR1);
        __m128 sumLR128_1 = _mm512_extractf32x4_ps(sumLR1, 1);
        __m128 sumLR128_2 = _mm512_extractf32x4_ps(sumLR1, 2);
        __m128 sumLR128_3 = _mm512_extractf32x4_ps(sumLR1, 3);

        __m128 sumRR128_0 = _mm512_castps512_ps128(sumRR1);
        __m128 sumRR128_1 = _mm512_extractf32x4_ps(sumRR1, 1);
        __m128 sumRR128_2 = _mm512_extractf32x4_ps(sumRR1, 2);
        __m128 sumRR128_3 = _mm512_extractf32x4_ps(sumRR1, 3);

        __m128 sumLL128 = _mm_add_ps(_mm_add_ps(sumLL128_0, sumLL128_1),
                                     _mm_add_ps(sumLL128_2, sumLL128_3));
        __m128 sumLR128 = _mm_add_ps(_mm_add_ps(sumLR128_0, sumLR128_1),
                                     _mm_add_ps(sumLR128_2, sumLR128_3));
        __m128 sumRR128 = _mm_add_ps(_mm_add_ps(sumRR128_0, sumRR128_1),
                                     _mm_add_ps(sumRR128_2, sumRR128_3));

        // Transpose+add trick
        __m128 t0 = sumLL128;
        __m128 t1 = sumLR128;
        __m128 t2 = sumRR128;
        __m128 t3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        t0 = _mm_add_ps(t0, t1);
        t0 = _mm_add_ps(t0, t2);
        t0 = _mm_add_ps(t0, t3);

        alignas(16) float res[4];
        _mm_store_ps(res, t0);

        TTriWayDotProduct<float> result{res[0], res[1], res[2]};
        return result;
    }

    static TTriWayDotProduct<float>
    TwoWayDotProductImplAvx512(const float* lhs, const float* rhs, size_t length, float ll) noexcept
    {
        __m512 sumLR1 = _mm512_setzero_ps();
        __m512 sumRR1 = _mm512_setzero_ps();
        __m512 sumLR2 = _mm512_setzero_ps();
        __m512 sumRR2 = _mm512_setzero_ps();

        // Main AVX-512 loop: process 32 floats per iteration (2 * 16)
        while (length >= 32) {
            TwoWayDotProductIterationAvx512(sumLR1, sumRR1, _mm512_loadu_ps(lhs + 0), _mm512_loadu_ps(rhs + 0));
            TwoWayDotProductIterationAvx512(sumLR2, sumRR2, _mm512_loadu_ps(lhs + 16), _mm512_loadu_ps(rhs + 16));
            length -= 32;
            lhs += 32;
            rhs += 32;
        }

        // Handle one more 16-float block if present
        if (length >= 16) {
            TwoWayDotProductIterationAvx512(sumLR1, sumRR1, _mm512_loadu_ps(lhs + 0), _mm512_loadu_ps(rhs + 0));
            length -= 16;
            lhs += 16;
            rhs += 16;
        }

        // Merge the two accumulators
        sumLR1 = _mm512_add_ps(sumLR1, sumLR2);
        sumRR1 = _mm512_add_ps(sumRR1, sumRR2);

        // Tail < 16 floats
        if (length) {
            __m512 a, b;
            switch (length) {
                case 15:
                    a = _mm512_set_ps(0.0f,
                                    lhs[14], lhs[13], lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f,
                                    rhs[14], rhs[13], rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 14:
                    a = _mm512_set_ps(0.0f, 0.0f,
                                    lhs[13], lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f,
                                    rhs[13], rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 13:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f,
                                    lhs[12], lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f,
                                    rhs[12], rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 12:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[11],
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[11],
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 11:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[10], lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[10], rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 10:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[9], lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[9], rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 9:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[8], lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[8], rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 8:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[7],
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[7],
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 7:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[6], lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[6], rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 6:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[5], lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[5], rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 5:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[4], lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[4], rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 4:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[3],
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[3],
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 3:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[2], lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[2], rhs[1], rhs[0]);
                    break;
                case 2:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[1], lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[1], rhs[0]);
                    break;
                case 1:
                    a = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    lhs[0]);
                    b = _mm512_set_ps(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    rhs[0]);
                    break;
                default:
                    Y_UNREACHABLE();
            }
            TwoWayDotProductIterationAvx512(sumLR1, sumRR1, a, b);
        }

        // Collapse 512-bit sums into 128-bit sums (4-lane partials)
        // Extract 4 chunks of 128 bits (lanes 0-3, 4-7, 8-11, 12-15) and sum them
        __m128 sumLR128_0 = _mm512_castps512_ps128(sumLR1);           // lanes 0-3
        __m128 sumLR128_1 = _mm512_extractf32x4_ps(sumLR1, 1);        // lanes 4-7
        __m128 sumLR128_2 = _mm512_extractf32x4_ps(sumLR1, 2);        // lanes 8-11
        __m128 sumLR128_3 = _mm512_extractf32x4_ps(sumLR1, 3);        // lanes 12-15

        __m128 sumRR128_0 = _mm512_castps512_ps128(sumRR1);           // lanes 0-3
        __m128 sumRR128_1 = _mm512_extractf32x4_ps(sumRR1, 1);        // lanes 4-7
        __m128 sumRR128_2 = _mm512_extractf32x4_ps(sumRR1, 2);        // lanes 8-11
        __m128 sumRR128_3 = _mm512_extractf32x4_ps(sumRR1, 3);        // lanes 12-15

        __m128 sumLR128 = _mm_add_ps(_mm_add_ps(sumLR128_0, sumLR128_1),
                                     _mm_add_ps(sumLR128_2, sumLR128_3));
        __m128 sumRR128 = _mm_add_ps(_mm_add_ps(sumRR128_0, sumRR128_1),
                                     _mm_add_ps(sumRR128_2, sumRR128_3));

        // LL: put ll into lane 0, others zero
        __m128 sumLL128 = _mm_set_ps(0.0f, 0.0f, 0.0f, ll);

        // Same transpose+add trick as before
        __m128 t0 = sumLL128;
        __m128 t1 = sumLR128;
        __m128 t2 = sumRR128;
        __m128 t3 = _mm_setzero_ps();

        _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
        t0 = _mm_add_ps(t0, t1);
        t0 = _mm_add_ps(t0, t2);
        t0 = _mm_add_ps(t0, t3);

        alignas(16) float res[4];
        _mm_store_ps(res, t0);

        TTriWayDotProduct<float> result{res[0], res[1], res[2]};
        return result;
    }

    double CosineSimilarityTwoWay(const TStringBuf cluster, const TStringBuf embedding, double ll = 0) {
        const TArrayRef<const float> lhs = GetArray<float>(cluster);
        const TArrayRef<const float> rhs = GetArray<float>(embedding);

        TTriWayDotProduct<float> res = TwoWayDotProductImpl(lhs.data(), rhs.data(), lhs.size(), ll);

        const auto norm = std::sqrt(res.LL * res.RR); // TODO: faster sqrt?
        if (Y_LIKELY(norm != 0)) {
            return res.LR / norm;
        }

        return res.LR;
    }

    double CosineSimilarityTwoWayAvx2(const TStringBuf cluster, const TStringBuf embedding, double ll = 0) {
        const TArrayRef<const float> lhs = GetArray<float>(cluster);
        const TArrayRef<const float> rhs = GetArray<float>(embedding);

        TTriWayDotProduct<float> res = TwoWayDotProductImplAvx2(lhs.data(), rhs.data(), lhs.size(), ll);

        const auto norm = std::sqrt(res.LL * res.RR); // TODO: faster sqrt?
        if (Y_LIKELY(norm != 0)) {
            return res.LR / norm;
        }

        return res.LR;
    }

    double CosineSimilarityTwoWayAvx512(const TStringBuf cluster, const TStringBuf embedding, double ll = 0) {
        const TArrayRef<const float> lhs = GetArray<float>(cluster);
        const TArrayRef<const float> rhs = GetArray<float>(embedding);

        TTriWayDotProduct<float> res = TwoWayDotProductImplAvx512(lhs.data(), rhs.data(), lhs.size(), ll);

        const auto norm = std::sqrt(res.LL * res.RR); // TODO: faster sqrt?
        if (Y_LIKELY(norm != 0)) {
            return res.LR / norm;
        }

        return res.LR;
    }

    double CosineSimilarityTriWayAvx2(const TStringBuf cluster, const TStringBuf embedding) {
        const TArrayRef<const float> lhs = GetArray<float>(cluster);
        const TArrayRef<const float> rhs = GetArray<float>(embedding);

        TTriWayDotProduct<float> res = TriWayDotProductImplAvx2(lhs.data(), rhs.data(), lhs.size());

        const auto norm = std::sqrt(res.LL * res.RR); // TODO: faster sqrt?
        if (Y_LIKELY(norm != 0)) {
            return res.LR / norm;
        }

        return res.LR;
    }

    double CosineSimilarityTriWayAvx512(const TStringBuf cluster, const TStringBuf embedding) {
        const TArrayRef<const float> lhs = GetArray<float>(cluster);
        const TArrayRef<const float> rhs = GetArray<float>(embedding);

        TTriWayDotProduct<float> res = TriWayDotProductImplAvx512(lhs.data(), rhs.data(), lhs.size());

        const auto norm = std::sqrt(res.LL * res.RR); // TODO: faster sqrt?
        if (Y_LIKELY(norm != 0)) {
            return res.LR / norm;
        }

        return res.LR;
    }

    constexpr ui64 MinVectorDimension = 1;
    constexpr ui64 MaxVectorDimension = 16384;
    constexpr ui64 MinLevels = 1;
    constexpr ui64 MaxLevels = 16;
    constexpr ui64 MinClusters = 2;
    constexpr ui64 MaxClusters = 2048;
    constexpr ui64 MaxClustersPowLevels = ui64(1) << 30;
    constexpr ui64 MaxVectorDimensionMultiplyClusters = ui64(4) << 20; // 4 bytes per dimension for float vector type ~= 16 MB

    bool ValidateSettingInRange(const TString& name, std::optional<ui64> value, ui64 minValue, ui64 maxValue, TString& error) {
        if (!value.has_value()) {
            error = TStringBuilder() << name << " should be set";
            return false;
        }

        if (minValue <= *value && *value <= maxValue) {
            return true;
        }

        error = TStringBuilder() << "Invalid " << name << ": " << *value << " should be between " << minValue << " and " << maxValue;
        return false;
    };

    Ydb::Table::VectorIndexSettings_Metric ParseDistance(const TString& distance_, TString& error) {
        const TString distance = to_lower(distance_);
        if (distance == "cosine")
            return Ydb::Table::VectorIndexSettings::DISTANCE_COSINE;
        else if (distance == "manhattan")
            return Ydb::Table::VectorIndexSettings::DISTANCE_MANHATTAN;
        else if (distance == "euclidean")
            return Ydb::Table::VectorIndexSettings::DISTANCE_EUCLIDEAN;
        else {
            error = TStringBuilder() << "Invalid distance: " << distance_;
            return Ydb::Table::VectorIndexSettings::METRIC_UNSPECIFIED;
        }
    };

    Ydb::Table::VectorIndexSettings_Metric ParseSimilarity(const TString& similarity_, TString& error) {
        const TString similarity = to_lower(similarity_);
        if (similarity == "cosine")
            return Ydb::Table::VectorIndexSettings::SIMILARITY_COSINE;
        else if (similarity == "inner_product")
            return Ydb::Table::VectorIndexSettings::SIMILARITY_INNER_PRODUCT;
        else {
            error = TStringBuilder() << "Invalid similarity: " << similarity_;
            return Ydb::Table::VectorIndexSettings::METRIC_UNSPECIFIED;
        }
    };

    Ydb::Table::VectorIndexSettings_VectorType ParseVectorType(const TString& vectorType_, TString& error) {
        const TString vectorType = to_lower(vectorType_);
        if (vectorType == "float")
            return Ydb::Table::VectorIndexSettings::VECTOR_TYPE_FLOAT;
        else if (vectorType == "uint8")
            return Ydb::Table::VectorIndexSettings::VECTOR_TYPE_UINT8;
        else if (vectorType == "int8")
            return Ydb::Table::VectorIndexSettings::VECTOR_TYPE_INT8;
        else if (vectorType == "bit")
            return Ydb::Table::VectorIndexSettings::VECTOR_TYPE_BIT;
        else {
            error = TStringBuilder() << "Invalid vector_type: " << vectorType_;
            return Ydb::Table::VectorIndexSettings::VECTOR_TYPE_UNSPECIFIED;
        }
    };

    ui32 ParseUInt32(const TString& name, const TString& value, ui64 minValue, ui64 maxValue, TString& error) {
        ui32 result = 0;
        if (!TryFromString(value, result)) {
            error = TStringBuilder() << "Invalid " << name << ": " << value;
            return result;
        }
        ValidateSettingInRange(name, result, minValue, maxValue, error);
        return result;
    }
}

// TODO(mbkkt) maybe compute floating sum in double? Needs benchmark
template <typename TCoord>
struct TMetric {
    using TCoord_ = TCoord;
    using TSum = std::conditional_t<std::is_floating_point_v<TCoord>, double, i64>;
};

template <typename TCoord>
struct TCosineDistance : TMetric<TCoord> {
    using TSum = typename TMetric<TCoord>::TSum;
    // double used to avoid precision issues
    using TRes = double;

    static TRes Init()
    {
        return std::numeric_limits<TRes>::max();
    }

    static auto Distance(const TStringBuf cluster, const TStringBuf embedding, double ll = 0)
    {
        Y_UNUSED(CosineSimilarityTwoWay);
        Y_UNUSED(CosineSimilarityTwoWayAvx2);
        Y_UNUSED(CosineSimilarityTwoWayAvx512);
        Y_UNUSED(ll);
        Y_UNUSED(CosineSimilarityTriWayAvx2);
        if (1) {
            const TRes similarity = CosineSimilarityTriWayAvx512(cluster, embedding);
            return 1 - similarity;
        }
        const TRes similarity = KnnDistance<TRes>::CosineSimilarity(cluster, embedding).value();
        return 1 - similarity;
    }
};

template <typename TCoord>
struct TL1Distance : TMetric<TCoord> {
    using TSum = typename TMetric<TCoord>::TSum;
    using TRes = std::conditional_t<std::is_floating_point_v<TCoord>, TCoord, ui64>;

    static TRes Init()
    {
        return std::numeric_limits<TRes>::max();
    }

    static auto Distance(const TStringBuf cluster, const TStringBuf embedding, double ll = 0)
    {
        Y_UNUSED(ll);
        const auto distance = KnnDistance<TRes>::ManhattanDistance(cluster, embedding).value();
        return distance;
    }
};

template <typename TCoord>
struct TL2Distance : TMetric<TCoord> {
    using TSum = typename TMetric<TCoord>::TSum;
    using TRes = std::conditional_t<std::is_floating_point_v<TCoord>, TCoord, ui64>;

    static TRes Init()
    {
        return std::numeric_limits<TRes>::max();
    }

    static auto Distance(const TStringBuf cluster, const TStringBuf embedding, double ll = 0)
    {
        Y_UNUSED(ll);
        const auto distance = KnnDistance<TRes>::EuclideanDistance(cluster, embedding).value();
        return distance;
    }
};

template <typename TCoord>
struct TMaxInnerProductSimilarity : TMetric<TCoord> {
    using TSum = typename TMetric<TCoord>::TSum;
    using TRes = std::conditional_t<std::is_floating_point_v<TCoord>, TCoord, i64>;

    static TRes Init()
    {
        return std::numeric_limits<TRes>::max();
    }

    static auto Distance(const TStringBuf cluster, const TStringBuf embedding, double ll = 0)
    {
        Y_UNUSED(ll);
        const TRes similarity = KnnDistance<TRes>::DotProduct(cluster, embedding).value();
        return -similarity;
    }
};

template <typename TMetric>
class TClusters: public IClusters {
    // If less than 1% of vectors are reassigned to new clusters we want to stop
    static constexpr double MinVectorsNeedsReassigned = 0.01;

    using TCoord = TMetric::TCoord_;
    using TSum = TMetric::TSum;
    using TEmbedding = TVector<TSum>;

    const ui32 Dimensions = 0;
    const ui32 MaxRounds = 0;
    const ui8 FormatByte = 0;

    TVector<TString> Clusters;
    TVector<ui64> ClusterSizes;
    TVector<TEmbedding> NextClusters;
    TVector<ui64> NextClusterSizes;

    ui32 Round = 0;

public:
    TClusters(ui32 dimensions, ui32 maxRounds, ui8 formatByte)
        : Dimensions(dimensions)
        , MaxRounds(maxRounds)
        , FormatByte(formatByte)
    {
    }

    void SetRound(ui32 round) override {
        Round = round;
    }

    TString Debug() const override {
        auto sb = TStringBuilder() << "K: " << Clusters.size();
        if (MaxRounds) {
            sb << " Round: " << Round << " / " << MaxRounds;
        }
        return sb;
    }

    const TVector<TString>& GetClusters() const override {
        return Clusters;
    }

    const TVector<ui64>& GetClusterSizes() const override {
        return ClusterSizes;
    }

    const TVector<ui64>& GetNextClusterSizes() const override {
        return NextClusterSizes;
    }

    virtual void SetClusterSize(ui32 num, ui64 size) override {
        ClusterSizes.at(num) = size;
    }

    void Clear() override {
        Clusters.clear();
        ClusterSizes.clear();
        NextClusterSizes.clear();
        NextClusters.clear();
        Round = 0;
    }

    bool SetClusters(TVector<TString> && newClusters) override {
        if (newClusters.size() == 0) {
            return false;
        }
        for (const auto& cluster: newClusters) {
            if (!IsExpectedFormat(cluster)) {
                return false;
            }
        }
        Clusters = std::move(newClusters);
        ClusterSizes.clear();
        ClusterSizes.resize(Clusters.size());
        NextClusterSizes.clear();
        NextClusterSizes.resize(Clusters.size());
        NextClusters.clear();
        NextClusters.resize(Clusters.size());
        for (auto& aggregate : NextClusters) {
            aggregate.resize(Dimensions, 0);
        }
        return true;
    }

    bool RecomputeClusters() override {
        ui64 vectorCount = 0;
        ui64 reassignedCount = 0;
        for (size_t i = 0; auto& aggregate : NextClusters) {
            auto newSize = NextClusterSizes[i];
            vectorCount += newSize;

            auto clusterSize = ClusterSizes[i];
            reassignedCount += clusterSize < newSize ? newSize - clusterSize : 0;

            if (newSize != 0) {
                this->Fill(Clusters[i], aggregate.data(), newSize);
            }
            ++i;
        }

        Y_ENSURE(reassignedCount <= vectorCount);
        if (Clusters.size() == 1) {
            return true;
        }

        bool last = Round >= MaxRounds;
        if (!last && Round > 1) {
            const auto changes = static_cast<double>(reassignedCount) / static_cast<double>(vectorCount);
            last = changes < MinVectorsNeedsReassigned;
        }
        if (!last) {
            return false;
        }
        return true;
    }

    void RemoveEmptyClusters() override {
        size_t w = 0;
        for (size_t r = 0; r < ClusterSizes.size(); ++r) {
            if (ClusterSizes[r] != 0) {
                ClusterSizes[w] = ClusterSizes[r];
                Clusters[w] = std::move(Clusters[r]);
                ++w;
            }
        }
        ClusterSizes.erase(ClusterSizes.begin() + w, ClusterSizes.end());
        Clusters.erase(Clusters.begin() + w, Clusters.end());
    }

    bool NextRound() override {
        bool isLast = RecomputeClusters();
        ClusterSizes = std::move(NextClusterSizes);
        RemoveEmptyClusters();
        if (isLast) {
            NextClusters.clear();
            return true;
        }
        ++Round;
        NextClusterSizes.clear();
        NextClusterSizes.resize(Clusters.size());
        NextClusters.clear();
        NextClusters.resize(Clusters.size());
        for (auto& aggregate : NextClusters) {
            aggregate.resize(Dimensions, 0);
        }
        return false;
    }

    std::optional<ui32> FindCluster(const TStringBuf embedding) override {
        if (!IsExpectedFormat(embedding)) {
            return {};
        }
        auto min = TMetric::Init();
        std::optional<ui32> closest = {};
        for (size_t i = 0; const auto& cluster : Clusters) {
            auto distance = TMetric::Distance(cluster, embedding);
            if (distance < min) {
                min = distance;
                closest = i;
            }
            ++i;
        }
        return closest;
    }

    std::optional<ui32> FindCluster(TArrayRef<const TCell> row, ui32 embeddingPos) override {
        Y_ENSURE(embeddingPos < row.size());
        return FindCluster(row.at(embeddingPos).AsBuf());
    }

    double CalcDistance(const TStringBuf a, const TStringBuf b, double aa) override {
        return TMetric::Distance(a, b, aa);
    }

    void AggregateToCluster(ui32 pos, const TArrayRef<const char>& embedding, ui64 weight) override {
        auto& aggregate = NextClusters.at(pos);
        auto* coords = aggregate.data();
        Y_ENSURE(IsExpectedFormat(embedding));

        if (IsBitQuantized()) {
            const ui8* data = reinterpret_cast<const ui8*>(embedding.data());
            for (size_t i = 0; i < Dimensions; ++i) {
                const bool coord = data[i / 8] & (1 << (i % 8));
                *coords++ += (TSum)coord * weight;
            }
        } else {
            for (const auto coord : this->GetCoords(embedding.data())) {
                *coords++ += (TSum)coord * weight;
            }
        }
        NextClusterSizes.at(pos) += weight;
    }

    bool IsExpectedFormat(const TArrayRef<const char>& data) override {
        if (FormatByte != data.back()) {
            return false;
        }

        if (IsBitQuantized()) {
            return data.size() >= 2 && Dimensions == (data.size() - 2) * 8 - data[data.size() - 2];
        }

        return data.size() == 1 + sizeof(TCoord) * Dimensions;
    }

    TString GetEmptyRow() const override {
        TString str;
        const size_t bufferSize = NKnnVectorSerialization::GetBufferSize<TCoord>(Dimensions);
        str.resize(bufferSize);
        str[bufferSize - HeaderLen] = FormatByte;
        if (IsBitQuantized()) {
            str[bufferSize - HeaderLen - 1] = 8 - Dimensions % 8;
        }
        return str;
    }

private:
    static constexpr bool IsBitQuantized() {
        return std::is_same_v<TCoord, bool>;
    }

    auto GetCoords(const char* coords) {
        return std::span{reinterpret_cast<const TCoord*>(coords), Dimensions};
    }

    auto GetData(char* data) {
        return std::span{reinterpret_cast<TCoord*>(data), Dimensions};
    }

    void Fill(TString& d, TSum* embedding, ui64& c) {
        Y_ENSURE(c > 0);
        const auto count = static_cast<TSum>(c);

        if (IsBitQuantized()) {
            ui8* const data = reinterpret_cast<ui8*>(d.MutRef().data());
            for (size_t i = 0; i < Dimensions; ++i) {
                if (i % 8 == 0) {
                    data[i / 8] = 0;
                }
                const bool bitValue = embedding[i] >= (count + 1) / 2;
                if (bitValue) {
                    data[i / 8] |= (1 << (i % 8));
                }
            }
        } else {
            auto data = GetData(d.MutRef().data());
            for (auto& coord : data) {
                coord = *embedding / count;
                embedding++;
            }
        }
    }
};

std::unique_ptr<IClusters> CreateClusters(const Ydb::Table::VectorIndexSettings& settings, ui32 maxRounds, TString& error) {
    if (!ValidateSettings(settings, error)) {
        return nullptr;
    }

    const ui32 dim = settings.vector_dimension();

    auto handleMetric = [&]<typename T>() -> std::unique_ptr<IClusters> {
        constexpr ui8 formatByte = Format<T>;
        switch (settings.metric()) {
            case Ydb::Table::VectorIndexSettings::SIMILARITY_INNER_PRODUCT:
                return std::make_unique<TClusters<TMaxInnerProductSimilarity<T>>>(dim, maxRounds, formatByte);
            case Ydb::Table::VectorIndexSettings::SIMILARITY_COSINE:
            case Ydb::Table::VectorIndexSettings::DISTANCE_COSINE:
                // We don't need to have separate implementation for distance,
                // because clusters will be same as for similarity
                return std::make_unique<TClusters<TCosineDistance<T>>>(dim, maxRounds, formatByte);
            case Ydb::Table::VectorIndexSettings::DISTANCE_MANHATTAN:
                return std::make_unique<TClusters<TL1Distance<T>>>(dim, maxRounds, formatByte);
            case Ydb::Table::VectorIndexSettings::DISTANCE_EUCLIDEAN:
                return std::make_unique<TClusters<TL2Distance<T>>>(dim, maxRounds, formatByte);
            default:
                error = TStringBuilder() << "Invalid metric: " << static_cast<int>(settings.metric());
                return nullptr;
        }
    };

    switch (settings.vector_type()) {
        case Ydb::Table::VectorIndexSettings::VECTOR_TYPE_FLOAT:
            return handleMetric.template operator()<float>();
        case Ydb::Table::VectorIndexSettings::VECTOR_TYPE_UINT8:
            return handleMetric.template operator()<ui8>();
        case Ydb::Table::VectorIndexSettings::VECTOR_TYPE_INT8:
            return handleMetric.template operator()<i8>();
        case Ydb::Table::VectorIndexSettings::VECTOR_TYPE_BIT:
            return handleMetric.template operator()<bool>();
        default:
            error = TStringBuilder() << "Invalid vector_type: " << static_cast<int>(settings.vector_type());
            return nullptr;
    }
}

bool ValidateSettings(const Ydb::Table::KMeansTreeSettings& settings, TString& error) {
    error = "";

    if (!settings.has_settings()) {
        error = TStringBuilder() << "vector index settings should be set";
        return false;
    }

    if (!ValidateSettings(settings.settings(), error)) {
        return false;
    }

    if (!ValidateSettingInRange("levels",
        settings.has_levels() ? std::optional<ui64>(settings.levels()) : std::nullopt,
        MinLevels, MaxLevels,
        error))
    {
        return false;
    }

    if (!ValidateSettingInRange("clusters",
        settings.has_clusters() ? std::optional<ui64>(settings.clusters()) : std::nullopt,
        MinClusters, MaxClusters,
        error))
    {
        return false;
    }

    ui64 clustersPowLevels = 1;
    for (ui64 i = 0; i < settings.levels(); ++i) {
        clustersPowLevels *= settings.clusters();
        if (clustersPowLevels > MaxClustersPowLevels) {
            error = TStringBuilder() << "Invalid clusters^levels: " << settings.clusters() << "^" << settings.levels() << " should be less than " << MaxClustersPowLevels;
            return false;
        }
    }

    if (settings.settings().vector_dimension() * settings.clusters() > MaxVectorDimensionMultiplyClusters) {
        error = TStringBuilder() << "Invalid vector_dimension*clusters: " << settings.settings().vector_dimension() << "*" << settings.clusters()
            << " should be less than " << MaxVectorDimensionMultiplyClusters;
        return false;
    }

    error = "";
    return true;
}

bool ValidateSettings(const Ydb::Table::VectorIndexSettings& settings, TString& error) {
    if (!settings.has_metric() || settings.metric() == Ydb::Table::VectorIndexSettings::METRIC_UNSPECIFIED) {
        error = TStringBuilder() << "either distance or similarity should be set";
        return false;
    }
    if (!Ydb::Table::VectorIndexSettings::Metric_IsValid(settings.metric())) {
        error = TStringBuilder() << "Invalid metric: " << static_cast<int>(settings.metric());
        return false;
    }

    if (!settings.has_vector_type() || settings.vector_type() == Ydb::Table::VectorIndexSettings::VECTOR_TYPE_UNSPECIFIED) {
        error = TStringBuilder() << "vector_type should be set";
        return false;
    }
    if (!Ydb::Table::VectorIndexSettings::VectorType_IsValid(settings.vector_type())) {
        error = TStringBuilder() << "Invalid vector_type: " << static_cast<int>(settings.vector_type());
        return false;
    }

    if (!ValidateSettingInRange("vector_dimension",
        settings.has_vector_dimension() ? std::optional<ui64>(settings.vector_dimension()) : std::nullopt,
        MinVectorDimension, MaxVectorDimension,
        error))
    {
        Y_ASSERT(error);
        return false;
    }

    error = "";
    return true;
}

bool FillSetting(Ydb::Table::KMeansTreeSettings& settings, const TString& name, const TString& value, TString& error) {
    error = "";

    const TString nameLower = to_lower(name);
    if (nameLower == "distance") {
        if (settings.mutable_settings()->has_metric()) {
            error = "only one of distance or similarity should be set, not both";
            return false;
        }
        settings.mutable_settings()->set_metric(ParseDistance(value, error));
    } else if (nameLower == "similarity") {
        if (settings.mutable_settings()->has_metric()) {
            error = "only one of distance or similarity should be set, not both";
            return false;
        }
        settings.mutable_settings()->set_metric(ParseSimilarity(value, error));
    } else if (nameLower =="vector_type") {
        settings.mutable_settings()->set_vector_type(ParseVectorType(value, error));
    } else if (nameLower =="vector_dimension") {
        settings.mutable_settings()->set_vector_dimension(ParseUInt32(name, value, MinVectorDimension, MaxVectorDimension, error));
    } else if (nameLower =="clusters") {
        settings.set_clusters(ParseUInt32(name, value, MinClusters, MaxClusters, error));
    } else if (nameLower =="levels") {
        settings.set_levels(ParseUInt32(name, value, MinLevels, MaxLevels, error));
    } else {
        error = TStringBuilder() << "Unknown index setting: " << name;
        return false;
    }

    return !error;
}

}
