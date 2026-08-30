#include "llviewerprecompiledheaders.h"

#include "racingoverlaymanager.h"

// Viewer core
#include "llcharacter.h"
#include "llhudtext.h"
#include "llhudobject.h"
#include "llhudmanager.h"
#include "llviewercontrol.h"    // gSavedSettings
#include "llviewerobjectlist.h" // gObjectList
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "llviewerwindow.h"
#include "llagentcamera.h"

// Math / util
#include "llmath.h"
#include "llstring.h"
#include "lltrans.h"            // LLTrans::getString for localised strings

// Standard library
#include <set>
#include <vector>
#include <sstream>
#include <algorithm>



const F32 RacingOverlayManager::SCAN_INTERVAL = 0.5f; // seconds

// HUD label colours  (R, G, B, A)
static const LLColor4 COLOR_SELF      (0.2f, 1.0f, 0.2f, 1.0f);  // bright green
static const LLColor4 COLOR_OTHER     (1.0f, 0.9f, 0.2f, 1.0f);  // amber
static const LLColor4 COLOR_FAR       (1.0f, 1.0f, 1.0f, 0.55f); // dim white

// Maximum range beyond which we don't draw labels (overridden by setting)
static const F32 DEFAULT_RANGE_M = 96.f;

// LSL channel for optional race-position packets from an in-world HUD
// An LSL script can llSay(RACING_CHANNEL, "POS:<avatarUUID>:<position>")
static const S32 RACING_CHANNEL = -7832461; // arbitrary negative channel


RacingOverlayManager::RacingOverlayManager()
    : mEnabled(false)
    , mUpdateAccum(0.f)
{
    // Read initial state from saved settings
    mEnabled = gSavedSettings.getBOOL("EnableRacingOverlay");
}

RacingOverlayManager::~RacingOverlayManager()
{
    clearAll();
}


void RacingOverlayManager::setEnabled(bool enabled)
{
    if (mEnabled == enabled) return;
    mEnabled = enabled;

    if (!mEnabled)
    {
        clearAll();
    }
}

void RacingOverlayManager::refresh()
{
    clearAll();
    mUpdateAccum = SCAN_INTERVAL; // force immediate re-scan next update()
}

S32 RacingOverlayManager::getRacerCount() const
{
    return static_cast<S32>(mRacers.size());
}

//Per-frame update

void RacingOverlayManager::update(F32 dt)
{
    if (!mEnabled) return;

    mUpdateAccum += dt;

    // Speed update runs every frame for all known racers
    for (auto& kv : mRacers)
    {
        RacerInfo& info = kv.second;

        LLViewerObject* vehicle = gObjectList.findObject(info.mVehicleID);
        if (!vehicle)
        {
            // Vehicle has gone — will be cleaned up in scanAvatars
            continue;
        }

        LLVector3 newPos = vehicle->getPositionAgent();
        info.mSpeedMPS   = (newPos - info.mVehiclePos).length() / llmax(dt, 0.001f);
        info.mPrevVehiclePos = info.mVehiclePos;
        info.mVehiclePos     = newPos;

        // Update distance from local avatar
        if (isAgentAvatarValid())
        {
            info.mDistanceFromSelf =
                (newPos - gAgentAvatarp->getPositionAgent()).length();
        }

        updateHUDLabel(info);
    }

    // Full avatar scan at SCAN_INTERVAL
    if (mUpdateAccum >= SCAN_INTERVAL)
    {
        mUpdateAccum = 0.f;
        scanAvatars();
        updatePositions();
    }
}


void RacingOverlayManager::scanAvatars()
{
    F32 range = gSavedSettings.getF32("RacingMarkerRange");
    if (range <= 0.f) range = DEFAULT_RANGE_M;

    // Build a set of avatar UUIDs we find this scan, to detect departures
    std::set<LLUUID> foundThisScan;

    // Iterate all characters known to the viewer (sInstances is a std::list,
    // so we use a range-based for loop rather than operator[])
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead() || !avatar->isFullyLoaded())
        {
            continue;
        }

        LLViewerObject* parent =
            dynamic_cast<LLViewerObject*>(avatar->getParent());

        if (!parent || parent->isAvatar())
        {
            if (mRacers.count(avatar->getID()))
            {
                removeRacer(avatar->getID());
            }
            continue;
        }

        //Is the parent a racing vehicle?
        if (!isRacingVehicle(parent))
        {
            if (mRacers.count(avatar->getID()))
            {
                removeRacer(avatar->getID());
            }
            continue;
        }

        // Range check
        LLVector3 vehiclePos = parent->getPositionAgent();
        F32 dist = 0.f;
        if (isAgentAvatarValid())
        {
            dist = (vehiclePos - gAgentAvatarp->getPositionAgent()).length();
        }
        if (dist > range)
        {
            if (mRacers.count(avatar->getID()))
            {
                removeRacer(avatar->getID());
            }
            continue;
        }

        foundThisScan.insert(avatar->getID());

        auto it = mRacers.find(avatar->getID());
        if (it == mRacers.end())
        {
            // New racer — insert
            RacerInfo info;
            info.mAvatarID       = avatar->getID();
            info.mVehicleID      = parent->getID();
            info.mDisplayName    = avatar->getFullname();
            info.mVehiclePos     = vehiclePos;
            info.mPrevVehiclePos = vehiclePos;
            info.mSpeedMPS       = 0.f;
            info.mDistanceFromSelf = dist;
            info.mIsLocalAvatar  = avatar->isSelf();

            mRacers[avatar->getID()] = info;
            updateHUDLabel(mRacers[avatar->getID()]);
        }
        else
        {
            // Existing — refresh name (display names can load late) and vehicle
            it->second.mVehicleID   = parent->getID();
            it->second.mDisplayName = avatar->getFullname();
            it->second.mDistanceFromSelf = dist;
        }
    }

    std::vector<LLUUID> toRemove;
    for (auto& kv : mRacers)
    {
        if (!foundThisScan.count(kv.first))
        {
            toRemove.push_back(kv.first);
        }
    }
    for (const LLUUID& id : toRemove)
    {
        removeRacer(id);
    }
}

