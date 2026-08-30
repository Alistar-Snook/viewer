#ifndef RACINGOVERLAYMANAGER_H
#define RACINGOVERLAYMANAGER_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llvector3.h"
#include "llcolor4.h"
#include "llpointer.h"

#include <map>
#include <string>

class LLVOAvatar;
class LLViewerObject;
class LLHUDText;

// ---------------------------------------------------------------------------
// Per-racer data tracked each frame
// ---------------------------------------------------------------------------
struct RacerInfo
{
    LLUUID          mAvatarID;
    LLUUID          mVehicleID;         // UUID of the root prim being sat on
    std::string     mDisplayName;
    LLVector3       mVehiclePos;        // world position of vehicle this frame
    LLVector3       mPrevVehiclePos;    // world position previous frame
    F32             mSpeedMPS;          // metres/second, computed frame-over-frame
    F32             mDistanceFromSelf;  // metres from the local avatar
    S32             mRacePosition;      // 1-based, sorted by distance from start or lap
    bool            mIsLocalAvatar;     // true if this is the viewer's own avatar
    LLPointer<LLHUDText> mHUDLabel;     // the 3-D nameplate above the vehicle

    RacerInfo()
      : mSpeedMPS(0.f), mDistanceFromSelf(0.f),
        mRacePosition(0), mIsLocalAvatar(false)
    {}
};

// ---------------------------------------------------------------------------
// Manager singleton
// ---------------------------------------------------------------------------
class RacingOverlayManager : public LLSingleton<RacingOverlayManager>
{
    LLSINGLETON(RacingOverlayManager);
    ~RacingOverlayManager();

public:
    // Called every viewer frame from LLAppViewer::idle() (or equivalent hook)
    void            update(F32 dt);

    // Called when the user toggles EnableRacingOverlay in Preferences
    void            setEnabled(bool enabled);
    bool            isEnabled() const { return mEnabled; }

    // Force a full rebuild of the racer list (e.g. after range change)
    void            refresh();

    // How many racers are currently tracked
    S32             getRacerCount() const;

    // Read-only access to tracked racers (for radar / floater display)
    const std::map<LLUUID, RacerInfo>& getRacers() const { return mRacers; }

private:

    // Scan nearby avatars; populate/update mRacers
    void            scanAvatars();

    // Return true if 'obj' qualifies as a rideable racing vehicle:
    //   - physics enabled
    //   - NOT an avatar
    //   - NOT an attachment
    bool            isRacingVehicle(const LLViewerObject* obj) const;

    // Create or update the HUD label for a racer
    void            updateHUDLabel(RacerInfo& info);

    // Remove the HUD label and erase the entry
    void            removeRacer(const LLUUID& avatarID);

    // Remove all labels and clear the map
    void            clearAll();

    // Sort mRacers by distance from self and assign mRacePosition
    void            updatePositions();

    // Build the text string shown in the 3-D label
    std::string     buildLabelText(const RacerInfo& info) const;

    bool            mEnabled;
    F32             mUpdateAccum;       // seconds since last full scan
    static const F32 SCAN_INTERVAL;    // how often to re-scan (seconds)

    std::map<LLUUID, RacerInfo> mRacers;  // key = avatar UUID
};

#endif 
