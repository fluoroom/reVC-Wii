#include <cmath>
#include <cstdio>

#include <gccore.h>
#include <ogc/consol.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "common.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "Timer.h"
#include "WiiCheats.h"
#include "WiiConfig.h"
#include "WiiPad.h"
#include "WiiTrace.h"
#include "platform.h"
#include "skeleton.h"

CPlayerPed *FindPlayerPed(void);

// Stick handling ported from the BetaPlusPlus Wii port (src/wii/wii/
// WiiPadState.cpp in the reference tree).  The failure modes it works around
// are the ones that only show up on real hardware, so the reasoning is kept
// with the code rather than rediscovered later.

namespace
{

// Every axis in CControllerState is measured against 128, not against the range
// the int16 holding it could carry.  CPad::LookAroundUpDown only starts turning
// the camera past 85 and treats 128 as the edge of the gate,
// CPad::GetSteeringLeftRight subtracts 35 and rescales by 128/(128 - 35),
// CPad::GetAnalogueUpDown weighs the stick against a D-pad worth 127, and every
// other joystick backend writes value*128*sensitivity (the XInput path in
// Pad.cpp, sdl2.cpp, glfw.cpp).  Filling the int16 range instead puts a barely
// moved stick two orders of magnitude past hard over, so all of those thresholds
// are already saturated and the first nudge reads as a full speed turn.
constexpr float kAxisFullScale = 128.0f;

// PAD_ScanPads() calls PAD_Read() and nothing else, so the sticks arrive
// unclamped: PAD_Stick* return the raw sample minus the calibrated origin, with
// neither the deadzone nor the gate PAD_Clamp() would have imposed.  Where that
// gate is written down is the useful part, because it is not a circle: PAD_Clamp
// subtracts a 15 count deadzone per axis and then clamps into an OCTAGON, with
// vertices at (72, 0) and (40, 40) for the main stick and (59, 0) and (31, 31)
// for the C-stick.  Adding the deadzone back gives the same gate in the raw
// counts this backend actually sees -- 87 out to a cardinal notch, 55 on each
// axis out to a diagonal one.
//
// Those are the limits every pad is guaranteed to reach, which is why they are
// what to normalise against: the ~100 counts the s8 could carry is a number many
// pads never report, so dividing by it leaves a stick held hard against the gate
// still reading as only two thirds deflected.
constexpr float kGameCubeStickRange = 87.0f;
constexpr float kGameCubeStickCorner = 55.0f;
constexpr float kGameCubeSubStickRange = 74.0f;
constexpr float kGameCubeSubStickCorner = 46.0f;

// A deadzone of one or more would divide the rescale in applyDeadzone by zero,
// and nothing validates the settings on the way in.
constexpr float kMaximumDeadzone = 0.9f;

constexpr float kDegreesToRadians = 3.14159265358979323846f/180.0f;

// --- the pointer -----------------------------------------------------------
// IR is ABSOLUTE and BOUNDED: it says where on the screen the player is aiming.
// That is what a menu cursor wants, and what a 3rd-person gun crosshair wants.
// The follow camera is RELATIVE and UNBOUNDED, so using the pointer as a rate
// camera (offset from centre = turn speed) is reserved for a bare Wiimote --
// there is no nunchuk stick to look with -- and for 1st-person weapon scopes.
// With a nunchuk attached the follow camera stays on Z+stick; the pointer only
// writes CHair while a firearm is being aimed or during drive-by, and otherwise
// reports no mouse motion at all.
//
// Differencing successive pointer positions for a rate camera is the obvious
// translation and it is wrong in a specific way: the total camera travel
// available becomes one screen width, so aiming into a corner and holding swings
// the view until the pointer clamps and then stops.  There is no way to keep
// turning without sweeping back and forth.  So when the pointer IS a camera,
// the offset from the centre of the screen is read as a RATE instead: direction
// and speed, not distance.  Hold the Wiimote up and right and the camera keeps
// going up and right, faster the further out it points.  In a MENU the pointer
// stays absolute, because there it really is a cursor and aiming at an option
// has to put the cursor on that option.
//
// Neutral is the centre of the screen, always.  Latching it to wherever the
// player happened to be aiming sounds friendlier and is not: close a menu with
// the pointer in a corner and the whole mapping is half a screen out, with
// nothing on screen to say that it happened or how to undo it.
//
// All three of these are fractions of half the screen HEIGHT, including the
// horizontal one -- see irPointerRate for why not each axis' own extent.
//
//   deadzone    how far off centre the player can aim before the camera moves.
//               Has to cover ordinary hand shake plus the sensor's own jitter or
//               the view drifts while they hold still, and it is also what keeps
//               a centred pointer from overriding the sticks.
//   saturation  where the turn rate reaches its maximum.  Deliberately short of
//               1.0: the pointer gets unreliable near the edge of the sensor's
//               field, and having to aim there to turn quickly is what makes a
//               rate camera feel like it is fighting the player.
//   curve       how much of the response is linear rather than quadratic.  The
//               quadratic part is what keeps small offsets slow enough to aim
//               with; the linear part stops the first third from doing nothing.
constexpr float kPointerDeadzone = 0.13f;
constexpr float kPointerSaturation = 0.75f;
constexpr float kPointerCurveLinear = 0.45f;

// Turn rate at full deflection, in the units GetMouseX and GetMouseY are read
// in, PER SECOND.  Per second and not per frame because this port's frame rate
// moves with the scene, and a per frame constant makes the camera turn faster in
// an empty street than in traffic, which reads as the sensitivity changing by
// itself.
//
// Cam.cpp turns a horizontal delta into 2.5*x*m_fMouseAccelHorzntl radians of
// yaw, so at the default sensitivity of 0.0025 one unit is 0.00625 rad and this
// works out to about 143 degrees per second at 60Hz.  The options screen's mouse
// sensitivity slider scales m_fMouseAccelHorzntl, and therefore scales the
// pointer with it, which is the control to reach for rather than this number:
// Frontend.cpp clamps it between 1/3200 and 1/200, so the pointer can be tuned
// across a factor of sixteen without touching the source.
constexpr float kPointerRatePerSec = 400.0f;

// The same delta pitches further than it yaws: Cam.cpp scales the vertical one
// by 4.0*m_fMouseAccelVertical against 2.5*m_fMouseAccelHorzntl for the
// horizontal, and m_fMouseAccelVertical is m_fMouseAccelHorzntl + 0.0005, so at
// the default settings pitch comes out about twice as fast as yaw.  This takes
// that back out and leaves the pitch a little slower than the yaw instead, the
// way the stick path's own 0.6 factor in Cam.cpp does.
constexpr float kPointerPitchScale = 0.32f;

// The pointer stops being tracked the moment it leaves the sensor bar's field,
// which is exactly what happens at the END of a long turn: the remote is still
// held out to the side and the player still wants to keep turning.  Freezing
// there caps a single turn at the sensor's field of view and forces them to saw
// the remote back and forth, so the last rate is held instead.
//
// Not held forever, though.  A remote set down, or pointed at the floor, looks
// identical from here, and a camera that keeps spinning until someone picks it
// back up reads as a hang rather than a feature.  This is long enough to finish
// any turn a player is actually in the middle of.
constexpr float kPointerHoldSeconds = 2.0f;

// Wheel steering.  The remote lies across the hands the way a Wii Wheel holds
// it: buttons facing the player, 2 under the right thumb.  Gravity along the
// long axis is then the steering angle.  Full lock is short of 90 degrees so
// a small turn of the wrists is a full turn of the car, which is the Mario
// Kart feel.  The deadzone covers the shake of holding still.
constexpr float kWheelDeadzoneDeg = 10.0f;
constexpr float kWheelFullLockDeg = 50.0f;
// |g.z| is about 1 with the buttons toward the ceiling (the pointer grip) and
// about 0 with the buttons facing the player (the wheel).  The two thresholds
// are split so a reading that sits on the boundary does not flicker modes.
constexpr float kWheelPoseEnterZ = 0.50f;
constexpr float kWheelPoseExitZ = 0.80f;

// Frame time clamp for the rate above.  A dt of zero (two polls inside one
// timebase tick) would freeze the camera and a huge one -- the first frame after
// a streaming stall -- would fling it, so the rate is only integrated over
// plausible frame times.
constexpr float kMinPointerDt = 1.0f/240.0f;
constexpr float kMaxPointerDt = 1.0f/15.0f;

// --- what one scan leaves behind for the rest of the frame -------------------
// WiiPadScan fills these and everything below reads them, which is the whole
// reason it is a separate entry point; see WiiPad.h.
uint32 s_connectedGameCubePads;
float s_pointerDt = 1.0f/60.0f;   // seeded so the first frame is not a special case
u64 s_pointerLastTime;

// The last rate the pointer asked for, kept so it can go on being applied while
// tracking is lost.  Stored per second rather than per frame: it outlives the
// frame it was measured in, and the frames it is replayed over are not the same
// length as that one.
float s_heldRateX;
float s_heldRateY;
float s_heldSeconds;
bool s_pointerAimActive;
bool s_wasPointerAiming;
bool s_haveCHair;
bool s_suppressFireUntilRelease;
float s_lastCHairX = 0.53f;
float s_lastCHairY = 0.4f;

// Resting remote is about 1g.  The hardware clips near +/-3g, so a 2.6g peak
// gate never fires on a normal jab: the extra acceleration is only about one g
// on top of gravity, and it lasts a couple of frames.  A punch is a JERK --
// |g| jumping -- not a sustained high reading.  Peak still has to leave rest so
// a slow tilt cannot count.
constexpr float kPunchPeakG = 1.65f;
constexpr float kPunchJerkG = 0.40f;
constexpr float kPunchOffG = 1.30f;
constexpr float kPunchHoldSeconds = 0.22f;
constexpr float kPunchCooldownSeconds = 0.28f;

float s_punchHold;
float s_punchCooldown;
float s_punchLastMag = 1.0f;
bool s_punchHaveLastMag;
bool s_punchArmed = true;
uword s_punchLastRawX, s_punchLastRawY, s_punchLastRawZ;
bool s_punchHaveLastRaw;

bool s_wheelPose;
float s_wheelFiltered;

int16
toAxis(float value, float sensitivity)
{
	if(value > 1.0f)
		value = 1.0f;
	if(value < -1.0f)
		value = -1.0f;
	return (int16)(value*kAxisFullScale*sensitivity);
}

void
setButton(int16 &field, bool down)
{
	if(down)
		field = 255;
}

float
clampDeadzone(float deadzone)
{
	// Written as a failed greater-than so a NaN out of the settings lands here
	// instead of propagating into every axis.
	if(!(deadzone > 0.0f))
		return 0.0f;
	if(deadzone > kMaximumDeadzone)
		return kMaximumDeadzone;
	return deadzone;
}

// Deadzone and rescale for a stick as a whole rather than for each of its axes
// on its own.  A per axis deadzone lets an axis through the moment it clears the
// threshold by itself, so a stick pushed nearly horizontally still leaks its
// small vertical component and the camera climbs while the player believes they
// are only panning.  Working from the magnitude also leaves the direction alone:
// only the length is rescaled, so a diagonal stays a diagonal instead of being
// bent towards the nearer axis.
void
applyDeadzone(float &x, float &y, float deadzone)
{
	const float magnitude = std::sqrt(x*x + y*y);
	if(magnitude <= deadzone || magnitude <= 0.0f){
		x = 0.0f;
		y = 0.0f;
		return;
	}

	// Movement starts at zero just outside the deadzone instead of jumping
	// straight to whatever fraction of full deflection the deadzone covers.
	float length = (magnitude - deadzone)/(1.0f - deadzone);
	if(length > 1.0f)
		length = 1.0f;
	const float scale = length/magnitude;
	x *= scale;
	y *= scale;
}

// Rescales a raw GameCube reading so that every point on the gate reaches full
// deflection, a diagonal notch as much as a cardinal one, with the direction
// left alone.  Dividing both axes by the cardinal reach instead treats the gate
// as a square: a diagonal notch only carries 55 counts per axis against the 87 a
// cardinal one carries, so the same physical travel reads about a fifth shorter
// once the stick is held off axis, and the camera turns visibly slower on the
// diagonals than it does straight up or sideways.
//
// The gate's radius in the direction being pushed needs no trigonometry.  The
// octagon's boundary, in the sector where one axis dominates, is
// corner*major + (range - corner)*minor = corner*range, so evaluating that left
// hand side against corner*range IS the deflection as a fraction of the gate.
void
normalizeGameCubeStick(float &x, float &y, float range, float corner)
{
	const float magnitude = std::sqrt(x*x + y*y);
	if(magnitude <= 0.0f)
		return;

	const float absX = std::fabs(x);
	const float absY = std::fabs(y);
	const float deflection = (corner*Max(absX, absY) +
		(range - corner)*Min(absX, absY))/(corner*range);

	const float scale = deflection/magnitude;
	x *= scale;
	y *= scale;
}

// libogc reports an expansion stick in polar form derived from a calibration
// block read during the expansion handshake, and that block is what goes wrong
// in practice: on a third party Classic Controller, or while the handshake is
// still in flight, centre/min/max come back as zeros and the polar pair is then
// either stuck at zero (the stick does nothing) or wildly out of range (the
// player walks in one direction forever).  So the polar values are used when
// they are sane and the raw position measured against the calibrated centre is
// the fallback.  The fallback also produces zero for a genuinely centred stick,
// which is why no "is the calibration broken" flag is needed anywhere.
void
readJoystick(const joystick_t &joystick, float &outX, float &outY)
{
	outX = 0.0f;
	outY = 0.0f;

	const float magnitude = joystick.mag > 1.0f ? 1.0f : joystick.mag;
	if(magnitude > 0.0f && std::isfinite(magnitude) && std::isfinite(joystick.ang)){
		const float radians = joystick.ang*kDegreesToRadians;
		outX = std::sin(radians)*magnitude;
		outY = std::cos(radians)*magnitude;
		return;
	}

	// A centre of zero means no calibration was ever read.  There is nothing to
	// measure against, so report centred rather than invent a direction.
	const int centerX = joystick.center.x;
	const int centerY = joystick.center.y;
	if(centerX == 0 && centerY == 0)
		return;

	// Half the calibrated travel, or the nominal counts when the calibration
	// block is obviously unusable.
	const float rangeX = joystick.max.x > joystick.min.x ?
		(joystick.max.x - joystick.min.x)*0.5f : 100.0f;
	const float rangeY = joystick.max.y > joystick.min.y ?
		(joystick.max.y - joystick.min.y)*0.5f : 100.0f;
	outX = ((float)joystick.pos.x - (float)centerX)/rangeX;
	outY = ((float)joystick.pos.y - (float)centerY)/rangeY;

	if(outX > 1.0f)
		outX = 1.0f;
	else if(outX < -1.0f)
		outX = -1.0f;
	if(outY > 1.0f)
		outY = 1.0f;
	else if(outY < -1.0f)
		outY = -1.0f;
}

struct StickAccumulator
{
	float leftX;
	float leftY;
	float rightX;
	float rightY;
};

// The joystick tuning the rest of the game already carries, read once per
// capture so every device merged into this pad is shaped the same way and the
// numbers live where the other backends already look for them.
struct StickSettings
{
	float leftDeadzone;
	float rightDeadzone;
	float leftSensitivityX;
	float leftSensitivityY;
	float rightSensitivityX;
	float rightSensitivityY;
};

StickSettings
currentStickSettings(void)
{
	StickSettings settings;
	settings.leftDeadzone = clampDeadzone(ControlsManager.m_lStickDeadzone);
	settings.rightDeadzone = clampDeadzone(ControlsManager.m_rStickDeadzone);
	settings.leftSensitivityX = ControlsManager.m_lStickSensX;
	settings.leftSensitivityY = ControlsManager.m_lStickSensY;
	settings.rightSensitivityX = ControlsManager.m_rStickSensX;
	settings.rightSensitivityY = ControlsManager.m_rStickSensY;
	return settings;
}

// The deadzone is applied per device, before the merge, because each one has its
// own idle noise: a Classic Controller resting off centre must not be able to
// push a GameCube pad that is genuinely centred past the threshold.
void
addStick(float x, float y, float deadzone, float &outX, float &outY)
{
	applyDeadzone(x, y, deadzone);
	outX += x;
	outY += y;
}

// Circle is GetWeapon() in setup 1, which is both gunfire and melee.  Guns stay
// on B.  Melee also accepts a punch: the accelerometer is already in the IR
// report, so this is a threshold on |g|, not a new sensor.  Circle is held for
// a couple of frames because knives and bats attack on GetWeapon held, while
// fists only need WeaponJustDown.
float
gforceMagnitude(const gforce_t &g)
{
	if(!std::isfinite(g.x) || !std::isfinite(g.y) || !std::isfinite(g.z))
		return 0.0f;
	return std::sqrt(g.x*g.x + g.y*g.y + g.z*g.z);
}

bool
wiimotePunchLatched(const WPADData &data)
{
	if(s_punchHold > 0.0f){
		s_punchHold -= s_pointerDt;
		if(s_punchHold < 0.0f)
			s_punchHold = 0.0f;
	}
	if(s_punchCooldown > 0.0f){
		s_punchCooldown -= s_pointerDt;
		if(s_punchCooldown < 0.0f)
			s_punchCooldown = 0.0f;
	}

	if(data.err != WPAD_ERR_NONE)
		return s_punchHold > 0.0f;

	const bool haveAccel = (data.data_present & WPAD_DATA_ACCEL) != 0;
	float mag = haveAccel ? gforceMagnitude(data.gforce) : 0.0f;
	float jerk = 0.0f;
	if(s_punchHaveLastMag && mag > 0.0f)
		jerk = mag > s_punchLastMag ? mag - s_punchLastMag : s_punchLastMag - mag;
	if(mag > 0.0f){
		s_punchLastMag = mag;
		s_punchHaveLastMag = true;
	}

	// Calibration of 0 makes gforce Inf and the peak test never fires.  Fall
	// back to the raw 10-bit counts (~100 per g) so a jab still registers.
	if(haveAccel && mag <= 0.0f && s_punchHaveLastRaw){
		const float dx = (float)data.accel.x - (float)s_punchLastRawX;
		const float dy = (float)data.accel.y - (float)s_punchLastRawY;
		const float dz = (float)data.accel.z - (float)s_punchLastRawZ;
		jerk = std::sqrt(dx*dx + dy*dy + dz*dz) / 100.0f;
		mag = 1.0f + jerk;
	}
	if(haveAccel){
		s_punchLastRawX = data.accel.x;
		s_punchLastRawY = data.accel.y;
		s_punchLastRawZ = data.accel.z;
		s_punchHaveLastRaw = true;
	}

	if(mag > 0.0f && mag < kPunchOffG)
		s_punchArmed = true;

	if(s_punchArmed && s_punchCooldown <= 0.0f && mag >= kPunchPeakG &&
		jerk >= kPunchJerkG){
		s_punchArmed = false;
		s_punchHold = kPunchHoldSeconds;
		s_punchCooldown = kPunchCooldownSeconds;
		WiiTraceReport("WII pad: punch mag=%.2f jerk=%.2f\n", mag, jerk);
	}

	return s_punchHold > 0.0f;
}

void
applyMeleePunch(const WPADData &data, CControllerState &state)
{
	CPlayerPed *ped = FindPlayerPed();
	if(ped == nil || ped->GetWeapon() == nil || !ped->GetWeapon()->IsTypeMelee())
		return;

	if(wiimotePunchLatched(data))
		setButton(state.Circle, true);
}

// PAD_ScanPads() returns the mask of channels whose status came back without
// error, so it is a real connected test.  Guessing presence from stick
// deflection instead lets an idle pad resting a couple of counts off centre
// claim every frame.
bool
playerInVehicle(void)
{
	CPlayerPed *ped = FindPlayerPed();
	return ped != nil && ped->bInVehicle;
}

bool
captureGameCube(int channel, uint32 connectedMask, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	if((connectedMask & (1 << channel)) == 0)
		return false;

	const u16 buttons = PAD_ButtonsHeld(channel);
	const bool inCar = playerInVehicle();

	// Face buttons: A=Cross (accelerate / sprint), B=Circle (Mode 0 GetWeapon =
	// shoot / brake).  Keep that so B is always fire on foot.
	setButton(state.Cross, buttons & PAD_BUTTON_A);
	setButton(state.Circle, buttons & PAD_BUTTON_B);
	setButton(state.Square, buttons & PAD_BUTTON_X);
	setButton(state.Triangle, buttons & PAD_BUTTON_Y);
	setButton(state.Start, buttons & PAD_BUTTON_START);

	// Mode 0 GetTarget / auto-aim reads RightShoulder1.  L owns that now; R is
	// free of aim so it does not double up.  Horn (LeftShock) moves to Z so L
	// is not shared with radio/aim again.
	setButton(state.RightShoulder1, buttons & PAD_TRIGGER_L);
	setButton(state.LeftShock, buttons & PAD_TRIGGER_Z);

	if(inCar){
		// Only in a vehicle: D-Pad is remapped away from steering.
		// Radio = ChangeStationJustDown -> LeftShoulder1 <- D-Pad Up.
		// Look L/R = LeftShoulder2 / RightShoulder2 <- D-Pad Left / Right.
		setButton(state.LeftShoulder1, buttons & PAD_BUTTON_UP);
		setButton(state.LeftShoulder2, buttons & PAD_BUTTON_LEFT);
		setButton(state.RightShoulder2, buttons & PAD_BUTTON_RIGHT);
	}else{
		// On foot: leave state.DPad* unset so the pad does not walk / steer.
		// CycleWeaponLeft/Right read L2/R2 -- bind those to D-Pad Left / Right.
		setButton(state.LeftShoulder2, buttons & PAD_BUTTON_LEFT);
		setButton(state.RightShoulder2, buttons & PAD_BUTTON_RIGHT);
		// CollectPickupJustDown Mode 0 still wants LeftShoulder1; R fills it on foot.
		setButton(state.LeftShoulder1, buttons & PAD_TRIGGER_R);
	}

	// PAD_Stick* report raw counts offset from the calibrated origin, so the gate
	// is normalised away before anything else looks at them.  Their Y grows
	// upwards while the game's grows down the screen; the octagon is symmetric
	// about both axes, so flipping it first costs nothing.
	float x = PAD_StickX(channel);
	float y = -PAD_StickY(channel);
	normalizeGameCubeStick(x, y, kGameCubeStickRange, kGameCubeStickCorner);
	addStick(x, y, settings.leftDeadzone, sticks.leftX, sticks.leftY);

	x = PAD_SubStickX(channel);
	y = -PAD_SubStickY(channel);
	normalizeGameCubeStick(x, y, kGameCubeSubStickRange, kGameCubeSubStickCorner);
	addStick(x, y, settings.rightDeadzone, sticks.rightX, sticks.rightY);
	return true;
}

// WPAD_Probe is the supported "what is plugged into this channel" query and is
// consulted in preference to exp.type, which only describes the last data report
// that arrived: around a hot plug, or while a handshake is still in flight, it
// can read NONE with an expansion very much attached.  Neither query gets to veto
// the other, so a probe that fails for a single frame cannot silence the pad.
u32
probeExpansion(int channel, const WPADData &data)
{
	u32 expansion = WPAD_EXP_NONE;
	if(WPAD_Probe(channel, &expansion) != WPAD_ERR_NONE)
		expansion = WPAD_EXP_NONE;
	if(expansion == WPAD_EXP_NONE)
		expansion = (u32)data.exp.type;
	return expansion;
}

void
captureClassic(const WPADData &data, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	const u32 buttons = data.btns_h;
	const bool inCar = playerInVehicle();

	setButton(state.Cross, buttons & WPAD_CLASSIC_BUTTON_B);
	setButton(state.Circle, buttons & WPAD_CLASSIC_BUTTON_A);
	setButton(state.Square, buttons & WPAD_CLASSIC_BUTTON_Y);
	setButton(state.Triangle, buttons & WPAD_CLASSIC_BUTTON_X);
	setButton(state.LeftShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_L);
	setButton(state.RightShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_R);
	setButton(state.LeftShoulder2, buttons & WPAD_CLASSIC_BUTTON_ZL);
	setButton(state.RightShoulder2, buttons & WPAD_CLASSIC_BUTTON_ZR);
	// Mode 0 GetHorn reads LeftShock (PS2 L3).  D-Pad Up is the horn; do not
	// also write state.DPadUp or steering will fire with the horn.
	setButton(state.LeftShock, buttons & WPAD_CLASSIC_BUTTON_UP);
	setButton(state.Start, buttons & WPAD_CLASSIC_BUTTON_PLUS);
	setButton(state.Select, buttons & WPAD_CLASSIC_BUTTON_MINUS);

	if(inCar){
		// In a vehicle: D-Pad L/R look (GetLookLeft/Right) without dropping ZL/ZR.
		setButton(state.LeftShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZL) ||
			(buttons & WPAD_CLASSIC_BUTTON_LEFT));
		setButton(state.RightShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZR) ||
			(buttons & WPAD_CLASSIC_BUTTON_RIGHT));
	}else{
		// On foot: D-Pad L/R change weapons; other D-Pad bits stay unmapped.
		setButton(state.LeftShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZL) ||
			(buttons & WPAD_CLASSIC_BUTTON_LEFT));
		setButton(state.RightShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZR) ||
			(buttons & WPAD_CLASSIC_BUTTON_RIGHT));
		setButton(state.DPadDown, buttons & WPAD_CLASSIC_BUTTON_DOWN);
	}

	// readJoystick already normalises to -1..1 with +Y upwards, so only the sign
	// of Y has to be turned around to match the screen.
	const classic_ctrl_t &classic = data.exp.classic;
	float x, y;
	readJoystick(classic.ljs, x, y);
	addStick(x, -y, settings.leftDeadzone, sticks.leftX, sticks.leftY);
	readJoystick(classic.rjs, x, y);
	addStick(x, -y, settings.rightDeadzone, sticks.rightX, sticks.rightY);
}

