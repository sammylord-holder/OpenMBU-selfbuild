//-----------------------------------------------------------------------------
// Torque Shader Engine
// Copyright (C) GarageGames.com, Inc.
//-----------------------------------------------------------------------------

#ifndef INITGUID
#define INITGUID
#endif

#include "math/mMath.h"
#include "platformWin32/winDInputDevice.h"
#include "console/console.h"
#include "platform/gameInterface.h"
#include "core/unicode.h"

// Static class data:
LPDIRECTINPUT8 DInputDevice::smDInputInterface;
U8    DInputDevice::smKeyboardCount;
U8    DInputDevice::smMouseCount;
U8    DInputDevice::smJoystickCount;
U8    DInputDevice::smUnknownCount;
U8    DInputDevice::smModifierKeys;
bool  DInputDevice::smKeyStates[256];
bool  DInputDevice::smInitialized = false;

#ifdef LOG_INPUT
const char* getKeyName(U16 key);
#endif

//------------------------------------------------------------------------------
DInputDevice::DInputDevice(const DIDEVICEINSTANCE* dii)
{
    mDeviceInstance = *dii;
    mDevice = NULL;
    mAcquired = false;
    mNeedSync = false;
    mObjInstance = NULL;
    mObjFormat = NULL;
    mObjInfo = NULL;
    mObjBuffer1 = NULL;
    mObjBuffer2 = NULL;
    mPrevObjBuffer = NULL;

    //- Force Feedback ----------- jason_cahill -
    mForceFeedbackEffect = NULL;
    mNumForceFeedbackAxes = 0;
    mForceFeedbackAxes[0] = 0;
    mForceFeedbackAxes[1] = 0;

    switch (GET_DIDEVICE_TYPE(mDeviceInstance.dwDevType))
    {
    case DI8DEVTYPE_KEYBOARD:
        mDeviceType = KeyboardDeviceType;
        mDeviceID = smKeyboardCount++;
        dSprintf(mName, 29, "keyboard%d", mDeviceID);
        break;

    case DI8DEVTYPE_MOUSE:
        mDeviceType = MouseDeviceType;
        mDeviceID = smMouseCount++;
        dSprintf(mName, 29, "mouse%d", mDeviceID);
        break;

    case DI8DEVTYPE_JOYSTICK:
    case DI8DEVTYPE_GAMEPAD:                              // enables joysticks (not XInput controllers) ----- jason_cahill -
        mDeviceType = JoystickDeviceType;
        mDeviceID = smJoystickCount++;
        dSprintf(mName, 29, "joystick%d", mDeviceID);
        break;

    default:
        mDeviceType = UnknownDeviceType;
        mDeviceID = smUnknownCount++;
        dSprintf(mName, 29, "unknown%d", mDeviceID);
        break;
    }
}

//------------------------------------------------------------------------------
DInputDevice::~DInputDevice()
{
    destroy();
}

//------------------------------------------------------------------------------
void DInputDevice::init()
{
    if (!smInitialized)
    {
        dMemset(smKeyStates, 0, sizeof(smKeyStates));
        smInitialized = true;
    }

    // Reset all of the static variables:
    smDInputInterface = NULL;
    smKeyboardCount = 0;
    smMouseCount = 0;
    smJoystickCount = 0;
    smUnknownCount = 0;
    smModifierKeys = 0;
}

//------------------------------------------------------------------------------
void DInputDevice::resetModifierKeys()
{
    smModifierKeys = 0;
    setModifierKeys(0);
}

//------------------------------------------------------------------------------
bool DInputDevice::create()
{
    HRESULT result;

    if (smDInputInterface)
    {
        result = smDInputInterface->CreateDevice(mDeviceInstance.guidInstance, &mDevice, NULL);
        if (result == DI_OK)
        {
            mDeviceCaps.dwSize = sizeof(DIDEVCAPS);
            if (FAILED(mDevice->GetCapabilities(&mDeviceCaps)))
            {
                Con::errorf("  Failed to get the capabilities of the %s input device.", mName);
#ifdef LOG_INPUT
                Input::log("Failed to get the capabilities of &s!\n", mName);
#endif
                return false;
            }

#ifdef LOG_INPUT
            Input::log("%s detected, created as %s (%s).\n", mDeviceInstance.tszProductName, mName, (isPolled() ? "polled" : "asynchronous"));
#endif

            if (enumerateObjects())
            {
                // Set the device's data format:
                DIDATAFORMAT dataFormat;
                dMemset(&dataFormat, 0, sizeof(DIDATAFORMAT));
                dataFormat.dwSize = sizeof(DIDATAFORMAT);
                dataFormat.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
                dataFormat.dwFlags = (mDeviceType == MouseDeviceType) ? DIDF_RELAXIS : DIDF_ABSAXIS;
                dataFormat.dwDataSize = mObjBufferSize;
                dataFormat.dwNumObjs = mObjCount;
                dataFormat.rgodf = mObjFormat;

                result = mDevice->SetDataFormat(&dataFormat);
                if (FAILED(result))
                {
                    Con::errorf("  Failed to set the data format for the %s input device.", mName);
#ifdef LOG_INPUT
                    Input::log("Failed to set the data format for %s!\n", mName);
#endif
                    return false;
                }

                // Set up the data buffer for buffered input:
                DIPROPDWORD prop;
                prop.diph.dwSize = sizeof(DIPROPDWORD);
                prop.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                prop.diph.dwObj = 0;
                prop.diph.dwHow = DIPH_DEVICE;
                if (isPolled())
                    prop.dwData = mObjBufferSize;
                else
                    prop.dwData = QUEUED_BUFFER_SIZE;

                result = mDevice->SetProperty(DIPROP_BUFFERSIZE, &prop.diph);
                if (FAILED(result))
                {
                    Con::errorf("  Failed to set the buffer size property for the %s input device.", mName);
#ifdef LOG_INPUT
                    Input::log("Failed to set the buffer size property for %s!\n", mName);
#endif
                    return false;
                }

                // If this device is a mouse, set it to relative axis mode:
                if (mDeviceType == MouseDeviceType)
                {
                    prop.diph.dwObj = 0;
                    prop.diph.dwHow = DIPH_DEVICE;
                    prop.dwData = DIPROPAXISMODE_REL;

                    result = mDevice->SetProperty(DIPROP_AXISMODE, &prop.diph);
                    if (FAILED(result))
                    {
                        Con::errorf("  Failed to set relative axis mode for the %s input device.", mName);
#ifdef LOG_INPUT
                        Input::log("Failed to set relative axis mode for %s!\n", mName);
#endif
                        return false;
                    }
                }
            }
        }
        else
        {
#ifdef LOG_INPUT
            switch (result)
            {
            case DIERR_DEVICENOTREG:
                Input::log("CreateDevice failed -- The device or device instance is not registered with DirectInput.\n");
                break;

            case DIERR_INVALIDPARAM:
                Input::log("CreateDevice failed -- Invalid parameter.\n");
                break;

            case DIERR_NOINTERFACE:
                Input::log("CreateDevice failed -- The specified interface is not supported by the object.\n");
                break;

            case DIERR_OUTOFMEMORY:
                Input::log("CreateDevice failed -- Out of memory.\n");
                break;

            default:
                Input::log("CreateDevice failed -- Unknown error.\n");
                break;
            };
#endif // LOG_INPUT
            Con::printf("  CreateDevice failed for the %s input device!", mName);
            return false;
        }
    }

    Con::printf("   %s input device created.", mName);
    return true;
}

