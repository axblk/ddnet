TW_SAMPLER(0, 0) uniform sampler2D gTextureSampler;

TW_PUSH_BEGIN
#ifdef TW_PUSH_CONST
TW_PUSH(64, vec4 gVerticesColor)
#else
TW_PUSH(48, vec4 gVerticesColor)
#endif
TW_PUSH_END

TW_LOC(0) noperspective in vec2 texCoord;
TW_LOC(1) noperspective in vec4 vertColor;

TW_LOC(0) out vec4 FragClr;
void main()
{
	vec4 tex = texture(gTextureSampler, texCoord);
	FragClr = tex * vertColor * TW_PC(gVerticesColor);
}
