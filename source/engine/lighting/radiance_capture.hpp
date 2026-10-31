#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>

namespace deren::engine::radiance_detail {
    inline float decode_binary16(std::uint16_t value) noexcept {
        auto sign=std::uint32_t(value & 0x8000u)<<16;
        auto exponent=(value>>10)&31u, mantissa=value&1023u;
        if(exponent==0) {
            float result=std::ldexp(float(mantissa),-24);
            return sign?-result:result;
        }
        auto bits=sign | ((exponent==31?255u:exponent+112u)<<23) | (mantissa<<13);
        return std::bit_cast<float>(bits);
    }
    // RH cameras retain normal culling. Mirroring X converts readback to canonical cubemap UVs.
    // Source/output must be disjoint; alpha is a storage lane, not captured coverage.
    inline bool canonicalize_face(std::span<float const> source,int size,std::span<float> output) noexcept {
        if(size<1 || size>512 || source.size()!=std::size_t(size)*size*4 || output.size()!=source.size() || source.data()==output.data())return false;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
            auto in=(std::size_t(y)*size+size-1-x)*4,out=(std::size_t(y)*size+x)*4;
            for(unsigned c=0;c<3;++c) {
                float value=source[in+c];
                if(!std::isfinite(value) || value<0 || value>65504)return false;
                output[out+c]=value;
            }
            output[out+3]=1;
        }
        return true;
    }
}
