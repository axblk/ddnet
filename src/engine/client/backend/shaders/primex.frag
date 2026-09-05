#ifdef TW_TEXTURED
TW_SAMPLER(0, 0) uniform sampler2D gTextureSampler;
#endif

TW_PUSH_BEGIN
TW_PUSH(48, vec4 gVerticesColor)
TW_PUSH_END

TW_LOC(0) noperspective in vec2 texCoord;
TW_LOC(1) noperspective in vec4 vertColor;

TW_LOC(0) out vec4 FragClr;
void main()
{
#ifdef TW_TEXTURED
	vec4 tex = texture(gTextureSampler, texCoord);
	FragClr = tex * vertColor * TW_PC(gVerticesColor);
#else
	FragClr = vertColor * TW_PC(gVerticesColor);
#endif
}
