#ifndef __GL_CLOCK_H
#define __GL_CLOCK_H

#include "stats.h"
#include "m_fixed.h"

extern glcycle_t RenderWall,SetupWall,ClipWall;
extern glcycle_t RenderFlat,SetupFlat;
extern glcycle_t RenderSprite,SetupSprite;
extern glcycle_t All, Finish, PortalAll, Bsp;
extern glcycle_t ProcessAll, PostProcess;
extern glcycle_t VRSceneEyes, VRSceneBuild, VRSubsectors, VRSubsectorCull, VRSubsectorVisible, VRLineBuild, VRLineClip, VRLineDecide, VRThingBuild, VRFlatBuild, VRScenePostBSP, VRPlayerSprites, VRSceneDraw, VREyeComposite, VRFinalizeEye, VRSubmit;
extern glcycle_t VRPostProcessScene, VRSceneTransfer, VRFinalPresent, VRSubmitCopy, VRSubmitWait, VRRenderSyncWait;
extern glcycle_t RenderAll;
extern glcycle_t Dirty;
extern glcycle_t drawcalls, twoD, Flush3D;
extern glcycle_t MTWait, WTTotal;
extern glcycle_t WTWallJobs, WTFlatJobs, WTThingJobs;
extern glcycle_t WallWorkersElapsed, WallMerge, SceneWorkerElapsed;
extern int64_t WallWorkersCpuSumCycles, WallWorkersWallCpuSumCycles;
extern int64_t WallWorkersElapsedCycles;
extern int WallBatchCount, WallItemsProcessed;
extern int VRFinalPresentPasses, VRMirrorPreparePasses, VRSceneTransferOps, VRSubmitLayerBlits;

extern int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;
extern int rendered_lines,rendered_flats,rendered_sprites,rendered_decals,render_vertexsplit,render_texsplit;
extern int rendered_portals;
extern int lightbuffer_curindex, vertexbuffer_curindex, bonebuffer_curindex;

extern int vertexcount, flatvertices, flatprimitives;

void ResetProfilingData();
void CheckBench();
void CheckBenchActive();


#endif