//------------------------------------------------------------------------------
void DInputDevice::destroy()
{
    if (mDevice)
    {
        unacquire();

        // FORCE FEEDBACK - Clean up - jason_cahill -
        if (mForceFeedbackEffect)
        {
            mForceFeedbackEffect->Release();
            mForceFeedbackEffect = NULL;
            mNumForceFeedbackAxes = 0;
#ifdef LOG_INPUT
            Input::log("DInputDevice::destroy - releasing constant force feeback effect\n");
#endif
        }

        mDevice->Release();
        mDevice = NULL;

        delete[] mObjInstance;
        delete[] mObjFormat;
        delete[] mObjInfo;
        delete[] mObjBuffer1;
        delete[] mObjBuffer2;

        mObjInstance = NULL;
        mObjFormat = NULL;
        mObjInfo = NULL;
        mObjBuffer1 = NULL;
        mObjBuffer2 = NULL;
        mPrevObjBuffer = NULL;
        mName[0] = 0;
    }
}

//------------------------------------------------------------------------------
bool DInputDevice::acquire()
{
    if (mDevice)
    {
        if (mAcquired)
            return(true);

        bool result = false;
        // Set the cooperative level:
        // (do this here so that we have a valid app window)
        DWORD coopLevel = DISCL_BACKGROUND;
        if (mDeviceType == JoystickDeviceType
            // #ifndef DEBUG
            //         || mDeviceType == MouseDeviceType
            // #endif
            )
            // Exclusive access is required in order to perform force feedback -- jason_cahill -
            coopLevel = DISCL_EXCLUSIVE | DISCL_FOREGROUND;
        else
            coopLevel |= DISCL_NONEXCLUSIVE;

        result = mDevice->SetCooperativeLevel(winState.appWindow, coopLevel);
        if (FAILED(result))
        {
            Con::errorf("Failed to set the cooperative level for the %s input device.", mName);
#ifdef LOG_INPUT
            Input::log("Failed to set the cooperative level for %s!\n", mName);
#endif
            return false;
        }

        // Enumerate joystick axes to enable force feedback -- jason_cahill -
        if (NULL == mForceFeedbackEffect && JoystickDeviceType == mDeviceType)
        {
            // Since we will be playing force feedback effects, we should disable the auto-centering spring.
            DIPROPDWORD dipdw;
            dipdw.diph.dwSize = sizeof(DIPROPDWORD);
            dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dipdw.diph.dwObj = 0;
            dipdw.diph.dwHow = DIPH_DEVICE;
            dipdw.dwData = FALSE;

            if (FAILED(result = mDevice->SetProperty(DIPROP_AUTOCENTER, &dipdw.diph)))
                return false;
        }

        S32 code = mDevice->Acquire();
        result = SUCCEEDED(code);
        if (result)
        {
            Con::printf("%s input device acquired.", mName);
#ifdef LOG_INPUT
            Input::log("%s acquired.\n", mName);
#endif
            mAcquired = true;

            // FORCE FEEDBACK - jason_cahill
            // If we were previously playing a force feedback effect, before losing acquisition, we do not
            // automatically restart it. This is where you could call mForceFeedbackEffect->Start( INFINITE, 0 );
            // if you want that behavior.

            // Update all of the key states:
            if (!isPolled())
                mNeedSync = true;
        }
        else
        {
            const char* reason;
            switch (code)
            {
            case DIERR_INVALIDPARAM:      reason = "Invalid parameter"; break;
            case DIERR_NOTINITIALIZED:    reason = "Not initialized"; break;
            case DIERR_OTHERAPPHASPRIO:   reason = "Other app has priority"; break;
            case S_FALSE:                 reason = "Already acquired"; break;
            default:                      reason = "Unknown error"; break;
            }
            Con::warnf("%s input device NOT acquired: %s", mName, reason);
#ifdef LOG_INPUT
            Input::log("Failed to acquire %s: %s\n", mName, reason);
#endif
        }

        return(result);
    }

    return(false);
}

//------------------------------------------------------------------------------
bool DInputDevice::unacquire()
{
    if (mDevice)
    {
        if (!mAcquired)
            return(true);

        bool result = false;
        result = SUCCEEDED(mDevice->Unacquire());
        if (result)
        {
            Con::printf("%s input device unacquired.", mName);
#ifdef LOG_INPUT
            Input::log("%s unacquired.\n", mName);
#endif
            mAcquired = false;
        }
        else
        {
            Con::warnf(ConsoleLogEntry::General, "%s input device NOT unacquired.", mName);
#ifdef LOG_INPUT
            Input::log("Failed to unacquire %s!\n", mName);
#endif
        }

        return(result);
    }

    return(false);
}

//------------------------------------------------------------------------------
BOOL CALLBACK DInputDevice::EnumObjectsProc(const DIDEVICEOBJECTINSTANCE* doi, LPVOID pvRef)
{
    // Don't enumerate unknown types:
    if (doi->guidType == GUID_Unknown)
        return (DIENUM_CONTINUE);

    // Reduce a couple pointers:
    DInputDevice* diDevice = (DInputDevice*)pvRef;
    DIDEVICEOBJECTINSTANCE* objInstance = &diDevice->mObjInstance[diDevice->mObjEnumCount];
    DIOBJECTDATAFORMAT* objFormat = &diDevice->mObjFormat[diDevice->mObjEnumCount];

    // Fill in the object instance structure:
    *objInstance = *doi;

    // DWORD objects must be DWORD aligned:
    if (!(objInstance->dwType & DIDFT_BUTTON))
        diDevice->mObjBufferOfs = (diDevice->mObjBufferOfs + 3) & ~3;

    objInstance->dwOfs = diDevice->mObjBufferOfs;

    // Fill in the object data format structure:
    objFormat->pguid = &objInstance->guidType;
    objFormat->dwType = objInstance->dwType;
    objFormat->dwFlags = 0;
    objFormat->dwOfs = diDevice->mObjBufferOfs;

    // Advance the enumeration counters:
    if (objFormat->dwType & DIDFT_BUTTON)
        diDevice->mObjBufferOfs += SIZEOF_BUTTON;
    else
        diDevice->mObjBufferOfs += SIZEOF_AXIS;
    diDevice->mObjEnumCount++;

    return (DIENUM_CONTINUE);
}

