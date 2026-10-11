/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "live_stream.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/str.h>

#include <engine/demo.h>
#include <engine/shared/demo.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <limits>
#include <optional>

namespace
{
	std::optional<CLiveStream::EMarkerKind> MarkerKindFromName(const char *pName)
	{
		for(const CLiveStream::EMarkerKind Kind : {CLiveStream::EMarkerKind::MATCH_END, CLiveStream::EMarkerKind::MANUAL})
			if(str_comp(pName, CLiveStream::MarkerKindName(Kind)) == 0)
				return Kind;
		return std::nullopt;
	}

	// What an index has to say is there and of its type, or the index is not read.
	bool ReadInt64(const json_value *pObject, const char *pName, int64_t *pValue)
	{
		const json_value *pField = json_object_get(pObject, pName);
		if(pField->type != json_integer)
			return false;
		*pValue = pField->u.integer;
		return true;
	}

	bool ReadInt(const json_value *pObject, const char *pName, int *pValue)
	{
		int64_t Value;
		if(!ReadInt64(pObject, pName, &Value) || Value < 0 || Value > std::numeric_limits<int>::max())
			return false;
		*pValue = (int)Value;
		return true;
	}

	bool ReadString(const json_value *pObject, const char *pName, std::string *pValue)
	{
		const json_value *pField = json_object_get(pObject, pName);
		if(pField->type != json_string)
			return false;
		*pValue = json_string_get(pField);
		return true;
	}

	bool CopyFileInto(IStorage *pStorage, int StorageType, const char *pFilename, IOHANDLE Destination)
	{
		IOHANDLE Source = pStorage->OpenFile(pFilename, IOFLAG_READ, StorageType);
		if(!Source)
			return false;
		bool Ok = true;
		unsigned char aChunk[64 * 1024];
		while(true)
		{
			const unsigned Bytes = io_read(Source, aChunk, sizeof(aChunk));
			if(Bytes == 0)
				break;
			if(io_write(Destination, aChunk, Bytes) != Bytes)
			{
				Ok = false;
				break;
			}
		}
		io_close(Source);
		return Ok;
	}
} // namespace

bool CLiveStream::IsValidName(const char *pName)
{
	return pName[0] != '\0' && pName[0] != '.' && str_length(pName) < NAME_SIZE && str_valid_filename(pName) && !str_find(pName, "/") && !str_find(pName, "\\");
}

const char *CLiveStream::MarkerKindName(EMarkerKind Kind)
{
	switch(Kind)
	{
	case EMarkerKind::MATCH_END: return "match_end";
	case EMarkerKind::MANUAL: return "manual";
	}
	dbg_assert_failed("Invalid marker kind");
}

std::string CLiveStream::InitFilename(int Epoch)
{
	return "init-" + std::to_string(Epoch);
}

std::string CLiveStream::SegmentFilename(int Number)
{
	return "seg-" + std::to_string(Number);
}

