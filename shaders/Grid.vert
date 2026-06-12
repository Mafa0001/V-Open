#version 460 core
layout(location=0) in vec3 aPos;

layout(std140, binding = 0) uniform GridBlock {
    mat4  uView;
    mat4  uProj;
    vec4  uGridColor; // vec4 for std140 alignment
    float uGridAlpha;
};

out vec4 vColor;
out vec3 vWorldPos;

void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    vColor = vec4(uGridColor.rgb, uGridAlpha);
    vWorldPos = aPos;
}