//------------------------------------------------------------------------------
bool DInputDevice::enumerateObjects()
{
    if (!mDevice)
        return false;

    // Calculate the needed buffer sizes and allocate them:
    mObjCount = (mDeviceCaps.dwAxes + mDeviceCaps.dwButtons + mDeviceCaps.dwPOVs);
    mObjBufferSize = mObjCount * sizeof(DWORD);

    mObjInstance = new DIDEVICEOBJECTINSTANCE[mObjCount];
    mObjFormat = new DIOBJECTDATAFORMAT[mObjCount];
    mObjInfo = new ObjInfo[mObjCount];

    if (isPolled())
    {
        mObjBuffer1 = new U8[mObjBufferSize];
        dMemset(mObjBuffer1, 0, mObjBufferSize);
        mObjBuffer2 = new U8[mObjBufferSize];
        dMemset(mObjBuffer2, 0, mObjBufferSize);
    }
    mObjEnumCount = 0;
    mObjBufferOfs = 0;

    // FORCE FEEDBACK - We are about to enumerate, clear the axes we claim to know about - jason_cahill
    mNumForceFeedbackAxes = 0;

    // Enumerate all of the 'objects' detected on the device:
    if (FAILED(mDevice->EnumObjects(EnumObjectsProc, this, DIDFT_ALL)))
        return false;

    // FORCE FEEDBACK - We only supports one or two axis joysticks - jason_cahill
    if (mNumForceFeedbackAxes > 2)
        mNumForceFeedbackAxes = 2;

    // if we enumerated fewer objects that are supposedly available, reset the
    // object count
    if (mObjEnumCount < mObjCount)
        mObjCount = mObjEnumCount;

    mObjBufferSize = (mObjBufferSize + 3) & ~3;   // Fill in the actual size to nearest DWORD

    U32 buttonCount = 0;
    U32 povCount = 0;
    U32 xAxisCount = 0;
    U32 yAxisCount = 0;
    U32 zAxisCount = 0;
    U32 rAxisCount = 0;
    U32 uAxisCount = 0;
    U32 vAxisCount = 0;
    U32 sliderCount = 0;
    U32 keyCount = 0;
    U32 unknownCount = 0;

    // Fill in the device object's info structure:
    for (U32 i = 0; i < mObjCount; i++)
    {
        if (mObjInstance[i].guidType == GUID_Button)
        {
            mObjInfo[i].mType = SI_BUTTON;
            mObjInfo[i].mInst = KEY_BUTTON0 + buttonCount++;
        }
        else if (mObjInstance[i].guidType == GUID_POV)
        {
            mObjInfo[i].mType = SI_POV;
            mObjInfo[i].mInst = povCount++;
        }
        else if (mObjInstance[i].guidType == GUID_XAxis)
        {
            mObjInfo[i].mType = SI_XAXIS;
            mObjInfo[i].mInst = xAxisCount++;

            // FORCE FEEDBACK --------- jason_cahill -
            if (mObjInstance[i].dwFFMaxForce > 0)
                mForceFeedbackAxes[mNumForceFeedbackAxes++] = mObjInstance[i].dwOfs;
        }
        else if (mObjInstance[i].guidType == GUID_YAxis)
        {
            mObjInfo[i].mType = SI_YAXIS;
            mObjInfo[i].mInst = yAxisCount++;

            // FORCE FEEDBACK --------- jason_cahill -
            if (mObjInstance[i].dwFFMaxForce > 0)
                mForceFeedbackAxes[mNumForceFeedbackAxes++] = mObjInstance[i].dwOfs;
        }
        else if (mObjInstance[i].guidType == GUID_ZAxis)
        {
            mObjInfo[i].mType = SI_ZAXIS;
            mObjInfo[i].mInst = zAxisCount++;
        }
        else if (mObjInstance[i].guidType == GUID_RxAxis)
        {
            mObjInfo[i].mType = SI_RXAXIS;
            mObjInfo[i].mInst = rAxisCount++;
        }
        else if (mObjInstance[i].guidType == GUID_RyAxis)
        {
            mObjInfo[i].mType = SI_RYAXIS;
            mObjInfo[i].mInst = uAxisCount++;
        }
        else if (mObjInstance[i].guidType == GUID_RzAxis)
        {
            mObjInfo[i].mType = SI_RZAXIS;
            mObjInfo[i].mInst = vAxisCount++;
        }
        else if (mObjInstance[i].guidType == GUID_Slider)
        {
            mObjInfo[i].mType = SI_SLIDER;
            mObjInfo[i].mInst = sliderCount++;
        }
        else if (mObjInstance[i].guidType == GUID_Key)
        {
            mObjInfo[i].mType = SI_KEY;
            mObjInfo[i].mInst = DIK_to_Key(DIDFT_GETINSTANCE(mObjFormat[i].dwType));
            keyCount++;
        }
        else
        {
            mObjInfo[i].mType = SI_UNKNOWN;
            mObjInfo[i].mInst = unknownCount++;
        }

        // Set the device object's min and max values:
        if (mObjInstance[i].guidType == GUID_Button
            || mObjInstance[i].guidType == GUID_Key
            || mObjInstance[i].guidType == GUID_POV)
        {
            mObjInfo[i].mMin = DIPROPRANGE_NOMIN;
            mObjInfo[i].mMax = DIPROPRANGE_NOMAX;
        }
        else
        {
            // This is an axis or a slider, so find out its range:
            DIPROPRANGE pr;
            pr.diph.dwSize = sizeof(pr);
            pr.diph.dwHeaderSize = sizeof(pr.diph);
            pr.diph.dwHow = DIPH_BYID;
            pr.diph.dwObj = mObjFormat[i].dwType;

            if (SUCCEEDED(mDevice->GetProperty(DIPROP_RANGE, &pr.diph)))
            {
                mObjInfo[i].mMin = pr.lMin;
                mObjInfo[i].mMax = pr.lMax;
            }
            else
            {
                mObjInfo[i].mMin = DIPROPRANGE_NOMIN;
                mObjInfo[i].mMax = DIPROPRANGE_NOMAX;
            }
        }
    }

#ifdef LOG_INPUT
    Input::log("  %d total objects detected.\n", mObjCount);
    if (buttonCount)
        Input::log("  %d buttons.\n", buttonCount);
    if (povCount)
        Input::log("  %d POVs.\n", povCount);
    if (xAxisCount)
        Input::log("  %d x-axis.\n", xAxisCount);
    if (yAxisCount)
        Input::log("  %d y-axis.\n", yAxisCount);
    if (zAxisCount)
        Input::log("  %d z-axis.\n", zAxisCount);
    if (rAxisCount)
        Input::log("  %d r-axis.\n", rAxisCount);
    if (uAxisCount)
        Input::log("  %d u-axis.\n", uAxisCount);
    if (vAxisCount)
        Input::log("  %d v-axis.\n", vAxisCount);
    if (sliderCount)
        Input::log("  %d sliders.\n", sliderCount);
    if (keyCount)
        Input::log("  %d keys.\n", keyCount);
    if (unknownCount)
        Input::log("  %d unknown objects.\n", unknownCount);
    Input::log("\n");
#endif

    return true;
}

//------------------------------------------------------------------------------
const char* DInputDevice::getName()
{
#ifdef UNICODE
    static UTF8 buf[512];
    convertUTF16toUTF8(mDeviceInstance.tszInstanceName, buf, sizeof(buf));
    return (const char*)buf;
#else
    return mDeviceInstance.tszInstanceName;
#endif
}

//------------------------------------------------------------------------------
const char* DInputDevice::getProductName()
{
#ifdef UNICODE
    static UTF8 buf[512];
    convertUTF16toUTF8(mDeviceInstance.tszProductName, buf, sizeof(buf));
    return (const char*)buf;
#else
    return mDeviceInstance.tszProductName;
#endif
}

//------------------------------------------------------------------------------
bool DInputDevice::process()
{
    if (mAcquired)
    {
        if (isPolled())
            return processImmediate();
        else
            return processAsync();
    }

    return false;
}

//------------------------------------------------------------------------------
bool DInputDevice::processAsync()
{
    DIDEVICEOBJECTDATA eventBuffer[QUEUED_BUFFER_SIZE];
    DWORD numEvents = QUEUED_BUFFER_SIZE;
    HRESULT result;

    if (!mDevice)
        return false;

    // Test for the "need sync" flag:
    if (mNeedSync)
    {
        // For now, only sync the keyboard:
        if (mDeviceType == KeyboardDeviceType)
            syncKeyboardState();

        mNeedSync = false;
    }

    do
    {
        result = mDevice->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), eventBuffer, &numEvents, 0);

        if (!SUCCEEDED(result))
        {
            switch (result)
            {
            case DIERR_INPUTLOST:
                // Data stream was interrupted, so try to reacquire the device:
                mAcquired = false;
                acquire();
                break;

            case DIERR_INVALIDPARAM:
                Con::errorf("DInputDevice::processAsync -- Invalid parameter passed to GetDeviceData of the %s input device!", mName);
#ifdef LOG_INPUT
                Input::log("Invalid parameter passed to GetDeviceData for %s!\n", mName);
#endif
                break;

            case DIERR_NOTACQUIRED:
                // We can't get the device, so quit:
                mAcquired = false;
                // Don't error out - this is actually a natural occurrence...
                //Con::errorf( "DInputDevice::processAsync -- GetDeviceData called when %s input device is not acquired!", mName );
#ifdef LOG_INPUT
                Input::log("GetDeviceData called when %s is not acquired!\n", mName);
#endif
                break;
            }

            return false;
        }

        // We have buffered input, so act on it:
        for (DWORD i = 0; i < numEvents; i++)
            buildEvent(findObjInstance(eventBuffer[i].dwOfs), eventBuffer[i].dwData, eventBuffer[i].dwData);

        // Check for buffer overflow:
        if (result == DI_BUFFEROVERFLOW)
        {
            // This is a problem, but we can keep going...
            Con::errorf("DInputDevice::processAsync -- %s input device's event buffer overflowed!", mName);
#ifdef LOG_INPUT
            Input::log("%s event buffer overflowed!\n", mName);
#endif
            mNeedSync = true; // Let it know to resync next time through...
        }
    } while (numEvents);

    return true;
}

