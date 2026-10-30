// Copyright © 2013 CCP ehf.

#pragma once

extern "C" BLUEIMPORT bool BlueInitializeResourceLoading();

// Undoes BlueInitializeResourceLoading without exiting the process: joins the callback
// manager's threads, then shuts down the resource manager and MotherLode. Blue's resource
// loading cannot be started again afterwards. Does nothing if it was never started.
extern "C" BLUEIMPORT void BlueShutdownResourceLoading();
