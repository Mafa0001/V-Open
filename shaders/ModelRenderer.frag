#version 460 core
in vec3 vNormal;
in vec2 vTexCoord;
in vec3 vWorldPos;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D uBaseColorTex;

layout(std140, binding = 3) uniform MaterialBlock {
    vec4 uBaseColorFactor;
    int  uHasTexture;
};

// Lighting
struct DirLight {
    vec4 direction; // padding out to vec4
    vec4 color;     // padding out to vec4
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

vec3 dirLightContrib(DirLight L, vec3 normal, vec3 albedo) {
    float d = max(dot(normal, -normalize(L.direction.xyz)), 0.0);
    return albedo * d * L.color.rgb;
}

float shadowRayIntersection(vec3 ro, vec3 rd, vec3 sphereCenter, float sphereRadius) {
    vec3 oc = ro - sphereCenter;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    return -b - sqrt(h);
}

void main() {
    vec4 texColor = (uHasTexture != 0) ? texture(uBaseColorTex, vTexCoord) : vec4(1.0);
    vec4 albedo   = texColor * uBaseColorFactor;
    if (albedo.a < 0.01) discard;

    vec3 N = normalize(vNormal);

    vec3 lit = albedo.rgb * uAmbient.rgb;

    // Ray-Traced Shadowing (against main body bones using direct uniform world positions)
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
        vec3 toLight = -normalize(uKeyLight.direction.xyz);
        vec3 ro = vWorldPos + N * uShadowBias;
        
        // Ray trace against character spheres to determine self shadowing
        // To prevent self-shading local meshes, check distance
        float tHips = (distance(vWorldPos, hipPos) > 1.2 * uShadowHipPos.w) ? shadowRayIntersection(ro, toLight, hipPos, uShadowHipPos.w) : -1.0;
        float tChest = (distance(vWorldPos, chestPos) > 1.36 * uShadowChestPos.w) ? shadowRayIntersection(ro, toLight, chestPos, uShadowChestPos.w) : -1.0;
        float tHead = (distance(vWorldPos, headPos) > 1.38 * uShadowHeadPos.w) ? shadowRayIntersection(ro, toLight, headPos, uShadowHeadPos.w) : -1.0;
        
        if (tHips > 0.0 || tChest > 0.0 || tHead > 0.0) {
            shadow = 0.2; // Ray-traced self-shadow
        }
    }

    lit += dirLightContrib(uKeyLight,  N, albedo.rgb) * shadow;
    lit += dirLightContrib(uFillLight, N, albedo.rgb);
    lit += dirLightContrib(uRimLight,  N, albedo.rgb);

    // Dynamic Explosion Light Source Contribution (Point Light)
    if (uExplosionColor.w > 0.01) {
        vec3 toExp = uExplosionPos.xyz - vWorldPos;
        float dist = length(toExp);
        vec3 dirExp = toExp / dist;
        
        float expShadow = 1.0;
        if (uEnableShadows != 0) {
            vec3 ro = vWorldPos + N * uShadowBias;
            float tHipsExp = (distance(vWorldPos, hipPos) > 1.2 * uShadowHipPos.w) ? shadowRayIntersection(ro, dirExp, hipPos, uShadowHipPos.w) : -1.0;
            float tChestExp = (distance(vWorldPos, chestPos) > 1.36 * uShadowChestPos.w) ? shadowRayIntersection(ro, dirExp, chestPos, uShadowChestPos.w) : -1.0;
            float tHeadExp = (distance(vWorldPos, headPos) > 1.38 * uShadowHeadPos.w) ? shadowRayIntersection(ro, dirExp, headPos, uShadowHeadPos.w) : -1.0;
            
            if ((tHipsExp > 0.0 && tHipsExp < dist) || 
                (tChestExp > 0.0 && tChestExp < dist) || 
                (tHeadExp > 0.0 && tHeadExp < dist)) {
                expShadow = 0.0; // Explosion light is occluded
            }
        }
        
        float atten = max(1.0 - (dist / uExplosionPos.w), 0.0);
        float diffuse = max(dot(N, dirExp), 0.0);
        vec3 expLight = uExplosionColor.rgb * diffuse * atten * uExplosionColor.w * expShadow;
        lit += albedo.rgb * expLight;
    }

    fragColor = vec4(lit, albedo.a);
}
