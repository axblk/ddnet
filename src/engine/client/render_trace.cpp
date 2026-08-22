#include "render_trace.h"

#include <base/io.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <algorithm>

namespace
{
	uint64_t NowNanoseconds()
	{
		return time_get_nanoseconds().count();
	}

	void WriteUint64(CJsonWriter &Writer, const char *pName, uint64_t Value)
	{
		Writer.WriteAttribute(pName);
		Writer.WriteInt64Value(static_cast<int64_t>(Value));
	}
}

bool CRenderTrace::Start(IStorage *pStorage, int Seconds, const char *pFilename)
{
	if(m_Enabled || pStorage == nullptr || Seconds <= 0 || pFilename == nullptr || pFilename[0] == '\0')
		return false;
	IOHANDLE File = pStorage->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	if(File == nullptr)
		return false;
	io_close(File);

	Clear();
	m_pStorage = pStorage;
	m_Filename = pFilename;
	m_StartNanoseconds = NowNanoseconds();
	m_StopNanoseconds = m_StartNanoseconds + static_cast<uint64_t>(Seconds) * 1000000000ULL;
	++m_Generation;
	m_vNames.reserve(64);
	m_vFrames.reserve(MAX_FRAMES);
	m_vEvents.reserve(MAX_EVENTS);
	m_Enabled = true;
	return true;
}

bool CRenderTrace::Stop()
{
	if(!m_Enabled)
		return false;
	m_Enabled = false;
	const bool Saved = Save();
	if(Saved)
		log_info("render_trace", "saved %" PRIzu " frames and %" PRIzu " CPU events to '%s'", m_vFrames.size(), m_vEvents.size(), m_Filename.c_str());
	Clear();
	return Saved;
}

bool CRenderTrace::ShouldStop() const
{
	return m_Enabled && NowNanoseconds() >= m_StopNanoseconds;
}

void CRenderTrace::BeginFrame()
{
	if(!m_Enabled)
		return;
	++m_CurrentFrame;
	m_CurrentFrameStartNanoseconds = NowNanoseconds();
}

void CRenderTrace::RecordFrame(CFrame Frame)
{
	if(!m_Enabled || m_CurrentFrameStartNanoseconds == 0)
		return;
	Frame.m_Frame = m_CurrentFrame;
	Frame.m_TimestampNanoseconds = m_CurrentFrameStartNanoseconds - m_StartNanoseconds;
	if(m_vFrames.size() < MAX_FRAMES)
		m_vFrames.push_back(Frame);
	else
	{
		m_vFrames[m_FrameWriteIndex] = Frame;
		m_FrameWriteIndex = (m_FrameWriteIndex + 1) % MAX_FRAMES;
		++m_DroppedFrames;
	}
}

void CRenderTrace::RecordEvent(const char *pName, uint64_t StartNanoseconds, uint64_t DurationNanoseconds, uint64_t Generation)
{
	if(!m_Enabled || Generation != m_Generation || StartNanoseconds < m_StartNanoseconds)
		return;
	const CEvent Event{m_CurrentFrame, StartNanoseconds - m_StartNanoseconds, DurationNanoseconds, NameId(pName)};
	if(m_vEvents.size() < MAX_EVENTS)
		m_vEvents.push_back(Event);
	else
	{
		m_vEvents[m_EventWriteIndex] = Event;
		m_EventWriteIndex = (m_EventWriteIndex + 1) % MAX_EVENTS;
		++m_DroppedEvents;
	}
}

uint32_t CRenderTrace::NameId(const char *pName)
{
	const auto It = std::ranges::find(m_vNames, pName);
	if(It != m_vNames.end())
		return static_cast<uint32_t>(std::distance(m_vNames.begin(), It));
	m_vNames.emplace_back(pName);
	return static_cast<uint32_t>(m_vNames.size() - 1);
}

