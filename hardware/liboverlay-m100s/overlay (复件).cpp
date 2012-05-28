/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#define LOG_NDEBUG 0
#define LOG_TAG "TIOverlay"

#include <hardware/hardware.h>
#include <hardware/overlay.h>

extern "C" {
#include "v4l2_utils.h"
}
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../include/videodev.h"

#include <cutils/log.h>
#include <cutils/ashmem.h>
#include <cutils/atomic.h>
#include "overlay_common.h"

/*****************************************************************************/

#define LOG_FUNCTION_NAME       LOGV("%s() ###### ENTER %s() ######",  __FILE__,  __FUNCTION__);
#define LOG_FUNCTION_NAME_EXIT  LOGV("%s() ###### EXIT %s() ######", __FILE__, __FUNCTION__);

#define SHARED_DATA_MARKER             (0x68759746) // OVRLYSHM on phone keypad

/* These values should come from Surface Flinger */
#define LCD_WIDTH  480
#define LCD_HEIGHT 800

#define RSZ_720

#define OVERLAY_ANGLE_PATCH

extern int is720p;

extern int isCamera; //VIK_RSZ_720

typedef struct
{
    uint32_t posX;
    uint32_t posY;
    uint32_t posW;
    uint32_t posH;
    uint32_t colorkeyEn;
    uint32_t rotation;
} overlay_ctrl_t;

typedef struct
{
    uint32_t cropX;
    uint32_t cropY;
    uint32_t cropW;
    uint32_t cropH;
} overlay_data_t;

typedef struct 
{
    uint32_t marker;
    uint32_t size;

    uint32_t controlReady; // Only updated by the control side
    uint32_t dataReady;    // Only updated by the data side

    pthread_mutex_t lock;
    pthread_mutex_t stream_lock;
    pthread_mutexattr_t attr;
  
    uint32_t streamEn;

    uint32_t dispW;
    uint32_t dispH;

    // Need to count Qd buffers to be sure we don't block DQ'ing when exiting
    int qd_buf_count;
#ifdef OVERLAY_ANGLE_PATCH
    int cameraVendor;
    int cameraNum;
    int vtMode;
    int cameraCheck;
#endif
} overlay_shared_t;

// Only one instance is created per platform
struct overlay_control_context_t {
    struct overlay_control_device_t device;
    /* our private state goes below here */
    struct overlay_t* overlay_video1;
    struct overlay_t* overlay_video2;
};

// A separate instance is created per overlay data side user
struct overlay_d_t {
   /* struct overlay_data_device_t device; */
    /* our private state goes below here */
    int ctl_fd;
    int shared_fd;
    int shared_size;
    int width;
    int height;
    int format;
    int num_buffers;
    size_t *buffers_len;
    void **buffers;

    overlay_data_t    data;
    overlay_shared_t *shared;
    mapping_data_t    *mapping_data;    
    int cacheable_buffers;
    int maintain_coherency;
    int attributes_changed;

};

struct overlay_data_context_t {
    struct overlay_data_device_t device;
    /* our private state goes below here */
    struct overlay_d_t overlay_video1;
    struct overlay_d_t overlay_video2;
};

static int  create_shared_data( overlay_shared_t **shared );
static void destroy_shared_data( int shared_fd, overlay_shared_t *shared );
static int  open_shared_data( overlay_d_t *ctx );
static void close_shared_data( overlay_d_t *ctx );
static int  enable_streaming( overlay_shared_t *shared, int ovly_fd);
static int  disable_streaming( overlay_shared_t *shared, int ovly_fd);
static int overlay_device_open(const struct hw_module_t* module,
                               const char* name, struct hw_device_t** device);

static struct hw_module_methods_t overlay_module_methods = {
    open: overlay_device_open
};

struct overlay_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: OVERLAY_HARDWARE_MODULE_ID,
        name: "Sample Overlay module",
        author: "The Android Open Source Project",
        methods: &overlay_module_methods,
    }
};

/*****************************************************************************/

/*
 * This is the overlay_t object, it is returned to the user and represents
 * an overlay. here we use a subclass, where we can store our own state. 
 * This handles will be passed across processes and possibly given to other
 * HAL modules (for instance video decode modules).
 */
struct handle_t : public native_handle {
    /* add the data fields we need here, for instance: */
    int ctl_fd;
    int shared_fd;
    int width;
    int height;
    int format;		
    int num_buffers;
    int shared_size;
    int device_id;    /* Video pipe 1 and video pipe 2 */
};

static int handle_ctl_fd(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->ctl_fd;
}

static int handle_shared_fd(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->shared_fd;
}

static int handle_num_buffers(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->num_buffers;
}

static int handle_width(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->width;
}

static int handle_height(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->height;
}

static int handle_format(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->format;
}

static int handle_shared_size(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->shared_size;
}
/* Get the video pipe 1 or 2 */
static int handle_device_id(const overlay_handle_t overlay) {
    return static_cast<const struct handle_t *>(overlay)->device_id;
}

// A separate instance of this class is created per overlay 
class overlay_object : public overlay_t
{
    handle_t mHandle;

    overlay_ctrl_t    mCtl;
    overlay_ctrl_t    mCtlStage;
    overlay_shared_t *mShared;

    static overlay_handle_t getHandleRef(struct overlay_t* overlay) {
        /* returns a reference to the handle, caller doesn't take ownership */
        return &(static_cast<overlay_object *>(overlay)->mHandle);
    }

public:
    overlay_object(int ctl_fd, int shared_fd, int shared_size, int w, int h, int format, int num_buffers, int device_id)
    {
        this->overlay_t::getHandleRef = getHandleRef;
        mHandle.version     = sizeof(native_handle);
        mHandle.numFds      = 2;
        mHandle.numInts     = 5; // extra ints we have in our handle
        mHandle.ctl_fd      = ctl_fd;
        mHandle.shared_fd   = shared_fd;
        mHandle.width       = w;
        mHandle.height      = h;
        mHandle.format      = format;
        mHandle.num_buffers = num_buffers;
        mHandle.shared_size = shared_size;
        mHandle.device_id   = device_id;
        
        this->w      = w;
        this->h      = h;
        this->format = format;
        
        memset( &mCtl, 0, sizeof( mCtl ) );
        memset( &mCtlStage, 0, sizeof( mCtlStage ) );
    }

    int               ctl_fd()    { return mHandle.ctl_fd; }
    int               shared_fd() { return mHandle.shared_fd; }
    overlay_ctrl_t*   data()      { return &mCtl; }
    overlay_ctrl_t*   staging()   { return &mCtlStage; }
    overlay_shared_t* getShared() { return mShared; }
    void              setShared( overlay_shared_t *p ) { mShared = p; }
};

