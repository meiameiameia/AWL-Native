#pragma once
#include "awl/world_map_secondary_model.h"
#include <cmath>

namespace awl::detail {
inline bool matrix_scalar_supported(float value) {
    return std::isfinite(value) && std::fpclassify(value)!=FP_SUBNORMAL;
}
// 801B7D6C: preserve the multiply, two fused adds and paired [0,1]
// translation contribution for columns 2/3. Output may alias either input.
inline bool concatenate(const WorldMapModelMatrix& a,const WorldMapModelMatrix& b,WorldMapModelMatrix* out) {
    for (float value:a) if (!matrix_scalar_supported(value)) return false;
    for (float value:b) if (!matrix_scalar_supported(value)) return false;
    WorldMapModelMatrix result{};
    for (size_t row=0;row<3;++row) for (size_t column=0;column<4;++column) {
        float value=b[column]*a[row*4];
        if (!matrix_scalar_supported(value)) return false;
        value=std::fma(b[4+column],a[row*4+1],value);
        if (!matrix_scalar_supported(value)) return false;
        value=std::fma(b[8+column],a[row*4+2],value);
        if (!matrix_scalar_supported(value)) return false;
        if (column>=2) value=std::fma(column==3?1.0f:0.0f,a[row*4+3],value);
        if (!matrix_scalar_supported(value)) return false;
        result[row*4+column]=value;
    }
    *out=result;return true;
}
} // namespace awl::detail
