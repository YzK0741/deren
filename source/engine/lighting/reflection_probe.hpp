#pragma once
#include "radiance_capture.hpp"
#include <array>
#include <cstdint>
#include <vector>
#include <optional>
#include <glm/glm.hpp>
#include <algorithm>
#include <limits>
namespace deren::engine::probe_detail {
struct box { glm::vec3 position{},minimum{},maximum{}; float fade=.5f,gain=1; };
inline bool valid(box const& b) {
 if(!std::isfinite(b.fade)||!std::isfinite(b.gain)||b.fade<0||b.gain<0)return false;
 for(int32_t i=0;i<3;++i) if(!std::isfinite(b.position[i])||!std::isfinite(b.minimum[i])||!std::isfinite(b.maximum[i])||b.minimum[i]>=b.position[i]||b.position[i]>=b.maximum[i])return false;
 return true;
}
struct sample {glm::vec3 direction{};float weight=0;};
inline sample lookup(box const& b,glm::vec3 p,glm::vec3 r) {
 if(!valid(b))return {};
 float length=glm::length(r);if(!std::isfinite(length)||length<1e-8f)return {};
 r/=length;float t=std::numeric_limits<float>::max(),distance=t,fade=b.fade;
 for(int32_t i=0;i<3;++i) {
  if(!std::isfinite(p[i])||p[i]<b.minimum[i]||p[i]>b.maximum[i])return {};
  distance=std::min(distance,std::min(p[i]-b.minimum[i],b.maximum[i]-p[i]));
  fade=std::min(fade,(b.maximum[i]-b.minimum[i])*.5f);
  if(std::abs(r[i])>1e-8f)t=std::min(t,((r[i]>0?b.maximum[i]:b.minimum[i])-p[i])/r[i]);
 }
 auto direction=p+r*t-b.position;if(!std::isfinite(glm::length(direction))||glm::length(direction)<1e-8f)return {};
 float w=fade>0?std::clamp(distance/fade,0.f,1.f):1.f;w=w*w*(3-2*w);
 return {direction,w};
}
struct asset {uint32_t size=0,mips=0;std::vector<uint8_t> half;};
inline std::optional<asset> decode(std::span<std::byte const> bytes) {
 auto u32=[&](size_t o){uint32_t v=0;for(uint32_t i=0;i<4;++i)v|=uint32_t(std::to_integer<uint8_t>(bytes[o+i]))<<(i*8);return v;};
 auto u64=[&](size_t o){return uint64_t(u32(o))|(uint64_t(u32(o+4))<<32);};
 if(bytes.size()<32||u32(0)!=0x31425244||u32(4)!=1)return {};
 asset a;a.size=u32(8);a.mips=u32(12);
 if(a.size<16||a.size>512||(a.size&(a.size-1)))return {};
 uint32_t m=1;for(auto n=a.size;n>1;n>>=1)++m;if(a.mips!=m)return {};
 uint64_t count=0;for(uint32_t l=0;l<m;++l)count+=uint64_t(6)*(a.size>>l)*(a.size>>l)*8;
 if(u64(16)!=count||bytes.size()!=32+count)return {};
 uint64_t hash=14695981039346656037ull;for(auto b:bytes.subspan(32)){hash^=std::to_integer<uint8_t>(b);hash*=1099511628211ull;}
 if(hash!=u64(24))return {};
 a.half.reserve(count);
 for(size_t i=32;i<bytes.size();i+=2){uint16_t bits=std::to_integer<uint8_t>(bytes[i])|(uint16_t(std::to_integer<uint8_t>(bytes[i+1]))<<8);float v=radiance_detail::decode_binary16(bits);if(!std::isfinite(v)||v<0||((i-32)%8==6&&v!=1))return {};a.half.push_back(bits&255);a.half.push_back(bits>>8);}
 return a;
}
}
