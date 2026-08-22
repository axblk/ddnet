#ifndef ENGINE_CLIENT_RENDER_TRACE_H
#define ENGINE_CLIENT_RENDER_TRACE_H

#include <engine/graphics.h>
#include <engine/textrender.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class IStorage;

class CRenderTrace
{
public:
	class CFrame
	{
	public:
		uint64_t m_Frame = 0;
		uint64_t m_TimestampNanoseconds = 0;
		uint64_t m_FrametimeNanoseconds = 0;
		uint64_t m_RenderWallNanoseconds = 0;
		IGraphics::CFrameRenderStats m_Render;
		ITextRender::CTextRenderStats m_Text;
		IGraphics::SFrameMailboxStats m_Mailbox;
		uint64_t m_TextureMemory = 0;
		uint64_t m_BufferMemory = 0;
		uint64_t m_StreamedMemory = 0;
		uint64_t m_StagingMemory = 0;
	};

	bool Start(IStorage *pStorage, int Seconds, const char *pFilename);
	bool Stop();
	bool Enabled() const { return m_Enabled; }
	bool ShouldStop() const;
	uint64_t Generation() const { return m_Generation; }

	void BeginFrame();
	void RecordFrame(CFrame Frame);
	void RecordEvent(const char *pName, uint64_t StartNanoseconds, uint64_t DurationNanoseconds, uint64_t Generation);

private:
	class CEvent
	{
	public:
		uint64_t m_Frame;
		uint64_t m_StartNanoseconds;
		uint64_t m_DurationNanoseconds;
		uint32_t m_Name;
	};

	static constexpr size_t MAX_FRAMES = 131072;
	static constexpr size_t MAX_EVENTS = 1048576;

	uint32_t NameId(const char *pName);
	bool Save() const;
	void Clear();

	IStorage *m_pStorage = nullptr;
	std::string m_Filename;
	std::vector<std::string> m_vNames;
	std::vector<CFrame> m_vFrames;
	std::vector<CEvent> m_vEvents;
	size_t m_FrameWriteIndex = 0;
	size_t m_EventWriteIndex = 0;
	uint64_t m_DroppedFrames = 0;
	uint64_t m_DroppedEvents = 0;
	uint64_t m_StartNanoseconds = 0;
	uint64_t m_StopNanoseconds = 0;
	uint64_t m_CurrentFrame = 0;
	uint64_t m_CurrentFrameStartNanoseconds = 0;
	uint64_t m_Generation = 0;
	bool m_Enabled = false;
};

class CRenderTraceScope
{
	CRenderTrace *m_pTrace = nullptr;
	const char *m_pName = nullptr;
	uint64_t m_StartNanoseconds = 0;
	uint64_t m_Generation = 0;

public:
	CRenderTraceScope(CRenderTrace *pTrace, const char *pName);
	~CRenderTraceScope();

	CRenderTraceScope(const CRenderTraceScope &) = delete;
	CRenderTraceScope &operator=(const CRenderTraceScope &) = delete;
};

#endif