bool RacingOverlayManager::isRacingVehicle(const LLViewerObject* obj) const
{
    if (!obj) return false;

    // Must not be an avatar or attachment
    if (obj->isAvatar())       return false;
    if (obj->isAttachment())   return false;

    if (!obj->flagUsePhysics()) return false;


    return true;
}

void RacingOverlayManager::updateHUDLabel(RacerInfo& info)
{
    if (!gSavedSettings.getBOOL("DrawRacingMarkers"))
    {
        if (info.mHUDLabel)
        {
            info.mHUDLabel->setHidden(true);
        }
        return;
    }

    LLViewerObject* vehicle = gObjectList.findObject(info.mVehicleID);
    if (!vehicle)
    {
        if (info.mHUDLabel) info.mHUDLabel->setHidden(true);
        return;
    }

   
    if (!info.mHUDLabel)
    {
        info.mHUDLabel = static_cast<LLHUDText*>(
            LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT));

        info.mHUDLabel->setFont(LLFontGL::getFontSansSerifSmall());
        info.mHUDLabel->setZCompare(false);   // always draw on top
        info.mHUDLabel->setDoFade(true);
        info.mHUDLabel->setMaxLines(4);
        info.mHUDLabel->setSourceObject(vehicle); // anchor to the vehicle object
    }

    // ── Anchor position: top of vehicle bounding box
    // getScale().mV[VZ] gives the Z-extent of the object, so the label
    // floats just above the roof.
    LLVector3 offset(0.f, 0.f,
        vehicle->getScale().mV[VZ] * 0.5f + 0.5f);
    info.mHUDLabel->setPositionAgent(vehicle->getPositionAgent() + offset);

    // ── Colour: green for self, amber for close, white-dim for far 
    LLColor4 color;
    if (info.mIsLocalAvatar)
    {
        color = COLOR_SELF;
    }
    else if (info.mDistanceFromSelf < 32.f)
    {
        color = COLOR_OTHER;
    }
    else
    {
        color = COLOR_FAR;
    }
    info.mHUDLabel->setColor(color);

    info.mHUDLabel->setString(buildLabelText(info));
    info.mHUDLabel->setHidden(false);
}

std::string RacingOverlayManager::buildLabelText(const RacerInfo& info) const
{
    // Line 1: race position + name
    // Line 2: speed in km/h
    // Line 3: distance (only for other racers)

    std::ostringstream oss;

    if (info.mRacePosition > 0)
    {
        oss << "#" << info.mRacePosition << "  ";
    }
    oss << info.mDisplayName << "\n";

    // Speed: convert m/s -> km/h
    F32 kmh = info.mSpeedMPS * 3.6f;
    oss << llformat("%.0f km/h", kmh);

    if (!info.mIsLocalAvatar)
    {
        oss << llformat("  %.0fm", info.mDistanceFromSelf);
    }

    return oss.str();
}

void RacingOverlayManager::updatePositions()
{


    // Collect and sort
    std::vector<std::pair<F32, LLUUID>> sorted;
    sorted.reserve(mRacers.size());
    for (auto& kv : mRacers)
    {
        sorted.push_back({ kv.second.mDistanceFromSelf, kv.first });
    }
    std::sort(sorted.begin(), sorted.end()); // ascending distance

    S32 pos = 1;
    for (auto& entry : sorted)
    {
        mRacers[entry.second].mRacePosition = pos++;
    }
}

void RacingOverlayManager::removeRacer(const LLUUID& avatarID)
{
    auto it = mRacers.find(avatarID);
    if (it == mRacers.end()) return;

    if (it->second.mHUDLabel)
    {
        it->second.mHUDLabel->markDead();
    }
    mRacers.erase(it);
}

void RacingOverlayManager::clearAll()
{
    for (auto& kv : mRacers)
    {
        if (kv.second.mHUDLabel)
        {
            kv.second.mHUDLabel->markDead();
        }
    }
    mRacers.clear();
}
