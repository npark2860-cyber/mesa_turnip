#version 450
layout(location = 0) out vec4 ids;
layout(location = 1) out vec4 cmp;
layout(set = 0, binding = 0) uniform sampler2DArray tex_plain;
layout(set = 0, binding = 1) uniform sampler2DArrayShadow tex_cmp;
layout(push_constant) uniform PC { float inv_size; float ref; float layer; float pad; } pc;
const float offs[9] = float[9](-0.004, -0.002, -0.001, -0.001953125, 0.0,
                               0.001953125, 0.001, 0.002, 0.004);
void main()
{
   int ix = int(gl_FragCoord.x);
   int iy = int(gl_FragCoord.y);
   float tx = (7.5 + offs[ix]) * pc.inv_size;
   float ty = (7.5 + offs[iy]) * pc.inv_size;
   vec3 c = vec3(tx, ty, pc.layer);
   ids = textureGather(tex_plain, c) * 1024.0;
   cmp = textureGather(tex_cmp, c, pc.ref);
}
