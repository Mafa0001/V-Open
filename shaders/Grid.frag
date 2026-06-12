#version 460 core
in vec4 vColor;
in vec3 vWorldPos;
out vec4 fragColor;

// Lighting
struct DirLight {
    vec4 direction;
    vec4 color;
};

layout(std140, binding = 2) uniform LightingBlock {
    vec4 uAmbient;
    DirLight uKeyLight;
    DirLight uFillLight;
    DirLight uRimLight;
    int uEnableRayTracing;
    int uEnableShadows;
    float uShadowBias;
    int _pad;
    vec4 uExplosionPos;   // xyz = position, w = radius
    vec4 uExplosionColor; // rgb = color, w = intensity/fade factor
    vec4 uShadowHipPos;
    vec4 uShadowChestPos;
    vec4 uShadowHeadPos;
};

layout(std140, binding = 1) uniform BoneBlock {
    mat4 uBones[512];
};

float shadowRayIntersection(vec3 ro, vec3 rd, vec3 sphereCenter, float sphereRadius) {
    vec3 oc = ro - sphereCenter;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    return -b - sqrt(h);
}

void main() {
    vec3 litColor = vColor.rgb;

    // Default shadows and ray tracing
    float shadow = 1.0;

    vec3 hipPos = uShadowHipPos.xyz;
    vec3 headPos = uShadowHeadPos.xyz;
    vec3 chestPos = uShadowChestPos.xyz;

    // Fallbacks if positions aren't provided by main app (e.g. model not loaded or stickman mode)
    if (length(hipPos) < 0.001) {
        hipPos = vec3(0.0, 0.8, 0.0);
        headPos = vec3(0.0, 1.6, 0.0);
        chestPos = vec3(0.0, 1.2, 0.0);
    }

    if (uEnableShadows != 0) {
        // Trace to Key Light
        vec3 toLight = -normalize(uKeyLight.direction.xyz);
        vec3 ro = vWorldPos + vec3(0.0, 0.001, 0.0); // Offset upwards slightly

        float tHips = shadowRayIntersection(ro, toLight, hipPos, uShadowHipPos.w);
        float tChest = shadowRayIntersection(ro, toLight, chestPos, uShadowChestPos.w);
        float tHead = shadowRayIntersection(ro, toLight, headPos, uShadowHeadPos.w);

        if (tHips > 0.0 || tChest > 0.0 || tHead > 0.0) {
            shadow = 0.25; // Casts soft shadow on the ground grid
        }

        // Trace to Explosion Light
        if (uExplosionColor.w > 0.01) {
            vec3 toExp = uExplosionPos.xyz - vWorldPos;
            float dist = length(toExp);
            vec3 dirExp = toExp / dist;

            float expShadow = 1.0;
            float tHipsExp = shadowRayIntersection(ro, dirExp, hipPos, uShadowHipPos.w);
            float tChestExp = shadowRayIntersection(ro, dirExp, chestPos, uShadowChestPos.w);
            float tHeadExp = shadowRayIntersection(ro, dirExp, headPos, uShadowHeadPos.w);

            if ((tHipsExp > 0.0 && tHipsExp < dist) || 
                (tChestExp > 0.0 && tChestExp < dist) || 
                (tHeadExp > 0.0 && tHeadExp < dist)) {
                expShadow = 0.0; // Explosion light is shadowed on the ground
            }

            float atten = max(1.0 - (dist / uExplosionPos.w), 0.0);
            vec3 expLightContrib = uExplosionColor.rgb * atten * uExplosionColor.w * expShadow;
            litColor += expLightContrib * 0.8; // Lights up the grid floor
        }
    }

    litColor *= shadow;
    fragColor = vec4(litColor, vColor.a);
}