//------------------------------------------------------------------------------
bool DInputDevice::processImmediate()
{
    if (!mDevice)
        return false;

    mDevice->Poll();

    U8* buffer = (mPrevObjBuffer == mObjBuffer1) ? mObjBuffer2 : mObjBuffer1;
    HRESULT result = mDevice->GetDeviceState(mObjBufferSize, buffer);
    if (!SUCCEEDED(result))
    {
        switch (result)
        {
        case DIERR_INPUTLOST:
            // Data stream was interrupted, so try to reacquire the device:
            mAcquired = false;
            acquire();
            break;

        case DIERR_INVALIDPARAM:
            Con::errorf("DInputDevice::processPolled -- invalid parameter passed to GetDeviceState on %s input device!", mName);
#ifdef LOG_INPUT
            Input::log("Invalid parameter passed to GetDeviceState on %s.\n", mName);
#endif
            break;

        case DIERR_NOTACQUIRED:
            Con::errorf("DInputDevice::processPolled -- GetDeviceState called when %s input device is not acquired!", mName);
#ifdef LOG_INPUT
            Input::log("GetDeviceState called when %s is not acquired!\n", mName);
#endif
            break;

        case E_PENDING:
            Con::warnf("DInputDevice::processPolled -- Data not yet available for the %s input device!", mName);
#ifdef LOG_INPUT
            Input::log("Data pending for %s.", mName);
#endif
            break;
        }

        return false;
    }

    // Loop through all of the objects and produce events where
    // the states have changed:
    S32 newData, oldData = 0;                          // oldData = 0 prevents a crashing bug in Torque. There is a case where Torque accessed oldData without it ever being set. -- jason_cahill -
    for (DWORD i = 0; i < mObjCount; i++)
    {
        if (mObjFormat[i].dwType & DIDFT_BUTTON)
        {
            if (mPrevObjBuffer)
            {
                newData = *((U8*)(buffer + mObjFormat[i].dwOfs));
                oldData = *((U8*)(mPrevObjBuffer + mObjFormat[i].dwOfs));
                if (newData == oldData)
                    continue;
            }
            else
                continue;
        }
        else if (mObjFormat[i].dwType & DIDFT_POV)
        {
            if (mPrevObjBuffer)
            {
                newData = *((S32*)(buffer + mObjFormat[i].dwOfs));
                oldData = *((S32*)(mPrevObjBuffer + mObjFormat[i].dwOfs));
                if (LOWORD(newData) == LOWORD(oldData))
                    continue;
            }
            else
                continue;
        }
        else
        {
            // report normal axes every time through the loop:
            newData = *((S32*)(buffer + mObjFormat[i].dwOfs));
        }

        // Build an event:
        buildEvent(i, newData, oldData);
    }
    mPrevObjBuffer = buffer;

    return true;
}

//------------------------------------------------------------------------------
bool DInputDevice::processKeyEvent(InputEvent& event)
{
    if (event.deviceType != KeyboardDeviceType || event.objType != SI_KEY)
        return false;

    bool modKey = false;
    U8 DIKeyCode = Key_to_DIK(event.objInst);

    if (event.action == SI_MAKE)
    {
        // Maintain the key structure:
        smKeyStates[DIKeyCode] = true;

        switch (event.objInst)
        {
        case KEY_LSHIFT:
            smModifierKeys |= SI_LSHIFT;
            modKey = true;
            break;

        case KEY_RSHIFT:
            smModifierKeys |= SI_RSHIFT;
            modKey = true;
            break;

        case KEY_LCONTROL:
            smModifierKeys |= SI_LCTRL;
            modKey = true;
            break;

        case KEY_RCONTROL:
            smModifierKeys |= SI_RCTRL;
            modKey = true;
            break;

        case KEY_LALT:
            smModifierKeys |= SI_LALT;
            modKey = true;
            break;

        case KEY_RALT:
            smModifierKeys |= SI_RALT;
            modKey = true;
            break;
        }
    }
    else
    {
        // Maintain the keys structure:
        smKeyStates[DIKeyCode] = false;

        switch (event.objInst)
        {
        case KEY_LSHIFT:
            smModifierKeys &= ~SI_LSHIFT;
            modKey = true;
            break;

        case KEY_RSHIFT:
            smModifierKeys &= ~SI_RSHIFT;
            modKey = true;
            break;

        case KEY_LCONTROL:
            smModifierKeys &= ~SI_LCTRL;
            modKey = true;
            break;

        case KEY_RCONTROL:
            smModifierKeys &= ~SI_RCTRL;
            modKey = true;
            break;

        case KEY_LALT:
            smModifierKeys &= ~SI_LALT;
            modKey = true;
            break;

        case KEY_RALT:
            smModifierKeys &= ~SI_RALT;
            modKey = true;
            break;
        }
    }

    if (modKey)
    {
        setModifierKeys(smModifierKeys);
        event.modifier = 0;
    }
    else
        event.modifier = smModifierKeys;

    // TODO: alter this getAscii call
    KEY_STATE state = STATE_LOWER;
    if (event.modifier & (SI_CTRL | SI_ALT))
    {
        state = STATE_GOOFY;
    }
    //   else if ( event.modifier & SI_SHIFT )
    if (event.modifier & SI_SHIFT)
    {
        state = STATE_UPPER;
    }


    event.ascii = Input::getAscii(event.objInst, state);

    return modKey;
}

//------------------------------------------------------------------------------
void DInputDevice::syncKeyboardState()
{
    AssertFatal(mDeviceType == KeyboardDeviceType, "DInputDevice::syncKeyboardState - device is not a keyboard!");

#ifdef LOG_INPUT
    Input::log("Resynching key states for %s!\n", mName);
#endif

    U8* keyBuffer = new U8[mObjBufferSize];
    dMemset(keyBuffer, 0, sizeof(keyBuffer));
    HRESULT result = mDevice->GetDeviceState(mObjBufferSize, keyBuffer);
    if (SUCCEEDED(result))
    {
        S32 keyState;
        bool keyIsDown, keyWasDown;
        for (DWORD i = 1; i < mObjCount; i++)   // Valid key codes start at 1
        {
            keyState = *((U8*)(keyBuffer + mObjFormat[i].dwOfs));
            keyWasDown = smKeyStates[i];
            keyIsDown = bool(keyState & 0x80);
            if (keyWasDown != keyIsDown)
                buildEvent(i - 1, (keyState & 0x80), (keyWasDown ? 0x80 : 0));
        }

#ifdef LOG_INPUT
        Input::log("Resync done.\n");
#endif
    }
    else
    {
        const char* errorString = NULL;
        switch (result)
        {
        case DIERR_INPUTLOST:
            errorString = "DIERR_INPUTLOST";
            break;

        case DIERR_INVALIDPARAM:
            errorString = "DIERR_INVALIDPARAM";
            break;

        case DIERR_NOTACQUIRED:
            errorString = "DIERR_NOTACQUIRED";
            break;

        case E_PENDING:
            errorString = "E_PENDING";
            break;

        default:
            errorString = "Unknown Error";
        }

#ifdef LOG_INPUT
        Input::log("Resync GetDeviceState on %s failed! %s\n", mName, errorString);
#endif
        Con::errorf("DInputDevice::syncKeyboardState - %s", errorString);
    }

    delete[] keyBuffer;
}

//------------------------------------------------------------------------------
DWORD DInputDevice::findObjInstance(DWORD offset)
{
    DIDEVICEOBJECTINSTANCE* inst = mObjInstance;
    for (U32 i = 0; i < mObjCount; i++, inst++)
    {
        if (inst->dwOfs == offset)
            return i;
    }

    AssertFatal(false, "DInputDevice::findObjInstance -- failed to locate object instance.");
    return 0;
}