// -1..1 stick, already shaped so the vehicle's own square curve comes back
// out as a linear wheel.  Positive g.y means the IR end is up; that end is in
// the right hand, so it is a left turn, and left is a negative stick.
float
wheelDeflection(const gforce_t &g)
{
	const float horiz = std::sqrt(g.x*g.x + g.z*g.z);
	const float degrees = std::atan2(g.y, horiz)/kDegreesToRadians;
	const float magnitude = std::fabs(degrees);
	if(magnitude <= kWheelDeadzoneDeg)
		return 0.0f;

	float t = (magnitude - kWheelDeadzoneDeg)/
		(kWheelFullLockDeg - kWheelDeadzoneDeg);
	if(t > 1.0f)
		t = 1.0f;
	const float shaped = std::sqrt(t);
	return degrees > 0.0f ? -shaped : shaped;
}

void
clearWheelPose(void)
{
	s_wheelPose = false;
	s_wheelFiltered = 0.0f;
}

// Sideways, in a vehicle, with the option on.  A dropped packet or a knock
// that shoves the accelerometer off 1g keeps the last answer: letting go of
// the mode for one frame would snap the wheel back to the D-pad.
bool
updateWheelPose(const WPADData &data)
{
	if(!WiiConfigWheelDrive() || FrontEndMenuManager.m_bMenuActive ||
		FindPlayerVehicle() == nil){
		clearWheelPose();
		return false;
	}
	if(data.err != WPAD_ERR_NONE || (data.data_present & WPAD_DATA_ACCEL) == 0)
		return s_wheelPose;

	const float mag = gforceMagnitude(data.gforce);
	if(mag < 0.40f || mag > 2.50f)
		return s_wheelPose;

	const bool was = s_wheelPose;
	const float z = std::fabs(data.gforce.z);
	if(was)
		s_wheelPose = z < kWheelPoseExitZ;
	else
		s_wheelPose = z < kWheelPoseEnterZ;
	if(was && !s_wheelPose)
		clearWheelPose();
	if(!was && s_wheelPose)
		WiiTraceReport("WII pad: wheel steer\n");
	return s_wheelPose;
}

