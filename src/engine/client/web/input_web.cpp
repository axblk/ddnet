// The input of the web tools: the keys, the pointer, the wheel and the fingers
// on the canvas, as `web_platform.js` hands them over. The page's thread pushes
// them into a queue, the program takes them out once a frame in `Update`.
// There is no text input, no clipboard and no joystick: the tools are steered
// with a few keys and the pointer, and the page brings everything else.

#include "web_platform.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>

namespace
{
	// The numbers `web_platform.js` hands over events with.
	enum EWebInputEvent
	{
		WEB_KEY_DOWN = 1,
		WEB_KEY_UP,
		WEB_KEY_REPEAT,
		WEB_POINTER_DOWN,
		WEB_POINTER_MOVE,
		WEB_POINTER_UP,
		WEB_WHEEL,
		WEB_VISIBLE,
		WEB_RELEASE_ALL,
	};

	// What `Code` of a pointer event holds.
	constexpr int POINTER_BUTTON_MASK = 0xff;
	constexpr int POINTER_TOUCH = 1 << 8;
	constexpr int POINTER_PRIMARY = 1 << 9;

	struct SWebInputEvent
	{
		int m_Type;
		int m_Code;
		int m_Id;
		float m_X;
		float m_Y;
	};

	// Written by the page's thread alone and read by the program's alone, so
	// two counters are all the locking there is. What does not fit while the
	// program is busy is dropped, which is at worst a lost move of the pointer.
	class CWebInputQueue
	{
		static constexpr uint32_t SIZE = 1024;
		std::array<SWebInputEvent, SIZE> m_aEvents;
		std::atomic<uint32_t> m_Write{0};
		std::atomic<uint32_t> m_Read{0};

	public:
		void Push(const SWebInputEvent &Event)
		{
			const uint32_t Write = m_Write.load(std::memory_order_relaxed);
			if(Write - m_Read.load(std::memory_order_acquire) >= SIZE)
				return;
			m_aEvents[Write % SIZE] = Event;
			m_Write.store(Write + 1, std::memory_order_release);
		}

		bool Pop(SWebInputEvent &Event)
		{
			const uint32_t Read = m_Read.load(std::memory_order_relaxed);
			if(Read == m_Write.load(std::memory_order_acquire))
				return false;
			Event = m_aEvents[Read % SIZE];
			m_Read.store(Read + 1, std::memory_order_release);
			return true;
		}
	};

	CWebInputQueue gs_Queue;
	std::atomic<bool> gs_PageVisible{true};
	std::atomic<bool> gs_Quit{false};
	// A file the page put where the program can open it, see
	// `EmscriptenCallbackDropFile`.
	std::atomic<char *> gs_pDropFile{nullptr};
} // namespace

bool WebPageVisible()
{
	return gs_PageVisible.load(std::memory_order_relaxed);
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void WebInputPush(int Type, int Code, int Id, float X, float Y)
{
	if(Type == WEB_VISIBLE)
		gs_PageVisible.store(Code != 0, std::memory_order_relaxed);
	gs_Queue.Push({Type, Code, Id, X, Y});
}

// The page asks for these three by name, whatever program is behind the
// canvas.
void EmscriptenCallbackDropFile(const char *pFile)
{
	char *pOld = gs_pDropFile.exchange(strdup(pFile));
	free(pOld);
}

void EmscriptenCallbackQuit()
{
	gs_Quit.store(true);
}

void EmscriptenCallbackQuitForce()
{
	emscripten_force_exit(-1);
}
}

class CWebInput : public IEngineInput
{
	IEngineGraphics *m_pGraphics = nullptr;
	IEngineGraphicsWindow *m_pWindow = nullptr;

	std::vector<CEvent> m_vEvents;
	uint32_t m_InputCounter = 1;
	bool m_aCurrentKeyStates[KEY_LAST] = {};
	bool m_aFrameKeyStates[KEY_LAST] = {};
	std::vector<CTouchFingerState> m_vTouchFingerStates;
	// The pointer in CSS pixels of the canvas, and its buttons by the numbers
	// of `MouseEvent.button`.
	vec2 m_MousePos = vec2(0.0f, 0.0f);
	unsigned m_MouseButtons = 0;
	int64_t m_LastUpdate = 0;
	float m_UpdateTime = 0.0f;

	void AddKeyEvent(int Key, int Flags)
	{
		if(Key <= KEY_FIRST || Key >= KEY_LAST)
			return;
		// A key that is let go of without having been pressed was pressed
		// before the canvas had the focus.
		if((Flags & FLAG_RELEASE) != 0 && !m_aCurrentKeyStates[Key])
			return;
		CEvent Event;
		Event.m_Key = Key;
		Event.m_Flags = Flags;
		Event.m_aText[0] = '\0';
		Event.m_InputCount = m_InputCounter;
		m_vEvents.push_back(Event);
		if(Flags & FLAG_PRESS)
		{
			m_aCurrentKeyStates[Key] = true;
			m_aFrameKeyStates[Key] = true;
		}
		if(Flags & FLAG_RELEASE)
			m_aCurrentKeyStates[Key] = false;
	}