std::string CLiveStream::FormatIndex(int64_t Updated) const
{
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("version");
	Writer.WriteIntValue(INDEX_VERSION);
	Writer.WriteAttribute("name");
	Writer.WriteStrValue(m_Name.c_str());
	Writer.WriteAttribute("stream");
	Writer.WriteInt64Value(m_Id);
	Writer.WriteAttribute("state");
	Writer.WriteStrValue(m_Live ? "live" : "ended");
	Writer.WriteAttribute("updated");
	Writer.WriteInt64Value(Updated);
	Writer.WriteAttribute("tick_speed");
	Writer.WriteIntValue(SERVER_TICK_SPEED);
	Writer.WriteAttribute("delay_ms");
	Writer.WriteInt64Value(m_DelayMs);
	Writer.WriteAttribute("segment_seconds");
	Writer.WriteIntValue(m_SegmentSeconds);
	Writer.WriteAttribute("max_duration_seconds");
	Writer.WriteIntValue(m_MaxDurationSeconds);
	Writer.WriteAttribute("max_bytes");
	Writer.WriteInt64Value(m_MaxBytes);

	Writer.WriteAttribute("epochs");
	Writer.BeginArray();
	for(const CEpoch &Epoch : m_vEpochs)
	{
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(Epoch.m_Sha256, aSha256, sizeof(aSha256));
		Writer.BeginObject();
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Epoch.m_Id);
		Writer.WriteAttribute("init");
		Writer.WriteStrValue(InitFilename(Epoch.m_Id).c_str());
		Writer.WriteAttribute("bytes");
		Writer.WriteInt64Value(Epoch.m_InitBytes);
		Writer.WriteAttribute("map");
		Writer.WriteStrValue(Epoch.m_Map.c_str());
		Writer.WriteAttribute("sha256");
		Writer.WriteStrValue(aSha256);
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("segments");
	Writer.BeginArray();
	for(const CSegment &Segment : m_vSegments)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("n");
		Writer.WriteIntValue(Segment.m_Number);
		Writer.WriteAttribute("file");
		Writer.WriteStrValue(SegmentFilename(Segment.m_Number).c_str());
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Segment.m_Epoch);
		Writer.WriteAttribute("start_tick");
		Writer.WriteIntValue(Segment.m_StartTick);
		Writer.WriteAttribute("end_tick");
		Writer.WriteIntValue(Segment.m_EndTick);
		Writer.WriteAttribute("bytes");
		Writer.WriteInt64Value(Segment.m_Bytes);
		Writer.WriteAttribute("time");
		Writer.WriteInt64Value(Segment.m_WallTime);
		Writer.WriteAttribute("complete");
		Writer.WriteBoolValue(Segment.m_Complete);
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("markers");
	Writer.BeginArray();
	for(const CMarker &Marker : m_vMarkers)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("epoch");
		Writer.WriteIntValue(Marker.m_Epoch);
		Writer.WriteAttribute("tick");
		Writer.WriteIntValue(Marker.m_Tick);
		Writer.WriteAttribute("kind");
		Writer.WriteStrValue(MarkerKindName(Marker.m_Kind));
		if(!Marker.m_Label.empty())
		{
			Writer.WriteAttribute("label");
			Writer.WriteStrValue(Marker.m_Label.c_str());
		}
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();
	return Writer.GetOutputString();
}

