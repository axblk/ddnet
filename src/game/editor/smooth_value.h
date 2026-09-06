#ifndef GAME_EDITOR_SMOOTH_VALUE_H
#define GAME_EDITOR_SMOOTH_VALUE_H

#include "editor_object.h"

#include <base/bezier.h>

/**
 * A value that is changed smoothly over time.
 */
class CSmoothValue : public CEditorObject
{
public:
	CSmoothValue() = default;
	CSmoothValue(float InitialValue, float MinValue, float MaxValue);

	/**
	 * Set a new target which the value should change to.
	 */
	void SetValue(float Target);

	/**
	 * Scale the value by the given amount.
	 */
	void ScaleValue(float Factor);

	/**
	 * Set the value to the target instantly. If the value was changing the
	 * target will be discarded.
	 */
	void SetValueInstant(float Target);

	bool UpdateValue();

	float GetValue() const;
	void SetValueRange(float MinValue, float MaxValue);
	float GetMinValue() const;
	float GetMaxValue() const;

private:
	float Progress(float CurrentTime) const;

	bool m_Smoothing = false;
	float m_Value = 0.0f;
	CCubicBezier m_ValueSmoothing;
	float m_ValueSmoothingTarget = 0.0f;
	float m_ValueSmoothingStart = 0.0f;
	float m_ValueSmoothingEnd = 0.0f;

	float m_MinValue = 0.0f;
	float m_MaxValue = 0.0f;
};

#endif
