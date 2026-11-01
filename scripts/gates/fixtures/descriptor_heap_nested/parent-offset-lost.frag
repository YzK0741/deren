#version 460
#extension GL_EXT_descriptor_heap: require
layout(location=0) out vec4 colour;
struct Light {vec4 p;vec4 c;vec4 d;vec4 q;};
layout(descriptor_heap, descriptor_stride=64, std430) readonly buffer Data {vec4 prefix[22]; Light lights[128];} buffers[];
layout(descriptor_heap, descriptor_stride=64, std430) readonly buffer Raw {vec4 rows[];} rawBuffers[];
void main(){Light pl=buffers[17412].lights[0];bool ok=all(lessThan(abs(pl.p-rawBuffers[17412].rows[0]),vec4(.0001)))&&all(lessThan(abs(pl.c-rawBuffers[17412].rows[1]),vec4(.0001)))&&all(lessThan(abs(pl.d-rawBuffers[17412].rows[2]),vec4(.0001)))&&all(lessThan(abs(pl.q-rawBuffers[17412].rows[3]),vec4(.0001)));colour=ok?vec4(.1,.5,.1,1):vec4(.5,.1,.1,1);}