// Buttons follow the wheel, not the pointer grip.  2 and 1 are under the
// right thumb, B is the trigger the plastic wheel presses, A is the face
// button.  D-pad arrows are the ones printed on the remote: Up points at 1
// and 2.  None of them are written into DPad*, because that axis is also
// steering and would slam the car to full lock.
void
captureWheel(const WPADData &data, CControllerState &state, StickAccumulator &sticks)
{
	const u32 buttons = data.btns_h;

	setButton(state.Cross, buttons & WPAD_BUTTON_2);
	setButton(state.Square, buttons & WPAD_BUTTON_1);
	setButton(state.RightShoulder1, buttons & WPAD_BUTTON_B);
	setButton(state.Circle, buttons & WPAD_BUTTON_A);
	setButton(state.Triangle, buttons & WPAD_BUTTON_UP);
	setButton(state.Select, buttons & WPAD_BUTTON_MINUS);
	setButton(state.Start, buttons & WPAD_BUTTON_PLUS);
	if(buttons & WPAD_BUTTON_LEFT)
		setButton(state.LeftShoulder2, true);
	if(buttons & WPAD_BUTTON_RIGHT)
		setButton(state.RightShoulder2, true);
	if(buttons & WPAD_BUTTON_DOWN){
		setButton(state.LeftShoulder2, true);
		setButton(state.RightShoulder2, true);
	}

	float raw = s_wheelFiltered;
	const float mag = gforceMagnitude(data.gforce);
	if(mag >= 0.40f && mag <= 2.50f)
		raw = wheelDeflection(data.gforce);
	float alpha = s_pointerDt/0.05f;
	if(!(alpha > 0.0f))
		alpha = 1.0f;
	if(alpha > 1.0f)
		alpha = 1.0f;
	s_wheelFiltered += (raw - s_wheelFiltered)*alpha;
	sticks.leftX += s_wheelFiltered;
}

