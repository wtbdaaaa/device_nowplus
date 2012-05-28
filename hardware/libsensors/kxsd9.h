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

#ifndef ANDROID_kxsd9_SENSOR_H
#define ANDROID_kxsd9_SENSOR_H

#include <stdint.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>


#include "nusensors.h"
#include "SensorBase.h"
#include "InputEventReader.h"


#define __MAX(a,b) ((a)>=(b)?(a):(b))

/*****************************************************************************/

/*magic no*/
#define KXSD9_IOC_MAGIC  0xF6
/*max seq no*/
#define KXSD9_IOC_NR_MAX 12 

#define KXSD9_IOC_GET_ACC           _IOR(KXSD9_IOC_MAGIC, 0, kxsd9_acc_t)
#define KXSD9_IOC_DISB_MWUP         _IO(KXSD9_IOC_MAGIC, 1)
#define KXSD9_IOC_ENB_MWUP          _IOW(KXSD9_IOC_MAGIC, 2, unsigned short)
#define KXSD9_IOC_MWUP_WAIT         _IO(KXSD9_IOC_MAGIC, 3)
#define KXSD9_IOC_GET_SENSITIVITY   _IOR(KXSD9_IOC_MAGIC, 4, kxsd9_sensitivity_t)
#define KXSD9_IOC_GET_ZERO_G_OFFSET _IOR(KXSD9_IOC_MAGIC, 5, kxsd9_zero_g_offset_t )  
#define KXSD9_IOC_GET_DEFAULT       _IOR(KXSD9_IOC_MAGIC, 6, kxsd9_acc_t)
#define KXSD9_IOC_SET_RANGE         _IOWR( KXSD9_IOC_MAGIC, 7, int )
#define KXSD9_IOC_SET_BANDWIDTH     _IOWR( KXSD9_IOC_MAGIC, 8, int )
#define KXSD9_IOC_SET_MODE          _IOWR( KXSD9_IOC_MAGIC, 9, int )
#define KXSD9_IOC_SET_MODE_AKMD2    _IOWR( KXSD9_IOC_MAGIC, 10, int )
#define KXSD9_IOC_GET_INITIAL_VALUE _IOWR( KXSD9_IOC_MAGIC, 11, kxsd9_convert_t )
#define KXSD9_IOC_SET_DELAY         _IOWR( KXSD9_IOC_MAGIC, 12, int )


/*****************************************************************************/

struct input_event;

class kxsd9Sensor : public SensorBase {
public:
            kxsd9Sensor();
    virtual ~kxsd9Sensor();

    virtual int setDelay(int32_t handle, int64_t ns);
    virtual int enable(int32_t handle, int en);
    virtual int readEvents(sensors_event_t* data, int count);
    void processEvent(int code, int value);

private:
    uint32_t mEnabled;
    InputEventCircularReader mInputReader;
    sensors_event_t mPendingEvent;
};

/*****************************************************************************/

#endif  // ANDROID_kxsd9_SENSOR_H
