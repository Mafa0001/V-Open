#version 460 core
in  vec2 vUV;
out vec4 fragColor;

layout(std140, binding = 0) uniform BgBlock {
    int  uBgMode;
    vec4 uBgColor;
    vec4 uBgColorTop;
    vec4 uBgColorBot;
};

void main() {
    if (uBgMode == 0) {
        fragColor = uBgColor;
    } else if (uBgMode == 1) {
        fragColor = mix(uBgColorBot, uBgColorTop, vUV.y);
    } else {
        // Checkerboard 80px cells
        bool even = (mod(gl_FragCoord.x, 80.0) < 40.0) == (mod(gl_FragCoord.y, 80.0) < 40.0);
        fragColor = even ? vec4(0.12,0.12,0.14,1) : vec4(0.08,0.08,0.09,1);
    }
}
