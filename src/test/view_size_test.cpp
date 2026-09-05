/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/graphics.h>

#include <gtest/gtest.h>

static constexpr float ASPECT_4_3 = 4.0f / 3.0f;
static constexpr float ASPECT_16_9 = 16.0f / 9.0f;
static constexpr float ASPECT_21_9 = 21.0f / 9.0f;
static constexpr float ASPECT_32_9 = 32.0f / 9.0f;

TEST(ViewSize, AreaIsKeptUpToTheLimit)
{
	// Below the limit the view trades height for width and keeps its area, so
	// two screens of different shape show the same amount of the world.
	float NarrowWidth, NarrowHeight, WideWidth, WideHeight;
	CalcViewSize(ASPECT_4_3, 1.0f, ASPECT_16_9, &NarrowWidth, &NarrowHeight);
	CalcViewSize(ASPECT_16_9, 1.0f, ASPECT_16_9, &WideWidth, &WideHeight);

	EXPECT_NEAR(NarrowWidth * NarrowHeight, WideWidth * WideHeight, 1.0f);
	EXPECT_GT(WideWidth, NarrowWidth);
	EXPECT_LT(WideHeight, NarrowHeight);
}

TEST(ViewSize, TheLimitKeepsTheHeightAndWidensInstead)
{
	float ReferenceWidth, ReferenceHeight;
	CalcViewSize(ASPECT_16_9, 1.0f, ASPECT_16_9, &ReferenceWidth, &ReferenceHeight);

	for(const float Aspect : {ASPECT_21_9, ASPECT_32_9})
	{
		float Width, Height;
		CalcViewSize(Aspect, 1.0f, ASPECT_16_9, &Width, &Height);
		EXPECT_FLOAT_EQ(Height, ReferenceHeight) << "aspect " << Aspect;
		EXPECT_FLOAT_EQ(Width, Height * Aspect) << "aspect " << Aspect;
		EXPECT_GT(Width, ReferenceWidth) << "aspect " << Aspect;
	}
}

TEST(ViewSize, WithoutTheLimitAWiderScreenIsAShorterOne)
{
	float ReferenceWidth, ReferenceHeight, Width, Height;
	CalcViewSize(ASPECT_16_9, 1.0f, 0.0f, &ReferenceWidth, &ReferenceHeight);
	CalcViewSize(ASPECT_21_9, 1.0f, 0.0f, &Width, &Height);

	EXPECT_LT(Height, ReferenceHeight);
	// This is what makes an ultrawide screen look zoomed in: it gains far less
	// width than it loses height, because the width runs into its own limit.
	EXPECT_LT(Width - ReferenceWidth, ReferenceHeight - Height);
}

TEST(ViewSize, TheLimitDoesNotChangeTheUsualScreens)
{
	for(const float Aspect : {ASPECT_4_3, 16.0f / 10.0f, ASPECT_16_9})
	{
		float Limited[2], Unlimited[2];
		CalcViewSize(Aspect, 1.0f, ASPECT_16_9, &Limited[0], &Limited[1]);
		CalcViewSize(Aspect, 1.0f, 0.0f, &Unlimited[0], &Unlimited[1]);
		EXPECT_FLOAT_EQ(Limited[0], Unlimited[0]) << "aspect " << Aspect;
		EXPECT_FLOAT_EQ(Limited[1], Unlimited[1]) << "aspect " << Aspect;
	}
}

TEST(ViewSize, ZoomScalesBothSides)
{
	float Width, Height, ZoomedWidth, ZoomedHeight;
	CalcViewSize(ASPECT_21_9, 1.0f, ASPECT_16_9, &Width, &Height);
	CalcViewSize(ASPECT_21_9, 2.0f, ASPECT_16_9, &ZoomedWidth, &ZoomedHeight);

	EXPECT_FLOAT_EQ(ZoomedWidth, Width * 2.0f);
	EXPECT_FLOAT_EQ(ZoomedHeight, Height * 2.0f);
}

TEST(ViewSize, NothingIsUncoveredWhileTheViewFits)
{
	float Width, Height;
	CalcViewSize(ASPECT_16_9, 1.0f, ASPECT_16_9, &Width, &Height);
	// The vanilla clip distance, see the server's NetworkClipped.
	const vec2 Uncovered = CalcUncoveredViewSides(Width, 0.0f, 0.0f, 1000.0f);
	EXPECT_FLOAT_EQ(Uncovered.x, 0.0f);
	EXPECT_FLOAT_EQ(Uncovered.y, 0.0f);
}

TEST(ViewSize, AVeryWideViewReachesPastTheClipDistance)
{
	float Width, Height;
	CalcViewSize(32.0f / 9.0f, 1.0f, ASPECT_16_9, &Width, &Height);
	const vec2 Uncovered = CalcUncoveredViewSides(Width, 0.0f, 0.0f, 1000.0f);
	EXPECT_FLOAT_EQ(Uncovered.x, Width / 2.0f - 1000.0f);
	EXPECT_FLOAT_EQ(Uncovered.y, Width / 2.0f - 1000.0f);
	EXPECT_GT(Uncovered.x, 0.0f);
}

TEST(ViewSize, AnOffsetCameraUncoversOneSideFirst)
{
	// The dynamic camera moves the view away from the tee the server clips
	// around, so the side it moved towards runs out of entities first.
	const float Width = 2000.0f;
	const vec2 Uncovered = CalcUncoveredViewSides(Width, 300.0f, 0.0f, 1000.0f);
	EXPECT_FLOAT_EQ(Uncovered.x, 0.0f);
	EXPECT_FLOAT_EQ(Uncovered.y, 300.0f);
}