bool CLiveStream::ParseIndex(const char *pJson, size_t Size, char *pError, size_t ErrorSize)
{
	*this = CLiveStream();
	json_value *pIndex = JsonParse(pJson, Size);
	if(pIndex == nullptr || pIndex->type != json_object)
	{
		if(pIndex)
			json_value_free(pIndex);
		str_copy(pError, "not a JSON object", ErrorSize);
		return false;
	}

	const auto Read = [&]() -> const char * {
		int Version;
		if(!ReadInt(pIndex, "version", &Version))
			return "no version";
		if(Version != INDEX_VERSION)
		{
			str_format(pError, ErrorSize, "the stream is of version %d, this reads version %d", Version, INDEX_VERSION);
			return pError;
		}
		std::string State;
		if(!ReadString(pIndex, "name", &m_Name) || !IsValidName(m_Name.c_str()))
			return "no valid name";
		if(!ReadInt64(pIndex, "stream", &m_Id) || !ReadString(pIndex, "state", &State) || !ReadInt64(pIndex, "delay_ms", &m_DelayMs) ||
			!ReadInt(pIndex, "segment_seconds", &m_SegmentSeconds) || !ReadInt(pIndex, "max_duration_seconds", &m_MaxDurationSeconds) || !ReadInt64(pIndex, "max_bytes", &m_MaxBytes))
		{
			return "what it says of the stream is not complete";
		}
		m_Live = State == "live";
		int TickSpeed;
		if(!ReadInt(pIndex, "tick_speed", &TickSpeed) || TickSpeed != SERVER_TICK_SPEED)
			return "unknown tick speed";

		const json_value *pEpochs = json_object_get(pIndex, "epochs");
		const json_value *pSegments = json_object_get(pIndex, "segments");
		const json_value *pMarkers = json_object_get(pIndex, "markers");
		if(pEpochs->type != json_array || pSegments->type != json_array || pMarkers->type != json_array)
			return "no epochs, segments or markers";
		for(int i = 0; i < json_array_length(pEpochs); i++)
		{
			const json_value *pEpoch = json_array_get(pEpochs, i);
			CEpoch Epoch;
			std::string Sha256;
			if(!ReadInt(pEpoch, "epoch", &Epoch.m_Id) || !ReadInt64(pEpoch, "bytes", &Epoch.m_InitBytes) || !ReadString(pEpoch, "map", &Epoch.m_Map) ||
				!ReadString(pEpoch, "sha256", &Sha256) || sha256_from_str(&Epoch.m_Sha256, Sha256.c_str()) != 0)
			{
				return "invalid epoch";
			}
			m_vEpochs.push_back(Epoch);
		}
		for(int i = 0; i < json_array_length(pSegments); i++)
		{
			const json_value *pSegment = json_array_get(pSegments, i);
			const json_value *pComplete = json_object_get(pSegment, "complete");
			CSegment Segment;
			if(!ReadInt(pSegment, "n", &Segment.m_Number) || !ReadInt(pSegment, "epoch", &Segment.m_Epoch) || !ReadInt(pSegment, "start_tick", &Segment.m_StartTick) ||
				!ReadInt(pSegment, "end_tick", &Segment.m_EndTick) || !ReadInt64(pSegment, "bytes", &Segment.m_Bytes) || !ReadInt64(pSegment, "time", &Segment.m_WallTime) ||
				pComplete->type != json_boolean || Segment.m_EndTick < Segment.m_StartTick)
			{
				return "invalid segment";
			}
			// Later segments are later in the demo, which the cuts rely on.
			if(!m_vSegments.empty() && (Segment.m_Number <= m_vSegments.back().m_Number || (Segment.m_Epoch == m_vSegments.back().m_Epoch && Segment.m_StartTick < m_vSegments.back().m_EndTick)))
				return "segments out of order";
			Segment.m_Complete = json_boolean_get(pComplete) != 0;
			m_vSegments.push_back(Segment);
		}
		for(int i = 0; i < json_array_length(pMarkers); i++)
		{
			const json_value *pMarker = json_array_get(pMarkers, i);
			CMarker Marker;
			std::string Kind;
			if(!ReadInt(pMarker, "epoch", &Marker.m_Epoch) || !ReadInt(pMarker, "tick", &Marker.m_Tick) || !ReadString(pMarker, "kind", &Kind))
				return "invalid marker";
			const std::optional<EMarkerKind> MarkerKind = MarkerKindFromName(Kind.c_str());
			if(!MarkerKind.has_value())
				return "unknown kind of marker";
			Marker.m_Kind = *MarkerKind;
			// A marker says something only if it was given something to say.
			ReadString(pMarker, "label", &Marker.m_Label);
			m_vMarkers.push_back(Marker);
		}
		return nullptr;
	};

	const char *pProblem = Read();
	json_value_free(pIndex);
	if(pProblem == nullptr)
		return true;
	if(pProblem != pError)
		str_copy(pError, pProblem, ErrorSize);
	*this = CLiveStream();
	return false;
}

