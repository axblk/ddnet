#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 0) uniform sampler2D gTextureSampler;

layout(location = 0) noperspective in vec2 texCoord;
layout(location = 1) noperspective in vec4 vertColor;

layout(location = 0) out vec4 FragClr;
void main()
{
	vec2 TexelOffset = vertColor.rg / vec2(textureSize(gTextureSampler, 0));
	FragClr = texture(gTextureSampler, texCoord) * 0.2270270270;
	FragClr += texture(gTextureSampler, texCoord + TexelOffset * 1.3846153846) * 0.3162162162;
	FragClr += texture(gTextureSampler, texCoord - TexelOffset * 1.3846153846) * 0.3162162162;
	FragClr += texture(gTextureSampler, texCoord + TexelOffset * 3.2307692308) * 0.0702702703;
	FragClr += texture(gTextureSampler, texCoord - TexelOffset * 3.2307692308) * 0.0702702703;
}