	void ReleaseAll()
	{
		for(int Key = KEY_FIRST + 1; Key < KEY_LAST; ++Key)
		{
			if(m_aCurrentKeyStates[Key])
				AddKeyEvent(Key, FLAG_RELEASE);
		}
		m_MouseButtons = 0;
		m_vTouchFingerStates.clear();
	}

	// Where on the picture a place on the canvas is, from 0 to 1, as the SDL
	// input reports fingers: the picture may be narrower than the canvas.
	vec2 CanvasToViewport(vec2 Position) const
	{
		const SGraphicsSurfaceInfo &Surface = m_pWindow->Surface();
		const vec2 Canvas = vec2(std::max(Surface.m_WindowWidth, 1), std::max(Surface.m_WindowHeight, 1));
		const vec2 Screen = m_pGraphics->ScreenSize();
		if(Screen.x <= 0.0f || Screen.y <= 0.0f)
			return vec2(0.0f, 0.0f);
		const vec2 Scaled = (Position / Canvas * m_pGraphics->DrawableSize() - vec2(m_pGraphics->ViewportX(), 0.0f)) / Screen;
		return vec2(std::clamp(Scaled.x, 0.0f, 1.0f), std::clamp(Scaled.y, 0.0f, 1.0f));
	}

	std::vector<CTouchFingerState>::iterator FindFinger(int Id)
	{
		return std::find_if(m_vTouchFingerStates.begin(), m_vTouchFingerStates.end(), [Id](const CTouchFingerState &State) {
			return State.m_Finger.m_FingerId == Id;
		});
	}

	void HandlePointer(const SWebInputEvent &Event)
	{
		const vec2 Position(Event.m_X, Event.m_Y);
		const bool Touch = (Event.m_Code & POINTER_TOUCH) != 0;
		const bool Primary = (Event.m_Code & POINTER_PRIMARY) != 0;
		const int Button = Event.m_Code & POINTER_BUTTON_MASK;
		if(Touch)
		{
			const vec2 Viewport = CanvasToViewport(Position);
			auto Found = FindFinger(Event.m_Id);
			if(Event.m_Type == WEB_POINTER_DOWN && Found == m_vTouchFingerStates.end())
			{
				CTouchFingerState State;
				State.m_Finger.m_DeviceId = 0;
				State.m_Finger.m_FingerId = Event.m_Id;
				State.m_Position = Viewport;
				State.m_Delta = vec2(0.0f, 0.0f);
				State.m_PressTime = time_get_nanoseconds();
				m_vTouchFingerStates.push_back(State);
			}
			else if(Event.m_Type == WEB_POINTER_MOVE && Found != m_vTouchFingerStates.end())
			{
				Found->m_Delta += Viewport - Found->m_Position;
				Found->m_Position = Viewport;
			}
			else if(Event.m_Type == WEB_POINTER_UP && Found != m_vTouchFingerStates.end())
			{
				m_vTouchFingerStates.erase(Found);
			}
			// The first finger is the pointer as well, as it is with SDL.
			if(!Primary)
				return;
		}
		m_MousePos = Position;
		if(Event.m_Type == WEB_POINTER_MOVE || Button >= 9)
			return;
		const unsigned Bit = 1u << Button;
		const int Key = KEY_MOUSE_1 + Button;
		if(Event.m_Type == WEB_POINTER_DOWN && (m_MouseButtons & Bit) == 0)
		{
			m_MouseButtons |= Bit;
			AddKeyEvent(Key, FLAG_PRESS);
		}
		else if(Event.m_Type == WEB_POINTER_UP && (m_MouseButtons & Bit) != 0)
		{
			m_MouseButtons &= ~Bit;
			AddKeyEvent(Key, FLAG_RELEASE);
		}
	}

public:
	void Init() override
	{
		m_pGraphics = Kernel()->RequestInterface<IEngineGraphics>();
		m_pWindow = Kernel()->RequestInterface<IEngineGraphicsWindow>();
		ddnet_web_input_install();
		m_LastUpdate = time_get();
	}

	void Shutdown() override
	{
		ddnet_web_input_uninstall();
	}

