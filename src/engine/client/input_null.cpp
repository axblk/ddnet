#if defined(CONF_DEMO_RENDER_TOOL)

#include "keynames.h"

#include <base/str.h>

#include <engine/input.h>
#include <engine/keys.h>

class CInputNull : public IEngineInput
{
	std::vector<CTouchFingerState> m_vTouchFingerStates;

public:
	void ConsumeEvents(std::function<void(const CEvent &)>) const override {}
	void Clear() override {}
	float GetUpdateTime() const override { return 0.0f; }
	bool ModifierIsPressed() const override { return false; }
	bool ShiftIsPressed() const override { return false; }
	bool AltIsPressed() const override { return false; }
	bool KeyIsPressed(int) const override { return false; }
	bool KeyPress(int) const override { return false; }
	const char *KeyName(int Key) const override { return Key >= KEY_FIRST && Key < KEY_LAST ? g_aaKeyStrings[Key] : g_aaKeyStrings[KEY_UNKNOWN]; }
	int FindKeyByName(const char *pKeyName) const override
	{
		for(int Key = KEY_FIRST; Key < KEY_LAST; ++Key)
		{
			if(str_comp_nocase(pKeyName, g_aaKeyStrings[Key]) == 0)
				return Key;
		}
		return KEY_UNKNOWN;
	}
	size_t NumJoysticks() const override { return 0; }
	IJoystick *GetJoystick(size_t) override { return nullptr; }
	IJoystick *GetActiveJoystick() override { return nullptr; }
	void SetActiveJoystick(size_t) override {}
	vec2 NativeMousePos() const override { return {}; }
	bool NativeMousePressed(int) const override { return false; }
	void MouseModeRelative() override {}
	void MouseModeAbsolute() override {}
	bool MouseRelative(float *pX, float *pY) override
	{
		*pX = 0.0f;
		*pY = 0.0f;
		return false;
	}
	const std::vector<CTouchFingerState> &TouchFingerStates() const override { return m_vTouchFingerStates; }
	void ClearTouchDeltas() override {}
	std::string GetClipboardText() override { return {}; }
	void SetClipboardText(const char *) override {}
	void StartTextInput() override {}
	void StopTextInput() override {}
	void EnsureScreenKeyboardShown() override {}
	const char *GetComposition() const override { return ""; }
	bool HasComposition() const override { return false; }
	int GetCompositionCursor() const override { return 0; }
	int GetCompositionLength() const override { return 0; }
	const char *GetCandidate(int) const override { return ""; }
	int GetCandidateCount() const override { return 0; }
	int GetCandidateSelectedIndex() const override { return -1; }
	void SetCompositionWindowPosition(float, float, float) override {}
	bool GetDropFile(char *pBuffer, int BufferSize) override
	{
		if(BufferSize > 0)
			pBuffer[0] = '\0';
		return false;
	}
	void Init() override {}
	void Shutdown() override {}
	int Update() override { return 0; }
};

IEngineInput *CreateEngineInput()
{
	return new CInputNull();
}

#endif
