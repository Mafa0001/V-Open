#version 460 core
in  vec3  vColor;
in  float vDepth;
out vec4  fragColor;

uniform int uDepthViz;

void main() {
    if (uDepthViz != 0) {
        float d   = clamp(vDepth, 0.0, 1.0);
        vec3 near = vec3(1.0, 0.2, 0.2);
        vec3 mid  = vec3(0.2, 1.0, 0.2);
        vec3 far  = vec3(0.2, 0.2, 1.0);
        vec3 col  = (d < 0.5)
                  ? mix(near, mid, d * 2.0)
                  : mix(mid,  far, (d - 0.5) * 2.0);
        fragColor = vec4(col, 1.0);
    } else {
        fragColor = vec4(vColor, 1.0);
    }
}