	int Update() override
	{
		const int64_t Now = time_get();
		m_UpdateTime = m_UpdateTime * 0.9f + (Now - m_LastUpdate) / (float)time_freq() * 0.1f;
		m_LastUpdate = Now;
		// Events of an earlier frame that nobody consumed are over.
		++m_InputCounter;

		SWebInputEvent Event;
		while(gs_Queue.Pop(Event))
		{
			switch(Event.m_Type)
			{
			case WEB_KEY_DOWN: AddKeyEvent(Event.m_Code, FLAG_PRESS); break;
			case WEB_KEY_REPEAT: AddKeyEvent(Event.m_Code, FLAG_PRESS | FLAG_REPEAT); break;
			case WEB_KEY_UP: AddKeyEvent(Event.m_Code, FLAG_RELEASE); break;
			case WEB_POINTER_DOWN:
			case WEB_POINTER_MOVE:
			case WEB_POINTER_UP: HandlePointer(Event); break;
			case WEB_WHEEL:
				if(Event.m_Code != 0)
				{
					const int Key = Event.m_Code > 0 ? KEY_MOUSE_WHEEL_UP : KEY_MOUSE_WHEEL_DOWN;
					AddKeyEvent(Key, FLAG_PRESS);
					AddKeyEvent(Key, FLAG_RELEASE);
				}
				break;
			case WEB_VISIBLE:
				if(Event.m_Code == 0)
					ReleaseAll();
				break;
			case WEB_RELEASE_ALL: ReleaseAll(); break;
			default: break;
			}
		}
		return gs_Quit.load() ? 1 : 0;
	}

	void ConsumeEvents(std::function<void(const CEvent &Event)> Consumer) const override
	{
		for(const CEvent &Event : m_vEvents)
		{
			if(Event.m_InputCount == m_InputCounter)
				Consumer(Event);
		}
	}
	void Clear() override
	{
		std::fill(std::begin(m_aFrameKeyStates), std::end(m_aFrameKeyStates), false);
		m_vEvents.clear();
		ClearTouchDeltas();
	}
	float GetUpdateTime() const override { return m_UpdateTime; }

	bool ModifierIsPressed() const override { return KeyIsPressed(KEY_LCTRL) || KeyIsPressed(KEY_RCTRL) || KeyIsPressed(KEY_LGUI) || KeyIsPressed(KEY_RGUI); }
	bool ShiftIsPressed() const override { return KeyIsPressed(KEY_LSHIFT) || KeyIsPressed(KEY_RSHIFT); }
	bool AltIsPressed() const override { return KeyIsPressed(KEY_LALT) || KeyIsPressed(KEY_RALT); }
	bool KeyIsPressed(int Key) const override { return Key > KEY_FIRST && Key < KEY_LAST && m_aCurrentKeyStates[Key]; }
	bool KeyPress(int Key) const override { return Key > KEY_FIRST && Key < KEY_LAST && m_aFrameKeyStates[Key]; }
	void ClearFrameKey(int Key) override
	{
		if(Key > KEY_FIRST && Key < KEY_LAST)
			m_aFrameKeyStates[Key] = false;
	}
	// Nothing is bound by name: the tools know their keys, and the names are
	// left out of them.
	int FindKeyByName(const char *pKeyName) const override { return KEY_UNKNOWN; }

	size_t NumJoysticks() const override { return 0; }
	IJoystick *GetJoystick(size_t Index) override { return nullptr; }
	IJoystick *GetActiveJoystick() override { return nullptr; }
	void SetActiveJoystick(size_t Index) override {}

	vec2 NativeMousePos() const override { return m_MousePos; }
	bool NativeMousePressed(int Index) const override { return Index >= 1 && Index <= 9 && (m_MouseButtons & (1u << (Index - 1))) != 0; }
	// The pointer is the page's; it is never captured for aiming.
	void MouseModeRelative() override {}
	void MouseModeAbsolute() override {}
	bool MouseRelative(float *pX, float *pY) override
	{
		*pX = 0.0f;
		*pY = 0.0f;
		return false;
	}

	const std::vector<CTouchFingerState> &TouchFingerStates() const override { return m_vTouchFingerStates; }
	void ClearTouchDeltas() override
	{
		for(CTouchFingerState &State : m_vTouchFingerStates)
			State.m_Delta = vec2(0.0f, 0.0f);
	}

	std::string GetClipboardText() override { return {}; }
	void SetClipboardText(const char *pText) override {}
	void StartTextInput() override {}
	void StopTextInput() override {}
	void EnsureScreenKeyboardShown() override {}
	const char *GetComposition() const override { return ""; }
	bool HasComposition() const override { return false; }
	int GetCompositionCursor() const override { return 0; }
	int GetCompositionLength() const override { return 0; }
	const char *GetCandidate(int Index) const override { return ""; }
	int GetCandidateCount() const override { return 0; }
	int GetCandidateSelectedIndex() const override { return -1; }
	void SetCompositionWindowPosition(float X, float Y, float H) override {}

	bool GetDropFile(char *pBuffer, int BufferSize) override
	{
		char *pFile = gs_pDropFile.exchange(nullptr);
		if(pFile == nullptr)
			return false;
		str_copy(pBuffer, pFile, BufferSize);
		free(pFile);
		return true;
	}
};

IEngineInput *CreateEngineInput()
{
	return new CWebInput();
}