//------------------------------------------------------------------------------
bool DInputDevice::buildEvent(DWORD offset, S32 newData, S32 oldData)
{
    DIDEVICEOBJECTINSTANCE& objInstance = mObjInstance[offset];
    ObjInfo& objInfo = mObjInfo[offset];

    if (objInfo.mType == SI_UNKNOWN)
        return false;

    InputEvent newEvent;
    newEvent.deviceType = mDeviceType;
    newEvent.deviceInst = mDeviceID;
    newEvent.objType = objInfo.mType;
    newEvent.objInst = objInfo.mInst;
    newEvent.modifier = smModifierKeys;

    switch (newEvent.objType)
    {
    case SI_XAXIS:
    case SI_YAXIS:
    case SI_ZAXIS:
    case SI_RXAXIS:
    case SI_RYAXIS:
    case SI_RZAXIS:
    case SI_SLIDER:
        newEvent.action = SI_MOVE;
        if (newEvent.deviceType == MouseDeviceType)
        {
            newEvent.fValue = float(newData);

#ifdef LOG_INPUT
#ifdef LOG_MOUSEMOVE
            if (newEvent.objType == SI_XAXIS)
                Input::log("EVENT (DInput): %s move (%.1f, 0.0).\n", mName, newEvent.fValue);
            else if (newEvent.objType == SI_YAXIS)
                Input::log("EVENT (DInput): %s move (0.0, %.1f).\n", mName, newEvent.fValue);
            else
#endif
                if (newEvent.objType == SI_ZAXIS)
                    Input::log("EVENT (DInput): %s wheel move %.1f.\n", mName, newEvent.fValue);
#endif
        }
        else  // Joystick or other device:
        {
            // Scale to the range -1.0 to 1.0:
            if (objInfo.mMin != DIPROPRANGE_NOMIN && objInfo.mMax != DIPROPRANGE_NOMAX)
            {
                float range = float(objInfo.mMax - objInfo.mMin);
                //newEvent.fValue = float( newData - objInfo.mMin ) / range;
                newEvent.fValue = float((2 * newData) - objInfo.mMax - objInfo.mMin) / range;
            }
            else
                newEvent.fValue = newData;

#ifdef LOG_INPUT
            // Keep this commented unless you REALLY want these messages for something--
            // they come once per each iteration of the main game loop.
            //switch ( newEvent.objType )
            //{
               //case SI_XAXIS:
                  //if ( newEvent.fValue < -0.01f || newEvent.fValue > 0.01f )
                     //Input::log( "EVENT (DInput): %s X-axis move %.2f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_YAXIS:
                  //if ( newEvent.fValue < -0.01f || newEvent.fValue > 0.01f )
                     //Input::log( "EVENT (DInput): %s Y-axis move %.2f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_ZAXIS:
                  //Input::log( "EVENT (DInput): %s Z-axis move %.1f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_RXAXIS:
                  //Input::log( "EVENT (DInput): %s R-axis move %.1f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_RYAXIS:
                  //Input::log( "EVENT (DInput): %s U-axis move %.1f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_RZAXIS:
                  //Input::log( "EVENT (DInput): %s V-axis move %.1f.\n", mName, newEvent.fValue );
                  //break;
               //case SI_SLIDER:
                  //Input::log( "EVENT (DInput): %s slider move %.1f.\n", mName, newEvent.fValue );
                  //break;
            //};
#endif
        }

        Game->postEvent(newEvent);
        break;

    case SI_BUTTON:
        newEvent.action = (newData & 0x80) ? SI_MAKE : SI_BREAK;
        newEvent.fValue = (newEvent.action == SI_MAKE) ? 1.0f : 0.0f;

#ifdef LOG_INPUT
        if (newEvent.action == SI_MAKE)
            Input::log("EVENT (DInput): %s button%d pressed. MODS:%c%c%c\n",
                mName, newEvent.objInst - KEY_BUTTON0, (smModifierKeys & SI_SHIFT ? 'S' : '.'), (smModifierKeys & SI_CTRL ? 'C' : '.'), (smModifierKeys & SI_ALT ? 'A' : '.'));
        else
            Input::log("EVENT (DInput): %s button%d released.\n", mName, newEvent.objInst - KEY_BUTTON0);
#endif

        Game->postEvent(newEvent);
        break;

    case SI_KEY:
        newEvent.action = (newData & 0x80) ? SI_MAKE : SI_BREAK;
        newEvent.fValue = (newEvent.action == SI_MAKE) ? 1.0f : 0.0f;
        processKeyEvent(newEvent);

#ifdef LOG_INPUT
        if (newEvent.action == SI_MAKE)
            Input::log("EVENT (DInput): %s key pressed. MODS:%c%c%c\n", getKeyName(newEvent.objInst), (smModifierKeys & SI_SHIFT ? 'S' : '.'), (smModifierKeys & SI_CTRL ? 'C' : '.'), (smModifierKeys & SI_ALT ? 'A' : '.'));
        else
            Input::log("EVENT (DInput): %s key released.\n", getKeyName(newEvent.objInst));
#endif

        Game->postEvent(newEvent);
        break;

    case SI_POV:
        U16 objInst = newEvent.objInst;     // Cache this value. Torque previously overwrote this value in this section causing POV to not work right -- jason_cahill -

        newEvent.action = SI_MOVE;
        if (LOWORD(newData) == 0xffff)
        {
            newEvent.objInst = (objInst == 0) ? SI_XPOV : SI_XPOV2;
            newEvent.fValue = 0.5f;
            newEvent.objType = newEvent.objInst;
            Game->postEvent(newEvent);

            newEvent.objInst = (objInst == 0) ? SI_YPOV : SI_YPOV2;
            newEvent.objType = newEvent.objInst;
            Game->postEvent(newEvent);
        }
        else
        {
            // Map 0-35999 data into 0.0-M_2PI fdata:
            float fdata = float(newData) * (1.0 / 36000.0f) * M_2PI;
            float x, y;
            mSinCos(fdata, x, y);
            x = (x + 1.0f) * 0.5f;
            y = (-y + 1.0f) * 0.5f;
            newEvent.objInst = (objInst == 0) ? SI_XPOV : SI_XPOV2;
            newEvent.fValue = x;
#ifdef LOG_INPUT
            Input::log("EVENT (DInput): POV X-axis %d at %.1f.\n",
                (newEvent.objInst == SI_XPOV) ? 1 : 2, newEvent.fValue);
#endif
            newEvent.objType = newEvent.objInst;
            Game->postEvent(newEvent);

            newEvent.objInst = (objInst == 0) ? SI_YPOV : SI_YPOV2;
            newEvent.fValue = y;
#ifdef LOG_INPUT
            Input::log("EVENT (DInput): POV Y-axis %d at %.1f.\n",
                (newEvent.objInst == SI_YPOV) ? 1 : 2, newEvent.fValue);
#endif
            newEvent.objType = newEvent.objInst;
            Game->postEvent(newEvent);
        }

        // Handle artificial POV up/down/left/right buttons
        newData = LOWORD(newData);
        oldData = LOWORD(oldData);
        newData = (newData == 0xffff) ? 5 : (((newData + 31500) / 9000) - 3) & 0x03;
        oldData = (oldData == 0xffff) ? 5 : (((oldData + 31500) / 9000) - 3) & 0x03;

        if (newData != oldData)
        {
            if (oldData != 5)
            {
                newEvent.action = SI_BREAK;
                newEvent.fValue = 0.0f;
                switch (oldData)
                {
                case 0:
                    newEvent.objInst = (objInst == 0) ? SI_UPOV : SI_UPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Up POV %d released.\n",
                        (newEvent.objInst == SI_UPOV) ? 1 : 2);
#endif
                    break;

                case 1:
                    newEvent.objInst = (objInst == 0) ? SI_RPOV : SI_RPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Right POV %d released.\n",
                        (newEvent.objInst == SI_RPOV) ? 1 : 2);
#endif
                    break;

                case 2:
                    newEvent.objInst = (objInst == 0) ? SI_DPOV : SI_DPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Down POV %d released.\n",
                        (newEvent.objInst == SI_DPOV) ? 1 : 2);
#endif
                    break;

                case 3:
                    newEvent.objInst = (objInst == 0) ? SI_LPOV : SI_LPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Left POV %d released.\n",
                        (newEvent.objInst == SI_LPOV) ? 1 : 2);
#endif
                    break;
                }
                newEvent.objType = newEvent.objInst;
                Game->postEvent(newEvent);
            }

            if (newData != 5)
            {
                newEvent.action = SI_MAKE;
                newEvent.fValue = 1.0f;
                switch (newData)
                {
                case 0:
                    newEvent.objInst = (objInst == 0) ? SI_UPOV : SI_UPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Up POV %d pressed.\n",
                        (newEvent.objInst == SI_UPOV) ? 1 : 2);
#endif
                    break;

                case 1:
                    newEvent.objInst = (objInst == 0) ? SI_RPOV : SI_RPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Right POV %d pressed.\n",
                        (newEvent.objInst == SI_RPOV) ? 1 : 2);
