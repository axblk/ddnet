TW_SAMPLER(0, 0) uniform sampler2D gTextSampler;

TW_PUSH_BEGIN
TW_PUSH(48, vec4 gVertColor)
TW_PUSH(64, vec4 gVertOutlineColor)
TW_PUSH_END

TW_LOC(0) noperspective in vec2 texCoord;
TW_LOC(1) noperspective in vec4 outVertColor;

TW_LOC(0) out vec4 FragClr;
void main()
{
	// One atlas, two channels: red is the glyph body, green is its outline.
	vec2 atlas = texture(gTextSampler, texCoord).rg;
	vec4 textColor = TW_PC(gVertColor) * outVertColor * vec4(1.0, 1.0, 1.0, atlas.r);
	vec4 textOutlineTex = TW_PC(gVertOutlineColor) * vec4(1.0, 1.0, 1.0, atlas.g);

	// ratio between the two textures
	float OutlineBlend = (1.0 - textColor.a);

	// since the outline is always black, or even if it has decent colors, it can be just added to the actual color
	// without losing any or too much color

	// lerp isn't commutative, so add the color the fragment looses by lerping
	// this reduces the chance of false color calculation if the text is transparent

	// first get the right color
	vec4 textOutlineFrag = vec4(textOutlineTex.rgb * textOutlineTex.a, textOutlineTex.a) * OutlineBlend;
	vec3 textFrag = (textColor.rgb * textColor.a);
	vec3 finalFragColor = textOutlineFrag.rgb + textFrag;

	float RealAlpha = (textOutlineFrag.a + textColor.a);

	// simply add the color we will loose through blending
	if(RealAlpha > 0.0)
		FragClr = vec4(finalFragColor / RealAlpha, RealAlpha);
	else
		FragClr = vec4(0.0, 0.0, 0.0, 0.0);
}