std::vector<CLiveStream::CDemo> CLiveStream::Demos(ECut Cut) const
{
	std::vector<CDemo> vDemos;
	// The segments from one to another of an epoch are a demo.
	const auto Add = [&](const std::vector<const CSegment *> &vpSegments, size_t First, size_t Last, const std::string &Name) {
		CDemo &Demo = vDemos.emplace_back();
		Demo.m_Name = Name;
		Demo.m_Epoch = vpSegments[First]->m_Epoch;
		for(size_t i = First; i <= Last; i++)
			Demo.m_vSegments.push_back(vpSegments[i]->m_Number);
		Demo.m_FirstTick = vpSegments[First]->m_StartTick;
		Demo.m_LastTick = vpSegments[Last]->m_EndTick;
		// The same markers a recorder takes: inside the demo, one a second
		// at most, and as many as the header holds.
		for(const CMarker &Marker : m_vMarkers)
		{
			if(Marker.m_Epoch != Demo.m_Epoch || Marker.m_Tick < Demo.m_FirstTick || Marker.m_Tick > Demo.m_LastTick)
				continue;
			if(!Demo.m_vMarkers.empty() && Marker.m_Tick - Demo.m_vMarkers.back() < SERVER_TICK_SPEED)
				continue;
			if((int)Demo.m_vMarkers.size() == MAX_TIMELINE_MARKERS)
				break;
			Demo.m_vMarkers.push_back(Marker.m_Tick);
		}
	};

	for(const CEpoch &Epoch : m_vEpochs)
	{
		std::vector<const CSegment *> vpSegments;
		for(const CSegment &Segment : m_vSegments)
			if(Segment.m_Epoch == Epoch.m_Id && Segment.m_Complete)
				vpSegments.push_back(&Segment);
		if(vpSegments.empty())
			continue;
		if(Cut == ECut::EPOCHS)
		{
			Add(vpSegments, 0, vpSegments.size() - 1, m_Name + "_" + std::to_string(Epoch.m_Id));
			continue;
		}

		// Only the oldest segments are ever deleted. If the first one left is
		// not the first one written, nothing says that the epoch began with it.
		bool Whole = vpSegments.front() != &m_vSegments.front() || vpSegments.front()->m_Number == 0;
		size_t First = 0;
		for(const CMarker &Marker : m_vMarkers)
		{
			if(Marker.m_Epoch != Epoch.m_Id || Marker.m_Kind != EMarkerKind::MATCH_END || Marker.m_Tick < vpSegments[First]->m_StartTick || Marker.m_Tick > vpSegments.back()->m_EndTick)
				continue;
			size_t Last = First;
			while(Last + 1 < vpSegments.size() && vpSegments[Last + 1]->m_StartTick <= Marker.m_Tick)
				Last++;
			if(Whole)
				Add(vpSegments, First, Last, m_Name + "_" + std::to_string(Epoch.m_Id) + "_" + std::to_string(Marker.m_Tick));
			First = Last;
			Whole = true;
		}
	}
	if(Cut == ECut::EPOCHS && vDemos.size() == 1)
		vDemos.front().m_Name = m_Name;
	return vDemos;
}

bool CLiveStream::WriteDemo(const CDemo &Demo, IStorage *pStorage, int StorageType, const char *pStreamDir, const char *pFilename)
{
	IOHANDLE File = pStorage->OpenFile(pFilename, IOFLAG_WRITE, StorageType);
	if(!File)
		return false;
	const auto Copy = [&](const std::string &Name) {
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "%s/%s", pStreamDir, Name.c_str());
		return CopyFileInto(pStorage, StorageType, aPath, File);
	};
	bool Ok = Copy(InitFilename(Demo.m_Epoch));
	for(const int Segment : Demo.m_vSegments)
		Ok = Ok && Copy(SegmentFilename(Segment));
	Ok = Ok && CDemoRecorder::WriteLengthAndMarkers(File, (Demo.m_LastTick - Demo.m_FirstTick) / SERVER_TICK_SPEED, Demo.m_vMarkers.data(), (int)Demo.m_vMarkers.size());
	Ok = io_close(File) == 0 && Ok;
	if(!Ok)
		pStorage->RemoveFile(pFilename, StorageType);
	return Ok;
}