#endif
                    break;

                case 2:
                    newEvent.objInst = (objInst == 0) ? SI_DPOV : SI_DPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Down POV %d pressed.\n",
                        (newEvent.objInst == SI_DPOV) ? 1 : 2);
#endif
                    break;

                case 3:
                    newEvent.objInst = (objInst == 0) ? SI_LPOV : SI_LPOV2;
#ifdef LOG_INPUT
                    Input::log("EVENT (DInput): Left POV %d pressed.\n",
                        (newEvent.objInst == SI_LPOV) ? 1 : 2);
#endif
                    break;
                }
                newEvent.objType = newEvent.objInst;
                Game->postEvent(newEvent);
            }
        }
        break;
    }

    return true;
}

//- Shake it, baby! -------------------------------------------------- jason_cahill -
void DInputDevice::rumble(float x, float y)
{
    LONG            rglDirection[2] = { 0, 0 };
    DICONSTANTFORCE cf = { 0 };
    HRESULT         result;

    // Now set the new parameters and start the effect immediately.
    if (!mForceFeedbackEffect)
    {
#ifdef LOG_INPUT
        Input::log("DInputDevice::rumbleJoystick - creating constant force feeback effect\n");
#endif
        DIEFFECT eff;
        ZeroMemory(&eff, sizeof(eff));
        eff.dwSize = sizeof(DIEFFECT);
        eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        eff.dwDuration = INFINITE;
        eff.dwSamplePeriod = 0;
        eff.dwGain = DI_FFNOMINALMAX;
        eff.dwTriggerButton = DIEB_NOTRIGGER;
        eff.dwTriggerRepeatInterval = 0;
        eff.cAxes = mNumForceFeedbackAxes;
        eff.rgdwAxes = mForceFeedbackAxes;
        eff.rglDirection = rglDirection;
        eff.lpEnvelope = 0;
        eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
        eff.lpvTypeSpecificParams = &cf;
        eff.dwStartDelay = 0;

        // Create the prepared effect
        if (FAILED(result = mDevice->CreateEffect(GUID_ConstantForce, &eff, &mForceFeedbackEffect, NULL)))
        {
#ifdef LOG_INPUT
            Input::log("DInputDevice::rumbleJoystick - %s does not support force feedback.\n", mName);
#endif
            Con::errorf("DInputDevice::rumbleJoystick - %s does not support force feedback.\n", mName);
            return;
        }
        else
        {
#ifdef LOG_INPUT
            Input::log("DInputDevice::rumbleJoystick - %s supports force feedback.\n", mName);
#endif
            Con::printf("DInputDevice::rumbleJoystick - %s supports force feedback.\n", mName);
        }
    }

    // Clamp the input floats to [0 - 1]
    x = max(0, min(1, x));
    y = max(0, min(1, y));

    if (1 == mNumForceFeedbackAxes)
    {
        cf.lMagnitude = (DWORD)(x * DI_FFNOMINALMAX);
    }
    else
    {
        rglDirection[0] = (DWORD)(x * DI_FFNOMINALMAX);
        rglDirection[1] = (DWORD)(y * DI_FFNOMINALMAX);
        cf.lMagnitude = (DWORD)sqrt((double)(x * x * DI_FFNOMINALMAX * DI_FFNOMINALMAX + y * y * DI_FFNOMINALMAX * DI_FFNOMINALMAX));
    }

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = mNumForceFeedbackAxes;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams = &cf;
    eff.dwStartDelay = 0;

    if (FAILED(result = mForceFeedbackEffect->SetParameters(&eff, DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS | DIEP_START)))
    {
        const char* errorString = NULL;
        switch (result)
        {
        case DIERR_INPUTLOST:
            errorString = "DIERR_INPUTLOST";
            break;

        case DIERR_INVALIDPARAM:
            errorString = "DIERR_INVALIDPARAM";
            break;

        case DIERR_NOTACQUIRED:
            errorString = "DIERR_NOTACQUIRED";
            break;

        default:
            errorString = "Unknown Error";
        }

#ifdef LOG_INPUT
        Input::log("DInputDevice::rumbleJoystick - %s - Failed to start rumble effect\n", errorString);
#endif
        Con::errorf("DInputDevice::rumbleJoystick - %s - Failed to start rumble effect\n", errorString);
    }
}

