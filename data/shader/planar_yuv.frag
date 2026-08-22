// Turns a rendered frame into the planar YUV layout an encoder wants. See
// shader/vulkan/planar_yuv.frag for what the layout is; this is the same
// shader without the Vulkan decorations.

uniform sampler2D gTextureSampler;

noperspective in vec2 texCoord;
noperspective in vec4 vertColor;

out vec4 FragClr;

float Luma(vec3 Color)
{
	return (16.0 + 219.0 * dot(Color, vec3(0.2126, 0.7152, 0.0722))) / 255.0;
}

float ChromaBlue(vec3 Color)
{
	return (128.0 + 224.0 * dot(Color, vec3(-0.1146, -0.3854, 0.5))) / 255.0;
}

float ChromaRed(vec3 Color)
{
	return (128.0 + 224.0 * dot(Color, vec3(0.5, -0.4542, -0.0458))) / 255.0;
}

// One chroma sample stands for a two by two block of pixels.
vec3 Block(ivec2 Chroma)
{
	ivec2 Origin = Chroma * 2;
	return 0.25 * (
			     texelFetch(gTextureSampler, Origin, 0).rgb +
			     texelFetch(gTextureSampler, Origin + ivec2(1, 0), 0).rgb +
			     texelFetch(gTextureSampler, Origin + ivec2(0, 1), 0).rgb +
			     texelFetch(gTextureSampler, Origin + ivec2(1, 1), 0).rgb);
}

void main()
{
	ivec2 SourceSize = textureSize(gTextureSampler, 0);
	ivec2 Target = ivec2(gl_FragCoord.xy);
	bool Planar = vertColor.r > 0.5;
	int ChromaWidth = SourceSize.x / 2;
	// Where the second chroma plane starts, in rows of the target.
	int SecondPlaneRow = SourceSize.y + SourceSize.y / 4;
	for(int Component = 0; Component < 4; ++Component)
	{
		int Byte = Target.x * 4 + Component;
		if(Target.y < SourceSize.y)
		{
			FragClr[Component] = Luma(texelFetch(gTextureSampler, ivec2(Byte, Target.y), 0).rgb);
			continue;
		}
		if(!Planar)
		{
			// One row of the plane holds both components, alternating.
			FragClr[Component] = (Byte % 2) == 0 ?
						     ChromaBlue(Block(ivec2(Byte / 2, Target.y - SourceSize.y))) :
						     ChromaRed(Block(ivec2(Byte / 2, Target.y - SourceSize.y)));
			continue;
		}
		// A plane row is only half as wide as a target row, so one target row
		// holds two of them and the plane is a quarter of the frame's height.
		bool Second = Target.y >= SecondPlaneRow;
		int PlaneRow = Target.y - (Second ? SecondPlaneRow : SourceSize.y);
		bool Lower = Byte >= ChromaWidth;
		ivec2 Chroma = ivec2(Lower ? Byte - ChromaWidth : Byte, PlaneRow * 2 + (Lower ? 1 : 0));
		FragClr[Component] = Second ? ChromaRed(Block(Chroma)) : ChromaBlue(Block(Chroma));
	}
}
