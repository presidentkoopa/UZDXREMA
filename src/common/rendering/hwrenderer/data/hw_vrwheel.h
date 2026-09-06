#pragma once

struct HWDrawInfo;
class FRenderState;
class VSMatrix;

void VRWheel_OpenWeapon();
void VRWheel_CloseWeapon();
void VRWheel_OpenOffhandWeapon();
void VRWheel_CloseOffhandWeapon();
void VRWheel_OpenInventory();
void VRWheel_CloseInventory();
void VRWheel_Reset();
bool VRWheel_IsActive();
bool VRWheel_ShouldSuppressGameplayInput();
// True when the given hand is the one holding a wheel. The other hand is still
// playing the game and must keep its trigger, its reload and its laser.
bool VRWheel_ShouldSuppressHandInput(int hand);
// True while a stick is being used to drive a wheel, so the same stick does not
// also walk the player.
bool VRWheel_ShouldSuppressStickMove();
bool VRWheel_ShouldSuppressWeaponHand(int hand);
void VRWheel_Draw(HWDrawInfo* di, FRenderState& state);
bool VRWheel_GetTransform(VSMatrix& out);
