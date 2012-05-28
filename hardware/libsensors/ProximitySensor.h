/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_PROXIMITY_SENSOR_H
#define ANDROID_PROXIMITY_SENSOR_H

#include <stdint.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include "nusensors.h"
#include "SensorBase.h"
#include "InputEventReader.h"


/*****************************************************************************/
/****************           PROXIMITY SENSOR SPECIFIC          ***************/
/*****************************************************************************/
/*magic no*/
#define P_IOC_MAGIC  0xF6

/*min seq no*/
#define P_IOC_NR_MIN 10

/*max seq no*/
#define P_IOC_NR_MAX (P_IOC_NR_MIN + 5)

/*commands*/
#define P_IOC_POWERUP_SET_MODE _IOW(P_IOC_MAGIC, (P_IOC_NR_MIN + 0), unsigned short)

#define P_IOC_WAIT_ST_CHG _IO(P_IOC_MAGIC, (P_IOC_NR_MIN + 1))

#define P_IOC_GET_PROX_OP _IOR(P_IOC_MAGIC, (P_IOC_NR_MIN + 2), unsigned short)

#define P_IOC_RESET  _IO(P_IOC_MAGIC, (P_IOC_NR_MIN + 3))

#define P_IOC_SHUTDOWN  _IO(P_IOC_MAGIC, (P_IOC_NR_MIN + 4))

#define P_IOC_RETURN_TO_OP   _IO(P_IOC_MAGIC, (P_IOC_NR_MIN + 5))


/*****************************************************************************/

struct input_event;

class ProximitySensor : public SensorBase {
    int mEnabled;
    InputEventCircularReader mInputReader;
    sensors_event_t mPendingEvent;
    bool mHasPendingEvent;

    int setInitialState();
    float indexToValue(size_t index) const;

public:
            ProximitySensor();
    virtual ~ProximitySensor();
    virtual int readEvents(sensors_event_t* data, int count);
    virtual bool hasPendingEvents() const;
    virtual int enable(int32_t handle, int enabled);
};

/*****************************************************************************/

#endif  // ANDROID_PROXIMITY_SENSOR_H