// ****************************************************************************
// Local Functions
// ****************************************************************************

//=========================================================
// create_shared_data
//

static int create_shared_data( overlay_shared_t **shared )
{
    int fd = -1;
    int size = getpagesize(); // assuming sizeof(overlay_shared_t) < a single page
    int mode = PROT_READ | PROT_WRITE;
    int ret = 0;
    overlay_shared_t *p = NULL;
   
    if ( (fd = ashmem_create_region( "overlay_data", size )) < 0 ) {
        LOGE("Failed to Create Overlay Shared Data!\n");
    } else if ( (p = (overlay_shared_t*) mmap( 0, size, mode, MAP_SHARED, fd, 0 )) == MAP_FAILED ) {
        LOGE("Failed to Map Overlay Shared Data!\n");
        close( fd );
        fd = -1;
    } else {
        memset( p, 0, size );
        p->marker = SHARED_DATA_MARKER;
        p->size   = size;
        if ((ret = pthread_mutexattr_init(&p->attr)) != 0) {
            LOGE("Failed to initialize overlay mutex attr");
        }
        if (ret == 0 && (ret = pthread_mutexattr_setpshared(&p->attr, PTHREAD_PROCESS_SHARED)) != 0) {
           LOGE("Failed to set the overlay mutex attr to be shared across-processes");
        }
        if (ret == 0 && (ret = pthread_mutex_init(&p->lock, &p->attr)) != 0) {
            LOGE("Failed to initialize overlay mutex\n");
        }
        if (ret != 0) {
            munmap(p, size);
            close(fd);
            return -1;
        }
        *shared = p ;

    }
    
    return ( fd );
}

//=========================================================
// destroy_shared_data
//

static void destroy_shared_data( int shared_fd, overlay_shared_t *shared )
{
    if ( shared == NULL ) {
        // Not open, just return
    } else {
        if (pthread_mutex_destroy(&shared->lock)) {
            LOGE("Failed to uninitialize overlay mutex!\n");
        }
        
        if (pthread_mutexattr_destroy(&shared->attr)) {
            LOGE("Failed to uninitialize the overlay mutex attr!\n");
        }
  
        shared->marker = 0;

        int size = shared->size;
        
        if ( munmap( shared, size ) != 0 ) {
            LOGE("Failed to Unmap Overlay Shared Data!\n");
        }
    
        if ( close( shared_fd ) != 0 ) {
            LOGE("Failed to Close Overlay Shared Data!\n");
        }
    }
}

//=========================================================
// open_shared_data
//

static int open_shared_data( overlay_d_t *ctx )  /* aj Take care in initialize to send video pipe 1 or 2 no change required here */
{
    int rc   = -1;
    int mode = PROT_READ | PROT_WRITE;
    int fd   = ctx->shared_fd;
    int size = ctx->shared_size;

    if ( ctx->shared != NULL ) {
        // Already open, return success
        LOGE("Overlay Shared Data Already Open\n");
        rc = 0;
    } else if ( (ctx->shared = (overlay_shared_t*) mmap(0, size, mode, MAP_SHARED, fd, 0)) == MAP_FAILED ) {
        LOGE("Failed to Map Overlay Shared Data!\n");
    } else if ( ctx->shared->marker != SHARED_DATA_MARKER ) {
        LOGE("Invalid Overlay Shared Marker!\n");
        munmap( ctx->shared, size );
    } else if ( (int)ctx->shared->size != size ) {
        LOGE("Invalid Overlay Shared Size!\n");
        munmap( ctx->shared, size );
    } else {
        rc = 0;
    }
    
    return ( rc );
}

//=========================================================
// close_shared_data
//

static void close_shared_data( overlay_d_t *ctx ) /* aj Take care in initialize to send video pipe 1 or 2 no change required here */
{
    if ( ctx->shared == NULL ) {
        // Not open, just return
    } else {
        int size = ctx->shared->size;
        
        if ( munmap( ctx->shared, size ) != 0 ) {
            LOGE("Failed to Unmap Overlay Shared Data!\n");
        }
        
        ctx->shared = NULL;
    }
}

//=========================================================
// enable_streaming
//

static int enable_streaming( overlay_shared_t *shared, int ovly_fd)
{
    LOG_FUNCTION_NAME

    int rc = 0;

    if ( !shared->controlReady || !shared->dataReady ) {
        LOGE("Postponing Stream Enable/%d/%d\n", shared->controlReady, shared->dataReady );
    } else {
        if(shared->streamEn == 0) { 
            rc = v4l2_overlay_stream_on( ovly_fd );
            if ( rc ) {
                LOGE("Stream Enable Failed!/%d\n", rc);                
            } else {
                LOGV("stream enabled");
                shared->streamEn = 1;
            }
        }
    }

    return ( rc );
}

//=========================================================
// disable_streaming
//

static int disable_streaming( overlay_shared_t *shared, int ovly_fd)
{
    LOG_FUNCTION_NAME

    int rc = 0;

    if ( !shared->controlReady || !shared->dataReady ) {
        LOGE("Postponing Stream Enable/%d/%d\n", shared->controlReady, shared->dataReady );
    } else {
        if (shared->streamEn == 1) {
            rc = v4l2_overlay_stream_off( ovly_fd );
            if ( rc ) {
                LOGE("Stream Disable Failed!/%d\n", rc);
            } else {
                LOGV("Stream Disable Sucess. ret=%d", rc);
                shared->streamEn = 0;
            }
        }        
        shared->qd_buf_count = 0;
    }
    
    return ( rc );
}


// ****************************************************************************
// Control module
// ****************************************************************************

//=========================================================
// overlay_get
//

static int overlay_get(struct overlay_control_device_t *dev, int name)
{
    LOG_FUNCTION_NAME

    int result = -1;

    switch (name) {
    case OVERLAY_MINIFICATION_LIMIT:
        result = 0;  break; // 0 = no limit
    case OVERLAY_MAGNIFICATION_LIMIT:
        result = 0;  break; // 0 = no limit
    case OVERLAY_SCALING_FRAC_BITS:
        result = 0;  break; // 0 = infinite
    case OVERLAY_ROTATION_STEP_DEG:
        result = 90; break; // 90 rotation steps (for instance)
    case OVERLAY_HORIZONTAL_ALIGNMENT:
        result = 1;  break; // 1-pixel alignment
    case OVERLAY_VERTICAL_ALIGNMENT:
        result = 1;  break; // 1-pixel alignment
    case OVERLAY_WIDTH_ALIGNMENT:
        result = 1;  break; // 1-pixel alignment
    case OVERLAY_HEIGHT_ALIGNMENT:
        break;
    }

    LOG_FUNCTION_NAME_EXIT
    
    return result;
}