// The Wiimote itself, and the Nunchuk when one is attached.  The forward grip
// is the one the pointer needs: remote pointed at the screen, D-pad under the
// thumb, B under the index finger.  Held sideways that grip's D-pad is turned
// a quarter turn and the pointer is useless, so that pose is a separate
// mapping (captureWheel) and only while the wheel option is on.
//
// GTA still reads PS2 buttons.  Everyday actions sit on A/B/C/Z/stick/D-pad;
// 1 and 2 are at the speaker end and only get rare ones.  Z is look, not a
// shoulder: binding it to R1 would tap target/handbrake every time the player
// glanced.  The nunchuk stick already walks and steers, so DPad* is left unset
// -- writing both would strafe while cycling weapons.
void
captureWiimote(const WPADData &data, u32 expansion, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	const u32 buttons = data.btns_h;
	const bool menu = FrontEndMenuManager.m_bMenuActive;
	const bool nunchukAttached = expansion == WPAD_EXP_NUNCHUK;
	const bool inVehicle = !menu && FindPlayerVehicle() != nil;
	const bool lookingOut = (buttons & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) != 0;
	const bool bHeld = (buttons & WPAD_BUTTON_B) != 0;

	if(updateWheelPose(data)){
		captureWheel(data, state, sticks);
		return;
	}

	// Pause menus can still have FindPlayerVehicle() set.  Confirm/back stay on
	// A/B regardless of whether the player opened the menu from a car.
	setButton(state.Cross, buttons & WPAD_BUTTON_A);
	setButton(state.Start, buttons & WPAD_BUTTON_PLUS);

	if(!nunchukAttached){
		// Bare remote: D-pad walks and steers, A/B stay Cross/Circle, IR is the
		// only camera.  1/2/Minus keep the old face-button layout because there
		// is no nunchuk C/Z to put jump, handbrake, and look on.
		setButton(state.Circle, bHeld);
		setButton(state.Square, buttons & WPAD_BUTTON_1);
		setButton(state.Triangle, buttons & WPAD_BUTTON_2);
		setButton(state.Select, buttons & WPAD_BUTTON_MINUS);

		// CPad::GetPedWalkUpDown and GetSteeringUpDown weigh the D-pad against
		// the left stick at 255/2, so the D-pad alone walks and drives without
		// any synthetic stick behind it.
		setButton(state.DPadUp, buttons & WPAD_BUTTON_UP);
		setButton(state.DPadDown, buttons & WPAD_BUTTON_DOWN);
		setButton(state.DPadLeft, buttons & WPAD_BUTTON_LEFT);
		setButton(state.DPadRight, buttons & WPAD_BUTTON_RIGHT);

		// Classic vehicle look reads RightStick, not the mouse.  FOLLOWPED on
		// foot already prefers mouse deltas when they are non-zero, so feeding
		// the same IR rate into the stick does not double the on-foot turn.
		if(!CCamera::m_bUseMouse3rdPerson &&
			(s_heldRateX != 0.0f || s_heldRateY != 0.0f)){
			sticks.rightX += s_heldRateX / kPointerRatePerSec;
			sticks.rightY += -s_heldRateY /
				(kPointerRatePerSec * kPointerPitchScale);
		}
		if(!menu && !inVehicle)
			applyMeleePunch(data, state);
		return;
	}

	// libogc reports the expansion's buttons twice: merged into the Wiimote mask
	// (WPAD_NUNCHUK_BUTTON_* are the low bits shifted up by 16) and in the
	// expansion struct's own byte.  Either is normally enough; taking both costs
	// one OR and removes a whole class of "works on my Wiimote" difference.
	const nunchuk_t &nunchuk = data.exp.nunchuk;
	const bool cHeld = (buttons & WPAD_NUNCHUK_BUTTON_C) != 0 ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_C) != 0;
	const bool zHeld = (buttons & WPAD_NUNCHUK_BUTTON_Z) != 0 ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_Z) != 0;

	if(menu){
		setButton(state.Circle, bHeld);
	}else if(inVehicle){
		// B cannot be brake and fire at once.  D-pad left/right is the lean-out
		// that starts drive-by, so B is Circle (shoot) while it is held and
		// Square (brake) otherwise.  1 fires a mounted gun only when not leaning
		// out, because Circle is already spoken for then.
		if(lookingOut)
			setButton(state.Circle, bHeld);
		else{
			setButton(state.Square, bHeld);
			setButton(state.Circle, buttons & WPAD_BUTTON_1);
		}
		setButton(state.Triangle, buttons & WPAD_BUTTON_2);
		setButton(state.Select, buttons & WPAD_BUTTON_MINUS);
		setButton(state.RightShoulder1, cHeld);
		if(buttons & WPAD_BUTTON_LEFT)
			setButton(state.LeftShoulder2, true);
		if(buttons & WPAD_BUTTON_RIGHT)
			setButton(state.RightShoulder2, true);
		if(buttons & WPAD_BUTTON_DOWN){
			setButton(state.LeftShoulder2, true);
			setButton(state.RightShoulder2, true);
		}
		if(buttons & WPAD_BUTTON_UP)
			setButton(state.LeftShoulder1, true);
	}else{
		setButton(state.Circle, bHeld);
		setButton(state.Square, cHeld);
		setButton(state.RightShoulder1, buttons & WPAD_BUTTON_MINUS);
		setButton(state.Triangle, buttons & WPAD_BUTTON_DOWN);
		setButton(state.LeftShock, buttons & WPAD_BUTTON_1);
		if(buttons & WPAD_BUTTON_LEFT)
			setButton(state.LeftShoulder2, true);
		if(buttons & WPAD_BUTTON_RIGHT)
			setButton(state.RightShoulder2, true);
		if(buttons & WPAD_BUTTON_UP)
			setButton(state.LeftShoulder1, true);
	}

	if(!menu && !inVehicle)
		applyMeleePunch(data, state);

	float x, y;
	readJoystick(nunchuk.js, x, y);
	y = -y;
	if(!menu && zHeld)
		addStick(x, y, settings.rightDeadzone, sticks.rightX, sticks.rightY);
	else
		addStick(x, y, settings.leftDeadzone, sticks.leftX, sticks.leftY);
}

