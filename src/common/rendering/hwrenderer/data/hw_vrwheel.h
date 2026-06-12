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
bool VRWheel_ShouldSuppressWeaponHand(int hand);
void VRWheel_Draw(HWDrawInfo* di, FRenderState& state);
bool VRWheel_GetTransform(VSMatrix& out);
