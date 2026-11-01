#version 460
#extension GL_EXT_descriptor_heap: require
struct Light { vec4 p; vec4 c; vec4 d; vec4 q; };
layout(descriptor_heap, descriptor_stride=64, std430) readonly buffer Data {vec4 prefix[22]; Light lights[128];} buffers[];
layout(location=0) out vec4 colour;
void main(){Light pl=buffers[17412].lights[0];bool ok=all(lessThan(abs(pl.p-vec4(1.135,.942,.138,.194)),vec4(.0001))) && all(lessThan(abs(pl.c-vec4(13.5108,16.8156,18,3)),vec4(.0001)));colour=ok?vec4(.1,.5,.1,1):vec4(.5,.1,.1,1);}