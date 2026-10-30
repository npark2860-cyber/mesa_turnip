#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
layout(location = 0) out uvec4 o;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { float inv_size; } pc;
void main()
{
   uint lane = gl_SubgroupInvocationID;
   uint t = uint(texture(tex, gl_FragCoord.xy * pc.inv_size).r * 255.0 + 0.5);
   uint v = (lane + 1u) * 1024u + t;
   uint x1 = subgroupShuffleXor(v, 1u);
   uint x2 = subgroupShuffleXor(v, 2u);
   o = uvec4(lane + 1u, x1, x2, t + 1u);
}
