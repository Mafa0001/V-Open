#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform mat4  uView;
uniform mat4  uProj;
uniform float uDepthNear;
uniform float uDepthFar;
uniform float uPointSize;

out vec3  vColor;
out float vDepth;

void main() {
    vec4 viewPos = uView * vec4(aPos, 1.0);
    gl_Position  = uProj * viewPos;
    gl_PointSize = uPointSize;

    float d     = -viewPos.z;
    float range = uDepthFar - uDepthNear;
    vDepth = (range > 1e-4)
           ? clamp((d - uDepthNear) / range, 0.0, 1.0)
           : 0.5;
    vColor = aColor;
}