//------------------------------------------------------------------------------
//
// This function translates the DirectInput scan code to the associated
// internal key code (as defined in event.h).
//
//------------------------------------------------------------------------------
U16 DIK_to_Key(U8 dikCode)
{
    switch (dikCode)
    {
    case DIK_ESCAPE:        return KEY_ESCAPE;

    case DIK_1:             return KEY_1;
    case DIK_2:             return KEY_2;
    case DIK_3:             return KEY_3;
    case DIK_4:             return KEY_4;
    case DIK_5:             return KEY_5;
    case DIK_6:             return KEY_6;
    case DIK_7:             return KEY_7;
    case DIK_8:             return KEY_8;
    case DIK_9:             return KEY_9;
    case DIK_0:             return KEY_0;

    case DIK_MINUS:         return KEY_MINUS;
    case DIK_EQUALS:        return KEY_EQUALS;
    case DIK_BACK:          return KEY_BACKSPACE;
    case DIK_TAB:           return KEY_TAB;

    case DIK_Q:             return KEY_Q;
    case DIK_W:             return KEY_W;
    case DIK_E:             return KEY_E;
    case DIK_R:             return KEY_R;
    case DIK_T:             return KEY_T;
    case DIK_Y:             return KEY_Y;
    case DIK_U:             return KEY_U;
    case DIK_I:             return KEY_I;
    case DIK_O:             return KEY_O;
    case DIK_P:             return KEY_P;

    case DIK_LBRACKET:      return KEY_LBRACKET;
    case DIK_RBRACKET:      return KEY_RBRACKET;
    case DIK_RETURN:        return KEY_RETURN;
    case DIK_LCONTROL:      return KEY_LCONTROL;

    case DIK_A:             return KEY_A;
    case DIK_S:             return KEY_S;
    case DIK_D:             return KEY_D;
    case DIK_F:             return KEY_F;
    case DIK_G:             return KEY_G;
    case DIK_H:             return KEY_H;
    case DIK_J:             return KEY_J;
    case DIK_K:             return KEY_K;
    case DIK_L:             return KEY_L;

    case DIK_SEMICOLON:     return KEY_SEMICOLON;
    case DIK_APOSTROPHE:    return KEY_APOSTROPHE;
    case DIK_GRAVE:         return KEY_TILDE;
    case DIK_LSHIFT:        return KEY_LSHIFT;
    case DIK_BACKSLASH:     return KEY_BACKSLASH;

    case DIK_Z:             return KEY_Z;
    case DIK_X:             return KEY_X;
    case DIK_C:             return KEY_C;
    case DIK_V:             return KEY_V;
    case DIK_B:             return KEY_B;
    case DIK_N:             return KEY_N;
    case DIK_M:             return KEY_M;

    case DIK_COMMA:         return KEY_COMMA;
    case DIK_PERIOD:        return KEY_PERIOD;
    case DIK_SLASH:         return KEY_SLASH;
    case DIK_RSHIFT:        return KEY_RSHIFT;
    case DIK_MULTIPLY:      return KEY_MULTIPLY;
    case DIK_LALT:          return KEY_LALT;
    case DIK_SPACE:         return KEY_SPACE;
    case DIK_CAPSLOCK:      return KEY_CAPSLOCK;

    case DIK_F1:            return KEY_F1;
    case DIK_F2:            return KEY_F2;
    case DIK_F3:            return KEY_F3;
    case DIK_F4:            return KEY_F4;
    case DIK_F5:            return KEY_F5;
    case DIK_F6:            return KEY_F6;
    case DIK_F7:            return KEY_F7;
    case DIK_F8:            return KEY_F8;
    case DIK_F9:            return KEY_F9;
    case DIK_F10:           return KEY_F10;

    case DIK_NUMLOCK:       return KEY_NUMLOCK;
    case DIK_SCROLL:        return KEY_SCROLLLOCK;

    case DIK_NUMPAD7:       return KEY_NUMPAD7;
    case DIK_NUMPAD8:       return KEY_NUMPAD8;
    case DIK_NUMPAD9:       return KEY_NUMPAD9;
    case DIK_SUBTRACT:      return KEY_SUBTRACT;

    case DIK_NUMPAD4:       return KEY_NUMPAD4;
    case DIK_NUMPAD5:       return KEY_NUMPAD5;
    case DIK_NUMPAD6:       return KEY_NUMPAD6;
    case DIK_ADD:           return KEY_ADD;

    case DIK_NUMPAD1:       return KEY_NUMPAD1;
    case DIK_NUMPAD2:       return KEY_NUMPAD2;
    case DIK_NUMPAD3:       return KEY_NUMPAD3;
    case DIK_NUMPAD0:       return KEY_NUMPAD0;
    case DIK_DECIMAL:       return KEY_DECIMAL;

    case DIK_F11:           return KEY_F11;
    case DIK_F12:           return KEY_F12;
    case DIK_F13:           return KEY_F13;
    case DIK_F14:           return KEY_F14;
    case DIK_F15:           return KEY_F15;

    case DIK_KANA:          return 0;
    case DIK_CONVERT:       return 0;
    case DIK_NOCONVERT:     return 0;
    case DIK_YEN:           return 0;
    case DIK_NUMPADEQUALS:  return 0;
    case DIK_CIRCUMFLEX:    return 0;
    case DIK_AT:            return 0;
    case DIK_COLON:         return 0;
    case DIK_UNDERLINE:     return 0;
    case DIK_KANJI:         return 0;
    case DIK_STOP:          return 0;
    case DIK_AX:            return 0;
    case DIK_UNLABELED:     return 0;

    case DIK_NUMPADENTER:   return KEY_NUMPADENTER;
    case DIK_RCONTROL:      return KEY_RCONTROL;
    case DIK_NUMPADCOMMA:   return KEY_SEPARATOR;
    case DIK_DIVIDE:        return KEY_DIVIDE;
    case DIK_SYSRQ:         return KEY_PRINT;
    case DIK_RALT:          return KEY_RALT;
    case DIK_PAUSE:         return KEY_PAUSE;

    case DIK_HOME:          return KEY_HOME;
    case DIK_UP:            return KEY_UP;
    case DIK_PGUP:          return KEY_PAGE_UP;
    case DIK_LEFT:          return KEY_LEFT;
    case DIK_RIGHT:         return KEY_RIGHT;
    case DIK_END:           return KEY_END;
    case DIK_DOWN:          return KEY_DOWN;
    case DIK_PGDN:          return KEY_PAGE_DOWN;
    case DIK_INSERT:        return KEY_INSERT;
    case DIK_DELETE:        return KEY_DELETE;

    case DIK_LWIN:          return KEY_WIN_LWINDOW;
    case DIK_RWIN:          return KEY_WIN_RWINDOW;
    case DIK_APPS:          return KEY_WIN_APPS;
    case DIK_OEM_102:       return KEY_OEM_102;
    }

    return KEY_NULL;
}

//------------------------------------------------------------------------------
//
// This function translates an internal key code to the associated
// DirectInput scan code
//
//------------------------------------------------------------------------------
U8 Key_to_DIK(U16 keyCode)
{
    switch (keyCode)
    {
    case KEY_BACKSPACE:     return DIK_BACK;
    case KEY_TAB:           return DIK_TAB;
    case KEY_RETURN:        return DIK_RETURN;
        //KEY_CONTROL:
        //KEY_ALT:
        //KEY_SHIFT:
    case KEY_PAUSE:         return DIK_PAUSE;
    case KEY_CAPSLOCK:      return DIK_CAPSLOCK;
    case KEY_ESCAPE:        return DIK_ESCAPE;

    case KEY_SPACE:         return DIK_SPACE;
    case KEY_PAGE_DOWN:     return DIK_PGDN;
    case KEY_PAGE_UP:       return DIK_PGUP;
    case KEY_END:           return DIK_END;
    case KEY_HOME:          return DIK_HOME;
    case KEY_LEFT:          return DIK_LEFT;
    case KEY_UP:            return DIK_UP;
    case KEY_RIGHT:         return DIK_RIGHT;
    case KEY_DOWN:          return DIK_DOWN;
    case KEY_PRINT:         return DIK_SYSRQ;
    case KEY_INSERT:        return DIK_INSERT;
    case KEY_DELETE:        return DIK_DELETE;
    case KEY_HELP:          return 0;

    case KEY_0:             return DIK_0;
    case KEY_1:             return DIK_1;
    case KEY_2:             return DIK_2;
    case KEY_3:             return DIK_3;
    case KEY_4:             return DIK_4;
    case KEY_5:             return DIK_5;
    case KEY_6:             return DIK_6;
    case KEY_7:             return DIK_7;
    case KEY_8:             return DIK_8;
    case KEY_9:             return DIK_9;

    case KEY_A:             return DIK_A;
    case KEY_B:             return DIK_B;
    case KEY_C:             return DIK_C;
    case KEY_D:             return DIK_D;
    case KEY_E:             return DIK_E;
    case KEY_F:             return DIK_F;
    case KEY_G:             return DIK_G;
    case KEY_H:             return DIK_H;
    case KEY_I:             return DIK_I;
    case KEY_J:             return DIK_J;
    case KEY_K:             return DIK_K;
    case KEY_L:             return DIK_L;
    case KEY_M:             return DIK_M;
    case KEY_N:             return DIK_N;
    case KEY_O:             return DIK_O;
    case KEY_P:             return DIK_P;
    case KEY_Q:             return DIK_Q;
    case KEY_R:             return DIK_R;
    case KEY_S:             return DIK_S;
    case KEY_T:             return DIK_T;
    case KEY_U:             return DIK_U;
    case KEY_V:             return DIK_V;
    case KEY_W:             return DIK_W;
    case KEY_X:             return DIK_X;
    case KEY_Y:             return DIK_Y;
    case KEY_Z:             return DIK_Z;

    case KEY_TILDE:         return DIK_GRAVE;
    case KEY_MINUS:         return DIK_MINUS;
    case KEY_EQUALS:        return DIK_EQUALS;
    case KEY_LBRACKET:      return DIK_LBRACKET;
    case KEY_RBRACKET:      return DIK_RBRACKET;
    case KEY_BACKSLASH:     return DIK_BACKSLASH;
    case KEY_SEMICOLON:     return DIK_SEMICOLON;
    case KEY_APOSTROPHE:    return DIK_APOSTROPHE;
    case KEY_COMMA:         return DIK_COMMA;
    case KEY_PERIOD:        return DIK_PERIOD;
    case KEY_SLASH:         return DIK_SLASH;

    case KEY_NUMPAD0:       return DIK_NUMPAD0;
    case KEY_NUMPAD1:       return DIK_NUMPAD1;
    case KEY_NUMPAD2:       return DIK_NUMPAD2;
    case KEY_NUMPAD3:       return DIK_NUMPAD3;
    case KEY_NUMPAD4:       return DIK_NUMPAD4;
    case KEY_NUMPAD5:       return DIK_NUMPAD5;
    case KEY_NUMPAD6:       return DIK_NUMPAD6;
    case KEY_NUMPAD7:       return DIK_NUMPAD7;
    case KEY_NUMPAD8:       return DIK_NUMPAD8;
    case KEY_NUMPAD9:       return DIK_NUMPAD9;
    case KEY_MULTIPLY:      return DIK_MULTIPLY;
    case KEY_ADD:           return DIK_ADD;
    case KEY_SEPARATOR:     return DIK_NUMPADCOMMA;
    case KEY_SUBTRACT:      return DIK_SUBTRACT;
    case KEY_DECIMAL:       return DIK_DECIMAL;
    case KEY_DIVIDE:        return DIK_DIVIDE;
    case KEY_NUMPADENTER:   return DIK_NUMPADENTER;

    case KEY_F1:            return DIK_F1;
    case KEY_F2:            return DIK_F2;
    case KEY_F3:            return DIK_F3;
    case KEY_F4:            return DIK_F4;
    case KEY_F5:            return DIK_F5;
    case KEY_F6:            return DIK_F6;
    case KEY_F7:            return DIK_F7;
    case KEY_F8:            return DIK_F8;
    case KEY_F9:            return DIK_F9;
    case KEY_F10:           return DIK_F10;
    case KEY_F11:           return DIK_F11;
    case KEY_F12:           return DIK_F12;
    case KEY_F13:           return DIK_F13;
    case KEY_F14:           return DIK_F14;
    case KEY_F15:           return DIK_F15;
    case KEY_F16:
    case KEY_F17:
    case KEY_F18:
    case KEY_F19:
    case KEY_F20:
    case KEY_F21:
    case KEY_F22:
    case KEY_F23:
    case KEY_F24:           return 0;

    case KEY_NUMLOCK:       return DIK_NUMLOCK;
    case KEY_SCROLLLOCK:    return DIK_SCROLL;
    case KEY_LCONTROL:      return DIK_LCONTROL;
    case KEY_RCONTROL:      return DIK_RCONTROL;
    case KEY_LALT:          return DIK_LALT;
    case KEY_RALT:          return DIK_RALT;
    case KEY_LSHIFT:        return DIK_LSHIFT;
    case KEY_RSHIFT:        return DIK_RSHIFT;

    case KEY_WIN_LWINDOW:   return DIK_LWIN;
    case KEY_WIN_RWINDOW:   return DIK_RWIN;
    case KEY_WIN_APPS:      return DIK_APPS;
    case KEY_OEM_102:       return DIK_OEM_102;

    };

    return 0;
}