//=========================================================
// overlay_createOverlay
//

static overlay_t* overlay_createOverlay
( struct overlay_control_device_t *dev
, uint32_t w
, uint32_t h
, int32_t  format
)
{   
    overlay_object            *overlay = NULL;
    overlay_control_context_t *ctx = (overlay_control_context_t *)dev;
    overlay_shared_t          *shared;

    int ret;
    uint32_t num = NUM_OVERLAY_BUFFERS_REQUESTED;
    int fd;
    int shared_fd;
    
#ifdef OVERLAY_ANGLE_PATCH   
    int rotationDegree = 0;
    int cropValue = 0;
    int mirrorValue = 0;
    int cameraVendor = 1;
    int cameraNum = 0;
    int vtMode =0;
    int cameraCheck = 1;
    int cropCheck = 0;
#endif    

    LOG_FUNCTION_NAME   

#ifdef OVERLAY_ANGLE_PATCH   
    cameraNum     = ((format & 0xFF000000)>>24)& 0x1 ;
    vtMode        = ((format & 0xFF000000)>>25)& 0x1 ;
    cameraCheck   = ((format & 0xFF000000)>>26)& 0x1 ;
    cropCheck     = ((format & 0xFF000000)>>27)& 0x1 ;
    cameraVendor  = ((format & 0xFF000000)>>28)& 0x1 ;

    LOGV("cameraNum=%d vtMode=%d cameraCheck=%d cameraVendor=%d cropCheck=%d ", cameraNum, vtMode, cameraCheck, cameraVendor, cropCheck);
    
    isCamera = cameraCheck;

    if(cameraCheck == 1) {
        if(vtMode == 1) {
            if(cameraNum == 1) {
                rotationDegree = 0;
                mirrorValue = 1;
            } else {
                rotationDegree = 0;
                mirrorValue = 0;
            }            
        } else {
            if(cameraNum == 1) {
                rotationDegree = 270;
                mirrorValue = 1;
            } else {
                rotationDegree = 90;
                mirrorValue = 0;
            }
        }

        if(cropCheck == 1)
            cropValue = 80;
    } else {
        vtMode = ((format & 0xFF000000)>>27)& 0x1 ;
        if(vtMode) { 
            rotationDegree = 0; 
            mirrorValue = 1;
        } else {
            if(cameraNum == 1) {
                rotationDegree = 270;
                mirrorValue = 1;
            } else {
                rotationDegree = 90;
                mirrorValue = 0;
            }
        }
    }

    LOGV("rotationDegree=%d cropValue=%d", rotationDegree, cropValue);
    
    format &= 0x00FFFFFF;
    LOGV("VIK_DBG:: Format in OVerlay Before =   0x%X", format);
    
#endif
    LOGV("Create overlay. w=%d h=%d format=%d", w, h, format);

    if ( ctx->overlay_video1 && ctx->overlay_video2) {
        LOGE("Overlays already in use\n");
    }

    if(!ctx->overlay_video1) {
        if ( (shared_fd = create_shared_data( &shared )) == -1 ) {
            // Just return an error
            LOGE("Failed to create shared data\n");
        } else if ( (fd = v4l2_overlay_open(V4L2_OVERLAY_PLANE_VIDEO1)) < 0 ) {
            LOGE("Failed to open overlay device\n");
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_init(fd, w, h, format) != 0 ) {
            LOGE("Failed initializing overlays\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        }
#ifdef OVERLAY_ANGLE_PATCH
        else if ( v4l2_overlay_set_rotation(fd, rotationDegree, 0) != 0 )
#else
        else if ( v4l2_overlay_set_rotation(fd, 90, 0) != 0 )
#endif
        {
            LOGE("Failed defaulting rotation\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } 
#ifdef OVERLAY_ANGLE_PATCH
        else if ( v4l2_overlay_set_mirroring(fd, mirrorValue, 0) ) {
            LOGE("Failed defaulting mirror window\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );            
        }
#endif
        else if ( v4l2_overlay_set_crop(fd, 0, 0, w, h) != 0 ) {
            LOGE("Failed defaulting crop window\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_set_colorkey(fd, 1, 0) ) {
            LOGE("Failed enabling color key\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_req_buf(fd, &num, 1, 0) != 0 ) {//Changed 720p[[]] from 11 to 10
            LOGE("Failed requesting buffers\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( (overlay = new overlay_object(fd, shared_fd, shared->size, 
                    w, h, format, num, V4L2_OVERLAY_PLANE_VIDEO1)) == NULL ) {
            LOGE("Failed to create overlay object\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else {
            ctx->overlay_video1 = overlay;
            
            overlay->setShared( shared );
            
            shared->controlReady = 0;
            shared->streamEn     = 0;
            shared->dispW        = LCD_WIDTH; // Need to determine this properly
            shared->dispH        = LCD_HEIGHT; // Need to determine this properly
#ifdef OVERLAY_ANGLE_PATCH
            shared->cameraVendor = cameraVendor;
            shared->cameraNum    = cameraNum;
            shared->vtMode       = vtMode;
            shared->cameraCheck  = cameraCheck;
#endif
            LOGE("Opened video1/fd=%d/obj=%08lx/shm=%d/size=%d"
                , fd, (unsigned long)overlay, shared_fd, shared->size);
        }
    } else if(!ctx->overlay_video2) {    
        if ( (shared_fd = create_shared_data( &shared )) == -1 ) {
            // Just return an error
            LOGE("Failed to create shared data\n");
        } else if ( (fd = v4l2_overlay_open(V4L2_OVERLAY_PLANE_VIDEO2)) < 0 ) {
            LOGE("Failed to open overlay device\n");
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_init(fd, w, h, format) != 0 ) {
            LOGE("Failed initializing overlays\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        }
#ifdef OVERLAY_ANGLE_PATCH
        else if ( v4l2_overlay_set_rotation(fd, rotationDegree, 0) != 0 )
#else
        else if ( v4l2_overlay_set_rotation(fd, 0, 0) != 0 )
#endif
        {
            LOGE("Failed defaulting rotation\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        }
#ifdef OVERLAY_ANGLE_PATCH
        else if ( v4l2_overlay_set_mirroring(fd, mirrorValue, 0) ) {
            LOGE("Failed defaulting mirror window\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );            
        }
#endif
        else if ( v4l2_overlay_set_crop(fd,0, 0, w, h) != 0 ) {
            LOGE("Failed defaulting crop window\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_set_colorkey(fd, 1, 0) ) {
            LOGE("Failed enabling color key\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( v4l2_overlay_req_buf(fd, &num, 1, 1) != 0 ) {
            LOGE("Failed requesting buffers\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else if ( (overlay = new overlay_object(fd, shared_fd, shared->size, 
                    w, h, format, num, V4L2_OVERLAY_PLANE_VIDEO2)) == NULL ) {
            LOGE("Failed to create overlay object\n");
            close( fd );
            destroy_shared_data( shared_fd, shared );
        } else {
            ctx->overlay_video2 = overlay;
            
            overlay->setShared( shared );
            
            shared->controlReady = 0;
            shared->streamEn     = 0;
            shared->dispW        = LCD_WIDTH; // Need to determine this properly
            shared->dispH        = LCD_HEIGHT; // Need to determine this properly
#ifdef OVERLAY_ANGLE_PATCH
            shared->cameraVendor = cameraVendor;
            shared->cameraNum    = cameraNum;
            shared->vtMode       = vtMode;
            shared->cameraCheck  = cameraCheck;
#endif
            LOGE("Opened video2/fd=%d/obj=%08lx/shm=%d/size=%d"
                , fd, (unsigned long)overlay, shared_fd, shared->size);
        }
    } else {
        LOGE("Aj problems need to fix during testing");
    }

    LOG_FUNCTION_NAME_EXIT

    return ( overlay );
}

//=========================================================
// overlay_destroyOverlay
//

static void overlay_destroyOverlay ( struct overlay_control_device_t *dev, overlay_t* overlay )
{  
    overlay_control_context_t *ctx = (overlay_control_context_t *)dev;
    overlay_object *obj = static_cast<overlay_object *>(overlay);
    
    int fd = obj->ctl_fd();
    overlay_shared_t *shared = obj->getShared();
    
    LOG_FUNCTION_NAME   
        
    if ( shared == NULL ) {
        LOGE("Overlay already destroyed\n");
    } else {
        if ( disable_streaming(shared, fd) != 0 ) {
            LOGE("Error disabling the stream\n");
        }

        if ( overlay ) {
            if (ctx->overlay_video1 == overlay) 
              ctx->overlay_video1 = NULL;
            else if (ctx->overlay_video2 == overlay)
              ctx->overlay_video2 = NULL;
            else
              LOGE("aj check this out while testing");
              
            delete overlay;
        }
        
        if ( v4l2_overlay_set_colorkey(fd, 0, 0) != 0 ) {
            LOGE("Error disabling color key\n");
        }

        if ( v4l2_overlay_set_rotation(fd, 0, 0) != 0) {
            LOGE("Error disabling Mirroring\n");  
        }

        if ( v4l2_overlay_close(fd) != 0 ) {
            LOGE( "Error closing overlay fd\n" );
        }

        destroy_shared_data( obj->shared_fd(), shared );
        obj->setShared( NULL );

        LOGE("Destroying overlay/fd=%d/obj=%08lx", fd, (unsigned long)overlay);
    }

    LOG_FUNCTION_NAME_EXIT
}

//=========================================================
// overlay_setPosition
//

static int overlay_setPosition
( struct overlay_control_device_t *dev
, overlay_t* overlay
, int x
, int y
, uint32_t w
, uint32_t h
)
{
    overlay_object *obj = static_cast<overlay_object *>(overlay);

    overlay_ctrl_t   *stage  = obj->staging();
    overlay_shared_t *shared = obj->getShared();

    int rc = 0;
    int tmp = 0, incH = 0, incW = 0; //VIK_RSZ_720  // For aligning the Overlay window with UI

    LOG_FUNCTION_NAME   

    // FIXME:  This is a hack to deal with seemingly unintentional negative offset
    // that pop up now and again.  I believe the negative offsets are due to a
    // surface flinger bug that has not yet been found or fixed.
    // 
    // This logic here is to return an error if the rectangle is not fully within 
    // the display, unless we have not received a valid position yet, in which 
    // case we will do our best to adjust the rectangle to be within the display.
    
    // Require a minimum size
    if ( w < 16 || h < 16 ) {
        // Return an error
        rc = -1; 
    } else if ( !shared->controlReady ) {
        if ( x < 0 ) 
            x = 0;
        if ( y < 0 ) 
            y = 0;
        if ( w > shared->dispW ) 
            w = shared->dispW;
        if ( h > shared->dispH ) 
            h = shared->dispH;
        if ( (x + w) > shared->dispW ) 
            w = shared->dispW - x;
        if ( (y + h) > shared->dispH ) 
            h = shared->dispH - y;
    } else if ( x < 0 
            || y < 0
            || (x + w) > shared->dispW
            || (y + h) > shared->dispH ) {
        // Return an error
        rc = -1;
    }
    
#ifdef RSZ_720    
    if((is720p == 1) 
#ifdef OVERLAY_ANGLE_PATCH        
    && (shared->cameraCheck == 1)
#endif    
    ) {
        tmp = h%16;
        h += (16-tmp);
        incH = (16-tmp);   // For aligning the Overlay window with UI
        tmp = w%16;
        w += (16-tmp);
        incW = (16-tmp);  // For aligning the Overlay window with UI

        // For aligning the Overlay window with UI [[
        if(y) {
            y -= incH/2;
            tmp = y%2; 
            if(tmp)
            y -= (2-tmp);
            if ( y < 0 ) y = 0;
        }

        if(x) {
            x -= incW/2;
            tmp = x%2; 
            if(tmp)
            x -= (2-tmp);
            if ( x < 0 ) x = 0;
        }
        // For aligning the Overlay window with UI ]]
        
    }
#endif    

    if ( rc == 0 ) {
        stage->posX = x;
        stage->posY = y;
        stage->posW = w;
        stage->posH = h;
    }
    
    return ( rc );
}

//=========================================================
// overlay_getPosition
//

static int overlay_getPosition
( struct overlay_control_device_t *dev
, overlay_t* overlay
, int* x
, int* y
, uint32_t* w
, uint32_t* h
)
{
    int fd = static_cast<overlay_object *>(overlay)->ctl_fd();
    int rc = 0;

    LOG_FUNCTION_NAME   

    if ( v4l2_overlay_get_position(fd, x, y, (int32_t*)w, (int32_t*)h) != 0 ) {
        rc = -EINVAL;
    }
    
    LOG_FUNCTION_NAME_EXIT
        
    return ( rc );
}

//=========================================================
// overlay_setParameter
//

static int overlay_setParameter
( struct overlay_control_device_t *dev
, overlay_t* overlay
, int param
, int value
)
{
    overlay_ctrl_t *stage = static_cast<overlay_object *>(overlay)->staging();
    overlay_shared_t *shared = static_cast<overlay_object *>(overlay)->getShared();    
    int rc = 0;

    LOG_FUNCTION_NAME
    
    switch (param) 
    {
        case OVERLAY_DITHER:
            LOGV("Set Dither - Not Implemented Yet!!!!!!!!!!!!!!!!!!!");
            break;

        case OVERLAY_TRANSFORM:
            LOGE("rotation param=%d",value);
#ifdef OVERLAY_ANGLE_PATCH
            if(shared->vtMode == 1)
                value = 0;         
            else
                if(shared->cameraNum == 1)
                    value = OVERLAY_TRANSFORM_ROT_270;
#endif    
            switch ( value ) {
            //case OVERLAY_TRANSFORM_ROT_0:
            case 0:
                stage->rotation = 0;
                break;
            case OVERLAY_TRANSFORM_ROT_90:
                stage->rotation = 90;
                break;
            case OVERLAY_TRANSFORM_ROT_180:
                stage->rotation = 180;
                break;
            case OVERLAY_TRANSFORM_ROT_270:
                stage->rotation = 270;
                break;
            default:
                rc = -EINVAL;
                break;
            }
            break;
    }

    LOG_FUNCTION_NAME_EXIT
    
    return ( rc );
}

//=========================================================
// overlay_stage
//

static int overlay_stage
( struct overlay_control_device_t *dev
, overlay_t* overlay
)
{
    return ( 0 );
}

//=========================================================
// overlay_commit
//

static int overlay_commitUpdates
( struct overlay_control_device_t *dev
, overlay_t* overlay
)
{

    overlay_object *obj = static_cast<overlay_object *>(overlay);

    overlay_ctrl_t   *data   = obj->data();
    overlay_ctrl_t   *stage  = obj->staging();
    overlay_shared_t *shared = obj->getShared();

    int ret = -1;
    int rc;
    int x,y,w,h;
    int fd = obj->ctl_fd();
    overlay_data_t eCropData;

#ifdef RSZ_720
    uint32_t pix_width, pix_height;
#endif    

    LOG_FUNCTION_NAME

    if ( shared == NULL ) {
        LOGE("Shared Data Not Init'd!\n");
    } else {
        pthread_mutex_lock(&shared->lock);        
        shared->controlReady = 1;

        if(is720p == 1) {
            stage->rotation = 90;
#ifdef OVERLAY_ANGLE_PATCH
            if(shared->cameraCheck == 0)
#endif
            {
                stage->posX     = 0;
                stage->posY     = 0;
                stage->posW     = 1280;
                stage->posH     = 720;
                stage->rotation = 90;
            }
        }

        if (  data->posX       == stage->posX 
           && data->posY       == stage->posY
           && data->posW       == stage->posW
           && data->posH       == stage->posH
           && data->rotation   == stage->rotation
           && data->colorkeyEn == stage->colorkeyEn ) {
            // Nothing to do if it is non-720p playback or if it is camcorder
            if(is720p == 0 
#ifdef OVERLAY_ANGLE_PATCH                
            || shared->cameraCheck == 1
#endif            
            ) {  
                LOGE("Nothing to do. Ln=%d", __LINE__);
                pthread_mutex_unlock(&shared->lock);
                return 0;
            }
        }
        
    /* If Rotation has changed but window has not changed yet, ignore this commit.
        SurfaceFlinger will set the right window parameters and call commit again. */
        //gerit # I01055508
      if ((data->posX == stage->posX) &&
          (data->posY == stage->posY) &&
          (data->posW == stage->posW) &&
          (data->posH == stage->posH))
        {
            LOGI("Nothing to do");
            pthread_mutex_unlock(&shared->lock);
            return 0;
        }

        data->posX       = stage->posX;
        data->posY       = stage->posY;
        data->posW       = stage->posW;
        data->posH       = stage->posH;
        data->rotation   = stage->rotation;
        data->colorkeyEn = stage->colorkeyEn;
        
        LOGE("%s: rotation=%d\n",__func__,stage->rotation);
        // Adjust the coordinate system to match the V4L change
        switch ( data->rotation ) {
        case 90:
            x = data->posY;
            y = data->posX;//LCD_WIDTH - data->posX - data->posW;
            w = data->posH;
            h = data->posW;
            break;
        case 180:
            x = ((shared->dispW - data->posX) - data->posW);
            y = ((shared->dispH - data->posY) - data->posH);
            w = data->posW;
            h = data->posH;
            break;
        case 270:
            x = data->posY;
            y = data->posX;
            w = data->posH;
            h = data->posW;
            break;
        default: // 0
            x = data->posX;
            y = data->posY;
            w = data->posW;
            h = data->posH;
            break;
        }

#ifdef OVERLAY_ANGLE_PATCH
        if(shared->vtMode == 1)
            data->rotation = 0;  
        else
            if(shared->cameraNum == 1)
                data->rotation = 270;
#endif

        LOGV("Position/X%d/Y%d/W%d/H%d\n", data->posX, data->posY, data->posW, data->posH );
        LOGV("Adjusted Position/X%d/Y%d/W%d/H%d\n", x, y, w, h );
        LOGV("Rotation/%d\n", data->rotation );
        LOGV("ColorKey/%d\n", data->colorkeyEn );
        LOGV("shared->dispW = %d, shared->dispH = %d", shared->dispW, shared->dispH);

        if ( (rc = v4l2_overlay_get_crop(fd, &eCropData.cropX, &eCropData.cropY, &eCropData.cropW, &eCropData.cropH)) != 0) {
            LOGE("Get crop value Failed!/%d\n", rc);
            ret = rc;
        } else if ( (rc = disable_streaming(shared, fd)) != 0 ) {
            LOGE("Stream Off Failed!/%d\n", rc);
            ret = rc;
        } else if ((is720p == 1) && (rc=v4l2_overlay_init(fd,1280,720,OVERLAY_FORMAT_YCbYCr_422_I)!=0)) {
            LOGE("KR:NOT EXPECTED\n");
            ret=rc;
        }
#ifdef RSZ_720
        else if((rc = v4l2_overlay_get_input_size_and_format(fd, &pix_width, &pix_height))!=0) {
            LOGE("Error: couldn't get input Frame size and format");  
            ret = rc;
        }
#endif
        else if ( (rc = v4l2_overlay_set_rotation(fd, data->rotation, 0)) != 0 ) {
            LOGE("Set Rotation Failed!/%d\n", rc);
            ret = rc;
        }
#ifdef RSZ_720
        else if ( (rc = v4l2_overlay_set_crop(fd, 
                        eCropData.cropX, 
                        eCropData.cropY, 
                        pix_width - (eCropData.cropX*2), 
                        pix_height - (eCropData.cropY*2))) != 0 )
#else
        else if ( (rc = v4l2_overlay_set_crop(fd, 
                        eCropData.cropX, 
                        eCropData.cropY, 
                        eCropData.cropW, 
                        eCropData.cropH)) != 0 )
#endif
        {
            LOGE("Set Cropping Failed!/%d\n",rc);
            ret = rc;
        } else if ( (rc = v4l2_overlay_set_position(fd, x, y, w, h)) != 0 ) {
            LOGE("Set Position Failed!/%d\n", rc);
            ret = rc;
        } else if ( (rc = v4l2_overlay_set_colorkey(fd, 1, 0)) != 0 ) {
            LOGE("Failed enabling color key\n");
            ret = rc;
        }
        pthread_mutex_unlock(&shared->lock);
    }

    return ( ret );
}

//=========================================================
// overlay_control_close
//

static int overlay_control_close(struct hw_device_t *dev)
{    
    struct overlay_control_context_t* ctx = (struct overlay_control_context_t*)dev;
    overlay_object *overlay_v1;
    overlay_object *overlay_v2;

    LOG_FUNCTION_NAME

    if (ctx) {
        overlay_v1 = static_cast<overlay_object *>(ctx->overlay_video1);
        overlay_v2 = static_cast<overlay_object *>(ctx->overlay_video2);

        overlay_destroyOverlay((struct overlay_control_device_t *)ctx, overlay_v1);
        overlay_destroyOverlay((struct overlay_control_device_t *)ctx, overlay_v2);

        free(ctx);
    }

    LOG_FUNCTION_NAME_EXIT
        
    return ( 0 );
}

// ****************************************************************************
// Data module
// ****************************************************************************

//=========================================================
// overlay_initialize
//

int overlay_initialize
( struct overlay_data_device_t *dev
, overlay_handle_t handle
)
{
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct stat stat;
    struct overlay_d_t *data;
    
    int i;
    int rc = -1;

    LOG_FUNCTION_NAME   

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 
    
    data->num_buffers        = handle_num_buffers(handle);
    data->width              = handle_width(handle);
    data->height             = handle_height(handle);
    data->format             = handle_format(handle);
    data->ctl_fd             = handle_ctl_fd(handle);
    data->shared_fd          = handle_shared_fd(handle);
    data->shared_size        = handle_shared_size(handle);
    data->shared             = NULL;
    data->cacheable_buffers  = 0;
    data->maintain_coherency = 0;
    data->attributes_changed = 0;
    
    if ( fstat(data->ctl_fd, &stat) ) {
        LOGE("Error = %s from %s\n", strerror(errno), "overlay initialize");
    } else if ( open_shared_data( data ) != 0 ) {
        // Just return an error
        LOGE("Failed open shared data\n");
    } else {
        data->shared->dataReady = 0;
        data->shared->qd_buf_count = 0;

        data->mapping_data = new mapping_data_t; 
        data->buffers      = new void* [data->num_buffers];
        data->buffers_len  = new size_t[data->num_buffers];
        if ( !data->buffers || !data->buffers_len || !data->mapping_data) {
            LOGE("Failed alloc'ing buffer arrays\n");
            close_shared_data( data );
        } else {
            int ret;
            for (i = 0; i < data->num_buffers; i++) {
                ret = v4l2_overlay_map_buf(data->ctl_fd, i, &data->buffers[i], &data->buffers_len[i]);
                if ( ret != 0 ) {
                    LOGE("Failed mapping buffers\n");
                    close_shared_data( data );
                    break;
                }
            }
            
            if ( ret == 0 ) {
                rc = 0;
            }
        }
    }

    LOG_FUNCTION_NAME_EXIT
    
    return ( rc );
}

//=========================================================
// overlay_resizeInput
//

static int overlay_resizeInput(struct overlay_data_device_t *dev/*, overlay_handle_t handle */, uint32_t w, uint32_t h)
{
    int ret = 0;
    int rc;
    uint32_t numb = NUM_OVERLAY_BUFFERS_REQUESTED;
    overlay_data_t eCropData;
    int degree = 0;
    
    // Position and output width and heigh
    int32_t _x = 0;
    int32_t _y = 0;
    int32_t _w = 0;
    int32_t _h = 0;

    struct overlay_data_context_t* ctx =
            (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;

    LOG_FUNCTION_NAME   

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 

    if ((data->width == (int)w) && (data->height == (int)h) && (data->attributes_changed == 0)) {
        LOGE("Same as current width and height. Attributes did not change either. So do nothing.");
        return 0;
    }

    if ( data->shared == NULL ) {
        LOGE("Shared Data Not Init'd!\n");
        return -1;
    }

    if ( data->shared->dataReady ) {
        LOGE("Either setCrop() or queueBuffer() was called prior to this!Therefore failing this call.\n");
        return -1;
    }

    pthread_mutex_lock(&data->shared->lock);
    if ( (rc = disable_streaming(data->shared, data->ctl_fd)) != 0 ) {
        LOGE("Stream Off Failed!/%d\n", rc);
        ret = rc;
    } else {
        LOGI("Getting driver value before streamming off \n");
        if ( (rc = v4l2_overlay_get_crop(data->ctl_fd, &eCropData.cropX, &eCropData.cropY, &eCropData.cropW, &eCropData.cropH)) != 0) {
            LOGE("Get crop value Failed!/%d\n", rc);
            ret = rc;
        } else if( (rc=v4l2_overlay_get_position(data->ctl_fd, &_x,  &_y, &_w, &_h))) {
            LOGE(" Could not set the position when creating overlay \n");
            close(data->ctl_fd);
            ret = rc;
        } else if( (rc=v4l2_overlay_get_rotation(data->ctl_fd, &degree, NULL))) {
            LOGE("Get rotation value failed! \n");
            close(data->ctl_fd);
            ret = rc;
        }
        LOGE("$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$");    

        for (int i = 0; i < data->num_buffers; i++) {
            v4l2_overlay_unmap_buf(data->buffers[i], data->buffers_len[i]);
        }

        if ((rc = v4l2_overlay_init(data->ctl_fd, w, h, data->format)) != 0) {
            LOGE("Error initializing overlay");
            ret = rc;
        }
        
#ifdef OVERLAY_ANGLE_PATCH
        if(data->shared->vtMode == 1) 
            degree = 0;    
        else
            if(data->shared->cameraNum == 1)
                degree = 270;
#endif
      
        if ( (rc = v4l2_overlay_set_rotation(data->ctl_fd, degree, 0)) != 0 ) {
            LOGE("Failed rotation\n");
            close( data->ctl_fd);
            ret = rc;
        } else if ( (rc = v4l2_overlay_set_crop(data->ctl_fd, eCropData.cropX, eCropData.cropY, eCropData.cropW, eCropData.cropH)) != 0 ) {
            LOGE("Failed crop window\n");
            close( data->ctl_fd);
            ret = rc;
         } else if( (rc=v4l2_overlay_set_position(data->ctl_fd, _x,  _y, _w, _h))) { // TI Patch libOverlay changes for removing colorkey settings from resize
            LOGE(" Could not set the position when creating overlay \n");
            close(data->ctl_fd);
            ret = rc;
        } else if ((rc = v4l2_overlay_req_buf(data->ctl_fd, (uint32_t *)(&data->num_buffers), data->cacheable_buffers, data->maintain_coherency)) != 0) {
            LOGE("Error creating buffers");
            ret = rc;
        } else {
            for (int i = 0; i < data->num_buffers; i++)
                v4l2_overlay_map_buf(data->ctl_fd, i, &data->buffers[i], &data->buffers_len[i]);
      
            /* The control pameters just got set */
            data->shared->controlReady = 1;
            data->attributes_changed = 0; // Reset it
        }
    }
    pthread_mutex_unlock(&data->shared->lock);

    return ( ret );

}



//=========================================================
// overlay_setParameter
//

static int overlay_setParameter
( struct overlay_data_device_t *dev
/*, overlay_handle_t handle */
, int param
, int value
) 
{

    int ret = 0;
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 
    
    if ( data->shared == NULL ) {
        LOGE("Shared Data Not Init'd!\n");
        return -1;
    }
    
    if ( data->shared->dataReady ) {
        LOGE("Too late. Cant set it now!\n");
        return -1;
    }

    switch(param) {
    case CACHEABLE_BUFFERS:
        data->cacheable_buffers = value;
        data->attributes_changed = 1;
        break;
    case MAINTAIN_COHERENCY:
        data->maintain_coherency = value;
        data->attributes_changed = 1;
        break;
    case MIRRORING:
        ret = v4l2_overlay_set_mirroring(data->ctl_fd, value, 0); //selwin added.
        break;
    case WATERING:
        ret = v4l2_overlay_set_watering(data->ctl_fd, value, 0);
        break;   
    case ROTATION:
        ret = v4l2_overlay_set_rotation(data->ctl_fd, value, 0);
        break;       
        
    }

    LOG_FUNCTION_NAME_EXIT

    return ( ret );
}



//=========================================================
// overlay_setCrop
//

static int overlay_setCrop
( struct overlay_data_device_t *dev
/*, overlay_handle_t handle */
, uint32_t left
, uint32_t top
, uint32_t right
, uint32_t bottom
) 
{
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *d;

    int ret = -1;
    int rc;

    uint32_t x, y, w, h;

    LOG_FUNCTION_NAME
/*
    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        d = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        d = &ctx->overlay_video1;
        break;
    }
*/ 
   d = &ctx->overlay_video1;   
    x = left;
    y = top;
    w = right - left;
    h = bottom - top;

    if ( d->shared == NULL ) {
        LOGE("Shared Data Not Init'd!\n");
    } else {
        pthread_mutex_lock(&d->shared->lock);
        d->shared->dataReady = 1;
            
        if (  d->data.cropX == x 
           && d->data.cropY == y
           && d->data.cropW == w
           && d->data.cropH == h ) {
            LOGE("Nothing to do!\n");
            pthread_mutex_unlock(&d->shared->lock);
            return 0;
        }
            
        d->data.cropX = x;
        d->data.cropY = y;
        d->data.cropW = w;
        d->data.cropH = h;

        LOGV("Crop Win/X%d/Y%d/W%d/H%d\n", x, y, w, h );

        if ( (rc = disable_streaming(d->shared, d->ctl_fd)) != 0 ) {
            LOGE("Stream Off Failed!/%d\n", rc);
            ret = rc;
        } else {
            if ( (rc = v4l2_overlay_set_crop(d->ctl_fd, x, y, w, h)) != 0 ) {
                LOGE("Set Crop Window Failed!/%d\n", rc);
                ret = rc;
            }
        }
        pthread_mutex_unlock(&d->shared->lock);
    }

    LOG_FUNCTION_NAME_EXIT

    return ( ret );
}


//=========================================================
// overlay_dequeueBuffer
//

int overlay_dequeueBuffer
( struct overlay_data_device_t *dev
/*, overlay_handle_t handle */
, overlay_buffer_t *buffer
)
{
    /* blocks until a buffer is available and return an opaque structure
     * representing this buffer.
     */
     
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;

    int rc = 0;
    int i = -1;

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 

    // We need to avoid the case where are queuing in the middle of a streamoff event
    if ( !data->shared->controlReady ) 
        return -1;

    pthread_mutex_lock(&data->shared->lock);
    if (data->shared->qd_buf_count < NUM_BUFFERS_TO_BE_QUEUED_FOR_OPTIMAL_PERFORMANCE - 1) {
        LOGE("Not enough buffers to dequeue");
        rc = -EPERM;    
    } else if ( (rc = v4l2_overlay_dq_buf( data->ctl_fd, &i )) != 0 ) {
        LOGE("Failed to DQ/%d\n", rc);    
    } else if ( i < 0 || i > data->num_buffers ) {
        LOGE("dqbuffer i=%d",i);
        rc = -EINVAL;
    } else {
        *((int *)buffer) = i;
        data->shared->qd_buf_count --;
    }
    pthread_mutex_unlock(&data->shared->lock);

    return ( rc );
}

//=========================================================
// overlay_queueBuffer
//

int overlay_queueBuffer
( struct overlay_data_device_t *dev
/*, overlay_handle_t handle */
, overlay_buffer_t buffer
)
{
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 

    // We need to avoid the case where are queuing in the middle of a streamoff event
    if ( !data->shared->controlReady ) 
        return -1;

    pthread_mutex_lock(&data->shared->lock);
    int rc = v4l2_overlay_q_buf( data->ctl_fd, (int)buffer );   
    if ( rc == 0 && data->shared->qd_buf_count < data->num_buffers ) {
        data->shared->qd_buf_count ++;
    }
    pthread_mutex_unlock(&data->shared->lock);    

    // Catch the case where the data side had no need to set the crop window
    if ((data->shared->qd_buf_count >= NUM_BUFFERS_TO_BE_QUEUED_FOR_OPTIMAL_PERFORMANCE) 
        && (data->shared->streamEn == 0)) {
        data->shared->dataReady = 1;
        enable_streaming( data->shared, data->ctl_fd);
    }
    
    return ( rc );
}

//=========================================================
// overlay_getBufferAddress
//

void *overlay_getBufferAddress
( struct overlay_data_device_t *dev
/*, overlay_handle_t handle */
, overlay_buffer_t buffer
)
{       
    /* this may fail (NULL) if this feature is not supported. In that case,
     * presumably, there is some other HAL module that can fill the buffer,
     * using a DSP for instance
     */
    struct v4l2_buffer buf;
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;
    
    void *p = NULL;

    int ret;

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 
    
    ret = v4l2_overlay_query_buffer(data->ctl_fd, (int)buffer, &buf);
    LOGV ("v4l2_overlay_query_buffer ret = %d\n", ret);
    if (ret) {
        data->mapping_data->nQueuedToOverlay = -1;
        return NULL;
    }
    
    LOGV ("Before MEMSET\n");
    memset(data->mapping_data, 0, sizeof(mapping_data_t));

    data->mapping_data->fd = data->ctl_fd;
    data->mapping_data->length = buf.length;
    data->mapping_data->offset = buf.m.offset;
    data->mapping_data->ptr = NULL;

    if((buf.flags & V4L2_BUF_FLAG_QUEUED) || (buf.flags & V4L2_BUF_FLAG_DONE))
        data->mapping_data->nQueuedToOverlay = 1;
    else
        data->mapping_data->nQueuedToOverlay = 0;

    LOGV ("Buffer[%d] data_buff[%d] fd=%d addr=%08lx len=%d Stats=%d", (int)buffer, data->num_buffers , data->mapping_data->fd,
                (unsigned long)data->mapping_data->ptr, data->buffers_len[(int)buffer],data->mapping_data->nQueuedToOverlay);

    if ( (int)buffer >= 0 && (int)buffer < data->num_buffers ) {
        data->mapping_data->ptr = data->buffers[(int)buffer];    
        LOGV("Buffer[%d] fd=%d addr=%08lx len=%d Stats=%d", (int)buffer, data->mapping_data->fd,
                (unsigned long)data->mapping_data->ptr, data->buffers_len[(int)buffer],data->mapping_data->nQueuedToOverlay);
    }

    return ((void *)data->mapping_data );
}

//=========================================================
// overlay_getBufferCount
//

int overlay_getBufferCount(struct overlay_data_device_t *dev/*, overlay_handle_t handle */)
{
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;
    struct overlay_d_t *data;

    /*    switch(handle_device_id(handle)) {
    case V4L2_OVERLAY_PLANE_VIDEO2:
        data = &ctx->overlay_video2;
        break;
    case V4L2_OVERLAY_PLANE_VIDEO1:
    default:
        data = &ctx->overlay_video1;
        break;
    }  */   
      data = &ctx->overlay_video1;
 

    return ( data->num_buffers );
}

//=========================================================
// overlay_data_close
//

static int overlay_data_close( struct hw_device_t *dev )
{
    struct overlay_data_context_t* ctx = (struct overlay_data_context_t*)dev;   
    struct overlay_d_t *data1 = &ctx->overlay_video1;
    struct overlay_d_t *data2 = &ctx->overlay_video2;

    int rc;

    LOG_FUNCTION_NAME
        
    if (ctx) {
        overlay_data_device_t *overlay_dev = &ctx->device;
        if(data1) {
            overlay_buffer_t buf;
            int i;

            if(data1->shared != NULL) {
                LOGV("Queued Buffer Count is %d", data1->shared->qd_buf_count);
                if ( (rc = disable_streaming(data1->shared, data1->ctl_fd)) != 0 ) {
                    LOGE("Stream Off Failed!/%d\n", rc);
                }

                for ( i = 0; i < data1->num_buffers; i++ ) {
                    LOGV("Unmap Buffer/%d/%08lx/%d", i, (unsigned long)data1->buffers[i], data1->buffers_len[i] );
                    rc = v4l2_overlay_unmap_buf(data1->buffers[i], data1->buffers_len[i]);
                    if ( rc != 0 ) {
                        LOGE("Error unmapping the buffer/%d/%d", i, rc);
                    }
                }

                delete(data1->mapping_data);
                delete(data1->buffers);
                delete(data1->buffers_len);
                data1->shared->dataReady = 0;
                close_shared_data( data1 );
            }
        }
        
        if(data2) {
            overlay_buffer_t buf;
            int i;

            if(data2->shared != NULL) {
                LOGV("Queued Buffer Count is %d", data2->shared->qd_buf_count);
                if ( (rc = disable_streaming(data2->shared, data2->ctl_fd)) != 0 ) {
                    LOGE("Stream Off Failed!/%d\n", rc);
                }

                for ( i = 0; i < data2->num_buffers; i++ ) {
                    LOGV("Unmap Buffer/%d/%08lx/%d", i, (unsigned long)data2->buffers[i], data2->buffers_len[i] );
                    rc = v4l2_overlay_unmap_buf(data2->buffers[i], data2->buffers_len[i]);
                    if ( rc != 0 ) {
                        LOGE("Error unmapping the buffer/%d/%d", i, rc);
                    }
                }

                delete(data2->mapping_data);
                delete(data2->buffers);
                delete(data2->buffers_len);
                data2->shared->dataReady = 0;
                close_shared_data( data2 );
            }
        }
        free( ctx );
    }

    LOG_FUNCTION_NAME_EXIT
        
    return ( 0 );
}

/*****************************************************************************/

static int overlay_device_open(const struct hw_module_t* module,
                               const char* name, struct hw_device_t** device)
{
    int status = -EINVAL;

    LOG_FUNCTION_NAME
    
    if (!strcmp(name, OVERLAY_HARDWARE_CONTROL)) {
        struct overlay_control_context_t *dev;
        dev = (overlay_control_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = overlay_control_close;
        
        dev->device.get = overlay_get;
        dev->device.createOverlay = overlay_createOverlay;
        dev->device.destroyOverlay = overlay_destroyOverlay;
        dev->device.setPosition = overlay_setPosition;
        dev->device.getPosition = overlay_getPosition;
        dev->device.setParameter = overlay_setParameter;
        dev->device.commit = overlay_commitUpdates;

        *device = &dev->device.common;
        status = 0;
    } else if (!strcmp(name, OVERLAY_HARDWARE_DATA)) {
        struct overlay_data_context_t *dev;
        dev = (overlay_data_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = overlay_data_close;
        
        dev->device.initialize = overlay_initialize;
        dev->device.resizeInput = overlay_resizeInput;
        dev->device.setCrop = overlay_setCrop;
        dev->device.setParameter = overlay_setParameter;
        dev->device.dequeueBuffer = overlay_dequeueBuffer;
        dev->device.queueBuffer = overlay_queueBuffer;
        dev->device.getBufferAddress = overlay_getBufferAddress;
        dev->device.getBufferCount = overlay_getBufferCount;
        
        *device = &dev->device.common;
        status = 0;
    }

    LOG_FUNCTION_NAME_EXIT
        
    return status;
}