// Offset from the centre of the screen turned into a turn rate, in the units
// CPad::GetMouseX and GetMouseY are read in, PER SECOND -- the caller decides
// how long to apply it for, which is what lets the rate outlive the frame it was
// measured in when tracking drops out.  Returns false when the pointer is inside
// the dead zone, which is what leaves the sticks in charge of the camera while
// the player is not aiming anywhere in particular.
bool
irPointerRate(const WPADData &data, float &outX, float &outY)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return false;

	// Normalise BOTH axes by half the HEIGHT, not by each axis' own half extent.
	// Dividing x by w/2 and y by h/2 stretches the vector horizontally, so a
	// pointer 45 degrees up and right would not turn the camera 45 degrees up and
	// right, and matching the direction at every angle is the whole point.  The
	// cost is that the maximum yaw rate arrives about three quarters of the way
	// to the left and right edges rather than at the edge itself, which given
	// kPointerSaturation is no cost at all.
	const float half = height*0.5f;
	const float unitX = (data.ir.x - width*0.5f)/half;
	const float unitY = (data.ir.y - height*0.5f)/half;

	// Radial dead zone, not a per axis one.  A square dead zone lets a pointer
	// inside it on X but outside on Y turn the camera straight up, which is the
	// "near the middle it only moves vertically" complaint.
	const float magnitude = std::sqrt(unitX*unitX + unitY*unitY);
	if(magnitude <= kPointerDeadzone)
		return false;

	float t = (magnitude - kPointerDeadzone)/(kPointerSaturation - kPointerDeadzone);
	if(t > 1.0f)
		t = 1.0f;
	const float curve = t*(kPointerCurveLinear + (1.0f - kPointerCurveLinear)*t);

	// The unit direction comes from the raw vector, so only the SPEED goes
	// through the curve.  Curving each axis on its own would bend diagonals
	// towards the nearer axis, the same directional error the normalisation
	// above avoids.
	const float rate = kPointerRatePerSec*curve;
	outX = (unitX/magnitude)*rate;
	outY = (unitY/magnitude)*rate*kPointerPitchScale;
	return true;
}

