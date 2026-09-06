#ifdef TW_QUAD_TEXTURED
TW_SAMPLER(0, 0) uniform sampler2D gTextureSampler;
#endif

#ifndef TW_MAX_QUADS
#define TW_MAX_QUADS 256
#endif

// The same push constants as the vertex shader, so that both stages agree
// on the layout; the fragment stage only reads the colors.
#ifdef TW_VULKAN
struct SQuadUniformEl {
	vec4 gVertColor;
	vec2 gOffset;
	float gRotation;
};
#endif

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
#if defined(TW_VULKAN) && defined(TW_QUAD_GROUPED)
TW_PUSH(32, SQuadUniformEl gUniEls[1])
#endif
#ifndef TW_QUAD_GROUPED
TW_PUSH(32, int gQuadOffset)
#endif
TW_PUSH_END

#ifdef TW_VULKAN
#ifdef TW_QUAD_GROUPED
#define QUAD_COLOR(Index) TW_PC(gUniEls)[Index].gVertColor
#else
#ifdef TW_QUAD_TEXTURED
#define UBOSetIndex 1
#else
#define UBOSetIndex 0
#endif
layout (std140, set = UBOSetIndex, binding = 1) uniform SOffBO {
	SQuadUniformEl gUniEls[TW_MAX_QUADS];
} gQuadBO;
#define QUAD_COLOR(Index) gQuadBO.gUniEls[Index].gVertColor
#endif
#else
#ifdef TW_QUAD_GROUPED
uniform vec4 gVertColors[1];
#else
uniform vec4 gVertColors[TW_MAX_QUADS];
#endif
#define QUAD_COLOR(Index) gVertColors[Index]
#endif

TW_LOC(0) noperspective in vec4 QuadColor;
#ifndef TW_QUAD_GROUPED
TW_LOC(1) flat in int QuadIndex;
#ifdef TW_QUAD_TEXTURED
TW_LOC(2) noperspective in vec2 TexCoord;
#endif
#else
#define QuadIndex 0
#ifdef TW_QUAD_TEXTURED
TW_LOC(1) noperspective in vec2 TexCoord;
#endif
#endif

TW_LOC(0) out vec4 FragClr;
void main()
{
#ifdef TW_QUAD_TEXTURED
	vec4 TexColor = texture(gTextureSampler, TexCoord);
	FragClr = TexColor * QuadColor * QUAD_COLOR(QuadIndex);
#else
	FragClr = QuadColor * QUAD_COLOR(QuadIndex);
#endif
}
