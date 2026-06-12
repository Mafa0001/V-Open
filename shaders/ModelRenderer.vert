#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aJointIndices;
layout(location = 4) in vec4 aJointWeights;

layout(std140, binding = 0) uniform ViewProjBlock {
    mat4 uView;
    mat4 uProj;
    mat4 uModel;
};

// Skinning (max 512 bones)
layout(std140, binding = 1) uniform BoneBlock {
    mat4 uBones[512];
};

out vec3 vNormal;
out vec2 vTexCoord;
out vec3 vWorldPos;

void main() {
    // Build skin matrix from up to 4 influences
    mat4 skin = mat4(0.0);
    float totalW = aJointWeights.x + aJointWeights.y + aJointWeights.z + aJointWeights.w;
    if (totalW > 0.001) {
        skin += uBones[int(aJointIndices.x)] * (aJointWeights.x / totalW);
        skin += uBones[int(aJointIndices.y)] * (aJointWeights.y / totalW);
        skin += uBones[int(aJointIndices.z)] * (aJointWeights.z / totalW);
        skin += uBones[int(aJointIndices.w)] * (aJointWeights.w / totalW);
    } else {
        skin = mat4(1.0);
    }

    vec4 pos = uModel * skin * vec4(aPos, 1.0);
    vWorldPos   = pos.xyz;
    gl_Position = uProj * uView * pos;

    mat3 nm = transpose(inverse(mat3(uModel * skin)));
    vNormal   = normalize(nm * aNormal);
    vTexCoord = aTexCoord;
}