// Ends any hold in progress, so a rate that was being replayed while tracking
// was lost cannot survive into whatever the pointer does next.
void
stopPointerHold(void)
{
	s_heldRateX = 0.0f;
	s_heldRateY = 0.0f;
	s_heldSeconds = kPointerHoldSeconds;
}

void
writeIrToCHair(const WPADData &data)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return;

	float x = data.ir.x / width;
	float y = data.ir.y / height;

	// The sensor reports 0 or 1 (or a teleport into a corner) the moment the
	// pointer reaches the edge of its field.  Clamping that onto the frustum
	// edge is what makes border aim twitch, and a 0,0 sample is what slams
	// QuickAimPitch to the sky.
	constexpr float kInset = 0.05f;
	constexpr float kJump = 0.35f;
	if(s_haveCHair){
		const float dx = x - s_lastCHairX;
		const float dy = y - s_lastCHairY;
		const bool edge = x < 0.08f || x > 0.92f || y < 0.08f || y > 0.92f;
		if(edge && (dx*dx + dy*dy) > kJump*kJump){
			x = s_lastCHairX;
			y = s_lastCHairY;
		}
	}
	if(x < kInset)
		x = kInset;
	else if(x > 1.0f - kInset)
		x = 1.0f - kInset;
	if(y < kInset)
		y = kInset;
	else if(y > 1.0f - kInset)
		y = 1.0f - kInset;

	s_lastCHairX = x;
	s_lastCHairY = y;
	s_haveCHair = true;
	TheCamera.m_f3rdPersonCHairMultX = x;
	TheCamera.m_f3rdPersonCHairMultY = y;
}

void
restoreDefaultCHair(void)
{
	s_haveCHair = false;
	s_lastCHairX = 0.53f;
	s_lastCHairY = 0.4f;
	TheCamera.m_f3rdPersonCHairMultX = 0.53f;
	TheCamera.m_f3rdPersonCHairMultY = 0.4f;
}

bool
pointerWantsGunAim(const WPADData &data)
{
	CPlayerPed *ped = FindPlayerPed();
	if(ped == nil)
		return false;

	if(FindPlayerVehicle() != nil)
		return (data.btns_h & WPAD_BUTTON_B) != 0 &&
			(data.btns_h & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) != 0;

	CWeapon *weapon = ped->GetWeapon();
	if(weapon == nil || weapon->IsTypeMelee())
		return false;

	// B hip-fires at the pointer.  Minus is Classic lock-on; the pointer has to
	// run then too or the lock stays on whoever FindWeaponLockOnTarget picked.
	return (data.btns_h & (WPAD_BUTTON_B | WPAD_BUTTON_MINUS)) != 0;
}

