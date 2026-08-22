/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CHAT_H
#define GAME_CLIENT_COMPONENTS_CHAT_H

#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/shared/ringbuffer.h>

#include <generated/protocol7.h>

#include <game/client/component.h>
#include <game/client/game_view.h>
#include <game/client/lineinput.h>
#include <game/client/render.h>
#include <game/client/session_context.h>

#include <vector>

constexpr auto SAVES_FILE = "ddnet-saves.txt";

class CChat : public CComponent
{
	static constexpr float CHAT_HEIGHT_FULL = 200.0f;
	static constexpr float CHAT_HEIGHT_MIN = 50.0f;
	static constexpr float CHAT_FONTSIZE_WIDTH_RATIO = 2.5f;

	static constexpr int MAX_LINE_LENGTH = CSessionChatState::MAX_LINE_LENGTH;

	CLineInputBuffered<MAX_LINE_LENGTH> m_Input;
	class CCachedLine
	{
	public:
		CCachedLine();
		void Reset(CChat &This);
		void Invalidate(CChat &This);

		uint64_t m_LineId = 0;
		uint64_t m_Revision = 0;
		CGameStateId m_StateId;
		CGameViewId m_ViewId;
		CViewport m_Viewport;
		uint64_t m_OutputCacheKey = 0;
		bool m_ScoreboardOpen = false;
		bool m_ShowLargeArea = false;
		float m_YOffset = -1.0f;
		STextContainerIndex m_TextContainerIndex;
		int m_QuadContainerIndex = -1;
		std::shared_ptr<CManagedTeeRenderInfo> m_pManagedTeeRenderInfo;
		float m_TextYOffset = 0.0f;
	};

	class CSessionCache
	{
	public:
		CSessionId m_SessionId;
		std::array<CCachedLine, CSessionChatState::MAX_LINES> m_aLines;
	};
	std::vector<CSessionCache> m_vSessionCaches;

	enum
	{
		// client IDs for special messages
		CLIENT_MSG = -2,
		SERVER_MSG = -1,
	};

	enum
	{
		MODE_NONE = 0,
		MODE_ALL,
		MODE_TEAM,
	};

	enum
	{
		CHAT_SERVER = 0,
		CHAT_HIGHLIGHT,
		CHAT_CLIENT,
		CHAT_NUM,
	};

	int m_Mode;
	bool m_Show;
	CGameViewId m_ShowViewId;
	CSessionId m_ShowSessionId;
	CGameStateId m_ShowStateId;
	CGameViewId m_InputViewId;
	CSessionId m_InputSessionId;
	CGameStateId m_InputStateId;
	CStreamId m_InputStreamId;
	bool m_CompletionUsed;
	int m_CompletionChosen;
	char m_aCompletionBuffer[MAX_LINE_LENGTH];
	int m_PlaceholderOffset;
	int m_PlaceholderLength;
	static char ms_aDisplayText[MAX_LINE_LENGTH];
	class CRateablePlayer
	{
	public:
		int m_ClientId;
		int m_Score;
	};
	CRateablePlayer m_aPlayerCompletionList[MAX_CLIENTS];
	int m_PlayerCompletionListLength;

	struct CHistoryEntry
	{
		int m_Team;
		char m_aText[1];
	};
	CHistoryEntry *m_pHistoryEntry;
	CStaticRingBuffer<CHistoryEntry, 64 * 1024, CRingBufferBase::FLAG_RECYCLE> m_History;
	int64_t m_aaLastSoundPlayed[2][CHAT_NUM];
	bool m_IsInputCensored;
	char m_aCurrentInputText[MAX_LINE_LENGTH];
	bool m_EditingNewLine;

	static void ConSay(IConsole::IResult *pResult, void *pUserData);
	static void ConSayTeam(IConsole::IResult *pResult, void *pUserData);
	static void ConChat(IConsole::IResult *pResult, void *pUserData);
	static void ConShowChat(IConsole::IResult *pResult, void *pUserData);
	static void ConEcho(IConsole::IResult *pResult, void *pUserData);
	static void ConClearChat(IConsole::IResult *pResult, void *pUserData);

	static void ConchainChatOld(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainChatFontSize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainChatWidth(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	bool LineShouldHighlight(const char *pLine, const char *pName);
	void StoreSave(const char *pText, const CGameSessionContext &Session);
	CSessionCache &SessionCache(CSessionId SessionId);
	CCachedLine &CachedLine(CSessionId SessionId, uint64_t LineId);
	void CacheAppearance(CSessionId SessionId, const CGameState &State, const CSessionChatState::CLine &Line);
	void RenderLines(const CRenderContext &Context, float y);
	void OnPrepareLines(const CRenderContext &Context, float y);
	bool InputMatches(const CRenderContext &Context) const;
	bool ShowMatches(const CRenderContext &Context) const;

public:
	CChat();
	int Sizeof() const override { return sizeof(*this); }

	static constexpr float MESSAGE_TEE_PADDING_RIGHT = 0.5f;

	bool IsActive() const { return m_Mode != MODE_NONE; }
	void AddLine(CGameSessionContext &Session, const CGameState &State, int64_t Now, bool IsDemoPlayback, bool ApplicationEffects, int ClientId, int Team, const char *pLine);
	void HandleMessage(CGameSessionContext &Session, const CGameState &State, int64_t Now, bool SuppressEvents, bool IsDemoPlayback, bool ApplicationEffects, int MsgType, void *pRawMsg);
	void EnableMode(int Team);
	void DisableMode();
	void Echo(const char *pString);

	void OnWindowResize() override;
	void OnConsoleInit() override;
	void OnStateChange(int NewState, int OldState) override;
	void UpdateController(const CRenderContext &Context);
	void OnRender(const CRenderContext &Context) override;
	void RenderApplicationOverlay(const CRenderContext &Context);
	void Reset();
	void ResetSession(CSessionId SessionId);
	void OnRelease() override;
	bool OnInput(const IInput::CEvent &Event) override;
	void OnInit() override;

	void RebuildChat();
	void ClearLines(CSessionId SessionId);

	void EnsureCoherentFontSize() const;
	void EnsureCoherentWidth() const;

	float FontSize() const { return g_Config.m_ClChatFontSize / 10.0f; }
	float MessagePaddingX() const { return FontSize() * (5 / 6.f); }
	float MessagePaddingY() const { return FontSize() * (1 / 6.f); }
	float MessageTeeSize() const { return FontSize() * (7 / 6.f); }
	float MessageRounding() const { return FontSize() * (1 / 2.f); }

	// ----- send functions -----

	// Sends a chat message to the server.
	//
	// @param Team MODE_ALL=0 MODE_TEAM=1
	// @param pLine the chat message
	void SendChat(int Team, const char *pLine);
	void SendChat(int Team, const char *pLine, CSessionId SessionId, CStreamId StreamId);

	// Sends a chat message to the server.
	//
	// It uses a queue with a maximum of 3 entries
	// that ensures there is a minimum delay of one second
	// between sent messages.
	//
	// It uses team or public chat depending on m_Mode.
	void SendChatQueued(const char *pLine);
};
#endif