bool CRenderTrace::Save() const
{
	IOHANDLE File = m_pStorage->OpenFile(m_Filename.c_str(), IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	if(File == nullptr)
	{
		log_error("render_trace", "failed to open '%s'", m_Filename.c_str());
		return false;
	}

	CJsonFileWriter Writer(File);
	Writer.BeginObject();
	Writer.WriteAttribute("format");
	Writer.WriteStrValue("ddnet-render-trace");
	Writer.WriteAttribute("version");
	Writer.WriteIntValue(1);
	WriteUint64(Writer, "dropped_frames", m_DroppedFrames);
	WriteUint64(Writer, "dropped_events", m_DroppedEvents);

	Writer.WriteAttribute("names");
	Writer.BeginArray();
	for(const std::string &Name : m_vNames)
		Writer.WriteStrValue(Name.c_str());
	Writer.EndArray();
	Writer.WriteAttribute("gpu_zone_names");
	Writer.BeginArray();
	for(const char *pName : IGraphics::GPU_RENDER_ZONE_NAMES)
		Writer.WriteStrValue(pName);
	Writer.EndArray();

	Writer.WriteAttribute("frames");
	Writer.BeginArray();
	for(size_t FrameOffset = 0; FrameOffset < m_vFrames.size(); ++FrameOffset)
	{
		const size_t FrameIndex = m_vFrames.size() < MAX_FRAMES ? FrameOffset : (m_FrameWriteIndex + FrameOffset) % MAX_FRAMES;
		const CFrame &Frame = m_vFrames[FrameIndex];
		Writer.BeginObject();
		WriteUint64(Writer, "frame", Frame.m_Frame);
		WriteUint64(Writer, "timestamp_ns", Frame.m_TimestampNanoseconds);
		WriteUint64(Writer, "frametime_ns", Frame.m_FrametimeNanoseconds);
		WriteUint64(Writer, "render_wall_ns", Frame.m_RenderWallNanoseconds);
		WriteUint64(Writer, "gpu_time_ns", Frame.m_Render.m_GpuTimeNanoseconds);
		WriteUint64(Writer, "gpu_world_ns", Frame.m_Render.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::WORLD)]);
		WriteUint64(Writer, "gpu_interface_ns", Frame.m_Render.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::INTERFACE)]);
		WriteUint64(Writer, "gpu_zone_mask", Frame.m_Render.m_GpuRenderZoneMask);
		Writer.WriteAttribute("gpu_zones_ns");
		Writer.BeginArray();
		for(uint64_t Nanoseconds : Frame.m_Render.m_aGpuRenderZoneNanoseconds)
			Writer.WriteInt64Value(static_cast<int64_t>(Nanoseconds));
		Writer.EndArray();
		WriteUint64(Writer, "gpu_sample", Frame.m_Render.m_GpuSample);
		Writer.WriteAttribute("gpu_supported");
		Writer.WriteBoolValue(Frame.m_Render.m_GpuTimingSupported);
		WriteUint64(Writer, "commands", Frame.m_Render.m_Commands);
		WriteUint64(Writer, "resource_commands", Frame.m_Render.m_ResourceCommands);
		WriteUint64(Writer, "draw_commands", Frame.m_Render.m_DrawCommands);
		WriteUint64(Writer, "draw_calls", Frame.m_Render.m_DrawCalls);
		WriteUint64(Writer, "triangles", Frame.m_Render.m_Triangles);
		WriteUint64(Writer, "instances", Frame.m_Render.m_Instances);
		WriteUint64(Writer, "render_passes", Frame.m_Render.m_RenderPasses);
		WriteUint64(Writer, "buffer_creates", Frame.m_Render.m_BufferCreates);
		WriteUint64(Writer, "buffer_recreates", Frame.m_Render.m_BufferRecreates);
		WriteUint64(Writer, "buffer_updates", Frame.m_Render.m_BufferUpdates);
		WriteUint64(Writer, "texture_creates", Frame.m_Render.m_TextureCreates);
		WriteUint64(Writer, "texture_updates", Frame.m_Render.m_TextureUpdates);
		WriteUint64(Writer, "upload_bytes", Frame.m_Render.m_UploadBytes);
		WriteUint64(Writer, "streamed_bytes", Frame.m_Render.m_StreamedBytes);
		WriteUint64(Writer, "text_layout_ns", Frame.m_Text.m_LayoutTimeNanoseconds);
		WriteUint64(Writer, "text_layout_calls", Frame.m_Text.m_LayoutCalls);
		WriteUint64(Writer, "glyphs", Frame.m_Text.m_GlyphsLaidOut);
		WriteUint64(Writer, "text_creates", Frame.m_Text.m_ContainerCreates);
		WriteUint64(Writer, "text_soft_recreates", Frame.m_Text.m_ContainerSoftRecreates);
		WriteUint64(Writer, "text_deletes", Frame.m_Text.m_ContainerDeletes);
		WriteUint64(Writer, "text_renders", Frame.m_Text.m_ContainerRenders);
		WriteUint64(Writer, "text_upload_bytes", Frame.m_Text.m_UploadBytes);
		WriteUint64(Writer, "frames_produced", Frame.m_Mailbox.m_Produced);
		WriteUint64(Writer, "frames_rendered", Frame.m_Mailbox.m_Rendered);
		WriteUint64(Writer, "frames_dropped", Frame.m_Mailbox.m_Dropped);
		WriteUint64(Writer, "texture_memory", Frame.m_TextureMemory);
		WriteUint64(Writer, "buffer_memory", Frame.m_BufferMemory);
		WriteUint64(Writer, "streamed_memory", Frame.m_StreamedMemory);
		WriteUint64(Writer, "staging_memory", Frame.m_StagingMemory);
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("events");
	Writer.BeginArray();
	for(size_t EventOffset = 0; EventOffset < m_vEvents.size(); ++EventOffset)
	{
		const size_t EventIndex = m_vEvents.size() < MAX_EVENTS ? EventOffset : (m_EventWriteIndex + EventOffset) % MAX_EVENTS;
		const CEvent &Event = m_vEvents[EventIndex];
		Writer.BeginObject();
		WriteUint64(Writer, "frame", Event.m_Frame);
		WriteUint64(Writer, "start_ns", Event.m_StartNanoseconds);
		WriteUint64(Writer, "duration_ns", Event.m_DurationNanoseconds);
		Writer.WriteAttribute("name");
		Writer.WriteIntValue(static_cast<int>(Event.m_Name));
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();
	return true;
}

void CRenderTrace::Clear()
{
	m_pStorage = nullptr;
	m_Filename.clear();
	std::vector<std::string>().swap(m_vNames);
	std::vector<CFrame>().swap(m_vFrames);
	std::vector<CEvent>().swap(m_vEvents);
	m_FrameWriteIndex = 0;
	m_EventWriteIndex = 0;
	m_DroppedFrames = 0;
	m_DroppedEvents = 0;
	m_StartNanoseconds = 0;
	m_StopNanoseconds = 0;
	m_CurrentFrame = 0;
	m_CurrentFrameStartNanoseconds = 0;
}

CRenderTraceScope::CRenderTraceScope(CRenderTrace *pTrace, const char *pName)
{
	if(pTrace == nullptr || !pTrace->Enabled())
		return;
	m_pTrace = pTrace;
	m_pName = pName;
	m_StartNanoseconds = NowNanoseconds();
	m_Generation = pTrace->Generation();
}

CRenderTraceScope::~CRenderTraceScope()
{
	if(m_pTrace == nullptr)
		return;
	const uint64_t EndNanoseconds = NowNanoseconds();
	m_pTrace->RecordEvent(m_pName, m_StartNanoseconds, EndNanoseconds - m_StartNanoseconds, m_Generation);
}