void
applyIrRateCamera(const WPADData &data, CMouseControllerState &state, bool allowHold)
{
	if(data.ir.valid){
		float rateX, rateY;
		if(irPointerRate(data, rateX, rateY)){
			s_heldRateX = rateX;
			s_heldRateY = rateY;
			s_heldSeconds = 0.0f;
		}else{
			stopPointerHold();
		}
	}else if(allowHold){
		s_heldSeconds += s_pointerDt;
		if(s_heldSeconds >= kPointerHoldSeconds)
			stopPointerHold();
	}else{
		stopPointerHold();
	}

	if(s_heldRateX == 0.0f && s_heldRateY == 0.0f)
		return;

	const float deltaX = s_heldRateX*s_pointerDt;
	const float deltaY = s_heldRateY*s_pointerDt;
	state.x = MousePointerStateHelper.bInvertHorizontally ? -deltaX : deltaX;
	// IR Y grows downward, same as the HUD cursor.  Cam.cpp reads
	// LookUpDown = 4.0f * GetMouseY() with positive Alpha looking up, and
	// unlike yaw (LookLeftRight = -3.0f * MouseX) pitch has no minus of
	// its own -- on PC that minus is Invert Mouse, whose default is true
	// (the menu shows OFF).  A pointer is not a relative mouse: pointing
	// at the top of the screen has to look up whether or not that flag is
	// set, which is the 3rd-person IR mapping too.  Negate here and leave
	// the flag alone so a scope cannot end up with inverted pitch and
	// correct yaw.
	state.y = -deltaY;
}

} // namespace

void
WiiPadInitialise(int pointerWidth, int pointerHeight)
{
	PAD_Init();
	WPAD_Init();

	// wiiuse picks the report mode itself, from three state flags rather than
	// from the format asked for here: an attached expansion is what adds the
	// expansion bytes (report 0x34 without acceleration, 0x35 with it, 0x37 with
	// the pointer as well), so a Classic Controller would have reported its
	// sticks under any of these.  What this call does decide is the pointer,
	// which arrives only in the IR modes and which nothing else can turn on.
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);

	// ir.x and ir.y are the only pair of the three coordinate spaces in ir_t
	// expressed in a resolution of our choosing, and this is where that
	// resolution is set.  Without it they stay in wiiuse's default space and
	// every offset measured against the centre of the screen is wrong.
	WPAD_SetVRes(WPAD_CHAN_ALL, (u32)pointerWidth, (u32)pointerHeight);
	WiiPadApplyControlDefaults();
}

void
WiiPadApplyControlDefaults(void)
{
	FrontEndMenuManager.m_ControlMethod = CONTROL_CLASSIC;
	CCamera::m_bUseMouse3rdPerson = false;
	// PC gta_vc.set often stores a 0.3 deadzone for XInput.  Classic Zelda walk reads
	// stick magnitude directly; with our gate normalisation that deadzone leaves barely
	// any deflection and movement feels like a nudge of the stick.
	ControlsManager.m_lStickDeadzone = 0.12f;
	ControlsManager.m_lStickSensX = 1.15f;
	ControlsManager.m_lStickSensY = 1.15f;
	// LoadINISettings runs before this and can leave a value outside 0/1.
	WiiConfigClampWheelDrive();
}

void
WiiPadScan(void)
{
	s_connectedGameCubePads = (uint32)PAD_ScanPads();
	WPAD_ScanPads();

	// HOME is what a Wii player reaches for to leave a game, and it is not part
	// of any CControllerState, so it is handled here rather than mapped.  Both
	// masks are tested together because btns_d carries the Classic Controller's
	// buttons in its top half, so one test covers the remote and the pad on it.
	// It raises the same RsGlobal.quit the power and reset buttons raise; there
	// is no HOME overlay to fall back on, so this DISCARDS unsaved progress.
	if(WPAD_ButtonsDown(WPAD_CHAN_0) & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)){
		WiiTraceReport("WII pad: HOME pressed\n");
		HandleExit();
	}

	// Frame time for the pointer's rate camera.  gettime() is the timebase, which
	// is monotonic and always alive here, unlike CTimer, which stops with the
	// game and would freeze the pointer along with it in any paused state.
	const u64 now = gettime();
	if(s_pointerLastTime != 0){
		const float dt = (float)diff_usec(s_pointerLastTime, now)*1e-6f;
		s_pointerDt = dt < kMinPointerDt ? kMinPointerDt :
			(dt > kMaxPointerDt ? kMaxPointerDt : dt);
	}
	s_pointerLastTime = now;
}

void
WiiPadCapture(int padID, CControllerState &state)
{
	if(WiiCheatsMenuActive()){
		s_suppressFireUntilRelease = true;
		state.Clear();
		return;
	}

	const StickSettings settings = currentStickSettings();

	StickAccumulator sticks = { 0.0f, 0.0f, 0.0f, 0.0f };
	// A live GameCube pad owns this slot alone.  Merging Wiimote / Classic on
	// top of it was what made IR and remote buttons steal camera and actions
	// while a GC controller was already plugged in.
	if(!captureGameCube(padID, s_connectedGameCubePads, state, sticks, settings)){
		WPADData *data = WPAD_Data(padID);
		if(data != nullptr){
			const u32 expansion = probeExpansion(padID, *data);
			if(expansion == WPAD_EXP_CLASSIC)
				captureClassic(*data, state, sticks, settings);
			else
				captureWiimote(*data, expansion, state, sticks, settings);
		}
	}

	state.LeftStickX = toAxis(sticks.leftX, settings.leftSensitivityX);
	state.LeftStickY = toAxis(sticks.leftY, settings.leftSensitivityY);
	state.RightStickX = toAxis(sticks.rightX, settings.rightSensitivityX);
	state.RightStickY = toAxis(sticks.rightY, settings.rightSensitivityY);

	if(s_suppressFireUntilRelease){
		const u32 wiiHeld = WPAD_ButtonsHeld(WPAD_CHAN_0);
		const u16 gcHeld = PAD_ButtonsHeld(PAD_CHAN0);
		const bool confirmHeld =
			(wiiHeld & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_B)) != 0
			|| (gcHeld & (PAD_BUTTON_A | PAD_BUTTON_B)) != 0;
		if(confirmHeld){
			state.Circle = 0;
			state.Cross = 0;
		}else{
			s_suppressFireUntilRelease = false;
		}
	}
}

