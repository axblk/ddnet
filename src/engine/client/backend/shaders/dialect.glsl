// The one place where the two GLSL dialects differ. Vulkan wants locations on
// varyings, set and binding on samplers, and push constants where OpenGL has
// plain uniforms; a shader says what it means with these and the dialect says
// how. TW_VULKAN is defined by the build that makes SPIR-V; the OpenGL
// backends compile the same text at run time without it.
#ifdef TW_VULKAN
#define TW_LOC(Location) layout(location = Location)
#define TW_SAMPLER(Set, Binding) layout(set = Set, binding = Binding)
#define TW_PUSH_BEGIN layout(push_constant) uniform SPushConstants {
#define TW_PUSH(Offset, Decl) layout(offset = Offset) Decl;
#define TW_PUSH_END } gPush;
#define TW_PC(Name) gPush.Name
#define TW_VERTEX_ID gl_VertexIndex
#define TW_INSTANCE_ID gl_InstanceIndex
#else
#define TW_LOC(Location)
#define TW_SAMPLER(Set, Binding)
#define TW_PUSH_BEGIN
#define TW_PUSH(Offset, Decl) uniform Decl;
#define TW_PUSH_END
#define TW_PC(Name) Name
#define TW_VERTEX_ID gl_VertexID
#define TW_INSTANCE_ID gl_InstanceID
#endif
