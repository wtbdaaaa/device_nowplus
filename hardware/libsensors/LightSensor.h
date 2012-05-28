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

#ifndef ANDROID_LIGHT_SENSOR_H
#define ANDROID_LIGHT_SENSOR_H

#include <stdint.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include "nusensors.h"
#include "SensorBase.h"
#include "InputEventReader.h"


/*****************************************************************************/

/*****************************************************************************/
/******************           LIGHT SENSOR SPECIFIC          *****************/
/*****************************************************************************/
/*magic no*/
#define L_IOC_MAGIC  0xF6

/*min seq no*/
#define L_IOC_NR_MIN 20

/*max seq no*/
#define L_IOC_NR_MAX (L_IOC_NR_MIN + 3)

#define L_IOC_GET_ADC_VAL _IOR(L_IOC_MAGIC, (L_IOC_NR_MIN + 0), unsigned int)

#define L_IOC_GET_ILLUM_LVL _IOR(L_IOC_MAGIC, (L_IOC_NR_MIN + 1), unsigned short)

// 20091031 ryun for polling
#define L_IOC_POLLING_TIMER_SET _IOW(L_IOC_MAGIC, (L_IOC_NR_MIN + 2), unsigned short)

#define L_IOC_POLLING_TIMER_CANCEL _IOW(L_IOC_MAGIC, (L_IOC_NR_MIN + 3), unsigned short)

/*****************************************************************************/

/*****************************************************************************/

struct input_event;

class LightSensor : public SensorBase {
    int mEnabled;
    InputEventCircularReader mInputReader;
    sensors_event_t mPendingEvent;
    bool mHasPendingEvent;

    float indexToValue(size_t index) const;
    int setInitialState();

public:
            LightSensor();
    virtual ~LightSensor();
    virtual int readEvents(sensors_event_t* data, int count);
    virtual bool hasPendingEvents() const;
    virtual int enable(int32_t handle, int enabled);
};

/*****************************************************************************/

#endif  // ANDROID_LIGHT_SENSOR_H
