#version 450
void main()
{
   vec2 p = vec2(gl_VertexIndex == 1 ? 1.0 : -1.0, gl_VertexIndex == 2 ? 1.0 : -1.0);
   gl_Position = vec4(p, 0.0, 1.0);
}