void
WiiPadCaptureMouse(CMouseControllerState &state)
{
	state.Clear();
	state.x = 0.0f;
	state.y = 0.0f;
	s_pointerAimActive = false;

	// Same exclusivity as WiiPadCapture: any GameCube pad silences IR so a
	// live remote cannot steer the camera while driving with GC.
	if(s_connectedGameCubePads != 0){
		// The pad owns the car.  A remote lying on its side must not keep the
		// wheel latched, or 1+2 stays swallowed after the player put it down.
		clearWheelPose();
		stopPointerHold();
		return;
	}

	WPADData *data = WPAD_Data(WPAD_CHAN_0);
	if(data == nullptr){
		stopPointerHold();
		return;
	}

	const bool tracked = data->ir.valid != 0;
	const u32 expansion = probeExpansion(WPAD_CHAN_0, *data);
	const bool nunchuk = expansion == WPAD_EXP_NUNCHUK;

	if(WiiCheatsMenuActive()){
		s_suppressFireUntilRelease = true;
		stopPointerHold();
		return;
	}

	if(FrontEndMenuManager.m_bMenuActive){
		// A cursor, absolutely: aiming at an option has to put the cursor on that
		// option.  The frontend clamps and re-reads these itself.  Nothing is held
		// here -- a cursor that carried on drifting after the pointer was lost
		// would walk off the option the player had just settled on.
		if(tracked){
			FrontEndMenuManager.m_nMouseTempPosX = (int32)data->ir.x;
			FrontEndMenuManager.m_nMouseTempPosY = (int32)data->ir.y;
		}

		// The click, and only in here.  In game these same two buttons are already
		// Circle and Cross on the pad state, and CControllerConfigManager::
		// AffectPadFromMouse would bind them a second time to whatever the mouse
		// is configured for -- one press firing two actions.  Read whether or not
		// the pointer is tracked, so a click still lands on wherever the cursor
		// was last left.
		state.LMB = (data->btns_h & WPAD_BUTTON_B) != 0;
		state.RMB = (data->btns_h & WPAD_BUTTON_A) != 0;
		stopPointerHold();
		return;
	}

	// The wheel is not a pointer.  Leaving the rate camera running here spins
	// the view whenever the sideways remote happens to see the sensor bar.
	// A Classic Controller lives on the same remote; its sticks already steer,
	// and the remote's attitude must not take the wheel over the top of them.
	if(expansion == WPAD_EXP_CLASSIC)
		clearWheelPose();
	else if(updateWheelPose(*data)){
		stopPointerHold();
		return;
	}

	const bool scopeLook = TheCamera.Using1stPersonWeaponMode();

	if(nunchuk && !scopeLook && pointerWantsGunAim(*data)){
		// Absolute screen aim.  Dropouts freeze the last CHair rather than
		// replaying a turn rate -- that hold is what made the follow camera
		// spin when the remote left the sensor bar.
		s_pointerAimActive = true;
		s_wasPointerAiming = true;
		if(tracked)
			writeIrToCHair(*data);
		stopPointerHold();
		return;
	}

	if(s_wasPointerAiming){
		restoreDefaultCHair();
		s_wasPointerAiming = false;
	}

	if(nunchuk && !scopeLook){
		stopPointerHold();
		return;
	}

	// Bare Wiimote (the pointer is the only camera) or a 1st-person weapon
	// scope.  Hold-replay is allowed here because the player is still turning,
	// not aiming a 3rd-person crosshair.
	applyIrRateCamera(*data, state, true);
}

bool
WiiPadPointerAimActive(void)
{
	return s_pointerAimActive;
}

bool
WiiPadWheelDriving(void)
{
	return s_wheelPose;
}

void
WiiPadUpdateRumble(void)
{
	CPad *pad = CPad::GetPad(0);

	// Spending the duration down is this backend's job, the same way it is the
	// XInput path's in Pad.cpp and glfw's in glfw.cpp.  Without it StartShake
	// only ever raises ShakeDur and the motors would never stop.
	if(pad->ShakeDur < CTimer::GetTimeStepInMilliseconds())
		pad->ShakeDur = 0;
	else
		pad->ShakeDur -= CTimer::GetTimeStepInMilliseconds();
	if(pad->ShakeDur == 0)
		pad->ShakeFreq = 0;

	// Both motors are on/off, with no speed to set, so the frequency the game
	// asked for can only decide whether they run at all.
	const int running = pad->ShakeFreq != 0 ? 1 : 0;
	WPAD_Rumble(WPAD_CHAN_0, running);
	PAD_ControlMotor(PAD_CHAN0, running ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
}

WiiConnectedPad
WiiPadQueryPrimary(void)
{
	// Prefer whatever is already plugged into a GameCube port -- that is the
	// device that also silences Wiimote input in WiiPadCapture.
	if(s_connectedGameCubePads != 0)
		return WII_PAD_GAMECUBE;

	for(int channel = 0; channel < WPAD_MAX_WIIMOTES; channel++){
		WPADData *data = WPAD_Data(channel);
		if(data == nullptr || data->err != WPAD_ERR_NONE)
			continue;
		const u32 expansion = probeExpansion(channel, *data);
		if(expansion == WPAD_EXP_CLASSIC)
			return WII_PAD_CLASSIC;
		if(expansion == WPAD_EXP_NUNCHUK)
			return WII_PAD_WIIMOTE_NUNCHUK;
		return WII_PAD_WIIMOTE;
	}
	return WII_PAD_NONE;
}

const char *
WiiPadPrimaryName(WiiConnectedPad pad)
{
	switch(pad){
	case WII_PAD_GAMECUBE: return "GameCube Controller";
	case WII_PAD_CLASSIC: return "Classic Controller / Classic Pro";
	case WII_PAD_WIIMOTE_NUNCHUK: return "Wii Remote + Nunchuk";
	case WII_PAD_WIIMOTE: return "Wii Remote";
	default: return "No controller detected";
	}
}

void
WiiPadShowControllerSplash(void *xfb, GXRModeObj *rmode, int holdSeconds)
{
	if(xfb == nullptr || rmode == nullptr)
		return;

	// A few scans so hot-plug / wireless association has a chance to settle
	// before we decide what to draw.
	for(int i = 0; i < 8; i++){
		WiiPadScan();
		VIDEO_WaitVSync();
	}

	const WiiConnectedPad pad = WiiPadQueryPrimary();
	const char *name = WiiPadPrimaryName(pad);

	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	CON_Init(xfb, 40, 40, rmode->fbWidth - 40, rmode->xfbHeight - 40,
		rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	std::printf("\x1b[2;0H");
	std::printf("========================================\n");
	std::printf("  reVC Wii\n");
	std::printf("========================================\n\n");
	std::printf("  Active controller:\n\n");
	std::printf("    >>>  %s  <<<\n\n", name);
	std::printf("----------------------------------------\n");
	if(pad == WII_PAD_GAMECUBE){
		std::printf("  Aim: L    Shoot: B    Horn: Z\n");
		std::printf("  In car -- Radio: D-Pad Up | Look: D-Pad L/R\n");
	}else if(pad == WII_PAD_CLASSIC){
		std::printf("  Horn: D-Pad Up\n");
	}else if(pad == WII_PAD_WIIMOTE_NUNCHUK){
		std::printf("  Horn: Nunchuk C\n");
	}
	if(WiiConfigWheelDrive() &&
		(pad == WII_PAD_WIIMOTE || pad == WII_PAD_WIIMOTE_NUNCHUK)){
		std::printf("  Driving wheel: ON (Options - Controller)\n");
		std::printf("  Hold sideways, 2 on the right.  2 gas, 1 brake.\n");
	}
	std::printf("\n  GameCube pad has priority if plugged in.\n");
	std::printf("  Starting...\n");

	WiiTraceReport("WII pad: splash primary=%s\n", name);

	const int frames = holdSeconds * 60;
	for(int i = 0; i < frames; i++){
		WiiPadScan();
		VIDEO_WaitVSync();
	}
}