#ifdef LOG_INPUT
//------------------------------------------------------------------------------
const char* getKeyName(U16 key)
{
    switch (key)
    {
    case KEY_BACKSPACE:     return "Backspace";
    case KEY_TAB:           return "Tab";
    case KEY_RETURN:        return "Return";
    case KEY_PAUSE:         return "Pause";
    case KEY_CAPSLOCK:      return "CapsLock";
    case KEY_ESCAPE:        return "Esc";

    case KEY_SPACE:         return "SpaceBar";
    case KEY_PAGE_DOWN:     return "PageDown";
    case KEY_PAGE_UP:       return "PageUp";
    case KEY_END:           return "End";
    case KEY_HOME:          return "Home";
    case KEY_LEFT:          return "Left";
    case KEY_UP:            return "Up";
    case KEY_RIGHT:         return "Right";
    case KEY_DOWN:          return "Down";
    case KEY_PRINT:         return "PrintScreen";
    case KEY_INSERT:        return "Insert";
    case KEY_DELETE:        return "Delete";
    case KEY_HELP:          return "Help";

    case KEY_NUMPAD0:       return "Numpad 0";
    case KEY_NUMPAD1:       return "Numpad 1";
    case KEY_NUMPAD2:       return "Numpad 2";
    case KEY_NUMPAD3:       return "Numpad 3";
    case KEY_NUMPAD4:       return "Numpad 4";
    case KEY_NUMPAD5:       return "Numpad 5";
    case KEY_NUMPAD6:       return "Numpad 6";
    case KEY_NUMPAD7:       return "Numpad 7";
    case KEY_NUMPAD8:       return "Numpad 8";
    case KEY_NUMPAD9:       return "Numpad 9";
    case KEY_MULTIPLY:      return "Multiply";
    case KEY_ADD:           return "Add";
    case KEY_SEPARATOR:     return "Separator";
    case KEY_SUBTRACT:      return "Subtract";
    case KEY_DECIMAL:       return "Decimal";
    case KEY_DIVIDE:        return "Divide";
    case KEY_NUMPADENTER:   return "Numpad Enter";

    case KEY_F1:            return "F1";
    case KEY_F2:            return "F2";
    case KEY_F3:            return "F3";
    case KEY_F4:            return "F4";
    case KEY_F5:            return "F5";
    case KEY_F6:            return "F6";
    case KEY_F7:            return "F7";
    case KEY_F8:            return "F8";
    case KEY_F9:            return "F9";
    case KEY_F10:           return "F10";
    case KEY_F11:           return "F11";
    case KEY_F12:           return "F12";
    case KEY_F13:           return "F13";
    case KEY_F14:           return "F14";
    case KEY_F15:           return "F15";
    case KEY_F16:           return "F16";
    case KEY_F17:           return "F17";
    case KEY_F18:           return "F18";
    case KEY_F19:           return "F19";
    case KEY_F20:           return "F20";
    case KEY_F21:           return "F21";
    case KEY_F22:           return "F22";
    case KEY_F23:           return "F23";
    case KEY_F24:           return "F24";

    case KEY_NUMLOCK:       return "NumLock";
    case KEY_SCROLLLOCK:    return "ScrollLock";
    case KEY_LCONTROL:      return "LCtrl";
    case KEY_RCONTROL:      return "RCtrl";
    case KEY_LALT:          return "LAlt";
    case KEY_RALT:          return "RAlt";
    case KEY_LSHIFT:        return "LShift";
    case KEY_RSHIFT:        return "RShift";

    case KEY_WIN_LWINDOW:   return "LWin";
    case KEY_WIN_RWINDOW:   return "RWin";
    case KEY_WIN_APPS:      return "Apps";
    }

    static char returnString[5];
    dSprintf(returnString, sizeof(returnString), "%c", Input::getAscii(key, STATE_UPPER));
    return returnString;
}
#endif // LOG_INPUT

//------------------------------------------------------------------------------
const char* DInputDevice::getJoystickAxesString()
{
    if (mDeviceType != JoystickDeviceType)
        return("");

    U32 axisCount = mDeviceCaps.dwAxes;
    char buf[64];
    dSprintf(buf, sizeof(buf), "%d", axisCount);
    for (U32 i = 0; i < mObjCount; i++)
    {
        switch (mObjInfo[i].mType)
        {
        case SI_XAXIS:
            dStrcat(buf, "\tX");
            break;
        case SI_YAXIS:
            dStrcat(buf, "\tY");
            break;
        case SI_ZAXIS:
            dStrcat(buf, "\tZ");
            break;
        case SI_RXAXIS:
            dStrcat(buf, "\tR");
            break;
        case SI_RYAXIS:
            dStrcat(buf, "\tU");
            break;
        case SI_RZAXIS:
            dStrcat(buf, "\tV");
            break;
        case SI_SLIDER:
            dStrcat(buf, "\tS");
            break;
        }
    }

    char* returnString = Con::getReturnBuffer(dStrlen(buf) + 1);
    dStrcpy(returnString, buf);
    return(returnString);
}

//------------------------------------------------------------------------------
bool DInputDevice::joystickDetected()
{
    return(smJoystickCount > 0);
}



