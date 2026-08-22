#version 450
#extension GL_ARB_separate_shader_objects : enable

// Turns a rendered frame into the planar YUV layout an encoder wants, so that
// what crosses the bus is one and a half bytes per pixel instead of four, and
// no processor has to convert anything afterwards.
//
// The target holds four of those bytes per texel, so it is a quarter as wide
// as the source and half again as tall: the rows above the source height carry
// the luma plane, the rows below it the chroma at half resolution. Read back in
// one piece, that is exactly a frame in the encoder's format.
//
// vertColor.r picks the layout, the way the blur takes its axis: dark for NV12,
// which interleaves the two chroma components, bright for planar YUV, which
// puts them one after the other.
//
// Limited range BT.709, which is what the stream is tagged as.

layout(binding = 0) uniform sampler2D gTextureSampler;

layout(location = 0) noperspective in vec2 texCoord;
layout(location = 1) noperspective in vec4 vertColor;

layout(location = 0) out vec4 FragClr;

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
			FragClr[Component] = (Byte & 1) == 0 ?
						     ChromaBlue(Block(ivec2(Byte >> 1, Target.y - SourceSize.y))) :
						     ChromaRed(Block(ivec2(Byte >> 1, Target.y - SourceSize.y)));
			continue;
		}
		// A plane row is only half as wide as a target row, so one target row
		// holds two of them and the plane is a quarter of the frame's height.
		// Which of the two a byte falls into is one comparison, where the
		// division it stands for would cost the card twenty instructions.
		bool Second = Target.y >= SecondPlaneRow;
		int PlaneRow = Target.y - (Second ? SecondPlaneRow : SourceSize.y);
		bool Lower = Byte >= ChromaWidth;
		ivec2 Chroma = ivec2(Lower ? Byte - ChromaWidth : Byte, PlaneRow * 2 + (Lower ? 1 : 0));
		FragClr[Component] = Second ? ChromaRed(Block(Chroma)) : ChromaBlue(Block(Chroma));
	}
}
