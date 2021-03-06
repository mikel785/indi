#ifndef SIMPLESCOPE_H
#define SIMPLESCOPE_H

/*
   INDI Developers Manual
   Tutorial #2

   "Simple Telescope Driver"

   We develop a simple telescope simulator.

   Refer to README, which contains instruction on how to build this driver, and use it
   with an INDI-compatible client.

*/

/** \file simplescope.h
    \brief Construct a basic INDI telescope device that simulates GOTO commands.
    \author Jasem Mutlaq

    \example simplescope.h
    A simple GOTO telescope that simulator slewing operation.
*/

#include "indibase/inditelescope.h"

class SimpleScope : public INDI::Telescope
{
public:
    SimpleScope();

protected:
    // General device functions
    bool Connect();
    bool Disconnect();
    const char *getDefaultName();
    bool initProperties();

    // Telescoe specific functions
    bool ReadScopeStatus();
    bool Goto(double,double);
    bool Abort();

private:
    double currentRA;
    double currentDEC;
    double targetRA;
    double targetDEC;

    unsigned int DBG_SCOPE;

};

#endif // SIMPLESCOPE_H
