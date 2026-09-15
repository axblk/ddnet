#include <base/str.h>

#include <engine/shared/datafile.h>

#include <game/gamecore.h>
#include <game/map/document/map_file.h>
#include <game/mapitems.h>
#include <game/mapitems_ex.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace map_document
{
	namespace
	{
		void Warn(std::vector<std::string> *pvWarnings, const char *pFormat, ...)
		{
			if(pvWarnings == nullptr)
				return;
			char aBuf[256];
			va_list Args;
			va_start(Args, pFormat);
			str_format_v(aBuf, sizeof(aBuf), pFormat, Args);
			va_end(Args);
			pvWarnings->emplace_back(aBuf);
		}

		/** A name as the file holds it: a handful of ints with the bytes in them. */
		std::string ReadName(const int *pInts, size_t NumInts)
		{
			char aName[64];
			dbg_assert(NumInts * sizeof(int) < sizeof(aName), "Name too long for the buffer");
			if(!IntsToStr(pInts, NumInts, aName, sizeof(aName)))
				return std::string();
			return std::string(aName);
		}

		/** Whether a stretch of bytes is nothing but zeroes. */
		bool IsAllZero(const void *pData, size_t Size)
		{
			const uint8_t *pBytes = static_cast<const uint8_t *>(pData);
			for(size_t i = 0; i < Size; ++i)
			{
				if(pBytes[i] != 0)
					return false;
			}
			return true;
		}

		/**
		 * The envelope points of a file, in whichever of the three ways they
		 * are in there: plain points, plain points with the bezier tangents
		 * beside them in an item of their own (DDNet), or points with the
		 * tangents inside them (upstream Teeworlds, from envelope version 3).
		 */
		class CEnvelopePoints
		{
		public:
			explicit CEnvelopePoints(CDataFileReader &File)
			{
				bool Bezier = false;
				int Start, Num;
				File.GetType(MAPITEMTYPE_ENVELOPE, &Start, &Num);
				for(int i = 0; i < Num; ++i)
				{
					const CMapItemEnvelope *pEnvelope = static_cast<CMapItemEnvelope *>(File.GetItem(Start + i));
					if(pEnvelope->m_Version >= CMapItemEnvelope::VERSION_TEEWORLDS_BEZIER)
					{
						Bezier = true;
						break;
					}
				}

				File.GetType(MAPITEMTYPE_ENVPOINTS, &Start, &Num);
				if(Num <= 0)
					return;
				if(Bezier)
				{
					m_pUpstream = static_cast<CEnvPointBezier_upstream *>(File.GetItem(Start));
					m_NumPoints = File.GetItemSize(Start) / sizeof(CEnvPointBezier_upstream);
					return;
				}

				m_pPoints = static_cast<CEnvPoint *>(File.GetItem(Start));
				m_NumPoints = File.GetItemSize(Start) / sizeof(CEnvPoint);

				int BezierStart, BezierNum;
				File.GetType(MAPITEMTYPE_ENVPOINTS_BEZIER, &BezierStart, &BezierNum);
				if(BezierNum > 0 && File.GetItemSize(BezierStart) / (int)sizeof(CEnvPointBezier) == m_NumPoints)
					m_pBezier = static_cast<CEnvPointBezier *>(File.GetItem(BezierStart));
			}

			int NumPoints() const { return m_NumPoints; }

			/** One point with its tangents, wherever they came from. */
			CEnvPoint_runtime Point(int Index) const
			{
				CEnvPoint_runtime Point = {};
				if(Index < 0 || Index >= m_NumPoints)
					return Point;
				if(m_pUpstream != nullptr)
				{
					std::memcpy(&Point, &m_pUpstream[Index], sizeof(CEnvPointBezier_upstream));
					return Point;
				}
				if(m_pPoints != nullptr)
					std::memcpy(&Point, &m_pPoints[Index], sizeof(CEnvPoint));
				if(m_pBezier != nullptr)
					Point.m_Bezier = m_pBezier[Index];
				return Point;
			}

		private:
			CEnvPoint *m_pPoints = nullptr;
			CEnvPointBezier *m_pBezier = nullptr;
			CEnvPointBezier_upstream *m_pUpstream = nullptr;
			int m_NumPoints = 0;
		};

		void ReadInfo(CDataFileReader &File, CMapInfo *pInfo, std::vector<std::string> *pvWarnings)
		{
			int Start, Num;
			File.GetType(MAPITEMTYPE_INFO, &Start, &Num);
			for(int i = Start; i < Start + Num; ++i)
			{
				int ItemId;
				const int ItemSize = File.GetItemSize(i);
				const CMapItemInfoSettings *pItem = static_cast<CMapItemInfoSettings *>(File.GetItem(i, nullptr, &ItemId));
				if(pItem == nullptr || ItemId != 0)
					continue;

				const auto &&ReadString = [&](int Index, const char *pWhat) -> std::string {
					const char *pStr = File.GetDataString(Index);
					if(pStr != nullptr)
						return std::string(pStr);
					if(Index >= 0)
						Warn(pvWarnings, "Failed to read the %s from the map info.", pWhat);
					return std::string();
				};
				pInfo->m_Author = ReadString(pItem->m_Author, "author");
				pInfo->m_MapVersion = ReadString(pItem->m_MapVersion, "version");
				pInfo->m_Credits = ReadString(pItem->m_Credits, "credits");
				pInfo->m_License = ReadString(pItem->m_License, "license");

				if(pItem->m_Version != 1 || ItemSize < (int)sizeof(CMapItemInfoSettings) || pItem->m_Settings <= -1)
					break;

				// The settings are one blob of lines, each ended by a zero
				// byte, which is also how they are written back.
				const unsigned Size = File.GetDataSize(pItem->m_Settings);
				const char *pSettings = static_cast<const char *>(File.GetData(pItem->m_Settings));
				if(pSettings == nullptr)
				{
					Warn(pvWarnings, "Failed to read the server settings from the map info.");
					break;
				}
				std::vector<std::string> &vSettings = pInfo->m_Settings.Mutable();
				const char *pNext = pSettings;
				while(pNext < pSettings + Size)
				{
					vSettings.emplace_back(pNext);
					pNext += str_length(pNext) + 1;
				}
				break;
			}
		}

		void ReadImages(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings)
		{
			int Start, Num;
			File.GetType(MAPITEMTYPE_IMAGE, &Start, &Num);
			for(int i = 0; i < Num; ++i)
			{
				const CMapItemImage_v2 *pItem = static_cast<CMapItemImage_v2 *>(File.GetItem(Start + i));
				CImage Image;
				Image.m_External = pItem->m_External != 0;
				// Version 2 says an image is embedded by putting a 1 in a
				// field of its own; anything else means the pixels are not in
				// here, whatever the external flag says.
				if(pItem->m_Version > 1 && pItem->m_MustBe1 != 1)
					Image.m_External = true;

				const char *pName = File.GetDataString(pItem->m_ImageName);
				if(pName == nullptr || pName[0] == '\0')
				{
					Warn(pvWarnings, "Image %d has no name.", i);
				}
				else
				{
					Image.m_Name = pName;
				}

				if(!Image.m_External)
				{
					Image.m_Width = pItem->m_Width;
					Image.m_Height = pItem->m_Height;
					const size_t Size = (size_t)std::max(0, pItem->m_Width) * std::max(0, pItem->m_Height) * 4;
					const void *pData = File.GetData(pItem->m_ImageData);
					if(Size == 0 || pData == nullptr || (size_t)File.GetDataSize(pItem->m_ImageData) < Size)
					{
						Warn(pvWarnings, "Failed to read the pixels of image %d ('%s').", i, Image.m_Name.c_str());
						Image.m_Width = 0;
						Image.m_Height = 0;
					}
					else
					{
						std::vector<uint8_t> &vData = Image.m_Data.Mutable();
						vData.resize(Size);
						std::memcpy(vData.data(), pData, Size);
					}
					File.UnloadData(pItem->m_ImageData);
				}
				pState->AddImage(std::move(Image));
			}
		}

		void ReadSounds(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings)
		{
			int Start, Num;
			File.GetType(MAPITEMTYPE_SOUND, &Start, &Num);
			for(int i = 0; i < Num; ++i)
			{
				const CMapItemSound *pItem = static_cast<CMapItemSound *>(File.GetItem(Start + i));
				CSound Sound;
				Sound.m_External = pItem->m_External != 0;

				const char *pName = File.GetDataString(pItem->m_SoundName);
				if(pName == nullptr || pName[0] == '\0')
					Warn(pvWarnings, "Sound %d has no name.", i);
				else
					Sound.m_Name = pName;

				if(!Sound.m_External)
				{
					// The size in the item is not to be trusted, the one the
					// file itself knows is.
					const int Size = File.GetDataSize(pItem->m_SoundData);
					const void *pData = File.GetData(pItem->m_SoundData);
					if(Size <= 0 || pData == nullptr)
					{
						Warn(pvWarnings, "Failed to read the bytes of sound %d ('%s').", i, Sound.m_Name.c_str());
					}
					else
					{
						std::vector<uint8_t> &vData = Sound.m_Data.Mutable();
						vData.resize(Size);
						std::memcpy(vData.data(), pData, Size);
					}
					File.UnloadData(pItem->m_SoundData);
				}
				pState->AddSound(std::move(Sound));
			}
		}

		void ReadEnvelopes(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings)
		{
			const CEnvelopePoints Points(File);
			int Start, Num;
			File.GetType(MAPITEMTYPE_ENVELOPE, &Start, &Num);
			for(int i = 0; i < Num; ++i)
			{
				const CMapItemEnvelope *pItem = static_cast<CMapItemEnvelope *>(File.GetItem(Start + i));
				CEnvelope Envelope;
				Envelope.m_Channels = pItem->m_Channels;
				if(pItem->m_Version >= 2)
					Envelope.m_Synchronized = pItem->m_Synchronized != 0;
				// A name of -1 in its first int is what a map from before
				// envelopes had names looks like.
				if(pItem->m_aName[0] != -1)
					Envelope.m_Name = ReadName(pItem->m_aName, std::size(pItem->m_aName));

				std::vector<CEnvPoint_runtime> &vPoints = Envelope.m_Points.Mutable();
				vPoints.reserve(std::max(0, pItem->m_NumPoints));
				for(int p = 0; p < pItem->m_NumPoints; ++p)
				{
					const int Index = pItem->m_StartPoint + p;
					if(Index < 0 || Index >= Points.NumPoints())
					{
						Warn(pvWarnings, "Envelope %d points past the end of the points of the map.", i);
						break;
					}
					vPoints.push_back(Points.Point(Index));
				}
				pState->AddEnvelope(std::move(Envelope));
			}
		}

		/** Which kind of tile layer the flags of a tilemap item make it. */
		ETileLayerKind TileLayerKind(int Flags)
		{
			if(Flags & TILESLAYERFLAG_GAME)
				return ETileLayerKind::GAME;
			if(Flags & TILESLAYERFLAG_TELE)
				return ETileLayerKind::TELE;
			if(Flags & TILESLAYERFLAG_SPEEDUP)
				return ETileLayerKind::SPEEDUP;
			if(Flags & TILESLAYERFLAG_FRONT)
				return ETileLayerKind::FRONT;
			if(Flags & TILESLAYERFLAG_SWITCH)
				return ETileLayerKind::SWITCH;
			if(Flags & TILESLAYERFLAG_TUNE)
				return ETileLayerKind::TUNE;
			return ETileLayerKind::TILES;
		}

		/**
		 * Reads one plane of tiles out of a data item into a store, and says
		 * whether it was there to be read.
		 */
		template<typename TTile>
		bool ReadPlane(CDataFileReader &File, int DataIndex, CTileStore<TTile> *pStore, std::vector<std::string> *pvWarnings, const char *pWhat, int Layer)
		{
			const size_t Size = (size_t)pStore->Width() * pStore->Height() * sizeof(TTile);
			if(Size == 0)
				return true;
			const void *pData = File.GetData(DataIndex);
			if(pData == nullptr || (size_t)File.GetDataSize(DataIndex) < Size)
			{
				Warn(pvWarnings, "Failed to read the %s of layer %d.", pWhat, Layer);
				return false;
			}
			pStore->SetAll(static_cast<const TTile *>(pData));
			File.UnloadData(DataIndex);
			return true;
		}

		CLayer ReadTileLayer(CDataFileReader &File, int ItemIndex, int Layer, std::vector<std::string> *pvWarnings)
		{
			const CMapItemLayerTilemap *pItem = static_cast<CMapItemLayerTilemap *>(File.GetItem(ItemIndex));
			const int ItemSize = File.GetItemSize(ItemIndex);
			const int Width = std::max(0, pItem->m_Width);
			const int Height = std::max(0, pItem->m_Height);

			CTileLayer Tiles(TileLayerKind(pItem->m_Flags), Width, Height);
			Tiles.m_Detail = (pItem->m_Layer.m_Flags & LAYERFLAG_DETAIL) != 0;
			Tiles.m_Image = pItem->m_Image;
			Tiles.m_Color = pItem->m_Color;
			Tiles.m_ColorEnvelope = pItem->m_ColorEnv;
			Tiles.m_ColorEnvelopeOffset = pItem->m_ColorEnvOffset;
			// Names arrived with version 3 of the item; asking an older one
			// for its name reads whatever comes after it in the file.
			if(pItem->m_Version >= 3 && ItemSize >= (int)sizeof(CMapItemLayerTilemap_v3Teeworlds))
				Tiles.m_Name = ReadName(pItem->m_aName, std::size(pItem->m_aName));

			// A physics layer keeps its own plane in a data item of its own,
			// and the plane of tiles every layer has is air in the file. The
			// front layer is the exception: it has no second plane, its tiles
			// are the ones in `m_Front`.
			const bool HasSecondPlane = ItemSize >= (int)sizeof(CMapItemLayerTilemap);
			switch(Tiles.m_Kind)
			{
			case ETileLayerKind::FRONT:
				if(HasSecondPlane)
				{
					const size_t Size = (size_t)Width * Height * sizeof(CTile);
					const void *pAir = File.GetData(pItem->m_Data);
					if(Size > 0 && pAir != nullptr && (size_t)File.GetDataSize(pItem->m_Data) >= Size && !IsAllZero(pAir, Size))
						Warn(pvWarnings, "The front layer %d has tiles in the plane every layer has, which are dropped.", Layer);
					File.UnloadData(pItem->m_Data);
					ReadPlane(File, pItem->m_Front, &Tiles.m_Tiles, pvWarnings, "front tiles", Layer);
				}
				break;
			case ETileLayerKind::TELE:
				ReadPlane(File, pItem->m_Data, &Tiles.m_Tiles, pvWarnings, "tiles", Layer);
				if(HasSecondPlane)
					ReadPlane(File, pItem->m_Tele, &std::get<CTileStore<CTeleTile>>(Tiles.m_ExtraTiles), pvWarnings, "tele tiles", Layer);
				break;
			case ETileLayerKind::SPEEDUP:
				ReadPlane(File, pItem->m_Data, &Tiles.m_Tiles, pvWarnings, "tiles", Layer);
				if(HasSecondPlane)
					ReadPlane(File, pItem->m_Speedup, &std::get<CTileStore<CSpeedupTile>>(Tiles.m_ExtraTiles), pvWarnings, "speedup tiles", Layer);
				break;
			case ETileLayerKind::SWITCH:
				ReadPlane(File, pItem->m_Data, &Tiles.m_Tiles, pvWarnings, "tiles", Layer);
				if(HasSecondPlane)
					ReadPlane(File, pItem->m_Switch, &std::get<CTileStore<CSwitchTile>>(Tiles.m_ExtraTiles), pvWarnings, "switch tiles", Layer);
				break;
			case ETileLayerKind::TUNE:
				ReadPlane(File, pItem->m_Data, &Tiles.m_Tiles, pvWarnings, "tiles", Layer);
				if(HasSecondPlane)
					ReadPlane(File, pItem->m_Tune, &std::get<CTileStore<CTuneTile>>(Tiles.m_ExtraTiles), pvWarnings, "tune tiles", Layer);
				break;
			case ETileLayerKind::TILES:
			case ETileLayerKind::GAME:
				ReadPlane(File, pItem->m_Data, &Tiles.m_Tiles, pvWarnings, "tiles", Layer);
				break;
			}
			return CLayer(std::move(Tiles));
		}

		CLayer ReadQuadLayer(CDataFileReader &File, int ItemIndex, int Layer, std::vector<std::string> *pvWarnings)
		{
			const CMapItemLayerQuads *pItem = static_cast<CMapItemLayerQuads *>(File.GetItem(ItemIndex));
			CQuadLayer Quads;
			Quads.m_Detail = (pItem->m_Layer.m_Flags & LAYERFLAG_DETAIL) != 0;
			Quads.m_Image = pItem->m_Image;
			if(File.GetItemSize(ItemIndex) >= (int)sizeof(CMapItemLayerQuads))
				Quads.m_Name = ReadName(pItem->m_aName, std::size(pItem->m_aName));

			if(pItem->m_NumQuads > 0)
			{
				const size_t Size = sizeof(CQuad) * (size_t)pItem->m_NumQuads;
				// Quads are written as ints the other way round on a machine
				// that stores them the other way round.
				const void *pData = File.GetDataSwapped(pItem->m_Data);
				if(pData == nullptr || (size_t)File.GetDataSize(pItem->m_Data) < Size)
				{
					Warn(pvWarnings, "Failed to read the quads of layer %d.", Layer);
				}
				else
				{
					std::vector<CQuad> &vQuads = Quads.m_Quads.Mutable();
					vQuads.resize(pItem->m_NumQuads);
					std::memcpy(vQuads.data(), pData, Size);
				}
				File.UnloadData(pItem->m_Data);
			}
			return CLayer(std::move(Quads));
		}

		CLayer ReadSoundLayer(CDataFileReader &File, int ItemIndex, int Layer, std::vector<std::string> *pvWarnings)
		{
			const CMapItemLayerSounds *pItem = static_cast<CMapItemLayerSounds *>(File.GetItem(ItemIndex));
			CSoundLayer Sounds;
			Sounds.m_Detail = (pItem->m_Layer.m_Flags & LAYERFLAG_DETAIL) != 0;
			Sounds.m_Sound = pItem->m_Sound;
			if(File.GetItemSize(ItemIndex) >= (int)sizeof(CMapItemLayerSounds))
				Sounds.m_Name = ReadName(pItem->m_aName, std::size(pItem->m_aName));

			if(pItem->m_NumSources > 0)
			{
				const size_t Size = sizeof(CSoundSource) * (size_t)pItem->m_NumSources;
				const void *pData = File.GetDataSwapped(pItem->m_Data);
				if(pData == nullptr || (size_t)File.GetDataSize(pItem->m_Data) < Size)
				{
					Warn(pvWarnings, "Failed to read the sound sources of layer %d.", Layer);
				}
				else
				{
					std::vector<CSoundSource> &vSources = Sounds.m_Sources.Mutable();
					vSources.resize(pItem->m_NumSources);
					std::memcpy(vSources.data(), pData, Size);
				}
				File.UnloadData(pItem->m_Data);
			}
			return CLayer(std::move(Sounds));
		}

		void ReadGroups(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings)
		{
			int LayersStart, LayersNum;
			File.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);

			int Start, Num;
			File.GetType(MAPITEMTYPE_GROUP, &Start, &Num);
			for(int g = 0; g < Num; ++g)
			{
				const int ItemIndex = Start + g;
				const CMapItemGroup *pItem = static_cast<CMapItemGroup *>(File.GetItem(ItemIndex));
				const int ItemSize = File.GetItemSize(ItemIndex);
				// Version 3 is the newest; a group from a newer one would
				// have fields in it that are not read here.
				if(pItem->m_Version < 1 || pItem->m_Version > 3)
				{
					Warn(pvWarnings, "Group %d is of version %d, which cannot be read.", g, pItem->m_Version);
					continue;
				}

				CGroup Group;
				Group.m_OffsetX = pItem->m_OffsetX;
				Group.m_OffsetY = pItem->m_OffsetY;
				Group.m_ParallaxX = pItem->m_ParallaxX;
				Group.m_ParallaxY = pItem->m_ParallaxY;
				if(pItem->m_Version >= 2 && ItemSize >= (int)sizeof(CMapItemGroup))
				{
					Group.m_UseClipping = pItem->m_UseClipping != 0;
					Group.m_ClipX = pItem->m_ClipX;
					Group.m_ClipY = pItem->m_ClipY;
					Group.m_ClipW = pItem->m_ClipW;
					Group.m_ClipH = pItem->m_ClipH;
				}
				if(pItem->m_Version >= 3 && ItemSize >= (int)sizeof(CMapItemGroup))
					Group.m_Name = ReadName(pItem->m_aName, std::size(pItem->m_aName));

				for(int l = 0; l < pItem->m_NumLayers; ++l)
				{
					const int LayerIndex = pItem->m_StartLayer + l;
					if(LayerIndex < 0 || LayerIndex >= LayersNum)
					{
						Warn(pvWarnings, "Group %d points at layer %d, which the map does not have.", g, LayerIndex);
						continue;
					}
					const int LayerItem = LayersStart + LayerIndex;
					const CMapItemLayer *pLayerItem = static_cast<CMapItemLayer *>(File.GetItem(LayerItem));
					if(pLayerItem == nullptr)
						continue;

					if(pLayerItem->m_Type == LAYERTYPE_TILES)
						Group.m_vpLayers.push_back(std::make_shared<const CLayer>(ReadTileLayer(File, LayerItem, LayerIndex, pvWarnings)));
					else if(pLayerItem->m_Type == LAYERTYPE_QUADS)
						Group.m_vpLayers.push_back(std::make_shared<const CLayer>(ReadQuadLayer(File, LayerItem, LayerIndex, pvWarnings)));
					else if(pLayerItem->m_Type == LAYERTYPE_SOUNDS)
						Group.m_vpLayers.push_back(std::make_shared<const CLayer>(ReadSoundLayer(File, LayerItem, LayerIndex, pvWarnings)));
					else
						Warn(pvWarnings, "Layer %d is of a kind (%d) that is not read.", LayerIndex, pLayerItem->m_Type);
				}
				pState->AddGroup(std::move(Group));
			}
		}
	} // namespace

	bool ReadMapState(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings)
	{
		const CMapItemVersion *pVersion = static_cast<CMapItemVersion *>(File.FindItem(MAPITEMTYPE_VERSION, 0));
		// Version 1 is the only one there has ever been.
		if(pVersion == nullptr || pVersion->m_Version != 1)
			return false;

		// In this order, because a layer names an image by number and an
		// envelope by number, and a reader that has them already can say so
		// when a number points at nothing.
		ReadInfo(File, &pState->m_Info, pvWarnings);
		ReadImages(File, pState, pvWarnings);
		ReadSounds(File, pState, pvWarnings);
		ReadEnvelopes(File, pState, pvWarnings);
		ReadGroups(File, pState, pvWarnings);
		return true;
	}
} // namespace map_document
