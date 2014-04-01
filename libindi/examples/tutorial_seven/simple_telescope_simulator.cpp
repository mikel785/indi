#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "simple_telescope_simulator.h"
#include "indicom.h"

#include <memory>

using namespace INDI::AlignmentSubsystem;

#define POLLMS 1000 // Default timer tick

// We declare an auto pointer to ScopeSim.
std::auto_ptr<ScopeSim> telescope_sim(0);

void ISInit()
{
   static int isInit =0;

   if (isInit == 1)
       return;

    isInit = 1;
    if(telescope_sim.get() == 0) telescope_sim.reset(new ScopeSim());
}

void ISGetProperties(const char *dev)
{
        ISInit();
        telescope_sim->ISGetProperties(dev);
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
{
        ISInit();
        telescope_sim->ISNewSwitch(dev, name, states, names, num);
}

void ISNewText(	const char *dev, const char *name, char *texts[], char *names[], int num)
{
        ISInit();
        telescope_sim->ISNewText(dev, name, texts, names, num);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
{
        ISInit();
        telescope_sim->ISNewNumber(dev, name, values, names, num);
}

void ISNewBLOB (const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[], int n)
{
    ISInit();
    telescope_sim->ISNewBLOB (dev, name, sizes, blobsizes, blobs, formats, names, n);
}

void ISSnoopDevice (XMLEle *root)
{
    INDI_UNUSED(root);
}

// One definition rule (ODR) constants
const long ScopeSim::MICROSTEPS_PER_REVOLUTION = 1000000;
const double ScopeSim::MICROSTEPS_PER_DEGREE = MICROSTEPS_PER_REVOLUTION / 360.0;
const double ScopeSim::DEFAULT_SLEW_RATE = MICROSTEPS_PER_DEGREE * 2.0;

// Private methods

bool ScopeSim::Abort()
{
    if (MovementNSSP.s == IPS_BUSY)
    {
        IUResetSwitch(&MovementNSSP);
        MovementNSSP.s = IPS_IDLE;
        IDSetSwitch(&MovementNSSP, NULL);
    }

    if (MovementWESP.s == IPS_BUSY)
    {
        MovementWESP.s = IPS_IDLE;
        IUResetSwitch(&MovementWESP);
        IDSetSwitch(&MovementWESP, NULL);
    }

    if (EqNP.s == IPS_BUSY)
    {
        EqNP.s = IPS_IDLE;
        IDSetNumber(&EqNP, NULL);
    }

    TrackState = SCOPE_IDLE;

    AxisStatusRA = AxisStatusDEC = STOPPED; // This marvelous inertia free scope can be stopped instantly!

    AbortSP.s      = IPS_OK;
    IUResetSwitch(&AbortSP);
    IDSetSwitch(&AbortSP, NULL);
    DEBUG(INDI::Logger::DBG_SESSION, "Telescope aborted.");

    return true;
}

bool ScopeSim::canSync()
{
    return true;
}

bool ScopeSim::Connect()
{
    SetTimer(POLLMS);
    return true;
}

bool ScopeSim::Disconnect()
{
    return true;
}

const char * ScopeSim::getDefaultName()
{
    return (char *)"Simple Telescope Simulator";
}

bool ScopeSim::Goto(double ra,double dec)
{
    if (ISS_ON == IUFindSwitch(&CoordSP,"TRACK")->s)
    {
        char RAStr[32], DecStr[32];
        fs_sexa(RAStr, ra, 2, 3600);
        fs_sexa(DecStr, dec, 2, 3600);
        CurrentTrackingTarget.ra = ra;
        CurrentTrackingTarget.dec = dec;
    }

    // Call the alignment subsystem to translate the celestial reference frame coordinate
    // into a telescope reference frame coordinate
    TelescopeDirectionVector TDV;
    ln_hrz_posn AltAz;
    if (TransformCelestialToTelescope(ra, dec, 0.0, TDV))
    {
        // The alignment subsystem has successfully transformed my coordinate
        AltitudeAzimuthFromTelescopeDirectionVector(TDV, AltAz);
    }
    else
    {
        // The alignment subsystem cannot transform the coordinate.
        // Try some simple rotations using the stored observatory position if any
        bool HavePosition = false;
        ln_lnlat_posn Position;
        if ((NULL != IUFindNumber(&LocationNP, "LAT")) && ( 0 != IUFindNumber(&LocationNP, "LAT")->value)
            && (NULL != IUFindNumber(&LocationNP, "LONG")) && ( 0 != IUFindNumber(&LocationNP, "LONG")->value))
        {
            // I assume that being on the equator and exactly on the prime meridian is unlikely
            Position.lat = IUFindNumber(&LocationNP, "LAT")->value;
            Position.lng = IUFindNumber(&LocationNP, "LONG")->value;
            HavePosition = true;
        }
        struct ln_equ_posn EquatorialCoordinates;
        // libnova works in decimal degrees
        EquatorialCoordinates.ra = ra * 360.0 / 24.0;
        EquatorialCoordinates.dec = dec;
        if (HavePosition)
        {
            ln_get_hrz_from_equ(&EquatorialCoordinates, &Position, ln_get_julian_from_sys(), &AltAz);
            TDV = TelescopeDirectionVectorFromAltitudeAzimuth(AltAz);
            switch (GetApproximateMountAlignment())
            {
                case ZENITH:
                    break;

                case NORTH_CELESTIAL_POLE:
                    // Rotate the TDV coordinate system clockwise (negative) around the y axis by 90 minus
                    // the (positive)observatory latitude. The vector itself is rotated anticlockwise
                    TDV.RotateAroundY(Position.lat - 90.0);
                    break;

                case SOUTH_CELESTIAL_POLE:
                    // Rotate the TDV coordinate system anticlockwise (positive) around the y axis by 90 plus
                    // the (negative)observatory latitude. The vector itself is rotated clockwise
                    TDV.RotateAroundY(Position.lat + 90.0);
                    break;
            }
            AltitudeAzimuthFromTelescopeDirectionVector(TDV, AltAz);
        }
        else
        {
            // The best I can do is just do a direct conversion to Alt/Az
            TDV = TelescopeDirectionVectorFromEquatorialCoordinates(EquatorialCoordinates);
            AltitudeAzimuthFromTelescopeDirectionVector(TDV, AltAz);
        }
    }

    long AltitudeTargetMicrosteps = int(AltAz.alt * MICROSTEPS_PER_DEGREE);
    long AzimuthTargetMicrosteps = int(AltAz.az * MICROSTEPS_PER_DEGREE);

    // Do I need to take out any complete revolutions before I do this test?
    if (AltitudeTargetMicrosteps > MICROSTEPS_PER_REVOLUTION / 2)
    {
        // Going the long way round - send it the other way
        AltitudeTargetMicrosteps -= MICROSTEPS_PER_REVOLUTION;
    }

    if (AzimuthTargetMicrosteps > MICROSTEPS_PER_REVOLUTION / 2)
    {
        // Going the long way round - send it the other way
        AzimuthTargetMicrosteps -= MICROSTEPS_PER_REVOLUTION;
    }

    GotoTargetMicrostepsDEC = AltitudeTargetMicrosteps;
    AxisStatusDEC = SLEWING_TO;
    GotoTargetMicrostepsRA = AzimuthTargetMicrosteps;
    AxisStatusRA = SLEWING_TO;

    TrackState = SCOPE_SLEWING;

    EqNP.s    = IPS_BUSY;

    return true;
}

bool ScopeSim::initProperties()
{
    /* Make sure to init parent properties first */
    INDI::Telescope::initProperties();

    // Let's simulate it to be an F/10 8" telescope
    ScopeParametersN[0].value = 203;
    ScopeParametersN[1].value = 2000;
    ScopeParametersN[2].value = 203;
    ScopeParametersN[3].value = 2000;

    TrackState=SCOPE_IDLE;

    /* Add debug controls so we may debug driver if necessary */
    addDebugControl();

    // Add alignment properties
    InitProperties(this);

    return true;
}

bool ScopeSim::ISNewBLOB (const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[], int n)
{
    if(strcmp(dev,getDeviceName())==0)
    {
        // Process alignment properties
        ProcessBlobProperties(this, name, sizes, blobsizes, blobs, formats, names, n);
    }
    // Pass it up the chain
    return INDI::Telescope::ISNewBLOB(dev, name, sizes, blobsizes, blobs, formats, names, n);
}

bool ScopeSim::ISNewNumber (const char *dev, const char *name, double values[], char *names[], int n)
{
    //  first check if it's for our device

    if(strcmp(dev,getDeviceName())==0)
    {
        // Process alignment properties
        ProcessNumberProperties(this, name, values, names, n);

    }

    //  if we didn't process it, continue up the chain, let somebody else
    //  give it a shot
    return INDI::Telescope::ISNewNumber(dev,name,values,names,n);
}

bool ScopeSim::ISNewSwitch (const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if(strcmp(dev,getDeviceName())==0)
    {
        // Process alignment properties
        ProcessSwitchProperties(this, name, states, names, n);
    }

    //  Nobody has claimed this, so, ignore it
    return INDI::Telescope::ISNewSwitch(dev,name,states,names,n);
}

bool ScopeSim::ISNewText (const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if(strcmp(dev,getDeviceName())==0)
    {
        // Process alignment properties
        ProcessTextProperties(this, name, texts, names, n);
    }
    // Pass it up the chain
    return INDI::Telescope::ISNewText(dev, name, texts, names, n);
}

bool ScopeSim::MoveNS(TelescopeMotionNS dir)
{
    switch (dir)
    {
        case MOTION_NORTH:
            if (PreviousNSMotion != PREVIOUS_NS_MOTION_NORTH)
            {
                AxisSlewRateDEC = DEFAULT_SLEW_RATE;
                AxisDirectionDEC = FORWARD;
                AxisStatusDEC = SLEWING;
                PreviousNSMotion = PREVIOUS_NS_MOTION_NORTH;
            }
            else
            {
                AxisStatusDEC = STOPPED;
                PreviousNSMotion = PREVIOUS_NS_MOTION_UNKNOWN;
                IUResetSwitch(&MovementNSSP);
                MovementNSSP.s = IPS_IDLE;
                IDSetSwitch(&MovementNSSP, NULL);
            }
            break;

        case MOTION_SOUTH:
            if (PreviousNSMotion != PREVIOUS_NS_MOTION_SOUTH)
            {
                AxisSlewRateDEC = DEFAULT_SLEW_RATE;
                AxisDirectionDEC = REVERSE;
                AxisStatusDEC = SLEWING;
                PreviousNSMotion = PREVIOUS_NS_MOTION_SOUTH;
            }
            else
            {
                AxisStatusDEC = STOPPED;
                PreviousNSMotion = PREVIOUS_NS_MOTION_UNKNOWN;
                IUResetSwitch(&MovementNSSP);
                MovementNSSP.s = IPS_IDLE;
                IDSetSwitch(&MovementNSSP, NULL);
            }
            break;
    }
    return false;
}

bool ScopeSim::MoveWE(TelescopeMotionWE dir)
{
    switch (dir)
    {
        case MOTION_WEST:
            if (PreviousWEMotion != PREVIOUS_WE_MOTION_WEST)
            {
                AxisSlewRateRA = DEFAULT_SLEW_RATE;
                AxisDirectionRA = FORWARD;
                AxisStatusRA = SLEWING;
                PreviousWEMotion = PREVIOUS_WE_MOTION_WEST;
            }
            else
            {
                AxisStatusRA = STOPPED;
                PreviousWEMotion = PREVIOUS_WE_MOTION_UNKNOWN;
                IUResetSwitch(&MovementWESP);
                MovementWESP.s = IPS_IDLE;
                IDSetSwitch(&MovementWESP, NULL);
            }
            break;

        case MOTION_EAST:
            if (PreviousWEMotion != PREVIOUS_WE_MOTION_EAST)
            {
                AxisSlewRateRA = DEFAULT_SLEW_RATE;
                AxisDirectionRA = REVERSE;
                AxisStatusRA = SLEWING;
                PreviousWEMotion = PREVIOUS_WE_MOTION_EAST;
            }
            else
            {
                AxisStatusRA = STOPPED;
                PreviousWEMotion = PREVIOUS_WE_MOTION_UNKNOWN;
                IUResetSwitch(&MovementWESP);
                MovementWESP.s = IPS_IDLE;
                IDSetSwitch(&MovementWESP, NULL);
            }
            break;
    }
    return false;
}

bool ScopeSim::ReadScopeStatus()
{
    struct ln_hrz_posn AltAz;
    AltAz.alt = double(CurrentEncoderMicrostepsDEC) / MICROSTEPS_PER_DEGREE;
    AltAz.az = double(CurrentEncoderMicrostepsRA) / MICROSTEPS_PER_DEGREE;
    TelescopeDirectionVector TDV = TelescopeDirectionVectorFromAltitudeAzimuth(AltAz);

    double RightAscension, Declination;
    if (!TransformTelescopeToCelestial( TDV, RightAscension, Declination))
    {
        bool HavePosition = false;
        ln_lnlat_posn Position;
        if ((NULL != IUFindNumber(&LocationNP, "LAT")) && ( 0 != IUFindNumber(&LocationNP, "LAT")->value)
            && (NULL != IUFindNumber(&LocationNP, "LONG")) && ( 0 != IUFindNumber(&LocationNP, "LONG")->value))
        {
            // I assume that being on the equator and exactly on the prime meridian is unlikely
            Position.lat = IUFindNumber(&LocationNP, "LAT")->value;
            Position.lng = IUFindNumber(&LocationNP, "LONG")->value;
            HavePosition = true;
        }
        struct ln_equ_posn EquatorialCoordinates;
        if (HavePosition)
        {
            TelescopeDirectionVector RotatedTDV(TDV);
            switch (GetApproximateMountAlignment())
            {
                case ZENITH:
                    break;

                case NORTH_CELESTIAL_POLE:
                    // Rotate the TDV coordinate system anticlockwise (positive) around the y axis by 90 minus
                    // the (positive)observatory latitude. The vector itself is rotated clockwise
                    RotatedTDV.RotateAroundY(90.0 - Position.lat);
                    AltitudeAzimuthFromTelescopeDirectionVector(RotatedTDV, AltAz);
                    break;

                case SOUTH_CELESTIAL_POLE:
                    // Rotate the TDV coordinate system clockwise (negative) around the y axis by 90 plus
                    // the (negative)observatory latitude. The vector itself is rotated anticlockwise
                    RotatedTDV.RotateAroundY(-90.0 - Position.lat);
                    AltitudeAzimuthFromTelescopeDirectionVector(RotatedTDV, AltAz);
                    break;
            }
            ln_get_equ_from_hrz(&AltAz, &Position, ln_get_julian_from_sys(), &EquatorialCoordinates);
        }
        else
            // The best I can do is just do a direct conversion to RA/DEC
            EquatorialCoordinatesFromTelescopeDirectionVector(TDV, EquatorialCoordinates);
        // libnova works in decimal degrees
        RightAscension = EquatorialCoordinates.ra * 24.0 / 360.0;
        Declination = EquatorialCoordinates.dec;
    }

    NewRaDec(RightAscension, Declination);

    return true;
}

bool ScopeSim::Sync(double ra, double dec)
{
    struct ln_hrz_posn AltAz;
    AltAz.alt = double(CurrentEncoderMicrostepsDEC) / MICROSTEPS_PER_DEGREE;
    AltAz.az = double(CurrentEncoderMicrostepsRA) / MICROSTEPS_PER_DEGREE;

    AlignmentDatabaseEntry NewEntry;
    NewEntry.ObservationJulianDate = ln_get_julian_from_sys();
    NewEntry.RightAscension = ra;
    NewEntry.Declination = dec;
    NewEntry.TelescopeDirection = TelescopeDirectionVectorFromAltitudeAzimuth(AltAz);
    NewEntry.PrivateDataSize = 0;

    if (!CheckForDuplicateSyncPoint(NewEntry))
    {

        GetAlignmentDatabase().push_back(NewEntry);

        // Tell the client about size change
        UpdateSize();

        // Tell the math plugin to reinitialise
        Initialise(this);

        return true;
    }
    return false;
}

void ScopeSim::TimerHit()
{
    // Simulate mount movement

    static struct timeval ltv; // previous system time
    struct timeval tv; // new system time
    double dt; // Elapsed time in seconds since last tick


    gettimeofday (&tv, NULL);

    if (ltv.tv_sec == 0 && ltv.tv_usec == 0)
        ltv = tv;

    dt = tv.tv_sec - ltv.tv_sec + (tv.tv_usec - ltv.tv_usec)/1e6;
    ltv = tv;


    // RA axis
    long SlewSteps = dt * AxisSlewRateRA;

    DEBUGF(DBG_SCOPE, "TimerHit - RA Current Encoder %ld SlewSteps %ld Direction %d Target %ld Status %d",
                        CurrentEncoderMicrostepsRA, SlewSteps, AxisDirectionRA, GotoTargetMicrostepsRA, AxisStatusRA);

    switch(AxisStatusRA)
    {
        case STOPPED:
            // Do nothing
            break;

        case SLEWING:
        {
            // Update the encoder
            SlewSteps = SlewSteps % MICROSTEPS_PER_REVOLUTION; // Just in case ;-)
            if (FORWARD == AxisDirectionRA)
                CurrentEncoderMicrostepsRA += SlewSteps;
            else
                CurrentEncoderMicrostepsRA -= SlewSteps;
            if (CurrentEncoderMicrostepsRA < 0)
                CurrentEncoderMicrostepsRA += MICROSTEPS_PER_REVOLUTION;
            else if (CurrentEncoderMicrostepsRA >= MICROSTEPS_PER_REVOLUTION)
                CurrentEncoderMicrostepsRA -= MICROSTEPS_PER_REVOLUTION;
            break;
        }

        case SLEWING_TO:
        {
            // Calculate steps to target
            int StepsToTarget;
            if (FORWARD == AxisDirectionRA)
            {
                if (CurrentEncoderMicrostepsRA <= GotoTargetMicrostepsRA)
                    StepsToTarget = GotoTargetMicrostepsRA - CurrentEncoderMicrostepsRA;
                else
                    StepsToTarget = CurrentEncoderMicrostepsRA - GotoTargetMicrostepsRA;
            }
            else
            {
                // Axis in reverse
                if (CurrentEncoderMicrostepsRA >= GotoTargetMicrostepsRA)
                    StepsToTarget = CurrentEncoderMicrostepsRA - GotoTargetMicrostepsRA;
                else
                    StepsToTarget = GotoTargetMicrostepsRA - CurrentEncoderMicrostepsRA;
            }
            if (StepsToTarget <= SlewSteps)
            {
                // Target was hit this tick
                AxisStatusRA = STOPPED;
                CurrentEncoderMicrostepsRA = GotoTargetMicrostepsRA;
            }
            else
            {
                if (FORWARD == AxisDirectionRA)
                    CurrentEncoderMicrostepsRA += SlewSteps;
                else
                    CurrentEncoderMicrostepsRA -= SlewSteps;
                if (CurrentEncoderMicrostepsRA < 0)
                    CurrentEncoderMicrostepsRA += MICROSTEPS_PER_REVOLUTION;
                else if (CurrentEncoderMicrostepsRA >= MICROSTEPS_PER_REVOLUTION)
                    CurrentEncoderMicrostepsRA -= MICROSTEPS_PER_REVOLUTION;
            }
        }
    }

    DEBUGF(DBG_SCOPE, "TimerHit - RA New Encoder %d New Status %d",  CurrentEncoderMicrostepsRA, AxisStatusRA);

    // DEC axis
    SlewSteps = dt * AxisSlewRateDEC;

    DEBUGF(DBG_SCOPE, "TimerHit - DEC Current Encoder %ld SlewSteps %d Direction %ld Target %ld Status %d",
                        CurrentEncoderMicrostepsDEC, SlewSteps, AxisDirectionDEC, GotoTargetMicrostepsDEC, AxisStatusDEC);

    switch(AxisStatusDEC)
    {
        case STOPPED:
            // Do nothing
            break;

        case SLEWING:
        {
            // Update the encoder
            SlewSteps = SlewSteps % MICROSTEPS_PER_REVOLUTION; // Just in case ;-)
            if (FORWARD == AxisDirectionDEC)
                CurrentEncoderMicrostepsDEC += SlewSteps;
            else
                CurrentEncoderMicrostepsDEC -= SlewSteps;
            if (CurrentEncoderMicrostepsDEC < 0)
                CurrentEncoderMicrostepsDEC += MICROSTEPS_PER_REVOLUTION;
            else if (CurrentEncoderMicrostepsDEC >= MICROSTEPS_PER_REVOLUTION)
                CurrentEncoderMicrostepsDEC -= MICROSTEPS_PER_REVOLUTION;
            break;
        }

        case SLEWING_TO:
        {
            // Calculate steps to target
            int StepsToTarget;
            if (FORWARD == AxisDirectionDEC)
            {
                if (CurrentEncoderMicrostepsDEC <= GotoTargetMicrostepsDEC)
                    StepsToTarget = GotoTargetMicrostepsDEC - CurrentEncoderMicrostepsDEC;
                else
                    StepsToTarget = CurrentEncoderMicrostepsDEC - GotoTargetMicrostepsDEC;
            }
            else
            {
                // Axis in reverse
                if (CurrentEncoderMicrostepsDEC >= GotoTargetMicrostepsDEC)
                    StepsToTarget = CurrentEncoderMicrostepsDEC - GotoTargetMicrostepsDEC;
                else
                    StepsToTarget = GotoTargetMicrostepsDEC - CurrentEncoderMicrostepsDEC;
            }
            if (StepsToTarget <= SlewSteps)
            {
                // Target was hit this tick
                AxisStatusDEC = STOPPED;
                CurrentEncoderMicrostepsDEC = GotoTargetMicrostepsDEC;
            }
            else
            {
                if (FORWARD == AxisDirectionDEC)
                    CurrentEncoderMicrostepsDEC += SlewSteps;
                else
                    CurrentEncoderMicrostepsDEC -= SlewSteps;
                if (CurrentEncoderMicrostepsDEC < 0)
                    CurrentEncoderMicrostepsDEC += MICROSTEPS_PER_REVOLUTION;
                else if (CurrentEncoderMicrostepsDEC >= MICROSTEPS_PER_REVOLUTION)
                    CurrentEncoderMicrostepsDEC -= MICROSTEPS_PER_REVOLUTION;
            }
        }
    }

    DEBUGF(DBG_SCOPE, "TimerHit - DEC New Encoder %d New Status %d",  CurrentEncoderMicrostepsDEC, AxisStatusDEC);

    INDI::Telescope::TimerHit(); // This will call ReadScopeStatus

    // OK I have updated the celestial reference frame RA/DEC in ReadScopeStatus
    // Now handle the tracking state
    switch(TrackState)
    {
        case SCOPE_SLEWING:
            if ((STOPPED == AxisStatusRA) && (STOPPED == AxisStatusDEC))
            {
                if (ISS_ON == IUFindSwitch(&CoordSP,"TRACK")->s)
                {
                    // Goto has finished start tracking
                    TrackState = SCOPE_TRACKING;
                    // Fall through to tracking case
                }
                else
                {
                    TrackState = SCOPE_IDLE;
                    break;
                }
            }
            else
                break;

        case SCOPE_TRACKING:
        {
            // Continue or start tracking
            // Calculate where the mount needs to be in POLLMS time
            // POLLMS is hardcoded to be one second
            double JulianOffset = 1.0 / (24.0 * 60 * 60); // TODO may need to make this longer to get a meaningful result
            TelescopeDirectionVector TDV;
            ln_hrz_posn AltAz;
            if (TransformCelestialToTelescope(CurrentTrackingTarget.ra, CurrentTrackingTarget.dec,
                                                JulianOffset, TDV))
                AltitudeAzimuthFromTelescopeDirectionVector(TDV, AltAz);
            else
            {
                // Try a conversion with the stored observatory position if any
                bool HavePosition = false;
                ln_lnlat_posn Position;
                if ((NULL != IUFindNumber(&LocationNP, "LAT")) && ( 0 != IUFindNumber(&LocationNP, "LAT")->value)
                    && (NULL != IUFindNumber(&LocationNP, "LONG")) && ( 0 != IUFindNumber(&LocationNP, "LONG")->value))
                {
                    // I assume that being on the equator and exactly on the prime meridian is unlikely
                    Position.lat = IUFindNumber(&LocationNP, "LAT")->value;
                    Position.lng = IUFindNumber(&LocationNP, "LONG")->value;
                    HavePosition = true;
                }
                struct ln_equ_posn EquatorialCoordinates;
                // libnova works in decimal degrees
                EquatorialCoordinates.ra = CurrentTrackingTarget.ra * 360.0 / 24.0;
                EquatorialCoordinates.dec = CurrentTrackingTarget.dec;
                if (HavePosition)
                    ln_get_hrz_from_equ(&EquatorialCoordinates, &Position,
                                            ln_get_julian_from_sys() + JulianOffset, &AltAz);
                else
                {
                    // No sense in tracking in this case
                    TrackState = SCOPE_IDLE;
                    break;
                }
            }
            long AltitudeOffsetMicrosteps = int(AltAz.alt * MICROSTEPS_PER_DEGREE - CurrentEncoderMicrostepsDEC);
            long AzimuthOffsetMicrosteps = int(AltAz.az * MICROSTEPS_PER_DEGREE - CurrentEncoderMicrostepsRA);

            if (AzimuthOffsetMicrosteps > MICROSTEPS_PER_REVOLUTION / 2)
            {
                // Going the long way round - send it the other way
                AzimuthOffsetMicrosteps -= MICROSTEPS_PER_REVOLUTION;
            }
            if (0 != AzimuthOffsetMicrosteps)
            {
                // Calculate the slewing rates needed to reach that position
                // at the correct time. This is simple as interval is one second
                AxisSlewRateRA = abs(AzimuthOffsetMicrosteps);
                AxisDirectionRA = AzimuthOffsetMicrosteps > 0 ? FORWARD : REVERSE;  // !!!! BEWARE INERTIA FREE MOUNT
            }
            else
            {
                // Nothing to do - stop the axis
                AxisStatusRA = STOPPED; // !!!! BEWARE INERTIA FREE MOUNT
            }

            if (AltitudeOffsetMicrosteps > MICROSTEPS_PER_REVOLUTION / 2)
            {
                // Going the long way round - send it the other way
                AltitudeOffsetMicrosteps -= MICROSTEPS_PER_REVOLUTION;
            }
            if (0 != AltitudeOffsetMicrosteps)
            {
                 // Calculate the slewing rates needed to reach that position
                // at the correct time.
                AxisSlewRateDEC = abs(AltitudeOffsetMicrosteps);
                AxisDirectionDEC = AltitudeOffsetMicrosteps > 0 ? FORWARD : REVERSE;  // !!!! BEWARE INERTIA FREE MOUNT
            }
            else
            {
                // Nothing to do - stop the axis
                AxisStatusDEC = STOPPED;  // !!!! BEWARE INERTIA FREE MOUNT
            }

            OldTrackingTargetMicrostepsDEC = AzimuthOffsetMicrosteps + CurrentEncoderMicrostepsRA;
            OldTrackingTargetMicrostepsDEC = AltitudeOffsetMicrosteps + CurrentEncoderMicrostepsDEC;
            break;
        }

        default:
            break;
    }
}
