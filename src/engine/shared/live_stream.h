/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_LIVE_STREAM_H
#define ENGINE_SHARED_LIVE_STREAM_H

#include <base/hash.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class IStorage;

/**
 * What the `index.json` of a live demo stream says (see `CLiveRecorder`, which
 * writes the stream), and how the files of a stream make demos again. The
 * server keeps demos with this when a stream stops, and `live_to_demo` makes
 * them from a stream directory afterwards.
 */
class CLiveStream
{
public:
	/**
	 * The version of `index.json`, which the web demo player checks
	 * (`LIVE_INDEX_VERSION` in `demo-player.js`).
	 */
	static constexpr int INDEX_VERSION = 1;
	/**
	 * A name of a stream is shorter than this.
	 */
	static constexpr int NAME_SIZE = 128;

	enum class EMarkerKind
	{
		MATCH_END,
		MANUAL,
	};

	/**
	 * The demo of one map from where the stream began or woke up, with its
	 * header and the map in `init-<id>`.
	 */
	class CEpoch
	{
	public:
		int m_Id;
		std::string m_Map;
		SHA256_DIGEST m_Sha256;
		int64_t m_InitBytes;
	};

	/**
	 * A piece of an epoch's demo in `seg-<number>`, from a keyframe on.
	 */
	class CSegment
	{
	public:
		int m_Number;
		int m_Epoch;
		int m_StartTick;
		int m_EndTick;
		int64_t m_Bytes;
		int64_t m_WallTime;
		/**
		 * Whether the segment is written to its end. The last one of a
		 * stream that is live still grows.
		 */
		bool m_Complete;
	};

	class CMarker
	{
	public:
		int m_Epoch;
		int m_Tick;
		EMarkerKind m_Kind;
		std::string m_Label;
	};

	/**
	 * Where demos are cut.
	 */
	enum class ECut
	{
		/**
		 * One demo per epoch, with all that the stream still has of it.
		 */
		EPOCHS,
		/**
		 * One demo per match that ended, see `Demos`.
		 */
		MATCHES,
	};

	/**
	 * A demo that the files of the stream make: the init of the epoch and
	 * some of its segments in a row.
	 */
	class CDemo
	{
	public:
		/**
		 * The filename without directory and extension.
		 */
		std::string m_Name;
		int m_Epoch;
		/**
		 * The numbers of the segments, oldest first.
		 */
		std::vector<int> m_vSegments;
		int m_FirstTick;
		int m_LastTick;
		/**
		 * The ticks of the markers that go into the demo's header.
		 */
		std::vector<int> m_vMarkers;
	};

	std::string m_Name;
	/**
	 * When the stream started, in milliseconds since the Unix epoch. A stream
	 * of the same name that starts later has another one.
	 */
	int64_t m_Id = 0;
	bool m_Live = false;
	int64_t m_DelayMs = 0;
	int m_SegmentSeconds = 0;
	int m_MaxDurationSeconds = 0;
	/**
	 * Bytes, where `sv_live_max_size` is KiB.
	 */
	int64_t m_MaxBytes = 0;
	/**
	 * Oldest first, as the segments and the markers are.
	 */
	std::vector<CEpoch> m_vEpochs;
	std::vector<CSegment> m_vSegments;
	std::vector<CMarker> m_vMarkers;

	/**
	 * Whether a stream can have this name, which is also the name of its
	 * directory and the beginning of the names of its demos.
	 */
	static bool IsValidName(const char *pName);
	/**
	 * What the index calls a kind of marker.
	 */
	static const char *MarkerKindName(EMarkerKind Kind);
	/**
	 * The name of an epoch's init in the directory of the stream.
	 */
	static std::string InitFilename(int Epoch);
	/**
	 * The name of a segment in the directory of the stream.
	 */
	static std::string SegmentFilename(int Number);

	/**
	 * @param Updated The current time in milliseconds since the Unix epoch.
	 *
	 * @return The `index.json` that says this.
	 */
	std::string FormatIndex(int64_t Updated) const;
	/**
	 * Takes what an `index.json` says. Filenames in it are not trusted, the
	 * files are found by their epoch and number.
	 *
	 * @param pJson The contents of the file.
	 * @param Size Their size.
	 * @param pError Says what is wrong with it.
	 * @param ErrorSize The size of `pError`.
	 *
	 * @return Whether it is the index of a stream this version reads.
	 */
	bool ParseIndex(const char *pJson, size_t Size, char *pError, size_t ErrorSize);

	/**
	 * The demos that the stream makes. Only segments that are complete count.
	 *
	 * Cut by epochs, a single demo has the name of the stream and several
	 * have `_<epoch>` behind it. Their markers are those of the stream.
	 *
	 * Cut by matches, a demo is called `<stream>_<epoch>_<tick>` after the
	 * tick its match ended on, which stays the same however often a growing
	 * stream is read. A cut is where a segment begins: the demo starts with
	 * the segment the match before ended in, or with the epoch, and ends with
	 * the segment its own match ended in. So it opens with at most one segment
	 * of the match before and its scoreboard, and what follows the last match
	 * end of an epoch is in no demo. A match whose beginning the limits of the
	 * stream may already have deleted is left out as well.
	 *
	 * @param Cut Where to cut.
	 *
	 * @return The demos, oldest first.
	 */
	std::vector<CDemo> Demos(ECut Cut) const;

	/**
	 * Writes one of the demos. A demo that could not be written whole is
	 * removed again.
	 *
	 * @param Demo One of `Demos`.
	 * @param pStorage Reads the stream and writes the demo.
	 * @param StorageType Where both are, `IStorage::TYPE_SAVE` or `IStorage::TYPE_ABSOLUTE`.
	 * @param pStreamDir The directory of the stream.
	 * @param pFilename The demo to write.
	 *
	 * @return Whether the demo is there.
	 */
	static bool WriteDemo(const CDemo &Demo, IStorage *pStorage, int StorageType, const char *pStreamDir, const char *pFilename);
};

#endif
